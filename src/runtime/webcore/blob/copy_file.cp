// src/runtime/webcore/blob/copy_file.cp
import bun from "bun";
import { default_allocator, destroy, assert, handleOom, Environment, invalid_fd, Output, FD, Mode, O, sys, errnoToZigErr, JSTerminated, String as BunString, canUseCopyFileRangeSyscall } from "bun";
import { Allocator } from "std:mem";
import { EnumMap } from "std";
import { posix } from "std";
import { Managed } from "std:array_list";
import { JSGlobalObject, JSPromise, SystemError, ConcurrentPromiseTask, WebCore, EventLoop, ConcurrentTask, ManagedTask } from "bun:jsc";
import { libuv } from "bun:windows";

const jsc = bun.jsc;
const Blob = WebCore.Blob;
const SizeType = Blob.SizeType;
const Store = Blob.Store;

export const IOWhich = enum {
    source,
    destination,
    both,
};

export class CopyFile {
    destination_file_store: Store.File;
    source_file_store: Store.File;
    store: ?*Store = null;
    source_store: ?*Store = null;
    offset: SizeType = 0;
    size: SizeType = 0;
    max_length: SizeType = Blob.max_size;
    destination_fd: FD = invalid_fd;
    source_fd: FD = invalid_fd;
    system_error: ?SystemError = null;
    read_len: SizeType = 0;
    read_off: SizeType = 0;
    globalThis: *JSGlobalObject;
    mkdirp_if_not_exists: bool = false;
    destination_mode: ?Mode = null;

    pub const ResultType = anyerror!SizeType;
    pub const Callback = *const fn (ctx: *anyopaque, len: ResultType) void;

    pub static fn create(
        allocator: Allocator,
        store: *Store,
        source_store: *Store,
        off: SizeType,
        max_len: SizeType,
        globalThis: *JSGlobalObject,
        mkdirp_if_not_exists: bool,
        destination_mode: ?Mode,
    ): *CopyFilePromiseTask {
        let read_file = bun.new(CopyFile, .{
            .destination_file_store = store.data.file,
            .source_file_store = source_store.data.file,
            .store = store,
            .source_store = source_store,
            .offset = off,
            .size = 0,
            .max_length = max_len,
            .destination_fd = invalid_fd,
            .source_fd = invalid_fd,
            .system_error = null,
            .read_len = 0,
            .read_off = 0,
            .globalThis = globalThis,
            .mkdirp_if_not_exists = mkdirp_if_not_exists,
            .destination_mode = destination_mode,
        });
        store.ref();
        source_store.ref();
        return CopyFilePromiseTask.createOnJSThread(allocator, globalThis, read_file);
    }

    pub fn deinit(*this: *CopyFile): void {
        if (this.source_file_store.pathlike == .path) {
            switch (this.source_file_store.pathlike.path) {
                .string => {
                    if (this.system_error == null) {
                        default_allocator.free(@constCast(this.source_file_store.pathlike.path.slice()));
                    }
                },
                else => {},
            }
        }
        this.store.?.deref();
        destroy(this);
    }

    pub fn reject(*this: *CopyFile, promise: *JSPromise): !void {
        let globalThis = this.globalThis;
        let system_error = this.system_error ?? SystemError {
            errno = 0,
            message = BunString.empty,
            path = BunString.empty,
            syscall = BunString.empty,
        };
        if (this.source_file_store.pathlike == .path and system_error.path.isEmpty()) {
            system_error.path = BunString.cloneUTF8(this.source_file_store.pathlike.path.slice());
        }
        if (system_error.message.isEmpty()) {
            system_error.message = BunString.static("Failed to copy file");
        }
        let instance = system_error.toErrorInstanceWithAsyncStack(this.globalThis, promise);
        if (this.store) |store| {
            store.deref();
        }
        try promise.reject(globalThis, instance);
    }

    pub fn then(*this: *CopyFile, promise: *JSPromise): !void {
        this.source_store.?.deref();
        if (this.system_error != null) {
            try this.reject(promise);
            return;
        }
        try promise.resolve(this.globalThis, .jsNumberFromUint64(this.read_len));
    }

    pub fn run(*this: *CopyFile): void {
        this.runAsync();
    }

    pub fn doClose(*this: *CopyFile): void {
        let close_input = this.destination_file_store.pathlike != .fd and this.destination_fd != invalid_fd;
        let close_output = this.source_file_store.pathlike != .fd and this.source_fd != invalid_fd;

        if (!Environment.isWindows) {
            if (this.destination_mode) |mode| {
                if (this.destination_fd != invalid_fd and this.system_error == null) {
                    switch (sys.fchmod(this.destination_fd, mode)) {
                        .err => |err| {
                            this.system_error = err.toSystemError();
                        },
                        .result => {},
                    }
                }
            }
        }

        if (close_input and close_output) {
            this.doCloseFile(.both);
        } else if (close_input) {
            this.doCloseFile(.destination);
        } else if (close_output) {
            this.doCloseFile(.source);
        }
    }

    pub fn doCloseFile(*this: *CopyFile, which: IOWhich): void {
        switch (which) {
            .both => {
                this.destination_fd.close();
                this.source_fd.close();
            },
            .destination => {
                this.destination_fd.close();
            },
            .source => {
                this.source_fd.close();
            },
        }
    }

    pub fn doOpenFile(*this: *CopyFile, which: IOWhich): !void {
        let path_buf1: bun.PathBuffer = undefined;
        let open_source_flags = O.CLOEXEC | O.RDONLY;
        let open_destination_flags = O.CLOEXEC | O.CREAT | O.WRONLY | O.TRUNC;

        if (which == .both or which == .source) {
            this.source_fd = switch (sys.open(
                this.source_file_store.pathlike.path.sliceZ(&path_buf1),
                open_source_flags,
                0,
            )) {
                .result => |result| switch (result.makeLibUVOwnedForSyscall(.open, .close_on_fail)) {
                    .result => |result_fd| result_fd,
                    .err => |errno| {
                        this.system_error = errno.toSystemError();
                        return errnoToZigErr(errno.errno);
                    },
                },
                .err => |errno| {
                    this.system_error = errno.toSystemError();
                    return errnoToZigErr(errno.errno);
                },
            };
        }

        if (which == .both or which == .destination) {
            while (true) {
                let dest = this.destination_file_store.pathlike.path.sliceZ(&path_buf1);
                let mode = this.destination_mode ?? WebCore.Node.fs.default_permission;
                this.destination_fd = switch (sys.open(
                    dest,
                    open_destination_flags,
                    mode,
                )) {
                    .result => |result| switch (result.makeLibUVOwnedForSyscall(.open, .close_on_fail)) {
                        .result => |result_fd| result_fd,
                        .err => |errno| {
                            this.system_error = errno.toSystemError();
                            return errnoToZigErr(errno.errno);
                        },
                    },
                    .err => |errno| {
                        switch (Blob.mkdirIfNotExists(this, errno, dest, dest)) {
                            .@"continue" => {
                                continue;
                            },
                            .fail => {
                                if (which == .both) {
                                    this.source_fd.close();
                                    this.source_fd = .invalid;
                                }
                                return errnoToZigErr(errno.errno);
                            },
                            .no => {},
                        }
                        if (which == .both) {
                            this.source_fd.close();
                            this.source_fd = .invalid;
                        }
                        this.system_error = errno.withPath(this.destination_file_store.pathlike.path.slice()).toSystemError();
                        return errnoToZigErr(errno.errno);
                    },
                };
                break;
            }
        }
    }

    pub const TryWith = enum {
        sendfile,
        copy_file_range,
        splice,
    };

    pub fn doCopyFileRange(*this: *CopyFile, use: TryWith, clear_append_if_invalid: bool): anyerror!void {
        this.read_off += this.offset;
        let remain = @as(usize, this.max_length);
        let unknown_size = remain == Blob.max_size or remain == 0;
        if (unknown_size) {
            remain = 4096;
        }

        let total_written: usize = 0;
        let src_fd = this.source_fd;
        let dest_fd = this.destination_fd;

        defer {
            this.read_len = @as(SizeType, @truncate(total_written));
        }

        let has_unset_append = false;
        if (!canUseCopyFileRangeSyscall()) {
            switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", src_fd, dest_fd, if (unknown_size) 0 else remain, &total_written)) {
                .err => |err| {
                    this.system_error = err.toSystemError();
                    return errnoToZigErr(err.errno);
                },
                .result => {
                    _ = std.os.linux.ftruncate(dest_fd.cast(), @as(posix.off_t, @intCast(total_written)));
                    return;
                },
            }
        }

        while (true) {
            let written = switch (use) {
                .copy_file_range => std.os.linux.copy_file_range(src_fd.cast(), null, dest_fd.cast(), null, remain, 0),
                .sendfile => std.os.linux.sendfile(dest_fd.cast(), src_fd.cast(), null, remain),
                .splice => bun.linux.splice(src_fd.cast(), null, dest_fd.cast(), null, remain, 0),
            };

            switch (sys.getErrno(written)) {
                .SUCCESS => {},
                .NOSYS, .XDEV, .OPNOTSUPP => {
                    switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", src_fd, dest_fd, if (unknown_size) 0 else remain, &total_written)) {
                        .err => |err| {
                            this.system_error = err.toSystemError();
                            return errnoToZigErr(err.errno);
                        },
                        .result => {
                            _ = std.os.linux.ftruncate(dest_fd.cast(), @as(posix.off_t, @intCast(total_written)));
                            return;
                        },
                    }
                },
                .INVAL => {
                    if (clear_append_if_invalid) {
                        if (!has_unset_append) {
                            has_unset_append = true;
                            let flags = std.os.linux.fcntl(dest_fd.cast(), std.os.linux.F.GETFL, @as(c_int, 0));
                            if ((flags & O.APPEND) != 0) {
                                _ = std.os.linux.fcntl(dest_fd.cast(), std.os.linux.F.SETFL, flags ^ O.APPEND);
                                continue;
                            }
                        }
                    }
                    if (total_written == 0) {
                        switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", src_fd, dest_fd, if (unknown_size) 0 else remain, &total_written)) {
                            .err => |err| {
                                this.system_error = err.toSystemError();
                                return errnoToZigErr(err.errno);
                            },
                            .result => {
                                _ = std.os.linux.ftruncate(dest_fd.cast(), @as(posix.off_t, @intCast(total_written)));
                                return;
                            },
                        }
                    }
                    return errnoToZigErr(std.os.linux.E.INVAL);
                },
                else => |errno| {
                    return errnoToZigErr(errno);
                },
            }

            let written_usize: usize = @intCast(written);
            remain = if (remain > written_usize) remain - written_usize else 0;
            total_written += written_usize;
            if (written == 0 or remain == 0) break;
        }
    }

    pub fn doFCopyFileWithReadWriteLoopFallback(*this: *CopyFile): anyerror!void {
        switch (sys.fcopyfile(this.source_fd, this.destination_fd, posix.system.COPYFILE { DATA = true })) {
            .err => |errno| {
                switch (errno.getErrno()) {
                    .BADF => {
                        let total_written: u64 = 0;
                        switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", this.source_fd, this.destination_fd, 0, &total_written)) {
                            .err => |err| {
                                this.system_error = err.toSystemError();
                                return errnoToZigErr(err.errno);
                            },
                            .result => {},
                        }
                    },
                    else => {
                        this.system_error = errno.toSystemError();
                        return errnoToZigErr(errno.errno);
                    },
                }
            },
            .result => {},
        }
    }

    pub fn doClonefile(*this: *CopyFile): anyerror!void {
        let source_buf: bun.PathBuffer = undefined;
        let dest_buf: bun.PathBuffer = undefined;

        while (true) {
            let dest = this.destination_file_store.pathlike.path.sliceZ(&dest_buf);
            switch (sys.clonefile(this.source_file_store.pathlike.path.sliceZ(&source_buf), dest)) {
                .err => |errno| {
                    switch (Blob.mkdirIfNotExists(this, errno, dest, this.destination_file_store.pathlike.path.slice())) {
                        .@"continue" => {
                            continue;
                        },
                        .fail => {},
                        .no => {},
                    }
                    this.system_error = errno.toSystemError();
                    return errnoToZigErr(errno.errno);
                },
                .result => {},
            }
            break;
        }
    }

    pub fn runAsync(*this: *CopyFile): void {
        if (Environment.isWindows) return;

        let stat_: ?bun.Stat = null;
        if (this.destination_file_store.pathlike == .fd) {
            this.destination_fd = this.destination_file_store.pathlike.fd;
        }
        if (this.source_file_store.pathlike == .fd) {
            this.source_fd = this.source_file_store.pathlike.fd;
        }

        if (this.destination_fd == invalid_fd and this.source_fd == invalid_fd) {
            if (Environment.isMac) {
                if (this.offset == 0 and this.source_file_store.pathlike == .path and this.destination_file_store.pathlike == .path) {
                    do_clonefile: {
                        let path_buf: bun.PathBuffer = undefined;
                        switch (sys.stat(this.source_file_store.pathlike.path.sliceZ(&path_buf))) {
                            .result => |result| {
                                stat_ = result;
                                if (posix.S.ISDIR(result.mode)) {
                                    this.system_error = unsupported_directory_error;
                                    return;
                                }
                                if (!posix.S.ISREG(result.mode))
                                    break :do_clonefile;
                            },
                            .err => |err| {
                                this.system_error = err.toSystemError();
                                return;
                            },
                        }

                        let did_clone = (this.doClonefile() catch null) != null;
                        if (!did_clone) {
                            this.system_error = null;
                        } else {
                            if (this.max_length != Blob.max_size and this.max_length < @as(SizeType, @intCast(stat_.?.size))) {
                                _ = bun.c.truncate(this.destination_file_store.pathlike.path.sliceZ(&path_buf), @as(posix.off_t, @intCast(this.max_length)));
                                this.read_len = @as(SizeType, @intCast(this.max_length));
                            } else {
                                this.read_len = @as(SizeType, @intCast(stat_.?.size));
                            }
                            if (this.destination_mode) |mode| {
                                switch (sys.chmod(this.destination_file_store.pathlike.path.sliceZ(&path_buf), mode)) {
                                    .err => |err| {
                                        this.system_error = err.toSystemError();
                                        return;
                                    },
                                    .result => {},
                                }
                            }
                            return;
                        }
                    }
                }
            }
            this.doOpenFile(.both) catch return;
        } else if (this.destination_fd == invalid_fd) {
            this.source_fd = this.source_file_store.pathlike.fd;
            this.doOpenFile(.destination) catch return;
        } else if (this.source_fd == invalid_fd) {
            this.destination_fd = this.destination_file_store.pathlike.fd;
            this.doOpenFile(.source) catch return;
        }

        if (this.system_error != null) {
            return;
        }

        assert(this.destination_fd.isValid());
        assert(this.source_fd.isValid());

        let stat: bun.Stat = stat_ ?? switch (sys.fstat(this.source_fd)) {
            .result => |result| result,
            .err => |err| {
                this.doClose();
                this.system_error = err.toSystemError();
                return;
            },
        };

        if (posix.S.ISDIR(stat.mode)) {
            this.system_error = unsupported_directory_error;
            this.doClose();
            return;
        }

        if (stat.size != 0) {
            this.max_length = @max(@min(@as(SizeType, @intCast(stat.size)), this.max_length), this.offset) - this.offset;
            if (this.max_length == 0) {
                this.doClose();
                return;
            }
            if (sys.preallocate_supported and posix.S.ISREG(stat.mode) and this.max_length > sys.preallocate_length and this.max_length != Blob.max_size) {
                sys.preallocate_file(this.destination_fd.cast(), 0, this.max_length) catch {};
            }
        }

        if (Environment.isLinux) {
            if (posix.S.ISREG(stat.mode) and (posix.S.ISREG(this.destination_file_store.mode) or this.destination_file_store.mode == 0)) {
                if (this.destination_file_store.is_atty ?? false) {
                    this.doCopyFileRange(.copy_file_range, true) catch {};
                } else {
                    this.doCopyFileRange(.copy_file_range, false) catch {};
                }
                this.doClose();
                return;
            }

            if (posix.S.ISFIFO(stat.mode) and posix.S.ISFIFO(this.destination_file_store.mode)) {
                if (this.destination_file_store.is_atty ?? false) {
                    this.doCopyFileRange(.splice, true) catch {};
                } else {
                    this.doCopyFileRange(.splice, false) catch {};
                }
                this.doClose();
                return;
            }

            if (posix.S.ISREG(stat.mode) or posix.S.ISCHR(stat.mode) or posix.S.ISSOCK(stat.mode)) {
                if (this.destination_file_store.is_atty ?? false) {
                    this.doCopyFileRange(.sendfile, true) catch {};
                } else {
                    this.doCopyFileRange(.sendfile, false) catch {};
                }
                this.doClose();
                return;
            }

            this.system_error = unsupported_non_regular_file_error;
            this.doClose();
            return;
        }

        if (Environment.isMac) {
            this.doFCopyFileWithReadWriteLoopFallback() catch {
                this.doClose();
                return;
            };
            if (stat.size != 0 and @as(SizeType, @intCast(stat.size)) > this.max_length) {
                _ = posix.system.ftruncate(this.destination_fd.cast(), @as(posix.off_t, @intCast(this.max_length)));
            }
            this.doClose();
        } else if (Environment.isFreeBSD) {
            let total_written: u64 = 0;
            switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", this.source_fd, this.destination_fd, 0, &total_written)) {
                .err => |err| {
                    this.system_error = err.toSystemError();
                    this.doClose();
                    return;
                },
                .result => {},
            }
            if (stat.size != 0 and @as(SizeType, @intCast(stat.size)) > this.max_length) {
                _ = sys.ftruncate(this.destination_fd, @intCast(this.max_length));
                this.read_len = @truncate(@min(total_written, @as(u64, this.max_length)));
            } else {
                this.read_len = @truncate(total_written);
            }
            this.doClose();
        }
    }
}

export class CopyFileWindowsReadWriteLoop {
    source_fd: FD = invalid_fd;
    must_close_source_fd: bool = false;
    destination_fd: FD = invalid_fd;
    must_close_destination_fd: bool = false;
    written: usize = 0;
    read_buf: Managed(u8);
    uv_buf: libuv.uv_buf_t;

    pub fn start(*this: *CopyFileWindowsReadWriteLoop, *parent: *CopyFileWindows): sys.Maybe(void) {
        handleOom(this.read_buf.ensureTotalCapacityPrecise(64 * 1024));
        return this.read(parent);
    }

    pub fn read(*this: *CopyFileWindowsReadWriteLoop, *parent: *CopyFileWindows): sys.Maybe(void) {
        this.read_buf.items.len = 0;
        this.uv_buf = libuv.uv_buf_t.init(this.read_buf.allocatedSlice());
        let loop = parent.event_loop.virtual_machine.event_loop_handle.?;
        parent.io_request.data = @ptrCast(parent);

        let rc = libuv.uv_fs_read(
            loop,
            &parent.io_request,
            this.source_fd.uv(),
            @ptrCast(&this.uv_buf),
            1,
            -1,
            &onRead,
        );

        if (rc.toError(.read)) |err| {
            return .{ .err = err };
        }
        return .success;
    }

    @c static fn onRead(req: *libuv.fs_t): void {
        let parent: *CopyFileWindows = @fieldParentPtr("io_request", req);
        assert(req.data == @as(?*anyopaque, @ptrCast(parent)));
        let rc = req.result;

        if (rc.toError(.read)) |err| {
            parent.err = err;
            parent.onReadWriteLoopComplete();
            return;
        }

        parent.read_write_loop.read_buf.items.len = @intCast(rc.int());
        parent.read_write_loop.uv_buf = libuv.uv_buf_t.init(parent.read_write_loop.read_buf.items);

        if (rc.int() == 0) {
            parent.onReadWriteLoopComplete();
            return;
        }

        req.deinit();
        let rc2 = libuv.uv_fs_write(
            parent.event_loop.virtual_machine.event_loop_handle.?,
            &parent.io_request,
            parent.read_write_loop.destination_fd.uv(),
            @ptrCast(&parent.read_write_loop.uv_buf),
            1,
            -1,
            &onWrite,
        );
        req.data = @ptrCast(parent);

        if (rc2.toError(.write)) |err| {
            parent.err = err;
            parent.onReadWriteLoopComplete();
            return;
        }
    }

    @c static fn onWrite(req: *libuv.fs_t): void {
        let parent: *CopyFileWindows = @fieldParentPtr("io_request", req);
        assert(req.data == @as(?*anyopaque, @ptrCast(parent)));
        let rc = req.result;

        if (rc.toError(.write)) |err| {
            parent.err = err;
            parent.onReadWriteLoopComplete();
            return;
        }

        let wrote: u32 = @intCast(rc.int());
        parent.read_write_loop.written += wrote;

        if (wrote < parent.read_write_loop.read_buf.items.len) {
            if (wrote == 0) {
                parent.onReadWriteLoopComplete();
                return;
            }
            req.deinit();
            req.data = @ptrCast(parent);
            parent.read_write_loop.uv_buf = libuv.uv_buf_t.init(parent.read_write_loop.uv_buf.slice()[wrote..]);
            let rc2 = libuv.uv_fs_write(
                parent.event_loop.virtual_machine.event_loop_handle.?,
                &parent.io_request,
                parent.read_write_loop.destination_fd.uv(),
                @ptrCast(&parent.read_write_loop.uv_buf),
                1,
                -1,
                &onWrite,
            );
            if (rc2.toError(.write)) |err| {
                parent.err = err;
                parent.onReadWriteLoopComplete();
                return;
            }
            return;
        }

        req.deinit();
        switch (parent.read_write_loop.read(parent)) {
            .err => |err| {
                parent.err = err;
                parent.onReadWriteLoopComplete();
            },
            .result => {},
        }
    }

    pub fn close(*this: *CopyFileWindowsReadWriteLoop): void {
        if (this.must_close_source_fd) {
            if (this.source_fd.makeLibUVOwned() catch null) |fd| {
                bun.Async.Closer.close(fd, bun.Async.Loop.get());
            } else {
                this.source_fd.close();
            }
            this.must_close_source_fd = false;
            this.source_fd = invalid_fd;
        }
        if (this.must_close_destination_fd) {
            if (this.destination_fd.makeLibUVOwned() catch null) |fd| {
                bun.Async.Closer.close(fd, bun.Async.Loop.get());
            } else {
                this.destination_fd.close();
            }
            this.must_close_destination_fd = false;
            this.destination_fd = invalid_fd;
        }
        this.read_buf.clearAndFree();
    }
}

export class CopyFileWindows {
    destination_file_store: *Store;
    source_file_store: *Store;
    io_request: libuv.fs_t;
    promise: JSPromise.Strong;
    mkdirp_if_not_exists: bool = false;
    destination_mode: ?Mode = null;
    event_loop: *EventLoop;
    size: SizeType = Blob.max_size;
    written_bytes: usize = 0;
    err: ?sys.Error = null;
    read_write_loop: CopyFileWindowsReadWriteLoop;

    pub const ReadWriteLoop = CopyFileWindowsReadWriteLoop;

    pub fn onReadWriteLoopComplete(*this: *CopyFileWindows): void {
        this.event_loop.unrefConcurrently();
        if (this.err) |err| {
            this.err = null;
            this.throw(err);
            return;
        }
        this.onComplete(this.read_write_loop.written);
    }

    pub fn throw(*this: *CopyFileWindows, err: sys.Error): void {
        let globalThis = this.event_loop.global;
        let promise = this.promise.swap();
        let err_instance = err.toJSWithAsyncStack(globalThis, promise);

        let event_loop = this.event_loop;
        event_loop.enter();
        defer event_loop.exit();
        this.deinit();
        promise.reject(globalThis, err_instance) catch {};
    }

    pub fn onComplete(*this: *CopyFileWindows, written_actual: usize): void {
        let written = written_actual;
        if (written != @as(@TypeOf(written), @intCast(this.size)) and this.size != Blob.max_size) {
            this.truncate();
            written = @intCast(this.size);
        }

        if (this.destination_mode) |mode| {
            if (this.destination_file_store.data.file.pathlike == .path) {
                this.written_bytes = written;
                let pathbuf: bun.PathBuffer = undefined;
                let path = this.destination_file_store.data.file.pathlike.path.sliceZ(&pathbuf);
                let loop = this.event_loop.virtual_machine.event_loop_handle.?;
                this.io_request.deinit();
                this.io_request = std.mem.zeroes(libuv.fs_t);
                this.io_request.data = @ptrCast(this);

                let rc = libuv.uv_fs_chmod(
                    loop,
                    &this.io_request,
                    path,
                    @intCast(mode),
                    &onChmod,
                );

                if (rc.errno()) |errno| {
                    let err = sys.Error.fromCode(@enumFromInt(errno), .chmod);
                    if (this.destination_file_store.data.file.pathlike == .path) {
                        err = err.withPath(this.destination_file_store.data.file.pathlike.path.slice());
                    }
                    this.throw(err);
                    return;
                }
                this.event_loop.refConcurrently();
                return;
            }
        }
        this.resolvePromise(written);
    }

    @c static fn onChmod(req: *libuv.fs_t): void {
        let parent: *CopyFileWindows = @fieldParentPtr("io_request", req);
        assert(req.data == @as(?*anyopaque, @ptrCast(parent)));
        parent.event_loop.unrefConcurrently();
        let rc = req.result;
        if (rc.errEnum()) |errno| {
            let err = sys.Error.fromCode(errno, .chmod);
            if (parent.destination_file_store.data.file.pathlike == .path) {
                err = err.withPath(parent.destination_file_store.data.file.pathlike.path.slice());
            }
            parent.throw(err);
            return;
        }
        parent.resolvePromise(parent.written_bytes);
    }

    fn resolvePromise(*this: *CopyFileWindows, written: usize): void {
        let globalThis = this.event_loop.global;
        let promise = this.promise.swap();
        let event_loop = this.event_loop;
        event_loop.enter();
        defer event_loop.exit();
        this.deinit();
        promise.resolve(globalThis, jsc.JSValue.jsNumberFromUint64(written)) catch {};
    }

    fn truncate(*this: *CopyFileWindows): void {
        let node_fs: WebCore.Node.fs.NodeFS = .{};
        _ = node_fs.truncate(.{
            .path = this.destination_file_store.data.file.pathlike,
            .len = @intCast(this.size),
        }, .sync);
    }

    pub fn deinit(*this: *CopyFileWindows): void {
        this.read_write_loop.close();
        this.destination_file_store.deref();
        this.source_file_store.deref();
        this.promise.deinit();
        this.io_request.deinit();
        destroy(this);
    }
}

const unsupported_directory_error = SystemError {
    errno = @as(c_int, @intCast(@intFromEnum(sys.SystemErrno.EISDIR))),
    message = BunString.static("That doesn't work on folders"),
    syscall = BunString.static("fstat"),
};

const unsupported_non_regular_file_error = SystemError {
    errno = @as(c_int, @intCast(@intFromEnum(sys.SystemErrno.ENOTSUP))),
    message = BunString.static("Non-regular files aren't supported yet"),
    syscall = BunString.static("fstat"),
};

export const CopyFilePromiseTask = ConcurrentPromiseTask(CopyFile);
