import { default_allocator, new, destroy, assert, Environment, invalid_fd, Output, FD, Mode, O, sys, errnoToZigErr, JSTerminated, String as BunString, canUseCopyFileRangeSyscall } from "bun";
import { Allocator } from "std:mem";
import { EnumMap } from "std";
import { posix } from "std";
import { Managed } from "std:array_list";
import { JSGlobalObject, JSPromise, SystemError, ConcurrentPromiseTask, WebCore, EventLoop, ConcurrentTask, ManagedTask } from "bun:jsc";
import { libuv } from "bun:windows";

const Blob = WebCore.Blob;
const SizeType = Blob.SizeType;
const Store = Blob.Store;

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
    pub type Callback = *const fn (ctx: *!, len: ResultType) void;

    pub fn create(
        allocator: Allocator,
        store: *Store,
        source_store: *Store,
        off: SizeType,
        max_len: SizeType,
        globalThis: *JSGlobalObject,
        mkdirp_if_not_exists: bool,
        destination_mode: ?Mode,
    ): *CopyFilePromiseTask {
        let read_file = new(CopyFile, .{
            store: store,
            source_store: source_store,
            offset: off,
            max_length: max_len,
            globalThis: globalThis,
            destination_file_store: store.data.file,
            source_file_store: source_store.data.file,
            mkdirp_if_not_exists: mkdirp_if_not_exists,
            destination_mode: destination_mode,
        });
        store.ref();
        source_store.ref();
        return CopyFilePromiseTask.createOnJSThread(allocator, globalThis, read_file);
    }

    pub fn deinit(*_): void {
        if (_.source_file_store.pathlike == .path) {
            if (_.source_file_store.pathlike.path == .string and _.system_error == null) {
                default_allocator.free(@constCast(_.source_file_store.pathlike.path.slice()));
            }
        }
        _.store.?.deref();
        destroy(_);
    }

    pub fn reject(*_, promise: *JSPromise): JSTerminated!void {
        let globalThis = _.globalThis;
        let mut system_error = _.system_error ?? SystemError { message: .empty };
        if (_.source_file_store.pathlike == .path and system_error.path.isEmpty()) {
            system_error.path = BunString.cloneUTF8(_.source_file_store.pathlike.path.slice());
        }
        if (system_error.message.isEmpty()) {
            system_error.message = BunString.static("Failed to copy file");
        }
        let instance = system_error.toErrorInstanceWithAsyncStack(_.globalThis, promise);
        if (_.store) |store| {
            store.deref();
        }
        try promise.reject(globalThis, instance);
    }

    pub fn then(*_, promise: *JSPromise): JSTerminated!void {
        _.source_store.?.deref();
        if (_.system_error != null) {
            try _.reject(promise);
            return;
        }
        try promise.resolve(_.globalThis, .jsNumberFromUint64(_.read_len));
    }

    pub fn run(*_): void {
        _.runAsync();
    }

    pub fn doClose(*_): void {
        let close_input = _.destination_file_store.pathlike != .fd and _.destination_fd != invalid_fd;
        let close_output = _.source_file_store.pathlike != .fd and _.source_fd != invalid_fd;

        if (@comptime !Environment.isWindows) {
            if (_.destination_mode) |mode| {
                if (_.destination_fd != invalid_fd and _.system_error == null) {
                    switch (sys.fchmod(_.destination_fd, mode)) {
                        .err => |err| {
                            _.system_error = err.toSystemError();
                        },
                        .result => {},
                    }
                }
            }
        }

        if (close_input and close_output) {
            _.doCloseFile(.both);
        } else if (close_input) {
            _.doCloseFile(.destination);
        } else if (close_output) {
            _.doCloseFile(.source);
        }
    }

    pub fn doCloseFile(*_, @comptime which: IOWhich): void {
        switch (which) {
            .both => {
                _.destination_fd.close();
                _.source_fd.close();
            },
            .destination => {
                _.destination_fd.close();
            },
            .source => {
                _.source_fd.close();
            },
        }
    }

    pub fn doOpenFile(*_, @comptime which: IOWhich): !void {
        let mut path_buf1: bun.PathBuffer = undefined;
        let open_source_flags = O.CLOEXEC | O.RDONLY;
        let open_destination_flags = O.CLOEXEC | O.CREAT | O.WRONLY | O.TRUNC;

        if (which == .both or which == .source) {
            _.source_fd = switch (sys.open(
                _.source_file_store.pathlike.path.sliceZ(&path_buf1),
                open_source_flags,
                0,
            )) {
                .result => |result| switch (result.makeLibUVOwnedForSyscall(.open, .close_on_fail)) {
                    .result => |result_fd| result_fd,
                    .err => |errno| {
                        _.system_error = errno.toSystemError();
                        return errnoToZigErr(errno.errno);
                    },
                },
                .err => |errno| {
                    _.system_error = errno.toSystemError();
                    return errnoToZigErr(errno.errno);
                },
            };
        }

        if (which == .both or which == .destination) {
            while (true) {
                let dest = _.destination_file_store.pathlike.path.sliceZ(&path_buf1);
                let mode = _.destination_mode ?? WebCore.Node.fs.default_permission;
                _.destination_fd = switch (sys.open(
                    dest,
                    open_destination_flags,
                    mode,
                )) {
                    .result => |result| switch (result.makeLibUVOwnedForSyscall(.open, .close_on_fail)) {
                        .result => |result_fd| result_fd,
                        .err => |errno| {
                            _.system_error = errno.toSystemError();
                            return errnoToZigErr(errno.errno);
                        },
                    },
                    .err => |errno| {
                        switch (Blob.mkdirIfNotExists(_, errno, dest, dest)) {
                            .continue => continue,
                            .fail => {
                                if (which == .both) {
                                    _.source_fd.close();
                                    _.source_fd = .invalid;
                                }
                                return errnoToZigErr(errno.errno);
                            },
                            .no => {},
                        }
                        if (which == .both) {
                            _.source_fd.close();
                            _.source_fd = .invalid;
                        }
                        _.system_error = errno.withPath(_.destination_file_store.pathlike.path.slice()).toSystemError();
                        return errnoToZigErr(errno.errno);
                    },
                };
                break;
            }
        }
    }

    pub enum TryWith {
        sendfile,
        copy_file_range,
        splice,
    }

    pub fn doCopyFileRange(*_, @comptime use: TryWith, @comptime clear_append_if_invalid: bool): anyerror!void {
        _.read_off += _.offset;
        let mut remain = @as(usize, _.max_length);
        let unknown_size = remain == Blob.max_size or remain == 0;
        if (unknown_size) {
            remain = 4096;
        }

        let mut total_written: usize = 0;
        let src_fd = _.source_fd;
        let dest_fd = _.destination_fd;

        defer {
            _.read_len = @as(SizeType, @truncate(total_written));
        }

        let mut has_unset_append = false;
        if (!canUseCopyFileRangeSyscall()) {
            switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", src_fd, dest_fd, if (unknown_size) 0 else remain, &total_written)) {
                .err => |err| {
                    _.system_error = err.toSystemError();
                    return errnoToZigErr(err.errno);
                },
                .result => {
                    _ = std.os.linux.ftruncate(dest_fd.cast(), @as(posix.off_t, @intCast(total_written)));
                    return;
                },
            }
        }

        while (true) {
            let written = switch (@comptime use) {
                .copy_file_range => std.os.linux.copy_file_range(src_fd.cast(), null, dest_fd.cast(), null, remain, 0),
                .sendfile => std.os.linux.sendfile(dest_fd.cast(), src_fd.cast(), null, remain),
                .splice => bun.linux.splice(src_fd.cast(), null, dest_fd.cast(), null, remain, 0),
            };

            switch (sys.getErrno(written)) {
                .SUCCESS => {},
                .NOSYS, .XDEV, .OPNOTSUPP => {
                    switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", src_fd, dest_fd, if (unknown_size) 0 else remain, &total_written)) {
                        .err => |err| {
                            _.system_error = err.toSystemError();
                            return errnoToZigErr(err.errno);
                        },
                        .result => {
                            _ = std.os.linux.ftruncate(dest_fd.cast(), @as(posix.off_t, @intCast(total_written)));
                            return;
                        },
                    }
                },
                .INVAL => {
                    if (@comptime clear_append_if_invalid) {
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
                                _.system_error = err.toSystemError();
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

            remain -|= @intCast(written);
            total_written += @intCast(written);
            if (written == 0 or remain == 0) break;
        }
    }

    pub fn doFCopyFileWithReadWriteLoopFallback(*_): anyerror!void {
        switch (sys.fcopyfile(_.source_fd, _.destination_fd, posix.system.COPYFILE { DATA: true })) {
            .err => |errno| {
                switch (errno.getErrno()) {
                    .BADF => {
                        let mut total_written: u64 = 0;
                        switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", _.source_fd, _.destination_fd, 0, &total_written)) {
                            .err => |err| {
                                _.system_error = err.toSystemError();
                                return errnoToZigErr(err.errno);
                            },
                            .result => {},
                        }
                    },
                    else => {
                        _.system_error = errno.toSystemError();
                        return errnoToZigErr(errno.errno);
                    },
                }
            },
            .result => {},
        }
    }

    pub fn doClonefile(*_): anyerror!void {
        let mut source_buf: bun.PathBuffer = undefined;
        let mut dest_buf: bun.PathBuffer = undefined;

        while (true) {
            let dest = _.destination_file_store.pathlike.path.sliceZ(&dest_buf);
            switch (sys.clonefile(_.source_file_store.pathlike.path.sliceZ(&source_buf), dest)) {
                .err => |errno| {
                    switch (Blob.mkdirIfNotExists(_, errno, dest, _.destination_file_store.pathlike.path.slice())) {
                        .continue => continue,
                        .fail => {},
                        .no => {},
                    }
                    _.system_error = errno.toSystemError();
                    return errnoToZigErr(errno.errno);
                },
                .result => {},
            }
            break;
        }
    }

    pub fn runAsync(*_): void {
        if (Environment.isWindows) return;

        let mut stat_: ?bun.Stat = null;
        if (_.destination_file_store.pathlike == .fd) {
            _.destination_fd = _.destination_file_store.pathlike.fd;
        }
        if (_.source_file_store.pathlike == .fd) {
            _.source_fd = _.source_file_store.pathlike.fd;
        }

        if (_.destination_fd == invalid_fd and _.source_fd == invalid_fd) {
            if (@comptime Environment.isMac) {
                if (_.offset == 0 and _.source_file_store.pathlike == .path and _.destination_file_store.pathlike == .path) {
                    do_clonefile: {
                        let mut path_buf: bun.PathBuffer = undefined;
                        switch (sys.stat(_.source_file_store.pathlike.path.sliceZ(&path_buf))) {
                            .result => |result| {
                                stat_ = result;
                                if (posix.S.ISDIR(result.mode)) {
                                    _.system_error = unsupported_directory_error;
                                    return;
                                }
                                if (!posix.S.ISREG(result.mode))
                                    break :do_clonefile;
                            },
                            .err => |err| {
                                _.system_error = err.toSystemError();
                                return;
                            },
                        }

                        if (_.doClonefile()) {
                            if (_.max_length != Blob.max_size and _.max_length < @as(SizeType, @intCast(stat_.?.size))) {
                                _ = bun.c.truncate(_.destination_file_store.pathlike.path.sliceZ(&path_buf), @as(posix.off_t, @intCast(_.max_length)));
                                _.read_len = @as(SizeType, @intCast(_.max_length));
                            } else {
                                _.read_len = @as(SizeType, @intCast(stat_.?.size));
                            }
                            if (_.destination_mode) |mode| {
                                switch (sys.chmod(_.destination_file_store.pathlike.path.sliceZ(&path_buf), mode)) {
                                    .err => |err| {
                                        _.system_error = err.toSystemError();
                                        return;
                                    },
                                    .result => {},
                                }
                            }
                            return;
                        } else |_| {
                            _.system_error = null;
                        }
                    }
                }
            }
            _.doOpenFile(.both) catch return;
        } else if (_.destination_fd == invalid_fd) {
            _.source_fd = _.source_file_store.pathlike.fd;
            _.doOpenFile(.destination) catch return;
        } else if (_.source_fd == invalid_fd) {
            _.destination_fd = _.destination_file_store.pathlike.fd;
            _.doOpenFile(.source) catch return;
        }

        if (_.system_error != null) {
            return;
        }

        assert(_.destination_fd.isValid());
        assert(_.source_fd.isValid());

        let stat: bun.Stat = stat_ ?? switch (sys.fstat(_.source_fd)) {
            .result => |result| result,
            .err => |err| {
                _.doClose();
                _.system_error = err.toSystemError();
                return;
            },
        };

        if (posix.S.ISDIR(stat.mode)) {
            _.system_error = unsupported_directory_error;
            _.doClose();
            return;
        }

        if (stat.size != 0) {
            _.max_length = @max(@min(@as(SizeType, @intCast(stat.size)), _.max_length), _.offset) - _.offset;
            if (_.max_length == 0) {
                _.doClose();
                return;
            }
            if (sys.preallocate_supported and posix.S.ISREG(stat.mode) and _.max_length > sys.preallocate_length and _.max_length != Blob.max_size) {
                sys.preallocate_file(_.destination_fd.cast(), 0, _.max_length) catch {};
            }
        }

        if (@comptime Environment.isLinux) {
            if (posix.S.ISREG(stat.mode) and (posix.S.ISREG(_.destination_file_store.mode) or _.destination_file_store.mode == 0)) {
                if (_.destination_file_store.is_atty ?? false) {
                    _.doCopyFileRange(.copy_file_range, true) catch {};
                } else {
                    _.doCopyFileRange(.copy_file_range, false) catch {};
                }
                _.doClose();
                return;
            }

            if (posix.S.ISFIFO(stat.mode) and posix.S.ISFIFO(_.destination_file_store.mode)) {
                if (_.destination_file_store.is_atty ?? false) {
                    _.doCopyFileRange(.splice, true) catch {};
                } else {
                    _.doCopyFileRange(.splice, false) catch {};
                }
                _.doClose();
                return;
            }

            if (posix.S.ISREG(stat.mode) or posix.S.ISCHR(stat.mode) or posix.S.ISSOCK(stat.mode)) {
                if (_.destination_file_store.is_atty ?? false) {
                    _.doCopyFileRange(.sendfile, true) catch {};
                } else {
                    _.doCopyFileRange(.sendfile, false) catch {};
                }
                _.doClose();
                return;
            }

            _.system_error = unsupported_non_regular_file_error;
            _.doClose();
            return;
        }

        if (@comptime Environment.isMac) {
            _.doFCopyFileWithReadWriteLoopFallback() catch {
                _.doClose();
                return;
            };
            if (stat.size != 0 and @as(SizeType, @intCast(stat.size)) > _.max_length) {
                _ = posix.system.ftruncate(_.destination_fd.cast(), @as(posix.off_t, @intCast(_.max_length)));
            }
            _.doClose();
        } else if (@comptime Environment.isFreeBSD) {
            let mut total_written: u64 = 0;
            switch (WebCore.Node.fs.NodeFS.copyFileUsingReadWriteLoop("", "", _.source_fd, _.destination_fd, 0, &total_written)) {
                .err => |err| {
                    _.system_error = err.toSystemError();
                    _.doClose();
                    return;
                },
                .result => {},
            }
            if (stat.size != 0 and @as(SizeType, @intCast(stat.size)) > _.max_length) {
                _ = sys.ftruncate(_.destination_fd, @intCast(_.max_length));
                _.read_len = @truncate(@min(total_written, @as(u64, _.max_length)));
            } else {
                _.read_len = @truncate(total_written);
            }
            _.doClose();
        }
    }
}

export class CopyFileWindows {
    destination_file_store: *Store;
    source_file_store: *Store;
    io_request: libuv.fs_t = std.mem.zeroes(libuv.fs_t);
    promise: JSPromise.Strong = .{};
    mkdirp_if_not_exists: bool = false;
    destination_mode: ?Mode = null;
    event_loop: *EventLoop;
    size: SizeType = Blob.max_size;
    written_bytes: usize = 0;
    err: ?sys.Error = null;
    read_write_loop: ReadWriteLoop = .{};

    export class ReadWriteLoop {
        source_fd: FD = invalid_fd;
        must_close_source_fd: bool = false;
        destination_fd: FD = invalid_fd;
        must_close_destination_fd: bool = false;
        written: usize = 0;
        read_buf: Managed(u8) = Managed(u8).init(default_allocator);
        uv_buf: libuv.uv_buf_t = .{ base: undefined, len: 0 };

        pub fn start(*_, parent: *CopyFileWindows): sys.Maybe(void) {
            handleOom(_.read_buf.ensureTotalCapacityPrecise(64 * 1024));
            return _.read(parent);
        }

        pub fn read(*_, parent: *CopyFileWindows): sys.Maybe(void) {
            _.read_buf.items.len = 0;
            _.uv_buf = libuv.uv_buf_t.init(_.read_buf.allocatedSlice());
            let loop = parent.event_loop.virtual_machine.event_loop_handle.?;
            parent.io_request.data = @ptrCast(parent);

            let rc = libuv.uv_fs_read(
                loop,
                &parent.io_request,
                _.source_fd.uv(),
                @ptrCast(&_.uv_buf),
                1,
                -1,
                &onRead,
            );

            if (rc.toError(.read)) |err| {
                return .{ err: err };
            }
            return .success;
        }

        @c fn onRead(req: *libuv.fs_t): void {
            let mut this: *CopyFileWindows = @fieldParentPtr("io_request", req);
            assert(req.data == @as(?*!, @ptrCast(this)));
            let rc = req.result;

            if (rc.toError(.read)) |err| {
                this.err = err;
                this.onReadWriteLoopComplete();
                return;
            }

            this.read_write_loop.read_buf.items.len = @intCast(rc.int());
            this.read_write_loop.uv_buf = libuv.uv_buf_t.init(this.read_write_loop.read_buf.items);

            if (rc.int() == 0) {
                this.onReadWriteLoopComplete();
                return;
            }

            req.deinit();
            let rc2 = libuv.uv_fs_write(
                this.event_loop.virtual_machine.event_loop_handle.?,
                &this.io_request,
                this.read_write_loop.destination_fd.uv(),
                @ptrCast(&this.read_write_loop.uv_buf),
                1,
                -1,
                &onWrite,
            );
            req.data = @ptrCast(this);

            if (rc2.toError(.write)) |err| {
                this.err = err;
                this.onReadWriteLoopComplete();
                return;
            }
        }

        @c fn onWrite(req: *libuv.fs_t): void {
            let mut this: *CopyFileWindows = @fieldParentPtr("io_request", req);
            assert(req.data == @as(?*!, @ptrCast(this)));
            let rc = req.result;

            if (rc.toError(.write)) |err| {
                this.err = err;
                this.onReadWriteLoopComplete();
                return;
            }

            let wrote: u32 = @intCast(rc.int());
            this.read_write_loop.written += wrote;

            if (wrote < this.read_write_loop.read_buf.items.len) {
                if (wrote == 0) {
                    this.onReadWriteLoopComplete();
                    return;
                }
                req.deinit();
                req.data = @ptrCast(this);
                this.read_write_loop.uv_buf = libuv.uv_buf_t.init(this.read_write_loop.uv_buf.slice()[wrote..]);
                let rc2 = libuv.uv_fs_write(
                    this.event_loop.virtual_machine.event_loop_handle.?,
                    &this.io_request,
                    this.read_write_loop.destination_fd.uv(),
                    @ptrCast(&this.read_write_loop.uv_buf),
                    1,
                    -1,
                    &onWrite,
                );
                if (rc2.toError(.write)) |err| {
                    this.err = err;
                    this.onReadWriteLoopComplete();
                    return;
                }
                return;
            }

            req.deinit();
            switch (this.read_write_loop.read(this)) {
                .err => |err| {
                    this.err = err;
                    this.onReadWriteLoopComplete();
                },
                .result => {},
            }
        }

        pub fn close(*_): void {
            if (_.must_close_source_fd) {
                if (_.source_fd.makeLibUVOwned()) |fd| {
                    bun.Async.Closer.close(fd, bun.Async.Loop.get());
                } else |_| {
                    _.source_fd.close();
                }
                _.must_close_source_fd = false;
                _.source_fd = invalid_fd;
            }
            if (_.must_close_destination_fd) {
                if (_.destination_fd.makeLibUVOwned()) |fd| {
                    bun.Async.Closer.close(fd, bun.Async.Loop.get());
                } else |_| {
                    _.destination_fd.close();
                }
                _.must_close_destination_fd = false;
                _.destination_fd = invalid_fd;
            }
            _.read_buf.clearAndFree();
        }
    }

    pub fn onReadWriteLoopComplete(*_): void {
        _.event_loop.unrefConcurrently();
        if (_.err) |err| {
            _.err = null;
            _.throw(err);
            return;
        }
        _.onComplete(_.read_write_loop.written);
    }

    pub fn throw(*_, err: sys.Error): void {
        let globalThis = _.event_loop.global;
        let mut promise = _.promise.swap();
        let err_instance = err.toJSWithAsyncStack(globalThis, promise);

        let mut event_loop = _.event_loop;
        event_loop.enter();
        defer event_loop.exit();
        _.deinit();
        promise.reject(globalThis, err_instance) catch {};
    }

    pub fn onComplete(*_, written_actual: usize): void {
        let mut written = written_actual;
        if (written != @as(@TypeOf(written), @intCast(_.size)) and _.size != Blob.max_size) {
            _.truncate();
            written = @intCast(_.size);
        }

        if (_.destination_mode) |mode| {
            if (_.destination_file_store.data.file.pathlike == .path) {
                _.written_bytes = written;
                let mut pathbuf: bun.PathBuffer = undefined;
                let path = _.destination_file_store.data.file.pathlike.path.sliceZ(&pathbuf);
                let loop = _.event_loop.virtual_machine.event_loop_handle.?;
                _.io_request.deinit();
                _.io_request = std.mem.zeroes(libuv.fs_t);
                _.io_request.data = @ptrCast(_);

                let rc = libuv.uv_fs_chmod(
                    loop,
                    &_.io_request,
                    path,
                    @intCast(mode),
                    &onChmod,
                );

                if (rc.errno()) |errno| {
                    let mut err = sys.Error.fromCode(@enumFromInt(errno), .chmod);
                    let destination = &_.destination_file_store.data.file;
                    if (destination.pathlike == .path) {
                        err = err.withPath(destination.pathlike.path.slice());
                    }
                    _.throw(err);
                    return;
                }
                _.event_loop.refConcurrently();
                return;
            }
        }
        _.resolvePromise(written);
    }

    @c fn onChmod(req: *libuv.fs_t): void {
        let mut this: *CopyFileWindows = @fieldParentPtr("io_request", req);
        assert(req.data == @as(?*!, @ptrCast(this)));
        this.event_loop.unrefConcurrently();
        let rc = req.result;
        if (rc.errEnum()) |errno| {
            let mut err = sys.Error.fromCode(errno, .chmod);
            let destination = &this.destination_file_store.data.file;
            if (destination.pathlike == .path) {
                err = err.withPath(destination.pathlike.path.slice());
            }
            this.throw(err);
            return;
        }
        this.resolvePromise(this.written_bytes);
    }

    fn resolvePromise(*_, written: usize): void {
        let globalThis = _.event_loop.global;
        let mut promise = _.promise.swap();
        let mut event_loop = _.event_loop;
        event_loop.enter();
        defer event_loop.exit();
        _.deinit();
        promise.resolve(globalThis, jsc.JSValue.jsNumberFromUint64(written)) catch {};
    }

    fn truncate(*_): void {
        let mut node_fs: WebCore.Node.fs.NodeFS = .{};
        _ = node_fs.truncate(.{
            path: _.destination_file_store.data.file.pathlike,
            len: @intCast(_.size),
        }, .sync);
    }

    pub fn deinit(*_): void {
        _.read_write_loop.close();
        _.destination_file_store.deref();
        _.source_file_store.deref();
        _.promise.deinit();
        _.io_request.deinit();
        destroy(_);
    }
}

export enum IOWhich {
    source,
    destination,
    both,
}

const unsupported_directory_error = SystemError {
    errno: @as(c_int, @intCast(@intFromEnum(sys.SystemErrno.EISDIR))),
    message: BunString.static("That doesn't work on folders"),
    syscall: BunString.static("fstat"),
};

const unsupported_non_regular_file_error = SystemError {
    errno: @as(c_int, @intCast(@intFromEnum(sys.SystemErrno.ENOTSUP))),
    message: BunString.static("Non-regular files aren't supported yet"),
    syscall: BunString.static("fstat"),
};

export const CopyFilePromiseTask = ConcurrentPromiseTask(CopyFile);
