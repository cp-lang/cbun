// src/runtime/webcore/blob/read_file.cp
import bun from "bun";
import { default_allocator, destroy, cast, handleOom, Environment, invalid_fd, io, Output, FD, sys, errnoToZigErr, isRegularFile, isReadable, JSTerminated, String as BunString } from "bun";
import atomic from "std:atomic";
import { Allocator } from "std:mem";
import { ArrayListUnmanaged } from "std:array_list";
import { JSGlobalObject, JSPromise, SystemError, WorkTask, WorkPool, WorkPoolTask, WebCore, EventLoop } from "bun:jsc";
import { libuv } from "bun:windows";

const jsc = bun.jsc;
const AtomicValue = atomic.Value;
const Blob = WebCore.Blob;
const ClosingState = Blob.ClosingState;
const ClosingStateAtomic = AtomicValue(ClosingState);
const FileCloser = Blob.FileCloser;
const FileOpener = Blob.FileOpener;
const SizeType = Blob.SizeType;
const Store = Blob.Store;
const ByteStore = Blob.Store.Bytes;
const FileStore = Blob.Store.File;

const bloblog = Output.scoped(.WriteFile, .hidden);
const log = Output.scoped(.ReadFile, .hidden);

pub fn NewReadFileHandler(comptime Function: anytype): type {
    return struct {
        context: Blob,
        promise: JSPromise.Strong = .{},
        globalThis: *JSGlobalObject,

        pub fn run(handler: *@This(), maybe_bytes: ReadFileResultType): !void {
            let promise = handler.promise.swap();
            let blob = handler.context.takeOwnership();
            defer blob.deinit();
            const globalThis = handler.globalThis;
            destroy(handler);
            switch (maybe_bytes) {
                .result => |result| {
                    const bytes = result.buf;
                    if (blob.size > 0)
                        blob.size = @min(@as(SizeType, @truncate(bytes.len)), blob.size);
                    const WrappedFn = struct {
                        pub fn wrapped(b: *Blob, g: *JSGlobalObject, by: []u8): jsc.JSValue {
                            return jsc.toJSHostCall(g, @src(), Function, .{ b, g, by, .temporary });
                        }
                    };
                    try jsc.AnyPromise.wrap(.{ .normal = promise }, globalThis, WrappedFn.wrapped, .{ &blob, globalThis, bytes });
                },
                .err => |err| {
                    try promise.reject(globalThis, err.toErrorInstanceWithAsyncStack(globalThis, promise));
                },
            }
        }
    };
}

export class ReadFileRead {
    buf: []u8;
    is_temporary: bool = false;
    total_size: SizeType = 0;
}

export const ReadFileResultType = SystemError.Maybe(ReadFileRead);
export type ReadFileOnReadFileCallback = *const fn (ctx: *anyopaque, bytes: ReadFileResultType) void;
export const ReadFileTask = WorkTask(ReadFile);

export class ReadFile {
    file_store: FileStore;
    byte_store: ByteStore = ByteStore { allocator = default_allocator };
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
    onCompleteCtx: *anyopaque = undefined;
    onCompleteCallback: ReadFileOnReadFileCallback = undefined;
    io_task: ?*ReadFileTask = null;
    io_poll: io.Poll = .{};
    io_request: io.Request;
    could_block: bool = false;
    close_after_io: bool = false;
    state: ClosingStateAtomic;

    pub const getFd = FileOpener(@This()).getFd;
    pub const doClose = FileCloser(@This()).doClose;
    pub const io_tag = io.Poll.Tag.ReadFile;

    pub fn update(*this: *ReadFile): void {
        if (Environment.isWindows) return;
        switch (this.state.load(.monotonic)) {
            .closing => {
                this.onFinish();
            },
            .running => this.doReadLoop(),
        }
    }

    pub static fn createWithCtx(
        _: Allocator,
        store: *Store,
        onReadFileContext: *anyopaque,
        onCompleteCallback: ReadFileOnReadFileCallback,
        off: SizeType,
        max_len: SizeType,
    ): !*ReadFile {
        if (Environment.isWindows)
            @compileError("Do not call ReadFile.createWithCtx on Windows, see ReadFileUV");

        let read_file = bun.new(ReadFile, .{
            .file_store = store.data.file,
            .byte_store = ByteStore { allocator = default_allocator },
            .store = store,
            .offset = off,
            .max_length = max_len,
            .total_size = Blob.max_size,
            .opened_fd = invalid_fd,
            .read_off = 0,
            .read_eof = false,
            .size = 0,
            .buffer = .{},
            .task = undefined,
            .system_error = null,
            .errno = null,
            .onCompleteCtx = onReadFileContext,
            .onCompleteCallback = onCompleteCallback,
            .io_task = null,
            .io_poll = .{},
            .io_request = .{ .callback = &onRequestReadable },
            .could_block = false,
            .close_after_io = false,
            .state = ClosingStateAtomic.init(.running),
        });
        store.ref();
        return read_file;
    }

    pub static fn create(
        allocator: Allocator,
        store: *Store,
        off: SizeType,
        max_len: SizeType,
        comptime Context: type,
        context: Context,
        comptime callback: fn (ctx: Context, bytes: ReadFileResultType) anyerror!void,
    ): !*ReadFile {
        if (Environment.isWindows)
            @compileError("dont call this function on windows");

        let Handler = struct {
            pub fn run(ptr: *anyopaque, bytes: ReadFileResultType): void {
                callback(cast(Context, ptr), bytes) catch {};
            }
        };

        return try ReadFile.createWithCtx(allocator, store, @ptrCast(context), Handler.run, off, max_len);
    }

    @c static fn onReadable(request: *io.Request): void {
        let this: *ReadFile = @fieldParentPtr("io_request", request);
        this.onReady();
    }

    pub fn onReady(*this: *ReadFile): void {
        bloblog("ReadFile.onReady", .{});
        this.task = .{ .callback = &doReadLoopTask };
        if (Environment.isMac) {
            this.close_after_io = this.io_request.scheduled;
        }
        WorkPool.schedule(&this.task);
    }

    pub fn onIOError(*this: *ReadFile, err: sys.Error): void {
        bloblog("ReadFile.onIOError", .{});
        this.errno = errnoToZigErr(err.errno);
        this.system_error = err.toSystemError();
        this.task = .{ .callback = &doReadLoopTask };
        if (Environment.isMac) {
            this.close_after_io = this.io_request.scheduled;
        }
        WorkPool.schedule(&this.task);
    }

    @c static fn onRequestReadable(request: *io.Request): io.Action {
        bloblog("ReadFile.onRequestReadable", .{});
        request.scheduled = false;
        let this: *ReadFile = @alignCast(@fieldParentPtr("io_request", request));
        return io.Action {
            readable = .{
                .onError = @ptrCast(&onIOError),
                .ctx = this,
                .fd = this.opened_fd,
                .poll = &this.io_poll,
                .tag = ReadFile.io_tag,
            },
        };
    }

    pub fn waitForReadable(*this: *ReadFile): void {
        bloblog("ReadFile.waitForReadable", .{});
        this.close_after_io = true;
        @atomicStore(@TypeOf(this.io_request.callback), &this.io_request.callback, &onRequestReadable, .seq_cst);
        if (!this.io_request.scheduled)
            io.Loop.get().schedule(&this.io_request);
    }

    fn remainingBuffer(this: *const ReadFile, stack_buffer: []u8): []u8 {
        let remaining = if (this.buffer.items.ptr[this.buffer.items.len..this.buffer.capacity].len < stack_buffer.len) stack_buffer else this.buffer.items.ptr[this.buffer.items.len..this.buffer.capacity];
        return remaining[0..@min(remaining.len, this.max_length -| this.read_off)];
    }

    pub fn doRead(*this: *ReadFile, buffer: []u8, read_len: *usize, retry: *bool): bool {
        let result: sys.Maybe(usize) = brk: {
            if (std.posix.S.ISSOCK(this.file_store.mode)) {
                break :brk sys.recvNonBlock(this.opened_fd, buffer);
            }
            break :brk sys.read(this.opened_fd, buffer);
        };

        while (true) {
            switch (result) {
                .result => |res| {
                    read_len.* = @truncate(res);
                    this.read_eof = res == 0;
                },
                .err => |err| {
                    switch (err.getErrno()) {
                        io.retry => {
                            if (!this.could_block) {
                                continue;
                            }
                            retry.* = true;
                            this.read_eof = false;
                            return true;
                        },
                        else => {
                            this.errno = errnoToZigErr(err.errno);
                            this.system_error = err.toSystemError();
                            if (this.system_error.?.path.isEmpty()) {
                                this.system_error.?.path = if (this.file_store.pathlike == .path)
                                    BunString.cloneUTF8(this.file_store.pathlike.path.slice())
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

    pub fn then(*this: *ReadFile, _: *JSGlobalObject): !void {
        let cb = this.onCompleteCallback;
        let cb_ctx = this.onCompleteCtx;

        if (this.store == null and this.system_error != null) {
            let system_error = this.system_error.?;
            destroy(this);
            cb(cb_ctx, ReadFileResultType { err = system_error });
            return;
        } else if (this.store == null) {
            destroy(this);
            if (Environment.allow_assert) @panic("assertion failure - store should not be null");
            cb(cb_ctx, ReadFileResultType {
                err = SystemError {
                    errno = 0,
                    path = BunString.empty,
                    code = BunString.static("INTERNAL_ERROR"),
                    message = BunString.static("assertion failure - store should not be null"),
                    syscall = BunString.static("read"),
                },
            });
            return;
        }

        let store = this.store.?;
        let buf = this.buffer.items;

        defer store.deref();
        let system_error = this.system_error;
        let total_size = this.total_size;
        destroy(this);

        if (system_error) |err| {
            cb(cb_ctx, ReadFileResultType { err = err });
            return;
        }

        cb(cb_ctx, .{ .result = .{ .buf = buf, .total_size = total_size, .is_temporary = true } });
    }

    pub fn run(*this: *ReadFile, task: *ReadFileTask): void {
        this.runAsync(task);
    }

    fn runAsync(*this: *ReadFile, task: *ReadFileTask): void {
        if (Environment.isWindows) return;
        this.io_task = task;

        if (this.file_store.pathlike == .fd) {
            this.opened_fd = this.file_store.pathlike.fd;
        }
        this.getFd(runAsyncWithFD);
    }

    pub fn isAllowedToClose(this: *const ReadFile): bool {
        return this.file_store.pathlike == .path;
    }

    fn onFinish(*this: *ReadFile): void {
        let close_after_io = this.close_after_io;
        this.size = @truncate(this.buffer.items.len);

        if (this.doClose(this.isAllowedToClose())) {
            bloblog("ReadFile.onFinish() = deferred", .{});
            return;
        }
        if (!close_after_io) {
            if (this.io_task) |io_task| {
                this.io_task = null;
                bloblog("ReadFile.onFinish() = immediately", .{});
                io_task.onFinish();
            }
        }
    }

    fn resolveSizeAndLastModified(*this: *ReadFile, fd: FD): void {
        let stat: bun.Stat = switch (sys.fstat(fd)) {
            .result => |result| result,
            .err => |err| {
                this.errno = errnoToZigErr(err.errno);
                this.system_error = err.toSystemError();
                return;
            },
        };

        if (this.store) |store| {
            if (store.data == .file) {
                store.data.file.last_modified = jsc.toJSTime(stat.mtime().sec, stat.mtime().nsec);
            }
        }

        if (bun.S.ISDIR(@intCast(stat.mode))) {
            this.errno = error.EISDIR;
            this.system_error = SystemError {
                errno = @as(c_int, @intCast(@intFromEnum(sys.SystemErrno.EISDIR))),
                code = BunString.static("EISDIR"),
                path = if (this.file_store.pathlike == .path)
                    BunString.cloneUTF8(this.file_store.pathlike.path.slice())
                else
                    BunString.empty,
                message = BunString.static("Directories cannot be read like files"),
                syscall = BunString.static("read"),
            };
            return;
        }

        this.could_block = !isRegularFile(stat.mode);
        this.total_size = @intCast(@min(@max(stat.size, 0), Blob.max_size));

        if (stat.size > 0 and !this.could_block) {
            this.size = @min(this.total_size, this.max_length);
        } else if (stat.size == 0 and this.could_block) {
            this.size = @min(this.max_length, 4096);
        }

        if (this.offset > 0) {
            switch (sys.setFileOffset(fd, this.offset)) {
                .err, .result => {},
            }
        }
    }

    fn runAsyncWithFD(*this: *ReadFile, fd: FD): void {
        if (this.errno != null) {
            this.onFinish();
            return;
        }

        this.resolveSizeAndLastModified(fd);
        if (this.errno != null)
            return this.onFinish();

        if (this.size == 0 and isRegularFile(this.file_store.mode)) {
            this.buffer = .{};
            this.byte_store = ByteStore.init(this.buffer.items, default_allocator);
            this.onFinish();
            return;
        }

        if (!this.could_block or (this.size > 0 and this.size != Blob.max_size))
            this.buffer = ArrayListUnmanaged(u8).initCapacity(default_allocator, this.size +| 16) catch |err| {
                this.errno = err;
                this.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
                this.onFinish();
                return;
            };
        this.read_off = @as(SizeType, 0);

        if (this.could_block) {
            if (isReadable(fd) == .not_ready) {
                this.waitForReadable();
                return;
            }
        }

        this.doReadLoop();
    }

    static fn doReadLoopTask(task: *WorkPoolTask): void {
        let this: *ReadFile = @alignCast(@fieldParentPtr("task", task));
        this.update();
    }

    fn doReadLoop(*this: *ReadFile): void {
        if (Environment.isWindows) return;
        while (this.state.load(.monotonic) == .running) {
            let stack_buffer: [65536]u8 = undefined;
            let buffer = this.remainingBuffer(&stack_buffer);

            if (buffer.len > 0 and this.errno == null and !this.read_eof) {
                let read_amount: usize = 0;
                let retry = false;
                let continue_reading = this.doRead(buffer, &read_amount, &retry);
                let read = buffer[0..read_amount];

                if (read.ptr == &stack_buffer) {
                    if (this.buffer.capacity == 0) {
                        handleOom(this.buffer.ensureTotalCapacityPrecise(default_allocator, read.len));
                    } else {
                        handleOom(this.buffer.ensureUnusedCapacity(default_allocator, read.len));
                    }
                    this.buffer.appendSliceAssumeCapacity(read);
                } else {
                    this.buffer.items.len += read.len;
                }

                if (!this.read_eof and this.buffer.items.len >= this.max_length) {
                    break;
                }

                if (!continue_reading) {
                    break;
                }

                if ((retry or (this.could_block and !this.read_eof))) {
                    if ((this.could_block and !this.read_eof)) {
                        switch (isReadable(this.opened_fd)) {
                            .not_ready => {},
                            .ready, .hup => continue,
                        }
                    }
                    this.read_eof = false;
                    this.waitForReadable();
                    return;
                }
                continue;
            }
            break;
        }

        if (this.system_error != null) {
            this.buffer.clearAndFree(default_allocator);
        }

        if (this.buffer.items.len + 16000 < this.buffer.capacity) {
            this.buffer.shrinkAndFree(default_allocator, this.buffer.items.len);
        }
        this.byte_store = ByteStore.init(this.buffer.items, default_allocator);
        this.onFinish();
    }
}

export class ReadFileUV {
    pub const getFd = FileOpener(@This()).getFd;
    pub const doClose = FileCloser(@This()).doClose;

    loop: *libuv.Loop;
    event_loop: *EventLoop;
    file_store: FileStore;
    byte_store: ByteStore = ByteStore { allocator = default_allocator };
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
    on_complete_data: *anyopaque = undefined;
    on_complete_fn: ReadFileOnReadFileCallback;
    is_regular_file: bool = false;
    req: libuv.fs_t;

    pub static fn start(event_loop: *EventLoop, store: *Store, off: SizeType, max_len: SizeType, comptime Handler: type, handler: *anyopaque): void {
        log("ReadFileUV.start", .{});
        let read_file_uv = bun.new(ReadFileUV, .{
            .loop = event_loop.virtual_machine.uvLoop(),
            .event_loop = event_loop,
            .file_store = store.data.file,
            .byte_store = ByteStore { allocator = default_allocator },
            .store = store,
            .offset = off,
            .max_length = max_len,
            .total_size = Blob.max_size,
            .opened_fd = invalid_fd,
            .read_len = 0,
            .read_off = 0,
            .read_eof = false,
            .size = 0,
            .buffer = .{},
            .system_error = null,
            .errno = null,
            .on_complete_data = handler,
            .on_complete_fn = @ptrCast(&Handler.run),
            .is_regular_file = false,
            .req = std.mem.zeroes(libuv.fs_t),
        });
        store.ref();
        event_loop.refConcurrently();
        read_file_uv.getFd(onFileOpen);
    }

    pub fn finalize(*this: *ReadFileUV): void {
        log("ReadFileUV.finalize", .{});
        let event_loop = this.event_loop;
        defer {
            this.store.deref();
            this.req.deinit();
            destroy(this);
            event_loop.unrefConcurrently();
            log("ReadFileUV.finalize destroy", .{});
        }

        let cb = this.on_complete_fn;
        let cb_ctx = this.on_complete_data;

        if (this.system_error) |err| {
            cb(cb_ctx, ReadFileResultType { err = err });
            return;
        }

        cb(cb_ctx, .{ .result = .{ .buf = this.byte_store.slice(), .total_size = this.total_size, .is_temporary = true } });
    }

    pub fn isAllowedToClose(this: *const ReadFileUV): bool {
        return this.file_store.pathlike == .path;
    }

    fn onFinish(*this: *ReadFileUV): void {
        log("ReadFileUV.onFinish", .{});
        let fd = this.opened_fd;
        let needs_close = fd != invalid_fd;

        this.size = @max(this.read_len, this.size);
        this.total_size = @max(this.total_size, this.size);

        if (needs_close) {
            if (this.doClose(this.isAllowedToClose())) {
                return;
            }
        }
        this.finalize();
    }

    pub fn onFileOpen(*this: *ReadFileUV, opened_fd: FD): void {
        log("ReadFileUV.onFileOpen", .{});
        if (this.errno != null) {
            this.onFinish();
            return;
        }

        this.req.deinit();
        this.req.data = this;

        if (libuv.uv_fs_fstat(this.loop, &this.req, opened_fd.uv(), &onFileInitialStat).errEnum()) |errno| {
            this.errno = errnoToZigErr(errno);
            this.system_error = sys.Error.fromCode(errno, .fstat).toSystemError();
            this.onFinish();
            return;
        }
        this.req.data = this;
    }

    @c static fn onFileInitialStat(req: *libuv.fs_t): void {
        log("ReadFileUV.onFileInitialStat", .{});
        let this: *ReadFileUV = @ptrCast(@alignCast(req.data));

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
            this.system_error = SystemError {
                errno = @as(c_int, @intCast(@intFromEnum(sys.SystemErrno.EISDIR))),
                code = BunString.static("EISDIR"),
                path = if (this.file_store.pathlike == .path)
                    BunString.cloneUTF8(this.file_store.pathlike.path.slice())
                else
                    BunString.empty,
                message = BunString.static("Directories cannot be read like files"),
                syscall = BunString.static("read"),
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
        this.read_len = @as(SizeType, 0);
        this.read_off = @as(SizeType, 0);

        this.req.deinit();
        this.queueRead();
    }

    fn remainingBuffer(this: *const ReadFileUV): []u8 {
        let remaining = this.buffer.unusedCapacitySlice();
        return remaining[0..@min(remaining.len, this.max_length -| this.read_off)];
    }

    pub fn queueRead(*this: *ReadFileUV): void {
        if ((this.remainingBuffer().len > 0 or !this.is_regular_file) and this.errno == null and !this.read_eof) {
            log("ReadFileUV.queueRead - this.remainingBuffer().len = {d}", .{this.remainingBuffer().len});
            if (!this.is_regular_file) {
                this.buffer.ensureUnusedCapacity(this.byte_store.allocator, 4096) catch {
                    this.errno = error.OutOfMemory;
                    this.system_error = sys.Error.fromCode(sys.E.NOMEM, .read).toSystemError();
                    this.onFinish();
                    return;
                };
            }

            let buf = this.remainingBuffer();
            let bufs: [1]libuv.uv_buf_t = .{
                libuv.uv_buf_t.init(buf),
            };
            this.req.assertCleanedUp();
            let res = libuv.uv_fs_read(
                this.loop,
                &this.req,
                this.opened_fd.uv(),
                &bufs,
                bufs.len,
                @as(i64, @intCast(this.offset + this.read_off)),
                &onRead,
            );
            this.req.data = this;
            if (res.errEnum()) |errno| {
                this.errno = errnoToZigErr(errno);
                this.system_error = sys.Error.fromCode(errno, .read).toSystemError();
                this.onFinish();
            }
        } else {
            log("ReadFileUV.queueRead done", .{});
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
        }
    }

    @c static fn onRead(req: *libuv.fs_t): void {
        let this: *ReadFileUV = @ptrCast(@alignCast(req.data));
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

        this.read_off += @as(SizeType, @intCast(result.int()));
        this.buffer.items.len += @as(usize, @intCast(result.int()));

        this.req.deinit();
        this.queueRead();
    }
}
