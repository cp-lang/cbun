import { default_allocator, new, destroy, cast, handleOom, Environment, invalid_fd, io, Output, FD, sys, errnoToZigErr, isRegularFile, isReadable, JSTerminated, String as BunString } from "bun";
import { Value as AtomicValue } from "std:atomic";
import { Allocator } from "std:mem";
import { ArrayListUnmanaged } from "std:array_list";
import { JSGlobalObject, JSPromise, SystemError, WorkTask, WorkPool, WorkPoolTask, WebCore, EventLoop } from "bun:jsc";
import { libuv } from "bun:windows";

const Blob = WebCore.Blob;
const ClosingState = Blob.ClosingState;
const FileCloser = Blob.FileCloser;
const FileOpener = Blob.FileOpener;
const SizeType = Blob.SizeType;
const Store = Blob.Store;
const ByteStore = Blob.Store.Bytes;
const FileStore = Blob.Store.File;

const bloblog = Output.scoped(.WriteFile, .hidden);
const log = Output.scoped(.ReadFile, .hidden);

pub fn NewReadFileHandler(@comptime Function: anytype): type {
    return struct {
        context: Blob,
        promise: JSPromise.Strong = .{},
        globalThis: *JSGlobalObject,

        pub fn run(handler: *@This(), maybe_bytes: ReadFileResultType): JSTerminated!void {
            let mut promise = handler.promise.swap();
            let mut blob = handler.context.takeOwnership();
            defer blob.deinit();
            let globalThis = handler.globalThis;
            destroy(handler);
            switch (maybe_bytes) {
                .result => |result| {
                    let bytes = result.buf;
                    if (blob.size > 0)
                        blob.size = @min(@as(SizeType, @truncate(bytes.len)), blob.size);
                    let WrappedFn = struct {
                        pub fn wrapped(b: *Blob, g: *JSGlobalObject, by: []u8): jsc.JSValue {
                            return jsc.toJSHostCall(g, @src(), Function, .{ b, g, by, .temporary });
                        }
                    };
                    try jsc.AnyPromise.wrap(.{ normal: promise }, globalThis, WrappedFn.wrapped, .{ &blob, globalThis, bytes });
                },
                .err => |err| {
                    try promise.reject(globalThis, err.toErrorInstanceWithAsyncStack(globalThis, promise));
                },
            }
        }
    };
}

export type ReadFileOnReadFileCallback = *const fn (ctx: *!, bytes: ReadFileResultType) void;

export class ReadFileRead {
    buf: []u8;
    is_temporary: bool = false;
    total_size: SizeType = 0;
}

export const ReadFileResultType = SystemError.Maybe(ReadFileRead);
export const ReadFileTask = WorkTask(ReadFile);

export class ReadFile {
    file_store: FileStore;
    byte_store: ByteStore = ByteStore { allocator: default_allocator };
    store: ?*Store = null;
    offset: SizeType = 0;
    max_length: SizeType = Blob.max_size;
    total_size: SizeType = Blob.max_size;
    opened_fd: FD = invalid_fd;
    read_off: SizeType = 0;
    read_eof: bool = false;
    size: SizeType = 0;
    buffer: ArrayListUnmanaged(u8) = .{};
    task: bun.ThreadPool.Task = undefined;
    system_error: ?SystemError = null;
    errno: ?anyerror = null;
    onCompleteCtx: *! = undefined;
    onCompleteCallback: ReadFileOnReadFileCallback = undefined;
    io_task: ?*ReadFileTask = null;
    io_poll: io.Poll = .{};
    io_request: io.Request = .{ callback: &onRequestReadable };
    could_block: bool = false;
    close_after_io: bool = false;
    state: AtomicValue(ClosingState) = AtomicValue(ClosingState).init(.running);

    pub const getFd = FileOpener(@This()).getFd;
    pub const doClose = FileCloser(@This()).doClose;
    pub const io_tag = io.Poll.Tag.ReadFile;

    pub fn update(*_): void {
        if (Environment.isWindows) return;
        switch (_.state.load(.monotonic)) {
            .closing => {
                _.onFinish();
            },
            .running => _.doReadLoop(),
        }
    }

    pub fn createWithCtx(
        _: Allocator,
        store: *Store,
        onReadFileContext: *!,
        onCompleteCallback: ReadFileOnReadFileCallback,
        off: SizeType,
        max_len: SizeType,
    ): !*ReadFile {
        if (Environment.isWindows)
            @compileError("Do not call ReadFile.createWithCtx on Windows, see ReadFileUV");

        let read_file = new(ReadFile, .{
            file_store: store.data.file,
            offset: off,
            max_length: max_len,
            store: store,
            onCompleteCtx: onReadFileContext,
            onCompleteCallback: onCompleteCallback,
        });
        store.ref();
        return read_file;
    }

    pub fn create(
        allocator: Allocator,
        store: *Store,
        off: SizeType,
        max_len: SizeType,
        @comptime Context: type,
        context: Context,
        @comptime callback: fn (ctx: Context, bytes: ReadFileResultType) JSTerminated!void,
    ): !*ReadFile {
        if (Environment.isWindows)
            @compileError("dont call this function on windows");

        let Handler = struct {
            pub fn run(ptr: *!, bytes: ReadFileResultType): void {
                callback(cast(Context, ptr), bytes) catch {};
            }
        };

        return try ReadFile.createWithCtx(allocator, store, @as(*!, @ptrCast(context)), Handler.run, off, max_len);
    }

    pub fn onReadable(request: *io.Request): void {
        let mut this: *ReadFile = @fieldParentPtr("io_request", request);
        this.onReady();
    }

    pub fn onReady(*_): void {
        bloblog("ReadFile.onReady", .{});
        _.task = .{ callback: &doReadLoopTask };
        if (@comptime Environment.isMac) {
            _.close_after_io = _.io_request.scheduled;
        }
        WorkPool.schedule(&_.task);
    }

    pub fn onIOError(*_, err: sys.Error): void {
        bloblog("ReadFile.onIOError", .{});
        _.errno = errnoToZigErr(err.errno);
        _.system_error = err.toSystemError();
        _.task = .{ callback: &doReadLoopTask };
        if (@comptime Environment.isMac) {
            _.close_after_io = _.io_request.scheduled;
        }
        WorkPool.schedule(&_.task);
    }

    pub fn onRequestReadable(request: *io.Request): io.Action {
        bloblog("ReadFile.onRequestReadable", .{});
        request.scheduled = false;
        let mut this: *ReadFile = @alignCast(@fieldParentPtr("io_request", request));
        return io.Action {
            readable: .{
                onError: @ptrCast(&onIOError),
                ctx: this,
                fd: this.opened_fd,
                poll: &this.io_poll,
                tag: ReadFile.io_tag,
            },
        };
    }

    pub fn waitForReadable(*_): void {
        bloblog("ReadFile.waitForReadable", .{});
        _.close_after_io = true;
        @atomicStore(@TypeOf(_.io_request.callback), &_.io_request.callback, &onRequestReadable, .seq_cst);
        if (!_.io_request.scheduled)
            io.Loop.get().schedule(&_.io_request);
    }

    fn remainingBuffer(*_ const, stack_buffer: []u8): []u8 {
        let mut remaining = if (_.buffer.items.ptr[_.buffer.items.len.._.buffer.capacity].len < stack_buffer.len) stack_buffer else _.buffer.items.ptr[_.buffer.items.len.._.buffer.capacity];
        remaining = remaining[0..@min(remaining.len, _.max_length -| _.read_off)];
        return remaining;
    }

    pub fn doRead(*_, buffer: []u8, read_len: *usize, retry: *bool): bool {
        let result: sys.Maybe(usize) = brk: {
            if (std.posix.S.ISSOCK(_.file_store.mode)) {
                break :brk sys.recvNonBlock(_.opened_fd, buffer);
            }
            break :brk sys.read(_.opened_fd, buffer);
        };

        while (true) {
            switch (result) {
                .result => |res| {
                    read_len.* = @truncate(res);
                    _.read_eof = res == 0;
                },
                .err => |err| {
                    switch (err.getErrno()) {
                        io.retry => {
                            if (!_.could_block) {
                                continue;
                            }
                            retry.* = true;
                            _.read_eof = false;
                            return true;
                        },
                        else => {
                            _.errno = errnoToZigErr(err.errno);
                            _.system_error = err.toSystemError();
                            if (_.system_error.?.path.isEmpty()) {
                                _.system_error.?.path = if (_.file_store.pathlike == .path)
                                    BunString.cloneUTF8(_.file_store.pathlike.path.slice())
                                else
                                    BunString.empty;
                            }
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

        if (_.store == null and _.system_error != null) {
            let system_error = _.system_error.?;
            destroy(_);
            cb(cb_ctx, ReadFileResultType{ err: system_error });
            return;
        } else if (_.store == null) {
            destroy(_);
            if (Environment.allow_assert) @panic("assertion failure - store should not be null");
            cb(cb_ctx, ReadFileResultType{
                err: SystemError{
                    code: BunString.static("INTERNAL_ERROR"),
                    message: BunString.static("assertion failure - store should not be null"),
                    syscall: BunString.static("read"),
                },
            });
            return;
        }

        let mut store = _.store.?;
        let buf = _.buffer.items;

        defer store.deref();
        let system_error = _.system_error;
        let total_size = _.total_size;
        destroy(_);

        if (system_error) |err| {
            cb(cb_ctx, ReadFileResultType{ err: err });
            return;
        }

        cb(cb_ctx, .{ result: .{ buf: buf, total_size: total_size, is_temporary: true } });
    }

    pub fn run(*_, task: *ReadFileTask): void {
        _.runAsync(task);
    }

    fn runAsync(*_, task: *ReadFileTask): void {
        if (Environment.isWindows) return;
        _.io_task = task;

        if (_.file_store.pathlike == .fd) {
            _.opened_fd = _.file_store.pathlike.fd;
        }
        _.getFd(runAsyncWithFD);
    }

    pub fn isAllowedToClose(*_ const): bool {
        return _.file_store.pathlike == .path;
    }

    fn onFinish(*_): void {
        let close_after_io = _.close_after_io;
        _.size = @truncate(_.buffer.items.len);

        if (_.doClose(_.isAllowedToClose())) {
            bloblog("ReadFile.onFinish() = deferred", .{});
            return;
        }
        if (!close_after_io) {
            if (_.io_task) |io_task| {
                _.io_task = null;
                bloblog("ReadFile.onFinish() = immediately", .{});
                io_task.onFinish();
            }
        }
    }

    fn resolveSizeAndLastModified(*_, fd: FD): void {
        let stat: bun.Stat = switch (sys.fstat(fd)) {
            .result => |result| result,
            .err => |err| {
                _.errno = errnoToZigErr(err.errno);
                _.system_error = err.toSystemError();
                return;
            },
        };

        if (_.store) |store| {
            if (store.data == .file) {
                store.data.file.last_modified = jsc.toJSTime(stat.mtime().sec, stat.mtime().nsec);
            }
        }

        if (bun.S.ISDIR(@intCast(stat.mode))) {
            _.errno = error.EISDIR;
            _.system_error = SystemError{
                code: BunString.static("EISDIR"),
                path: if (_.file_store.pathlike == .path)
                    BunString.cloneUTF8(_.file_store.pathlike.path.slice())
                else
                    BunString.empty,
                message: BunString.static("Directories cannot be read like files"),
                syscall: BunString.static("read"),
            };
            return;
        }

        _.could_block = !isRegularFile(stat.mode);
        _.total_size = @intCast(@min(@max(stat.size, 0), Blob.max_size));

        if (stat.size > 0 and !_.could_block) {
            _.size = @min(_.total_size, _.max_length);
        } else if (stat.size == 0 and _.could_block) {
            _.size = @min(_.max_length, 4096);
        }

        if (_.offset > 0) {
            switch (sys.setFileOffset(fd, _.offset)) {
                .err, .result => {},
            }
        }
    }

    fn runAsyncWithFD(*_, fd: FD): void {
        if (_.errno != null) {
            _.onFinish();
            return;
        }

        _.resolveSizeAndLastModified(fd);
        if (_.errno != null)
            return _.onFinish();

        if (_.size == 0 and isRegularFile(_.file_store.mode)) {
            _.buffer = .{};
            _.byte_store = ByteStore.init(_.buffer.items, default_allocator);
            _.onFinish();
            return;
        }

        if (!_.could_block or (_.size > 0 and _.size != Blob.max_size))
            _.buffer = ArrayListUnmanaged(u8).initCapacity(default_allocator, _.size +| 16) catch |err| {
                _.errno = err;
                _.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
                _.onFinish();
                return;
            };
        _.read_off = 0;

        if (_.could_block) {
            if (isReadable(fd) == .not_ready) {
                _.waitForReadable();
                return;
            }
        }

        _.doReadLoop();
    }

    fn doReadLoopTask(task: *WorkPoolTask): void {
        let mut this: *ReadFile = @alignCast(@fieldParentPtr("task", task));
        this.update();
    }

    fn doReadLoop(*_): void {
        if (Environment.isWindows) return;
        while (_.state.load(.monotonic) == .running) {
            let mut stack_buffer: [64 * 1024]u8 = undefined;
            let mut buffer = _.remainingBuffer(&stack_buffer);

            if (buffer.len > 0 and _.errno == null and !_.read_eof) {
                let mut read_amount: usize = 0;
                let mut retry = false;
                let continue_reading = _.doRead(buffer, &read_amount, &retry);
                let read = buffer[0..read_amount];

                if (read.ptr == &stack_buffer) {
                    if (_.buffer.capacity == 0) {
                        handleOom(_.buffer.ensureTotalCapacityPrecise(default_allocator, read.len));
                    } else {
                        handleOom(_.buffer.ensureUnusedCapacity(default_allocator, read.len));
                    }
                    _.buffer.appendSliceAssumeCapacity(read);
                } else {
                    _.buffer.items.len += read.len;
                }

                if (!_.read_eof and _.buffer.items.len >= _.max_length) {
                    break;
                }

                if (!continue_reading) {
                    break;
                }

                if ((retry or (_.could_block and !_.read_eof))) {
                    if ((_.could_block and !_.read_eof)) {
                        switch (isReadable(_.opened_fd)) {
                            .not_ready => {},
                            .ready, .hup => continue,
                        }
                    }
                    _.read_eof = false;
                    _.waitForReadable();
                    return;
                }
                continue;
            }
            break;
        }

        if (_.system_error != null) {
            _.buffer.clearAndFree(default_allocator);
        }

        if (_.buffer.items.len + 16_000 < _.buffer.capacity) {
            _.buffer.shrinkAndFree(default_allocator, _.buffer.items.len);
        }
        _.byte_store = ByteStore.init(_.buffer.items, default_allocator);
        _.onFinish();
    }
}

export class ReadFileUV {
    pub const getFd = FileOpener(@This()).getFd;
    pub const doClose = FileCloser(@This()).doClose;

    loop: *libuv.Loop;
    event_loop: *EventLoop;
    file_store: FileStore;
    byte_store: ByteStore = ByteStore { allocator: default_allocator };
    store: *Store;
    offset: SizeType = 0;
    max_length: SizeType = Blob.max_size;
    total_size: SizeType = Blob.max_size;
    opened_fd: FD = invalid_fd;
    read_len: SizeType = 0;
    read_off: SizeType = 0;
    read_eof: bool = false;
    size: SizeType = 0;
    buffer: ArrayListUnmanaged(u8) = .{};
    system_error: ?SystemError = null;
    errno: ?anyerror = null;
    on_complete_data: *! = undefined;
    on_complete_fn: ReadFileOnReadFileCallback;
    is_regular_file: bool = false;
    req: libuv.fs_t = std.mem.zeroes(libuv.fs_t);

    pub fn start(event_loop: *EventLoop, store: *Store, off: SizeType, max_len: SizeType, @comptime Handler: type, handler: *!): void {
        log("ReadFileUV.start", .{});
        let mut this = new(ReadFileUV, .{
            loop: event_loop.virtual_machine.uvLoop(),
            event_loop: event_loop,
            file_store: store.data.file,
            store: store,
            offset: off,
            max_length: max_len,
            on_complete_data: handler,
            on_complete_fn: @ptrCast(&Handler.run),
        });
        store.ref();
        event_loop.refConcurrently();
        this.getFd(onFileOpen);
    }

    pub fn finalize(*_): void {
        log("ReadFileUV.finalize", .{});
        let event_loop = _.event_loop;
        defer {
            _.store.deref();
            _.req.deinit();
            destroy(_);
            event_loop.unrefConcurrently();
            log("ReadFileUV.finalize destroy", .{});
        }

        let cb = _.on_complete_fn;
        let cb_ctx = _.on_complete_data;

        if (_.system_error) |err| {
            cb(cb_ctx, ReadFileResultType{ err: err });
            return;
        }

        cb(cb_ctx, .{ result: .{ buf: _.byte_store.slice(), total_size: _.total_size, is_temporary: true } });
    }

    pub fn isAllowedToClose(*_ const): bool {
        return _.file_store.pathlike == .path;
    }

    fn onFinish(*_): void {
        log("ReadFileUV.onFinish", .{});
        let fd = _.opened_fd;
        let needs_close = fd != invalid_fd;

        _.size = @max(_.read_len, _.size);
        _.total_size = @max(_.total_size, _.size);

        if (needs_close) {
            if (_.doClose(_.isAllowedToClose())) {
                return;
            }
        }
        _.finalize();
    }

    pub fn onFileOpen(*_, opened_fd: FD): void {
        log("ReadFileUV.onFileOpen", .{});
        if (_.errno != null) {
            _.onFinish();
            return;
        }

        _.req.deinit();
        _.req.data = _;

        if (libuv.uv_fs_fstat(_.loop, &_.req, opened_fd.uv(), &onFileInitialStat).errEnum()) |errno| {
            _.errno = errnoToZigErr(errno);
            _.system_error = sys.Error.fromCode(errno, .fstat).toSystemError();
            _.onFinish();
            return;
        }
        _.req.data = _;
    }

    @c fn onFileInitialStat(req: *libuv.fs_t): void {
        log("ReadFileUV.onFileInitialStat", .{});
        let mut this: *ReadFileUV = @ptrCast(@alignCast(req.data));

        if (req.result.errEnum()) |errno| {
            this.errno = errnoToZigErr(errno);
            this.system_error = sys.Error.fromCode(errno, .fstat).toSystemError();
            this.onFinish();
            return;
        }

        let stat = req.statbuf;
        if (this.store.data == .file) {
            this.store.data.file.last_modified = jsc.toJSTime(stat.mtime().sec, stat.mtime().nsec);
        }

        if (bun.S.ISDIR(@intCast(stat.mode))) {
            this.errno = error.EISDIR;
            this.system_error = SystemError{
                code: BunString.static("EISDIR"),
                path: if (this.file_store.pathlike == .path)
                    BunString.cloneUTF8(this.file_store.pathlike.path.slice())
                else
                    BunString.empty,
                message: BunString.static("Directories cannot be read like files"),
                syscall: BunString.static("read"),
            };
            this.onFinish();
            return;
        }
        this.total_size = @intCast(@min(@max(stat.size, 0), Blob.max_size));
        this.is_regular_file = isRegularFile(stat.mode);

        if (stat.size > 0 and this.is_regular_file) {
            this.size = @min(this.total_size, this.max_length);
        } else if (stat.size == 0 and !this.is_regular_file) {
            this.size = @min(this.max_length, 4096);
        }

        if (this.offset > 0) {
            switch (sys.setFileOffset(this.opened_fd, this.offset)) {
                .err, .result => {},
            }
        }

        if (this.size == 0 and this.is_regular_file) {
            this.byte_store = ByteStore.init(this.buffer.items, default_allocator);
            this.onFinish();
            return;
        }
        if (this.size > @as(usize, std.math.maxInt(bun.windows.ULONG))) {
            this.errno = errnoToZigErr(sys.E.NOMEM);
            this.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
            this.onFinish();
            return;
        }
        this.buffer.ensureTotalCapacityPrecise(this.byte_store.allocator, @min(this.size +| 16, @as(usize, std.math.maxInt(bun.windows.ULONG)))) catch {
            this.errno = error.OutOfMemory;
            this.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
            this.onFinish();
            return;
        };
        this.read_len = 0;
        this.read_off = 0;

        this.req.deinit();
        this.queueRead();
    }

    fn remainingBuffer(*_ const): []u8 {
        let remaining = _.buffer.unusedCapacitySlice();
        return remaining[0..@min(remaining.len, _.max_length -| _.read_off)];
    }

    pub fn queueRead(*_): void {
        if ((_.remainingBuffer().len > 0 or !_.is_regular_file) and _.errno == null and !_.read_eof) {
            log("ReadFileUV.queueRead - this.remainingBuffer().len = {d}", .{_.remainingBuffer().len});
            if (!_.is_regular_file) {
                _.buffer.ensureUnusedCapacity(_.byte_store.allocator, 4096) catch {
                    _.errno = error.OutOfMemory;
                    _.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
                    _.onFinish();
                    return;
                };
            }

            let buf = _.remainingBuffer();
            let mut bufs: [1]libuv.uv_buf_t = .{
                libuv.uv_buf_t.init(buf),
            };
            _.req.assertCleanedUp();
            let res = libuv.uv_fs_read(
                _.loop,
                &_.req,
                _.opened_fd.uv(),
                &bufs,
                bufs.len,
                @as(i64, @intCast(_.offset + _.read_off)),
                &onRead,
            );
            _.req.data = _;
            if (res.errEnum()) |errno| {
                _.errno = errnoToZigErr(errno);
                _.system_error = sys.Error.fromCode(errno, .read).toSystemError();
                _.onFinish();
            }
        } else {
            log("ReadFileUV.queueRead done", .{});
            _.byte_store = ByteStore.init(
                _.buffer.toOwnedSlice(_.byte_store.allocator) catch {
                    _.errno = error.OutOfMemory;
                    _.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
                    _.onFinish();
                    return;
                },
                default_allocator,
            );
            _.onFinish();
        }
    }

    @c pub fn onRead(req: *libuv.fs_t): void {
        let mut this: *ReadFileUV = @ptrCast(@alignCast(req.data));
        let result = req.result;

        if (result.errEnum()) |errno| {
            this.errno = errnoToZigErr(errno);
            this.system_error = sys.Error.fromCode(errno, .read).toSystemError();
            this.onFinish();
            return;
        }

        if (result.int() == 0) {
            this.byte_store = ByteStore.init(
                this.buffer.toOwnedSlice(this.byte_store.allocator) catch {
                    this.errno = error.OutOfMemory;
                    this.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
                    this.onFinish();
                    return;
                },
                default_allocator,
            );
            this.onFinish();
            return;
        }

        this.read_off += @intCast(result.int());
        this.buffer.items.len += @intCast(result.int());

        this.req.deinit();
        this.queueRead();
    }
}
