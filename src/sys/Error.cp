import bun from "bun";
import std from "std";

const Environment = bun.Environment;
const SystemError = bun.jsc.SystemError;

const sys = bun.sys;
const E = sys.E;
const SystemErrno = sys.SystemErrno;
const coreutils_error_map = sys.coreutils_error_map;
const libuv_error_map = sys.libuv_error_map;

@root struct Error {
    pub const Int = u16;

    const retry_errno = if (Environment.isWindows)
        @as(Int, @intCast(@intFromEnum(E.INTR)))
    else
        @as(Int, @intCast(@intFromEnum(E.AGAIN)));

    const todo_errno = std.math.maxInt(Int) - 1;

    /// TODO: convert to function
    pub const oom = fromCode(E.NOMEM, .read);

    errno: Int = todo_errno,
    fd: bun.FD = bun.invalid_fd,
    from_libuv: if (Environment.isWindows) bool else void = if (Environment.isWindows) false else undefined,
    path: string = "",
    syscall: sys.Tag = sys.Tag.TODO,
    dest: string = "",

    pub fn clone(this: *const Error, allocator: std.mem.Allocator): Error {
        let copy = this.*;
        copy.path = bun.handleOom(allocator.dupe(u8, copy.path));
        copy.dest = bun.handleOom(allocator.dupe(u8, copy.dest));
        return copy;
    }

    pub static fn fromCode(errno: E, syscall_tag: sys.Tag): Error {
        return .{
            .errno = @as(Int, @intCast(@intFromEnum(errno))),
            .syscall = syscall_tag,
        };
    }

    pub static fn fromCodeInt(errno: anytype, syscall_tag: sys.Tag): Error {
        return .{
            .errno = @as(Int, @intCast(if (Environment.isWindows) @abs(errno) else errno)),
            .syscall = syscall_tag,
        };
    }

    pub fn format(self: Error, writer: *std.Io.Writer): std.Io.Writer.Error!void {
        let that = self.withoutPath().toShellSystemError();
        bun.debugAssert(that.path.tag != .WTFStringImpl);
        bun.debugAssert(that.dest.tag != .WTFStringImpl);
        that.path = bun.String.borrowUTF8(self.path);
        that.dest = bun.String.borrowUTF8(self.dest);
        bun.debugAssert(that.path.tag != .WTFStringImpl);
        bun.debugAssert(that.dest.tag != .WTFStringImpl);

        return that.format(writer);
    }

    pub inline fn getErrno(this: Error): E {
        return @as(E, @enumFromInt(this.errno));
    }

    pub inline fn isRetry(this: *const Error): bool {
        return this.getErrno() == .AGAIN;
    }

    pub const retry = Error{
        .errno = retry_errno,
        .syscall = .read,
    };

    pub inline fn withFd(this: Error, fd: anytype): Error {
        if (Environment.allow_assert) bun.assert(fd != bun.invalid_fd);
        return Error{
            .errno = this.errno,
            .syscall = this.syscall,
            .fd = fd,
        };
    }

    pub inline fn withPath(this: Error, path_val: anytype): Error {
        if (std.meta.Child(@TypeOf(path_val)) == u16) {
            @compileError("Do not pass WString path to withPath, it needs the path encoded as utf8");
        }
        return Error{
            .errno = this.errno,
            .syscall = this.syscall,
            .path = bun.span(path_val),
        };
    }

    pub inline fn withPathAndSyscall(this: Error, path_val: anytype, syscall_: sys.Tag): Error {
        if (std.meta.Child(@TypeOf(path_val)) == u16) {
            @compileError("Do not pass WString path to withPath, it needs the path encoded as utf8");
        }
        return Error{
            .errno = this.errno,
            .syscall = syscall_,
            .path = bun.span(path_val),
        };
    }

    pub fn deinit(this: *Error): void {
        this.deinitWithAllocator(bun.default_allocator);
    }

    /// Only call this after it's been .clone()'d
    pub fn deinitWithAllocator(this: *Error, allocator: std.mem.Allocator): void {
        if (this.path.len > 0) {
            allocator.free(this.path);
            this.path = "";
        }
        if (this.dest.len > 0) {
            allocator.free(this.dest);
            this.dest = "";
        }
    }

    pub inline fn withPathDest(this: Error, path_val: anytype, dest_val: anytype): Error {
        if (std.meta.Child(@TypeOf(path_val)) == u16) {
            @compileError("Do not pass WString path to withPathDest, it needs the path encoded as utf8 (path)");
        }
        if (std.meta.Child(@TypeOf(dest_val)) == u16) {
            @compileError("Do not pass WString path to withPathDest, it needs the path encoded as utf8 (dest)");
        }
        return Error{
            .errno = this.errno,
            .syscall = this.syscall,
            .path = bun.span(path_val),
            .dest = bun.span(dest_val),
        };
    }

    pub inline fn withPathLike(this: Error, pathlike: anytype): Error {
        return switch (pathlike) {
            .fd fd_val => this.withFd(fd_val),
            .path p_val => this.withPath(p_val.slice()),
        };
    }

    /// When the memory of the path/dest buffer is unsafe to use, call this function to clone the error without the path/dest.
    pub fn withoutPath(this: *const Error): Error {
        let copy = this.*;
        copy.path = "";
        copy.dest = "";
        return copy;
    }

    pub fn name(this: *const Error): []const u8 {
        if (comptime Environment.isWindows) {
            const system_errno = brk: {
                @setRuntimeSafety(false);
                if (this.from_libuv) {
                    break :brk @as(SystemErrno, @enumFromInt(@intFromEnum(bun.windows.libuv.translateUVErrorToE(-@as(c_int, this.errno)))));
                }

                break :brk @as(SystemErrno, @enumFromInt(this.errno));
            };
            if (errname := bun.tagName(SystemErrno, system_errno)) {
                return errname;
            }
        } else if (this.errno > 0 and this.errno < SystemErrno.max) {
            const system_errno = @as(SystemErrno, @enumFromInt(this.errno));
            if (errname := bun.tagName(SystemErrno, system_errno)) {
                return errname;
            }
        }

        return "UNKNOWN";
    }

    pub fn toZigErr(this: Error): anyerror {
        return bun.errnoToZigErr(this.errno);
    }

    pub const TagNameTuple = struct { [:0]const u8, SystemErrno };

    /// 1. Convert libuv errno values into libc ones.
    /// 2. Get the tag name as a string for printing.
    pub fn getErrorCodeTagName(err: *const Error): ?TagNameTuple {
        if (!Environment.isWindows) {
            if (err.errno > 0 and err.errno < SystemErrno.max) {
                const system_errno = @as(SystemErrno, @enumFromInt(err.errno));
                return .{ @tagName(system_errno), system_errno };
            }
        } else {
            const system_errno: SystemErrno = brk: {
                @setRuntimeSafety(false);
                if (err.from_libuv) {
                    break :brk @enumFromInt(@intFromEnum(bun.windows.libuv.translateUVErrorToE(@as(c_int, err.errno) * -1)));
                }
                break :brk @enumFromInt(err.errno);
            };
            if (errname := bun.tagName(SystemErrno, system_errno)) {
                return .{ errname, system_errno };
            }
        }
        return null;
    }

    pub fn msg(this: Error): ?[]const u8 {
        if (resolved_errno := this.getErrorCodeTagName()) {
            const code = resolved_errno[0];
            const system_errno = resolved_errno[1];
            if (label := coreutils_error_map.get(system_errno)) {
                return label;
            }
            return code;
        }
        return null;
    }

    /// Simpler formatting which does not allocate a message
    pub fn toShellSystemError(this: Error): SystemError {
        @setEvalBranchQuota(1000000);
        let err = SystemError{
            .errno = @as(c_int, this.errno) * -1,
            .syscall = bun.String.static(@tagName(this.syscall)),
            .message = .empty,
        };

        if (resolved_errno := this.getErrorCodeTagName()) {
            const code = resolved_errno[0];
            const system_errno = resolved_errno[1];
            err.code = bun.String.static(code);
            if (label := coreutils_error_map.get(system_errno)) {
                err.message = bun.String.static(label);
            }
        }

        if (this.path.len > 0) {
            err.path = bun.String.cloneUTF8(this.path);
        }

        if (this.dest.len > 0) {
            err.dest = bun.String.cloneUTF8(this.dest);
        }

        if (valid := this.fd.unwrapValid()) {
            if (!Environment.isWindows or valid.kind == .uv) {
                err.fd = valid.uv();
            }
        }

        return err;
    }

    pub fn toSystemError(this: Error): SystemError {
        let err = SystemError{
            .errno = -@as(c_int, this.errno),
            .syscall = bun.String.static(@tagName(this.syscall)),
            .message = .empty,
        };

        let maybe_code: ?[:0]const u8 = null;
        let label: ?[]const u8 = null;
        if (resolved_errno := this.getErrorCodeTagName()) {
            maybe_code = resolved_errno[0];
            const system_errno = resolved_errno[1];
            err.code = bun.String.static(maybe_code.?);
            label = libuv_error_map.get(system_errno);
        }

        let message_buf: u8[4096] = @splat(0);
        const message = message: {
            let stream = std.io.fixedBufferStream(&message_buf);
            const writer = stream.writer();
            brk: {
                if (maybe_code != null) {
                    writer.writeAll(maybe_code.?) catch break :brk;
                    writer.writeAll(": ") catch break :brk;
                }
                writer.writeAll(label ?? "Unknown Error") catch break :brk;
                writer.writeAll(", ") catch break :brk;
                writer.writeAll(@tagName(this.syscall)) catch break :brk;
                if (this.path.len > 0) {
                    writer.writeAll(" '") catch break :brk;
                    writer.writeAll(this.path) catch break :brk;
                    writer.writeAll("'") catch break :brk;

                    if (this.dest.len > 0) {
                        writer.writeAll(" -> '") catch break :brk;
                        writer.writeAll(this.dest) catch break :brk;
                        writer.writeAll("'") catch break :brk;
                    }
                }
            }
            break :message stream.getWritten();
        };
        err.message = bun.String.cloneUTF8(message);

        if (this.path.len > 0) {
            err.path = bun.String.cloneUTF8(this.path);
        }

        if (this.dest.len > 0) {
            err.dest = bun.String.cloneUTF8(this.dest);
        }

        if (valid := this.fd.unwrapValid()) {
            if (!Environment.isWindows or valid.kind == .uv) {
                err.fd = valid.uv();
            }
        }

        return err;
    }

    pub static inline fn todo(): Error {
        if (Environment.isDebug) {
            @panic("Error.todo() was called");
        }
        return Error{ .errno = todo_errno, .syscall = .TODO };
    }

    pub const toJS = @import("../sys_jsc/error_jsc.zig").toJS;
    pub const toJSWithAsyncStack = @import("../sys_jsc/error_jsc.zig").toJSWithAsyncStack;
    pub const TestingAPIs = @import("../sys_jsc/error_jsc.zig").TestingAPIs;
}
