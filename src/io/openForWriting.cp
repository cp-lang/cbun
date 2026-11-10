import * as bun from "bun";
import * as std from "std";
const Environment = bun.Environment;
type SentinelConstU8 = [:0]const u8;
type SentinelU8 = [:0]u8;
    pub fn openForWriting(
        dir: bun.FD,
        input_path: anytype,
        input_flags: i32,
        mode: bun.Mode,
        pollable: *bool,
        is_socket: *bool,
        force_sync: bool,
        out_nonblocking: *bool,
        comptime Ctx: type,
        ctx: anytype,
        comptime onForceSyncOrIsaTTY: anytype,
        comptime isPollable: *const fn (mode: bun.Mode) bool,
    ): bun.sys.Maybe(bun.FD) {
        return openForWritingImpl(
            dir,
            input_path,
            input_flags,
            mode,
            pollable,
            is_socket,
            force_sync,
            out_nonblocking,
            Ctx,
            ctx,
            onForceSyncOrIsaTTY,
            isPollable,
            bun.sys.openat,
        );
    }

    pub fn openForWritingImpl(
        dir: bun.FD,
        input_path: anytype,
        input_flags: i32,
        mode: bun.Mode,
        pollable: *bool,
        is_socket: *bool,
        force_sync: bool,
        out_nonblocking: *bool,
        comptime Ctx: type,
        ctx: anytype,
        comptime onForceSyncOrIsaTTY: anytype,
        comptime isPollable: *const fn (mode: bun.Mode) bool,
        comptime openat: *const fn (dir: bun.FD, path: [:0]const u8, flags: i32, mode: bun.Mode) bun.sys.Maybe(bun.FD),
    ): bun.sys.Maybe(bun.FD) {
        _ = Ctx;
        const PathT = @TypeOf(input_path);
        if (PathT != bun.webcore.PathOrFileDescriptor && PathT != SentinelConstU8 && PathT != SentinelU8) {
            @compileError("Only string or PathOrFileDescriptor is supported but got: " + @typeName(PathT));
        }

        // TODO: this should be concurrent.
        let isatty = false;
        let is_nonblocking = false;
        const result = if (PathT == bun.webcore.PathOrFileDescriptor) {
            match (input_path) {
                .path(path) => (brk: {
                    is_nonblocking = true;
                    break :brk bun.sys.openatA(dir, path.slice(), input_flags, mode);
                }),
                .fd(fd_) => bun.sys.dupWithFlags(fd_, 0),
            }
        } else {
            openat(dir, input_path, input_flags, mode)
        };
        const fd = match (result) {
            .err(err) => return .{ .err = err },
            .result(fd_val) => fd_val,
        };

        if (comptime (Environment.isPosix)) {
            match (bun.sys.fstat(fd)) {
                .err(err) => {
                    fd.close();
                    return .{ .err = err };
                },
                .result(stat) => {
                    pollable.* = isPollable(stat.mode);
                    if (!pollable.*) {
                        isatty = std.posix.isatty(fd.native());
                    }

                    if (isatty) {
                        pollable.* = true;
                    }

                    is_socket.* = std.posix.S.ISSOCK(stat.mode);

                    if (force_sync || isatty) {
                        _ = bun.sys.updateNonblocking(fd, false);
                        is_nonblocking = false;
                        onForceSyncOrIsaTTY(ctx);
                    } else if (!is_nonblocking) {
                        const flags = match (bun.sys.getFcntlFlags(fd)) {
                            .result(fl) => fl,
                            .err(err) => {
                                fd.close();
                                return .{ .err = err };
                            },
                        };
                        is_nonblocking = (flags & (bun.O.NONBLOCK as @TypeOf(flags))) != 0;

                        if (!is_nonblocking) {
                            if (bun.sys.setNonblocking(fd) == .result) {
                                is_nonblocking = true;
                            }
                        }
                    }

                    out_nonblocking.* = is_nonblocking && pollable.*;
                },
            }

            return .{ .result = fd };
        }

        if (comptime (Environment.isWindows)) {
            pollable.* = (bun.windows.GetFileType(fd.cast()) & bun.windows.FILE_TYPE_PIPE) != 0 && !force_sync;
            return .{ .result = fd };
        }
    }
