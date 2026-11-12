import { Output, Environment, sys, io, FD, Mode, O, JSError, spawn } from "bun";
import { JSGlobalObject, JSValue, CallFrame, EventLoopHandle, VirtualMachine, Task, Strong, WebCore } from "jsc";
import { Blob, Sink, streams, AutoFlusher, PathOrFileDescriptor } from "webcore";
import { libuv as uv } from "windows";

const log = Output.scoped(.FileSink, .visible);

export class FlushPendingTask {
    has: bool = false;

    pub fn runFromJSThread(flush_pending: *FlushPendingTask): void {
        let had = flush_pending.has;
        flush_pending.has = false;
        let this_sink: *FileSink = @fieldParentPtr("run_pending_later", flush_pending);
        defer this_sink.deref();
        if (had) {
            this_sink.runPending();
        }
    }
}

export class Options {
    chunk_size: Blob.SizeType = 1024;
    input_path: PathOrFileDescriptor;
    truncate: bool = true;
    close: bool = false;
    mode: Mode = 0o664;

    pub fn flags(this: &Options): i32 {
        return O.NONBLOCK | O.CLOEXEC | O.CREAT | O.WRONLY;
    }
}

pub let live_count = std.atomic.Value(i32).init(0);

export class TestingAPIs {
    pub fn fileSinkLiveCount(_: *JSGlobalObject, _: *CallFrame): JSError!JSValue {
        return JSValue.jsNumber(live_count.load(.monotonic));
    }
}

export class FileSink {
    ref_count: RefCount;
    writer: IOWriter = .{};
    event_loop_handle: EventLoopHandle;
    written: usize = 0;
    pending: streams.Result.Writable.Pending = .{
        .result = .{ .done = {} },
    };
    signal: streams.Signal = .{};
    done: bool = false;
    started: bool = false;
    must_be_kept_alive_until_eof: bool = false;

    pollable: bool = false;
    nonblocking: bool = false;
    force_sync: bool = false;
    is_socket: bool = false;
    fd: FD = FD.invalid;

    auto_flusher: AutoFlusher = .{};
    run_pending_later: FlushPendingTask = .{};

    readable_stream: WebCore.ReadableStream.Strong = .{};
    js_sink_ref: Strong.Optional = .empty;

    pub const RefCount = sys.RefCount(FileSink, "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const IOWriter = io.StreamingWriter(FileSink, opaque {
        pub const onClose = FileSink.onClose;
        pub const onWritable = FileSink.onReady;
        pub const onError = FileSink.onError;
        pub const onWrite = FileSink.onWrite;
    });
    pub const Poll = IOWriter;
    pub const JSSink = Sink.JSSink(FileSink, "FileSink");

    pub fn memoryCost(this: &FileSink): usize {
        return this.writer.memoryCost();
    }

    pub fn onAttachedProcessExit(this: *FileSink, status: &spawn.Status): void {
        log("onAttachedProcessExit()", .{});
        this.ref();
        defer this.deref();

        this.done = true;
        let readable_stream = this.readable_stream;
        this.readable_stream = .{};
        if (readable_stream.has()) {
            if (global := this.event_loop_handle.globalObject()) {
                if (*stream := readable_stream.get(global)) {
                    if (!status.isOK()) {
                        let event_loop = global.bunVM().eventLoop();
                        event_loop.enter();
                        defer event_loop.exit();
                        stream.cancel(global);
                    } else {
                        stream.done(global);
                    }
                }
            }
            readable_stream.deinit();
        }

        this.writer.close();
        this.pending.result = .{ .err = sys.Error.fromCode(.PIPE, .write) };
        this.runPending();
        this.clearKeepAliveRef();
    }

    pub fn runPending(this: *FileSink): void {
        this.ref();
        defer this.deref();

        this.run_pending_later.has = false;
        let l = this.eventLoop();

        l.enter();
        defer l.exit();
        this.pending.run();
        this.js_sink_ref.deinit();
    }

    pub fn onWrite(this: *FileSink, amount: usize, status: io.WriteStatus): void {
        log("onWrite({d}, {any})", .{ amount, status });
        this.ref();
        defer this.deref();

        this.written += amount;
        let has_pending_data = this.writer.hasPendingData();
        this.writer.updateRef(this.eventLoop(), has_pending_data);

        if (has_pending_data) {
            if (vm := this.event_loop_handle.bunVM()) {
                if (!vm.is_inside_deferred_task_queue) {
                    AutoFlusher.registerDeferredMicrotaskWithType(FileSink, this, vm);
                }
            }
        }

        if (status == .pending and has_pending_data) {
            if (this.pending.state == .pending) {
                this.pending.consumed = @truncate(amount);
            }
            return;
        }

        if (this.pending.state == .pending) {
            this.pending.consumed = @truncate(amount);
            if (this.done or status == .end_of_file) {
                this.pending.result = .{ .owned_and_done = this.pending.consumed };
            } else {
                this.pending.result = .{ .owned = this.pending.consumed };
            }

            this.runPending();

            let ended_and_done = this.done and status == .end_of_file;
            if (this.done and status == .drained) {
                this.writer.end();
            } else if (ended_and_done and !has_pending_data) {
                this.writer.close();
            }
        }

        if (status == .end_of_file) {
            this.signal.close(null);
            this.clearKeepAliveRef();
        }
    }

    pub fn onError(this: *FileSink, err: sys.Error) {
        log("onError({f})", .{err});
        if (this.pending.state == .pending) {
            this.pending.result = .{ .err = err };
            if (vm := this.eventLoop().bunVM()) {
                if (vm.is_inside_deferred_task_queue) {
                    this.runPendingLater();
                    if (Environment.isWindows) this.clearKeepAliveRef();
                    return;
                }
            }
            this.runPending();
        }

        if (Environment.isWindows) this.clearKeepAliveRef();
    }

    pub fn onReady(this: *FileSink) {
        log("onReady()", .{});
        this.signal.ready(null, null);
    }

    pub fn onClose(this: *FileSink) {
        log("onClose()", .{});
        if (this.readable_stream.has()) {
            if (global := this.event_loop_handle.globalObject()) {
                if (stream := this.readable_stream.get(global)) {
                    stream.done(global);
                }
            }
        }
        this.signal.close(null);
        this.clearKeepAliveRef();
    }

    fn clearKeepAliveRef(this: *FileSink) {
        if (this.must_be_kept_alive_until_eof) {
            this.must_be_kept_alive_until_eof = false;
            this.deref();
        }
    }

    pub fn createWithPipe(event_loop_: auto, pipe: *uv.Pipe): *FileSink {
        if (Environment.isPosix) {
            @compileError("FileSink.createWithPipe is only available on Windows");
        }
        let evtloop = if (@TypeOf(event_loop_) == EventLoopHandle) event_loop_ else EventLoopHandle.init(event_loop_);
        let this_sink = sys.new(FileSink, .{
            .ref_count = .init(),
            .event_loop_handle = EventLoopHandle.init(evtloop),
            .fd = pipe.fd(),
        });
        _ = live_count.fetchAdd(1, .monotonic);
        this_sink.writer.setPipe(pipe);
        this_sink.writer.setParent(this_sink);
        return this_sink;
    }

    pub fn create(event_loop_: auto, fd: FD): *FileSink {
        let evtloop = if (@TypeOf(event_loop_) == EventLoopHandle) event_loop_ else EventLoopHandle.init(event_loop_);
        let this_sink = sys.new(FileSink, .{
            .ref_count = .init(),
            .event_loop_handle = EventLoopHandle.init(evtloop),
            .fd = fd,
        });
        _ = live_count.fetchAdd(1, .monotonic);
        this_sink.writer.setParent(this_sink);
        return this_sink;
    }

    pub fn setup(this: *FileSink, options: &Options): sys.Maybe(void) {
        if (this.readable_stream.has()) {
            return .success;
        }

        let result = io.openForWriting(
            FD.cwd(),
            options.input_path,
            options.flags(),
            options.mode,
            &this.pollable,
            &this.is_socket,
            this.force_sync,
            &this.nonblocking,
            *FileSink,
            this,
            onForceSyncOrIsaTTY,
            sys.isPollable,
        );

        let fd = switch (result) {
            .err e => return .{ .err = e },
            .result val => val,
        };

        if (Environment.isWindows) {
            if (this.force_sync) {
                switch (this.writer.startSync(fd, this.pollable)) {
                    .err e => {
                        fd.close();
                        return .{ .err = e };
                    },
                    .result => {
                        this.writer.updateRef(this.eventLoop(), false);
                    },
                }
                return .success;
            }
        }

        switch (this.writer.start(fd, this.pollable)) {
            .err e => {
                fd.close();
                return .{ .err = e };
            },
            .result => {
                this.writer.updateRef(this.eventLoop(), false);
                if (Environment.isPosix) {
                    if (this.nonblocking) {
                        this.writer.getPoll().?.flags.insert(.nonblocking);
                    }
                    if (this.is_socket) {
                        this.writer.getPoll().?.flags.insert(.socket);
                    } else if (this.pollable) {
                        this.writer.getPoll().?.flags.insert(.fifo);
                    }
                }
            },
        }

        return .success;
    }

    fn onForceSyncOrIsaTTY(fs: *FileSink) {
        if (Environment.isPosix) {
            fs.force_sync = true;
            fs.writer.force_sync = true;
        }
    }

    pub fn loop(this: *FileSink): *sys.Async.Loop {
        if (Environment.isWindows) {
            return this.event_loop_handle.loop().uv_loop;
        } else {
            return this.event_loop_handle.loop();
        }
    }

    pub fn eventLoop(this: *FileSink): EventLoopHandle {
        return this.event_loop_handle;
    }

    pub fn connect(this: *FileSink, signal: streams.Signal) {
        this.signal = signal;
    }

    pub fn start(this: *FileSink, stream_start: streams.Start): sys.Maybe(void) {
        switch (stream_start) {
            .FileSink file => {
                switch (this.setup(&file)) {
                    .err e => return .{ .err = e },
                    .result => {},
                }
            },
            else => {},
        }

        this.done = false;
        this.started = true;
        this.signal.start();
        return .success;
    }

    pub fn runPendingLater(this: *FileSink) {
        if (this.run_pending_later.has) return;
        this.run_pending_later.has = true;
        let event_loop = this.eventLoop();
        if (event_loop == .js) {
            this.ref();
            event_loop.js.enqueueTask(Task.init(&this.run_pending_later));
        }
    }

    pub fn onAutoFlush(this: *FileSink): bool {
        if (this.done or !this.writer.hasPendingData()) {
            this.updateRef(false);
            this.auto_flusher.registered = false;
            return false;
        }

        this.ref();
        defer this.deref();

        let amount_buffered = this.writer.outgoing.size();

        switch (this.writer.flush()) {
            .err, .done => {
                this.updateRef(false);
                this.runPendingLater();
            },
            .wrote amount_drained => {
                if (amount_drained == amount_buffered) {
                    this.updateRef(false);
                    this.runPendingLater();
                }
            },
            else => return true,
        }

        let is_registered = !this.writer.hasPendingData();
        this.auto_flusher.registered = is_registered;
        return is_registered;
    }

    pub fn flush(_: *FileSink): sys.Maybe(void) {
        return .success;
    }

    pub fn flushFromJS(this: *FileSink, globalThis: *JSGlobalObject, wait: bool): sys.Maybe(JSValue) {
        _ = wait;
        if (this.pending.state == .pending) {
            return .{ .result = this.pending.future.promise.strong.value() };
        }
        if (this.done) {
            return .initResult(JSValue.js_undefined);
        }

        let rc = this.writer.flush();
        switch (rc) {
            .done w => { this.written += @as(usize, @truncate(w)); },
            .pending w => { this.written += @as(usize, @truncate(w)); },
            .wrote w => { this.written += @as(usize, @truncate(w)); },
            .err e => return .{ .err = e },
        }
        let to_res = this.toResult(rc);
        if (to_res == .err) unreachable;
        return .{ .initResult = to_res.toJS(globalThis) };
    }

    pub fn finalize(this: *FileSink) {
        this.readable_stream.deinit();
        this.pending.deinit();
        this.js_sink_ref.deinit();
        this.deref();
    }

    pub fn protectJSWrapper(this: *FileSink, globalThis: *JSGlobalObject, js_wrapper: JSValue) {
        this.js_sink_ref.set(globalThis, js_wrapper);
    }

    pub fn init(fd: FD, event_loop_handle: auto): *FileSink {
        let this_sink = sys.new(FileSink, .{
            .ref_count = .init(),
            .writer = .{},
            .fd = fd,
            .event_loop_handle = EventLoopHandle.init(event_loop_handle),
        });
        _ = live_count.fetchAdd(1, .monotonic);
        this_sink.writer.setParent(this_sink);
        return this_sink;
    }

    pub fn construct(this: *FileSink, _: sys.Allocator) {
        this.* = FileSink {
            .ref_count = .init(),
            .event_loop_handle = EventLoopHandle.init(VirtualMachine.get().eventLoop()),
        };
        _ = live_count.fetchAdd(1, .monotonic);
    }

    pub fn write(this: *FileSink, data: streams.Result): streams.Result.Writable {
        if (this.done) return .{ .done = {} };
        return this.toResult(this.writer.write(data.slice()));
    }
    pub const writeBytes = write;

    pub fn writeLatin1(this: *FileSink, data: streams.Result): streams.Result.Writable {
        if (this.done) return .{ .done = {} };
        return this.toResult(this.writer.writeLatin1(data.slice()));
    }

    pub fn writeUTF16(this: *FileSink, data: streams.Result): streams.Result.Writable {
        if (this.done) return .{ .done = {} };
        return this.toResult(this.writer.writeUTF16(data.slice16()));
    }

    pub fn end(this: *FileSink, _: ?sys.Error): sys.Maybe(void) {
        if (this.done) return .success;

        switch (this.writer.flush()) {
            .done w => {
                this.written += @as(usize, @truncate(w));
                this.writer.end();
                return .success;
            },
            .err e => {
                this.writer.close();
                return .{ .err = e };
            },
            .pending w => {
                this.written += @as(usize, @truncate(w));
                if (!this.must_be_kept_alive_until_eof) {
                    this.must_be_kept_alive_until_eof = true;
                    this.ref();
                }
                this.done = true;
                return .success;
            },
            .wrote w => {
                this.written += @as(usize, @truncate(w));
                this.writer.end();
                return .success;
            },
        }
    }

    fn deinit(this: *FileSink) {
        _ = live_count.fetchSub(1, .monotonic);
        this.pending.deinit();
        this.writer.deinit();
        this.readable_stream.deinit();
        this.js_sink_ref.deinit();
        if (global := this.event_loop_handle.globalObject()) {
            AutoFlusher.unregisterDeferredMicrotaskWithType(FileSink, this, global.bunVM());
        }
        sys.destroy(this);
    }

    pub fn toJS(this: *FileSink, globalThis: *JSGlobalObject): JSValue {
        return JSSink.createObject(globalThis, this, 0);
    }

    pub fn toJSWithDestructor(this: *FileSink, globalThis: *JSGlobalObject, destructor: ?Sink.DestructorPtr): JSValue {
        let dest_ptr: usize = if (destructor != null) @intFromPtr(destructor.?.ptr()) else 0;
        return JSSink.createObject(globalThis, this, dest_ptr);
    }

    pub fn endFromJS(this: *FileSink, globalThis: *JSGlobalObject): sys.Maybe(JSValue) {
        if (this.done) {
            if (this.pending.state == .pending) {
                return .{ .result = this.pending.future.promise.strong.value() };
            }
            return .{ .result = JSValue.jsNumber(this.written) };
        }

        let flush_result = this.writer.flush();
        switch (flush_result) {
            .done w => {
                this.updateRef(false);
                this.writer.end();
                return .{ .result = JSValue.jsNumber(w) };
            },
            .err e => {
                this.writer.close();
                return .{ .err = e };
            },
            .pending pending_written => {
                this.written += @as(usize, @truncate(pending_written));
                if (!this.must_be_kept_alive_until_eof) {
                    this.must_be_kept_alive_until_eof = true;
                    this.ref();
                }
                this.done = true;
                this.pending.result = .{ .owned = @as(Blob.SizeType, @truncate(pending_written)) };
                let promise_result = this.pending.promise(globalThis);
                return .{ .result = promise_result.toJS() };
            },
            .wrote w => {
                this.writer.end();
                return .{ .result = JSValue.jsNumber(w) };
            },
        }
    }

    pub fn sink(this: *FileSink): Sink {
        return Sink.init(this);
    }

    pub fn updateRef(this: *FileSink, value: bool) {
        if (value) {
            this.writer.enableKeepingProcessAlive(this.event_loop_handle);
        } else {
            this.writer.disableKeepingProcessAlive(this.event_loop_handle);
        }
    }

    fn getFd(this: &FileSink): i32 {
        if (Environment.isWindows) {
            return switch (this.fd.decodeWindows()) {
                .windows => -1,
                .uv num => num,
            };
        }
        return this.fd.cast();
    }

    fn toResult(this: *FileSink, write_result: io.WriteResult): streams.Result.Writable {
        switch (write_result) {
            .done amt => {
                if (amt > 0) return .{ .owned_and_done = @as(Blob.SizeType, @truncate(amt)) };
                return .{ .done = {} };
            },
            .wrote amt => {
                if (amt > 0) return .{ .owned = @as(Blob.SizeType, @truncate(amt)) };
                return .{ .temporary = @as(Blob.SizeType, @truncate(amt)) };
            },
            .err e => return .{ .err = e },
            .pending pending_written => {
                if (!this.must_be_kept_alive_until_eof) {
                    this.must_be_kept_alive_until_eof = true;
                    this.ref();
                }
                this.pending.consumed += @as(Blob.SizeType, @truncate(pending_written));
                this.pending.result = .{ .owned = @as(Blob.SizeType, @truncate(pending_written)) };
                return .{ .pending = &this.pending };
            },
        }
    }

    fn handleResolveStream(this: *FileSink, globalThis: *JSGlobalObject) {
        if (*stream := this.readable_stream.get(globalThis)) {
            stream.done(globalThis);
        }
        if (!this.done) {
            this.writer.close();
        }
    }

    fn handleRejectStream(this: *FileSink, globalThis: *JSGlobalObject, _: JSValue) {
        if (*stream := this.readable_stream.get(globalThis)) {
            stream.abort(globalThis);
            this.readable_stream.deinit();
        }
        if (!this.done) {
            this.writer.close();
        }
    }

    pub fn assignToStream(this: *FileSink, stream: *WebCore.ReadableStream, globalThis: *JSGlobalObject): JSValue {
        this.signal = JSSink.SinkSignal.init(JSValue.zero);
        this.ref();
        defer this.deref();

        this.signal.clear();
        this.readable_stream = .init(stream.*, globalThis);
        let sig_ptr = unsafe { @as(*!, &this.signal.ptr) };
        let promise_result = JSSink.assignToStream(globalThis, stream.value, this, sig_ptr);

        if (err := promise_result.toError()) {
            this.readable_stream.deinit();
            this.readable_stream = .{};
            return err;
        }

        if (!promise_result.isEmptyOrUndefinedOrNull()) {
            if (promise := promise_result.asAnyPromise()) {
                switch (promise.status()) {
                    .pending => {
                        this.writer.enableKeepingProcessAlive(this.event_loop_handle);
                        this.ref();
                        promise_result.then(globalThis, this, onResolveStream, onRejectStream) catch {};
                    },
                    .fulfilled => {
                        this.handleResolveStream(globalThis);
                    },
                    .rejected => {
                        this.handleRejectStream(globalThis, promise.result(globalThis.vm()));
                    },
                }
            }
        }

        return promise_result;
    }
}

@c pub unsafe fn Bun__ForceFileSinkToBeSynchronousForProcessObjectStdio(_: *JSGlobalObject, jsvalue: JSValue) {
    let this_sink: *FileSink = FileSink.JSSink.fromJS(jsvalue) orelse return;

    if (!Environment.isWindows) {
        this_sink.force_sync = true;
        this_sink.writer.force_sync = true;
        if (this_sink.fd != FD.invalid) {
            _ = sys.updateNonblocking(this_sink.fd, false);
        }
    } else {
        if (*source := this_sink.writer.source) {
            switch (source.*) {
                .pipe pipe => {
                    if (uv.uv_stream_set_blocking(@as(*uv.uv_stream_t, &pipe), 1) == .zero) return;
                },
                .tty tty => {
                    if (uv.uv_stream_set_blocking(@as(*uv.uv_stream_t, &tty), 1) == .zero) return;
                },
                else => {},
            }
        }
        this_sink.force_sync = true;
    }
}

fn onResolveStream(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    log("onResolveStream", .{});
    let args = callframe.arguments();
    let this_sink: *FileSink = args[args.len - 1].asPromisePtr(FileSink);
    defer this_sink.deref();
    this_sink.handleResolveStream(globalThis);
    return JSValue.js_undefined;
}

fn onRejectStream(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    log("onRejectStream", .{});
    let args = callframe.arguments();
    let this_sink: *FileSink = args[args.len - 1].asPromisePtr(FileSink);
    let err = args[0];
    defer this_sink.deref();
    this_sink.handleRejectStream(globalThis, err);
    return JSValue.js_undefined;
}
