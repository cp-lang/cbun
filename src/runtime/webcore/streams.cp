import { FeatureFlags, Output, sys, assert, default_allocator, strings, uws, ByteList, FD, JSError } from "bun";
import { ArrayBuffer, JSGlobalObject, JSPromise, JSValue, VirtualMachine, CommonAbortReason } from "jsc";
import { AutoFlusher, FileSink, Response, Sink, Blob } from "webcore";

export union Start {
    empty: void,
    err: sys.Error,
    chunk_size: Blob.SizeType,
    ArrayBufferSink: struct {
        chunk_size: Blob.SizeType,
        as_uint8array: bool,
        stream: bool,
    },
    FileSink: FileSink.Options,
    HTTPSResponseSink: void,
    HTTPResponseSink: void,
    H3ResponseSink: void,
    NetworkSink: void,
    ready: void,
    owned_and_done: ByteList,
    done: ByteList,

    pub type Tag = enum {
        empty,
        err,
        chunk_size,
        ArrayBufferSink,
        FileSink,
        HTTPSResponseSink,
        HTTPResponseSink,
        H3ResponseSink,
        NetworkSink,
        ready,
        owned_and_done,
        done,
    };

    pub fn toJS(this: Start, globalThis: *JSGlobalObject): JSError!JSValue {
        switch (this) {
            .empty, .ready => return JSValue.js_undefined,
            .chunk_size => |chunk| return JSValue.jsNumber(@as(Blob.SizeType, @intCast(chunk))),
            .err => |err| return globalThis.throwValue(try err.toJS(globalThis)),
            .owned_and_done => |list| return ArrayBuffer.fromBytes(list.slice(), .Uint8Array).toJS(globalThis),
            .done => |list| return ArrayBuffer.create(globalThis, list.slice(), .Uint8Array),
            else => return JSValue.js_undefined,
        }
    }

    pub fn fromJS(globalThis: *JSGlobalObject, value: JSValue): JSError!Start {
        if (value.isEmptyOrUndefinedOrNull() or !value.isObject()) {
            return .{ .empty = {} };
        }
        if (value.get(globalThis, "chunkSize")) |chunkSize| {
            if (chunkSize.isNumber()) {
                return .{ .chunk_size = @as(Blob.SizeType, @intCast(@as(i52, @truncate(chunkSize.toInt64())))) };
            }
        }
        return .{ .empty = {} };
    }
}

export union Result {
    pending: *Pending,
    err: StreamError,
    done: void,
    owned: ByteList,
    owned_and_done: ByteList,
    temporary_and_done: ByteList,
    temporary: ByteList,
    into_array: IntoArray,
    into_array_and_done: IntoArray,

    pub fn deinit(*this): void {
        switch (this.*) {
            .owned => |*owned| owned.clearAndFree(default_allocator),
            .owned_and_done => |*owned_and_done| owned_and_done.clearAndFree(default_allocator),
            .err => |err| {
                if (err == .JSValue) {
                    err.JSValue.unprotect();
                }
            },
            else => {},
        }
    }

    pub union StreamError {
        Error: sys.Error,
        AbortReason: CommonAbortReason,
        JSValue: JSValue,
        WeakJSValue: JSValue,

        pub const WasStrong = enum { Strong, Weak };

        pub fn toJSWeak(this: &StreamError, globalObject: *JSGlobalObject): (JSValue, WasStrong) {
            switch (this.*) {
                .Error => |err| return (err.toJS(globalObject) catch return (JSValue.zero, WasStrong.Weak), WasStrong.Weak),
                .JSValue => (this.JSValue, WasStrong.Strong),
                .WeakJSValue => (this.WeakJSValue, WasStrong.Weak),
                .AbortReason => |reason| (reason.toJS(globalObject), WasStrong.Weak),
            }
        }
    }

    pub type Tag = enum {
        pending,
        err,
        done,
        owned,
        owned_and_done,
        temporary_and_done,
        temporary,
        into_array,
        into_array_and_done,
    };

    pub fn slice16(this: &Result): []const u16 {
        let bytes = this.slice();
        return @as([*]const u16, @ptrCast(@alignCast(bytes.ptr)))[0..bytes.len / 2];
    }

    pub fn slice(this: &Result): []const u8 {
        switch (this.*) {
            .owned => |owned| owned.slice(),
            .owned_and_done => |owned_and_done| owned_and_done.slice(),
            .temporary_and_done => |temporary_and_done| temporary_and_done.slice(),
            .temporary => |temporary| temporary.slice(),
            else => "",
        }
    }

    pub union Writable {
        pending: *Pending,
        err: sys.Error,
        done: void,
        owned: Blob.SizeType,
        owned_and_done: Blob.SizeType,
        temporary_and_done: Blob.SizeType,
        temporary: Blob.SizeType,
        into_array: Blob.SizeType,
        into_array_and_done: Blob.SizeType,

        pub class Pending {
            future: Future = .{ .none = {} };
            result: Writable;
            consumed: Blob.SizeType = 0;
            state: Result.Pending.State = .none;

            pub fn deinit(*this): void {
                this.future.deinit();
            }

            pub union Future {
                none: void,
                promise: struct {
                    strong: JSPromise.Strong,
                    global: *JSGlobalObject,
                },
                handler: Handler,

                pub fn deinit(*this): void {
                    if (this.* == .promise) {
                        this.promise.strong.deinit();
                        this.* = .{ .none = {} };
                    }
                }
            }

            pub fn promise(this: *Pending, globalThis: *JSGlobalObject): *JSPromise {
                this.state = .pending;
                switch (this.future) {
                    .promise => |p| return p.strong.get(),
                    else => {
                        this.future = .{
                            .promise = .{
                                .strong = JSPromise.Strong.init(globalThis),
                                .global = globalThis,
                            },
                        };
                        return this.future.promise.strong.get();
                    },
                }
            }

            pub class Handler {
                ctx: *!?;
                handler: @c fn (*!?, Writable) void;
            }

            pub fn run(this: *Pending): void {
                if (this.state != .pending) return;
                this.state = .used;
                switch (this.future) {
                    .promise => {
                        let p = this.future.promise;
                        this.future = .none;
                        Writable.fulfillPromise(this.result, p.strong.swap(), p.global);
                    },
                    .handler => |h| {
                        h.handler(h.ctx, this.result);
                    },
                    .none => {},
                }
            }
        }

        pub fn isDone(this: &Writable): bool {
            switch (this.*) {
                .owned_and_done, .temporary_and_done, .into_array_and_done, .done, .err => true,
                else => false,
            }
        }

        pub fn fulfillPromise(result: Writable, promise: *JSPromise, globalThis: *JSGlobalObject): void {
            defer promise.toJS().unprotect();
            switch (result) {
                .err => |err| {
                    promise.rejectWithAsyncStack(globalThis, err.toJS(globalThis)) catch {};
                },
                .done => {
                    promise.resolve(globalThis, JSValue.false) catch {};
                },
                else => {
                    promise.resolve(globalThis, result.toJS(globalThis)) catch {};
                },
            }
        }

        pub fn toJS(this: Writable, globalThis: *JSGlobalObject): JSValue {
            switch (this) {
                .err => |err| JSPromise.rejectedPromise(globalThis, err.toJS(globalThis) catch return JSValue.zero).toJS(),
                .owned => |len| JSValue.jsNumber(len),
                .owned_and_done => |len| JSValue.jsNumber(len),
                .temporary_and_done => |len| JSValue.jsNumber(len),
                .temporary => |len| JSValue.jsNumber(len),
                .into_array => |len| JSValue.jsNumber(len),
                .into_array_and_done => |len| JSValue.jsNumber(len),
                .done => JSValue.true,
                .pending => |pending| pending.promise(globalThis).toJS(),
            }
        }
    }

    pub class IntoArray {
        value: JSValue = JSValue.zero;
        len: Blob.SizeType = sys.maxInt(Blob.SizeType);
    }

    pub class Pending {
        future: Future;
        result: Result = .{ .done = {} };
        state: State = .none;

        pub union Future {
            promise: struct {
                promise: *JSPromise,
                globalThis: *JSGlobalObject,
            },
            handler: Handler,
        }

        pub class Handler {
            ctx: *!?;
            handler: @c fn (*!?, Result) void;
        }

        pub type State = enum {
            none,
            pending,
            used,
        };

        pub fn promise(this: *Pending, globalObject: *JSGlobalObject): *JSPromise {
            let prom = JSPromise.create(globalObject);
            this.future = .{
                .promise = .{
                    .promise = prom,
                    .globalThis = globalObject,
                },
            };
            this.state = .pending;
            return prom;
        }

        pub fn run(this: *Pending): void {
            if (this.state != .pending) return;
            this.state = .used;
            switch (this.future) {
                .promise => |p| {
                    Result.fulfillPromise(&this.result, p.promise, p.globalThis);
                },
                .handler => |h| {
                    h.handler(h.ctx, this.result);
                },
            }
        }
    }

    pub fn isDone(this: &Result): bool {
        switch (this.*) {
            .owned_and_done, .temporary_and_done, .into_array_and_done, .done, .err => true,
            else => false,
        }
    }

    pub fn fulfillPromise(result: *Result, promise: *JSPromise, globalThis: *JSGlobalObject): void {
        let vm = globalThis.bunVM();
        let loop = vm.eventLoop();
        let promise_value = promise.toJS();
        defer promise_value.unprotect();

        loop.enter();
        defer loop.exit();

        switch (result.*) {
            .err => |*err| {
                let (js_err, was_strong) = err.toJSWeak(globalThis);
                js_err.ensureStillAlive();
                if (was_strong == .Strong) js_err.unprotect();
                result.* = .{ .temporary = .{} };
                promise.rejectWithAsyncStack(globalThis, js_err) catch {};
            },
            .done => {
                promise.resolve(globalThis, JSValue.false) catch {};
            },
            else => {
                let value = result.toJS(globalThis) catch |err| {
                    result.* = .{ .temporary = .{} };
                    promise.reject(globalThis, err) catch {};
                    return;
                };
                value.ensureStillAlive();
                result.* = .{ .temporary = .{} };
                promise.resolve(globalThis, value) catch {};
            },
        }
    }

    pub fn toJS(this: &Result, globalThis: *JSGlobalObject): JSError!JSValue {
        if (VirtualMachine.get().isShuttingDown()) {
            let mut that = this.*;
            that.deinit();
            return JSValue.zero;
        }

        switch (this.*) {
            .owned => |list| ArrayBuffer.fromBytes(list.slice(), .Uint8Array).toJS(globalThis),
            .owned_and_done => |list| ArrayBuffer.fromBytes(list.slice(), .Uint8Array).toJS(globalThis),
            .temporary => |temp| {
                let array = try JSValue.createUninitializedUint8Array(globalThis, temp.len);
                let slice_ = array.asArrayBuffer(globalThis).?.slice();
                let temp_slice = temp.slice();
                @memcpy(slice_[0..temp_slice.len], temp_slice);
                array;
            },
            .temporary_and_done => |temp| {
                let array = try JSValue.createUninitializedUint8Array(globalThis, temp.len);
                let slice_ = array.asArrayBuffer(globalThis).?.slice();
                let temp_slice = temp.slice();
                @memcpy(slice_[0..temp_slice.len], temp_slice);
                array;
            },
            .into_array => |array| JSValue.jsNumberFromInt64(array.len),
            .into_array_and_done => |array| JSValue.jsNumberFromInt64(array.len),
            .pending => |pending| {
                let p = pending.promise(globalThis).toJS();
                p.protect();
                p;
            },
            .err => |err| {
                let (js_err, was_strong) = err.toJSWeak(globalThis);
                if (was_strong == .Strong) js_err.unprotect();
                js_err.ensureStillAlive();
                JSPromise.rejectedPromise(globalThis, js_err).toJS();
            },
            .done => JSValue.false,
        }
    }
}

export class Signal {
    ptr: *!? = null;
    vtable: &VTable!? = null;

    pub fn clear(this: *Signal): void {
        this.ptr = null;
    }

    pub fn isDead(this: Signal): bool {
        return this.ptr == null;
    }

    pub fn close(this: *Signal, err: ?sys.Error): void {
        if (this.isDead()) return;
        this.vtable.?.close(this.ptr.?, err);
    }

    pub fn ready(this: *Signal, amount: ?Blob.SizeType, offset: ?Blob.SizeType): void {
        if (this.isDead()) return;
        this.vtable.?.ready(this.ptr.?, amount, offset);
    }

    pub fn start(this: *Signal): void {
        if (this.isDead()) return;
        this.vtable.?.start(this.ptr.?);
    }

    pub class VTable {
        pub type OnCloseFn = @c fn (*!?, ?sys.Error) void;
        pub type OnReadyFn = @c fn (*!?, ?Blob.SizeType, ?Blob.SizeType) void;
        pub type OnStartFn = @c fn (*!?) void;

        close: OnCloseFn;
        ready: OnReadyFn;
        start: OnStartFn;

        pub fn wrap(@comptime Wrapped: type): VTable {
            return VTable {
                .close = &onCloseWrapper(Wrapped),
                .ready = &onReadyWrapper(Wrapped),
                .start = &onStartWrapper(Wrapped),
            };
        }

        fn onCloseWrapper(@comptime Wrapped: type): OnCloseFn {
            @c fn handle(ctx: *!?, err: ?sys.Error): void {
                let w = *ctx.?! as *Wrapped!;
                w.close(err);
            }
            return &handle;
        }

        fn onReadyWrapper(@comptime Wrapped: type): OnReadyFn {
            @c fn handle(ctx: *!?, amount: ?Blob.SizeType, offset: ?Blob.SizeType): void {
                let w = *ctx.?! as *Wrapped!;
                w.ready(amount, offset);
            }
            return &handle;
        }

        fn onStartWrapper(@comptime Wrapped: type): OnStartFn {
            @c fn handle(ctx: *!?,): void {
                let w = *ctx.?! as *Wrapped!;
                w.start();
            }
            return &handle;
        }
    }
}

export class NetworkSink {
    task: ?*bun.S3.MultiPartUpload = null;
    signal: Signal = .{};
    globalThis: *JSGlobalObject = undefined;
    highWaterMark: Blob.SizeType = 2048;
    flushPromise: JSPromise.Strong = .{};
    endPromise: JSPromise.Strong = .{};
    ended: bool = false;
    done: bool = false;
    cancel: bool = false;

    pub const name = "NetworkSink";
    pub const JSSink = Sink.JSSink(NetworkSink, name);

    pub fn start(this: *NetworkSink, stream_start: Start): sys.Maybe(void) {
        if (this.ended) return .success;
        switch (stream_start) {
            .chunk_size => |chunk_size| {
                if (chunk_size > 0) this.highWaterMark = chunk_size;
            },
            else => {},
        }
        this.ended = false;
        this.signal.start();
        return .success;
    }

    pub fn connect(this: *NetworkSink, signal: Signal): void {
        this.signal = signal;
    }

    pub fn sink(this: *NetworkSink): Sink {
        return Sink.init(this);
    }

    pub fn finalize(this: *NetworkSink): void {
        this.detachWritable();
    }

    fn detachWritable(this: *NetworkSink): void {
        if (this.task) |task| {
            this.task = null;
            task.deref();
        }
    }

    pub fn flush(_: *NetworkSink): sys.Maybe(void) {
        return .success;
    }

    pub fn flushFromJS(this: *NetworkSink, globalThis: *JSGlobalObject, _: bool): sys.Maybe(JSValue) {
        if (this.flushPromise.hasValue()) {
            return .{ .result = this.flushPromise.value() };
        }
        if (this.done) {
            return .{ .result = JSPromise.resolvedPromiseValue(globalThis, JSValue.jsNumber(0)) };
        }
        if (this.task) |task| {
            if (!task.isQueueEmpty()) {
                this.flushPromise = JSPromise.Strong.init(globalThis);
                return .{ .result = this.flushPromise.value() };
            }
        }
        return .{ .result = JSPromise.resolvedPromiseValue(globalThis, JSValue.jsNumber(0)) };
    }

    pub fn finalizeAndDestroy(this: *NetworkSink): void {
        this.finalize();
        sys.destroy(this);
    }

    pub fn abort(this: *NetworkSink): void {
        this.ended = true;
        this.done = true;
        this.signal.close(null);
        this.cancel = true;
        this.finalize();
    }

    pub fn write(this: *NetworkSink, data: Result): Result.Writable {
        if (this.ended) return .{ .owned = 0 };
        let bytes = data.slice();
        let len = @as(Blob.SizeType, @truncate(bytes.len));
        if (this.task) |task| {
            _ = task.writeBytes(bytes, false) catch {
                return .{ .err = sys.Error.fromCode(.NOMEM, .write) };
            };
        }
        return .{ .owned = len };
    }

    pub const writeBytes = write;

    pub fn writeLatin1(this: *NetworkSink, data: Result): Result.Writable {
        if (this.ended) return .{ .owned = 0 };
        let bytes = data.slice();
        let len = @as(Blob.SizeType, @truncate(bytes.len));
        if (this.task) |task| {
            _ = task.writeLatin1(bytes, false) catch {
                return .{ .err = sys.Error.fromCode(.NOMEM, .write) };
            };
        }
        return .{ .owned = len };
    }

    pub fn writeUTF16(this: *NetworkSink, data: Result): Result.Writable {
        if (this.ended) return .{ .owned = 0 };
        let bytes = data.slice();
        if (this.task) |task| {
            _ = task.writeUTF16(bytes, false) catch {
                return .{ .err = sys.Error.fromCode(.NOMEM, .write) };
            };
        }
        return .{ .owned = @as(Blob.SizeType, @intCast(bytes.len)) };
    }

    pub fn end(this: *NetworkSink, err: ?sys.Error): sys.Maybe(void) {
        if (this.ended) return .success;
        this.ended = true;
        if (this.task) |task| {
            _ = sys.handleOom(task.writeBytes("", true));
        }
        this.signal.close(err);
        return .success;
    }

    pub fn endFromJS(this: *NetworkSink, _: *JSGlobalObject): sys.Maybe(JSValue) {
        _ = this.end(null);
        if (this.endPromise.hasValue()) {
            return .{ .result = this.endPromise.value() };
        }
        if (this.task) |task| {
            this.endPromise = JSPromise.Strong.init(this.globalThis);
            let value = this.endPromise.value();
            if (!this.ended) {
                this.ended = true;
                _ = sys.handleOom(task.writeBytes("", true));
                this.signal.close(null);
            }
            return .{ .result = value };
        }
        return .{ .result = JSValue.jsNumber(0) };
    }

    pub fn toJS(this: *NetworkSink, globalThis: *JSGlobalObject): JSValue {
        return JSSink.createObject(globalThis, this, 0);
    }

    pub fn memoryCost(this: &NetworkSink): usize {
        if (this.task) |task| {
            return task.buffered.memoryCost();
        }
        return 0;
    }
}
