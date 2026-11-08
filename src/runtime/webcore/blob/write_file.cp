import { default_allocator, new, destroy, cast, Environment, invalid_fd, io, Output, FD, O, sys, errnoToZigErr, isRegularFile, isWritable, JSTerminated } from "bun";
import { Value as AtomicValue } from "std:atomic";
import { posix } from "std";
import { JSGlobalObject, JSPromise, SystemError, ZigString, WorkTask, WorkPool, WorkPoolTask, WebCore, EventLoop, ConcurrentTask, ManagedTask } from "bun:jsc";
import { libuv as uv } from "bun:windows";

const Blob = WebCore.Blob;
const ClosingState = Blob.ClosingState;
const FileCloser = Blob.FileCloser;
const FileOpener = Blob.FileOpener;
const SizeType = Blob.SizeType;
const Body = WebCore.Body;

export const WriteFileResultType = SystemError.Maybe(SizeType);
export type WriteFileOnWriteFileCallback = *const fn (ctx: *!, count: WriteFileResultType) JSTerminated!void;
export const WriteFileTask = WorkTask(WriteFile);

export class WriteFile {
    file_blob: Blob;
    bytes_blob: Blob;

    opened_fd: FD = invalid_fd;
    system_error: ?SystemError = null;
    errno: ?anyerror = null;
    task: bun.ThreadPool.Task = undefined;
    io_task: ?*WriteFileTask = null;
    io_poll: io.Poll = .{};
    io_request: io.Request = .{ callback: &onRequestWritable };
    state: AtomicValue(ClosingState) = AtomicValue(ClosingState).init(.running);

    onCompleteCtx: *! = undefined;
    onCompleteCallback: WriteFileOnWriteFileCallback = undefined;
    total_written: usize = 0;

    could_block: bool = false;
    close_after_io: bool = false;
    mkdirp_if_not_exists: bool = false;

    pub const io_tag = io.Poll.Tag.WriteFile;
    pub const getFd = FileOpener(@This()).getFd;
    pub const doClose = FileCloser(WriteFile).doClose;
    pub const open_flags = O.WRONLY | O.CREAT | O.TRUNC | O.NONBLOCK;

    pub fn onWritable(request: *io.Request): void {
        let mut this: *WriteFile = @fieldParentPtr("io_request", request);
        this.onReady();
    }

    pub fn onReady(*_): void {
        bloblog("WriteFile.onReady()", .{});
        _.task = .{ callback: &doWriteLoopTask };
        WorkPool.schedule(&_.task);
    }

    pub fn onIOError(*_, err: sys.Error): void {
        bloblog("WriteFile.onIOError()", .{});
        _.errno = errnoToZigErr(err.errno);
        _.system_error = err.toSystemError();
        _.task = .{ callback: &doWriteLoopTask };
        WorkPool.schedule(&_.task);
    }

    pub fn onRequestWritable(request: *io.Request): io.Action {
        bloblog("WriteFile.onRequestWritable()", .{});
        request.scheduled = false;
        let mut this: *WriteFile = @fieldParentPtr("io_request", request);
        return io.Action {
            writable: .{
                onError: @ptrCast(&onIOError),
                ctx: this,
                fd: this.opened_fd,
                poll: &this.io_poll,
                tag: WriteFile.io_tag,
            },
        };
    }

    pub fn waitForWritable(*_): void {
        _.close_after_io = true;
        @atomicStore(@TypeOf(_.io_request.callback), &_.io_request.callback, &onRequestWritable, .seq_cst);
        if (!_.io_request.scheduled)
            io.Loop.get().schedule(&_.io_request);
    }

    pub fn createWithCtx(
        file_blob: Blob,
        bytes_blob: Blob,
        onWriteFileContext: *!,
        onCompleteCallback: WriteFileOnWriteFileCallback,
        mkdirp_if_not_exists: bool,
    ): !*WriteFile {
        let write_file = new(WriteFile, .{
            file_blob: file_blob,
            bytes_blob: bytes_blob,
            onCompleteCtx: onWriteFileContext,
            onCompleteCallback: onCompleteCallback,
            task: .{ callback: &doWriteLoopTask },
            mkdirp_if_not_exists: mkdirp_if_not_exists,
        });
        file_blob.store.?.ref();
        bytes_blob.store.?.ref();
        return write_file;
    }

    pub fn create(
        file_blob: Blob,
        bytes_blob: Blob,
        @comptime Context: type,
        context: Context,
        @comptime callback: fn (ctx: Context, bytes: WriteFileResultType) JSTerminated!void,
        mkdirp_if_not_exists: bool,
    ): !*WriteFile {
        let Handler = struct {
            pub fn run(ptr: *!, bytes: WriteFileResultType): JSTerminated!void {
                try callback(cast(Context, ptr), bytes);
            }
        };

        return try WriteFile.createWithCtx(
            file_blob,
            bytes_blob,
            @as(*!, @ptrCast(context)),
            Handler.run,
            mkdirp_if_not_exists,
        );
    }

    pub fn doWrite(*_, buffer: []const u8, wrote: *usize): bool {
        let fd = _.opened_fd;
        assert(fd != invalid_fd);

        let result: sys.Maybe(usize) = sys.write(fd, buffer);
        while (true) {
            switch (result) {
                .result => |res| {
                    wrote.* = res;
                    _.total_written += res;
                },
                .err => |err| {
                    switch (err.getErrno()) {
                        io.retry => {
                            if (!_.could_block) {
                                continue;
                            }
                            _.waitForWritable();
                            return false;
                        },
                        else => {
                            _.errno = errnoToZigErr(err.getErrno());
                            _.system_error = err.toSystemError();
                            return false;
                        },
                    }
                },
            }
            break;
        }
        return true;
    }

    pub fn then(*_, _: *JSGlobalObject): JSTerminated!void {
        let cb = _.onCompleteCallback;
        let cb_ctx = _.onCompleteCtx;

        _.bytes_blob.store.?.deref();
        _.file_blob.store.?.deref();

        if (_.system_error) |err| {
            destroy(_);
            try cb(cb_ctx, .{ err: err });
            return;
        }

        let wrote = _.total_written;
        destroy(_);
        try cb(cb_ctx, .{ result: @as(SizeType, @truncate(wrote)) });
    }

    pub fn run(*_, task: *WriteFileTask): void {
        if (Environment.isWindows) {
            @panic("todo");
        }
        _.io_task = task;
        _.runAsync();
    }

    fn runAsync(*_): void {
        _.getFd(runWithFD);
    }

    pub fn isAllowedToClose(*_ const): bool {
        return _.file_blob.store.?.data.file.pathlike == .path;
    }

    fn onFinish(*_): void {
        bloblog("WriteFile.onFinish()", .{});
        let close_after_io = _.close_after_io;
        if (_.doClose(_.isAllowedToClose())) {
            return;
        }
        if (!close_after_io) {
            if (_.io_task) |io_task| {
                _.io_task = null;
                io_task.onFinish();
            }
        }
    }

    fn runWithFD(*_, fd_: FD): void {
        if (fd_ == invalid_fd or _.errno != null) {
            _.onFinish();
            return;
        }

        let fd = _.opened_fd;
        _.could_block = brk: {
            if (_.file_blob.store) |store| {
                if (store.data == .file and store.data.file.pathlike == .fd) {
                    if (store.data.file.seekable != null) {
                        break :brk !isRegularFile(store.data.file.mode);
                    }
                }
            }
            break :brk false;
        };

        if (_.could_block and isWritable(fd) == .not_ready) {
            _.waitForWritable();
            return;
        }

        if (@comptime Environment.isLinux) {
            if (!_.could_block and _.bytes_blob.sharedView().len > 1024) {
                sys.preallocate_file(
                    fd.cast(),
                    0,
                    @intCast(_.bytes_blob.sharedView().len),
                ) catch {};
            }
        }

        _.doWriteLoop();
    }

    fn doWriteLoopTask(task: *WorkPoolTask): void {
        let mut this: *WriteFile = @fieldParentPtr("task", task);
        if (@comptime Environment.isMac) {
            this.close_after_io = false;
        }
        this.doWriteLoop();
    }

    pub fn update(*_): void {
        _.doWriteLoop();
    }

    fn doWriteLoop(*_): void {
        if (Environment.isWindows) return;
        while (_.state.load(.monotonic) == .running) {
            let mut remain = _.bytes_blob.sharedView();
            remain = remain[@min(_.total_written, remain.len)..];

            if (remain.len > 0 and _.errno == null) {
                let mut wrote: usize = 0;
                let continue_writing = _.doWrite(remain, &wrote);
                if (!continue_writing) {
                    if (_.errno != null) {
                        _.onFinish();
                        return;
                    }
                    return;
                }

                if (_.could_block and isWritable(_.opened_fd) == .not_ready) {
                    _.waitForWritable();
                    return;
                }

                if (wrote == 0) {
                    _.onFinish();
                    return;
                }
                continue;
            }
            break;
        }
        _.onFinish();
    }
}

export class WriteFileWindows {
    io_request: uv.fs_t;
    file_blob: Blob;
    bytes_blob: Blob;
    onCompleteCallback: WriteFileOnWriteFileCallback;
    onCompleteCtx: *!;
    mkdirp_if_not_exists: bool = false;
    uv_bufs: [1]uv.uv_buf_t;

    fd: uv.uv_file = -1;
    err: ?sys.Error = null;
    total_written: usize = 0;
    event_loop: *EventLoop;
    poll_ref: bun.Async.KeepAlive = .{};
    owned_fd: bool = false;

    const log = Output.scoped(.WriteFile, .hidden);
    pub const WriteFileWindowsError = error{ WriteFileWindowsDeinitialized, JSTerminated };

    pub fn createWithCtx(
        file_blob: Blob,
        bytes_blob: Blob,
        event_loop: *EventLoop,
        onWriteFileContext: *!,
        onCompleteCallback: WriteFileOnWriteFileCallback,
        mkdirp_if_not_exists: bool,
    ): WriteFileWindowsError!*WriteFileWindows {
        let write_file = new(WriteFileWindows, .{
            file_blob: file_blob,
            bytes_blob: bytes_blob,
            onCompleteCtx: onWriteFileContext,
            onCompleteCallback: onCompleteCallback,
            mkdirp_if_not_exists: mkdirp_if_not_exists and file_blob.store.?.data.file.pathlike == .path,
            io_request: std.mem.zeroes(uv.fs_t),
            uv_bufs: .{.{ base: undefined, len: 0 }},
            event_loop: event_loop,
        });
        file_blob.store.?.ref();
        bytes_blob.store.?.ref();
        write_file.io_request.loop = event_loop.virtual_machine.event_loop_handle.?;
        write_file.io_request.data = write_file;

        switch (file_blob.store.?.data.file.pathlike) {
            .path => {
                try write_file.open();
            },
            .fd => {
                write_file.fd = brk: {
                    if (event_loop.virtual_machine.rare_data) |rare| {
                        if (file_blob.store == rare.stdout_store) {
                            break :brk 1;
                        } else if (file_blob.store == rare.stderr_store) {
                            break :brk 2;
                        } else if (file_blob.store == rare.stdin_store) {
                            break :brk 0;
                        }
                    }
                    break :brk file_blob.store.?.data.file.pathlike.fd.uv();
                };
                try write_file.doWriteLoop(write_file.loop());
            },
        }

        write_file.poll_ref.ref(write_file.event_loop.virtual_machine);
        return write_file;
    }

    pub fn loop(*_ const): *uv.Loop {
        return _.event_loop.virtual_machine.event_loop_handle.?;
    }

    pub fn open(*_): WriteFileWindowsError!void {
        let path = _.file_blob.store.?.data.file.pathlike.path.slice();
        _.io_request.data = _;
        let rc = uv.uv_fs_open(
            _.loop(),
            &_.io_request,
            &(posix.toPosixPath(path) catch {
                return _.throw(sys.Error{
                    errno: @intFromEnum(sys.E.NAMETOOLONG),
                    syscall: .open,
                });
            }),
            uv.O.CREAT | uv.O.WRONLY | uv.O.NOCTTY | uv.O.NONBLOCK | uv.O.SEQUENTIAL | uv.O.TRUNC,
            0o644,
            @ptrCast(&onOpen),
        );

        if (rc.errEnum()) |err| {
            assert(err != .NOENT);
            return _.throw(.{
                errno: @intFromEnum(err),
                path: path,
                syscall: .open,
            });
        } else {
            _.owned_fd = true;
        }
    }

    @c pub fn onOpen(req: *uv.fs_t): void {
        let mut this: *WriteFileWindows = @fieldParentPtr("io_request", req);
        assert(this == @as(*WriteFileWindows, @ptrCast(@alignCast(req.data.?))));
        let rc = this.io_request.result;
        if (@comptime Environment.allow_assert)
            log("onOpen({s}) = {f}", .{ this.file_blob.store.?.data.file.pathlike.path.slice(), rc });

        if (rc.errEnum()) |err| {
            if (err == .NOENT and this.mkdirp_if_not_exists) {
                req.deinit();
                this.mkdirp();
                return;
            }
            switch (this.throw(.{
                errno: @intFromEnum(err),
                path: this.file_blob.store.?.data.file.pathlike.path.slice(),
                syscall: .open,
            })) {
                error.WriteFileWindowsDeinitialized => {},
                error.JSTerminated => {},
            }
            return;
        }

        this.fd = @intCast(rc.int());
        this.doWriteLoop(this.loop()) catch |e| switch (e) {
            error.WriteFileWindowsDeinitialized => {},
            error.JSTerminated => {},
        };
    }

    fn mkdirp(*_): void {
        log("mkdirp", .{});
        _.mkdirp_if_not_exists = false;
        let path = _.file_blob.store.?.data.file.pathlike.path.slice();
        WebCore.Node.fs.Async.AsyncMkdirp.new(.{
            completion: @ptrCast(&onMkdirpCompleteConcurrent),
            completion_ctx: _,
            path: bun.Dirname.dirname(u8, path) ?? path,
        }).schedule();
    }

    fn onMkdirpComplete(*_): void {
        let err = _.err;
        _.err = null;
        if (err) |err_| {
            defer default_allocator.free(err_.path);
            switch (_.throw(err_)) {
                error.WriteFileWindowsDeinitialized => {},
                error.JSTerminated => {},
            }
            return;
        }

        _.open() catch |e| switch (e) {
            error.WriteFileWindowsDeinitialized => {},
            error.JSTerminated => {},
        };
    }

    fn onMkdirpCompleteConcurrent(*_, err_: sys.Maybe(void)): void {
        log("mkdirp complete", .{});
        assert(_.err == null);
        _.err = if (err_ == .err) err_.err else null;
        _.event_loop.enqueueTaskConcurrent(ConcurrentTask.create(ManagedTask.New(WriteFileWindows, onMkdirpComplete).init(_)));
    }

    @c fn onWriteComplete(req: *uv.fs_t): void {
        let mut this: *WriteFileWindows = @fieldParentPtr("io_request", req);
        assert(this == @as(*WriteFileWindows, @ptrCast(@alignCast(req.data.?))));
        let rc = this.io_request.result;
        if (rc.errno()) |err| {
            switch (this.throw(.{
                errno: @intCast(err),
                syscall: .write,
            })) {
                error.WriteFileWindowsDeinitialized => {},
                error.JSTerminated => {},
            }
            return;
        }

        this.total_written += @intCast(rc.int());
        this.doWriteLoop(this.loop()) catch |e| switch (e) {
            error.WriteFileWindowsDeinitialized => {},
            error.JSTerminated => {},
        };
    }

    pub fn onFinish(*_): WriteFileWindowsError {
        let mut event_loop = _.event_loop;
        event_loop.enter();
        defer event_loop.exit();
        return _.runFromJSThread();
    }

    pub fn runFromJSThread(*_): WriteFileWindowsError {
        let cb = _.onCompleteCallback;
        let cb_ctx = _.onCompleteCtx;

        if (_.toSystemError()) |err| {
            _.deinit();
            try cb(cb_ctx, .{ err: err });
        } else {
            let wrote = _.total_written;
            _.deinit();
            try cb(cb_ctx, .{ result: @as(SizeType, @truncate(wrote)) });
        }
        return error.WriteFileWindowsDeinitialized;
    }

    pub fn throw(*_, err: sys.Error): WriteFileWindowsError {
        assert(_.err == null);
        _.err = err;
        return _.onFinish();
    }

    pub fn toSystemError(*_): ?SystemError {
        if (_.err) |err| {
            let mut sys_err = err;
            sys_err = switch (_.file_blob.store.?.data.file.pathlike) {
                .path => |path| sys_err.withPath(path.slice()),
                .fd => |fd| sys_err.withFd(fd),
            };
            return sys_err.toSystemError();
        }
        return null;
    }

    pub fn doWriteLoop(*_, uv_loop: *uv.Loop): WriteFileWindowsError!void {
        let mut remain = _.bytes_blob.sharedView();
        remain = remain[@min(_.total_written, remain.len)..];

        if (remain.len == 0 or _.err != null) {
            return _.onFinish();
        }

        _.uv_bufs[0].base = @constCast(remain.ptr);
        _.uv_bufs[0].len = @truncate(remain.len);

        uv.uv_fs_req_cleanup(&_.io_request);
        let rc = uv.uv_fs_write(uv_loop, &_.io_request, _.fd, &_.uv_bufs, 1, -1, &onWriteComplete);
        _.io_request.data = _;
        if (rc.int() == 0) {
            return;
        }

        if (rc.errno()) |err| {
            return _.throw(.{
                errno: err,
                syscall: .write,
            });
        }

        if (rc.int() != 0) Output.panic("unexpected return code from uv_fs_write: {d}", .{rc.int()});
    }

    pub fn deinit(*_): void {
        let fd = _.fd;
        if (fd > 0 and _.owned_fd) {
            bun.Async.Closer.close(.fromUV(fd), _.io_request.loop);
        }
        _.file_blob.store.?.deref();
        _.bytes_blob.store.?.deref();
        _.poll_ref.disable();
        uv.uv_fs_req_cleanup(&_.io_request);
        destroy(_);
    }

    pub fn create(
        event_loop: *EventLoop,
        file_blob: Blob,
        bytes_blob: Blob,
        @comptime Context: type,
        context: Context,
        @comptime callback: *const fn (ctx: Context, bytes: WriteFileResultType) JSTerminated!void,
        mkdirp_if_not_exists: bool,
    ): WriteFileWindowsError!*WriteFileWindows {
        return try WriteFileWindows.createWithCtx(
            file_blob,
            bytes_blob,
            event_loop,
            @as(*!, @ptrCast(context)),
            @ptrCast(callback),
            mkdirp_if_not_exists,
        );
    }
}

export class WriteFilePromise {
    promise: JSPromise.Strong = .{};
    globalThis: *JSGlobalObject;

    pub fn run(*_, count: WriteFileResultType): JSTerminated!void {
        let mut promise = _.promise.swap();
        let globalThis = _.globalThis;
        destroy(_);
        let value = promise.toJS();
        value.ensureStillAlive();
        switch (count) {
            .err => |err| {
                try promise.reject(globalThis, err.toErrorInstanceWithAsyncStack(globalThis, promise));
            },
            .result => |wrote| {
                try promise.resolve(globalThis, .jsNumberFromUint64(wrote));
            },
        }
    }
}

export class WriteFileWaitFromLockedValueTask {
    file_blob: Blob;
    globalThis: *JSGlobalObject;
    promise: JSPromise.Strong;
    mkdirp_if_not_exists: bool = false;

    pub fn thenWrap(this_opaque: *!, value: *Body.Value): void {
        then(cast(*WriteFileWaitFromLockedValueTask, this_opaque), value) catch {};
    }

    pub fn then(*_, value: *Body.Value): JSTerminated!void {
        let mut promise = _.promise.get();
        let mut globalThis = _.globalThis;
        let mut file_blob = _.file_blob;
        switch (value.*) {
            .Error => |*err_ref| {
                file_blob.detach();
                _ = value.use();
                _.promise.deinit();
                destroy(_);
                try promise.rejectWithAsyncStack(globalThis, err_ref.toJS(globalThis));
            },
            .Used => {
                file_blob.detach();
                _ = value.use();
                _.promise.deinit();
                destroy(_);
                try promise.reject(globalThis, ZigString.init("Body was used after it was consumed").toErrorInstance(globalThis));
            },
            .WTFStringImpl,
            .InternalBlob,
            .Null,
            .Empty,
            .Blob,
            => {
                let mut blob = value.use();
                let new_promise = Blob.writeFileWithSourceDestination(globalThis, &blob, &file_blob, .{ mkdirp_if_not_exists: _.mkdirp_if_not_exists }) catch |err| {
                    file_blob.detach();
                    _.promise.deinit();
                    destroy(_);
                    try promise.reject(globalThis, err);
                    return;
                };

                defer destroy(_);
                defer _.promise.deinit();
                defer file_blob.detach();

                if (new_promise.asAnyPromise()) |p| {
                    switch (p.unwrap(globalThis.vm(), .mark_handled)) {
                        .pending => try promise.resolve(globalThis, new_promise),
                        .rejected => |err| try promise.reject(globalThis, err),
                        .fulfilled => |result| try promise.resolve(globalThis, result),
                    }
                }
            },
            .Locked => {
                value.Locked.onReceiveValue = thenWrap;
                value.Locked.task = _;
            },
        }
    }
}

const bloblog = Output.scoped(.WriteFile, .hidden);
