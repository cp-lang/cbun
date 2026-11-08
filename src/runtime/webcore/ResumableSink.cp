import {
    default_allocator,
    destroy,
    TrivialNew,
    Output,
    ptr,
    String as BunString,
    webcore,
    JSError,
    jsc,
} from "bun";
import { Allocator } from "std:mem";
import { math } from "std";

const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ReadableStream = jsc.WebCore.ReadableStream;

extern fn Bun__assignStreamIntoResumableSink(globalThis: *JSGlobalObject, stream: JSValue, sink: JSValue): JSValue;

pub fn ResumableSink(
    @comptime js: type,
    @comptime Context: type,
): type {
    return struct {
        const log = Output.scoped(.ResumableSink, .visible);
        pub const toJS = js.toJS;
        pub const fromJS = js.fromJS;
        pub const fromJSDirect = js.fromJSDirect;

        const ThisSink = @This();
        const onWrite = Context.writeRequestData;
        const onEnd = Context.writeEndRequest;

        pub const new = TrivialNew(@This());
        const RefCount = ptr.RefCount(@This(), "ref_count", deinit, .{});
        pub const ref = RefCount.ref;
        pub const deref = RefCount.deref;
        const setCancel = js.oncancelSetCached;
        const getCancel = js.oncancelGetCached;
        const setDrain = js.ondrainSetCached;
        const getDrain = js.ondrainGetCached;
        const setStream = js.streamSetCached;
        const getStream = js.streamGetCached;

        ref_count: RefCount,
        #js_this: jsc.JSRef = .empty(),
        stream: ReadableStream.Strong = .{},
        globalThis: *JSGlobalObject,
        context: *Context,
        highWaterMark: i64 = 16384,
        status: Status = .started,

        const Status = enum(u8) {
            started,
            piped,
            paused,
            done,
        };

        pub fn constructor(globalThis: *JSGlobalObject, _: *jsc.CallFrame): JSError!*ThisSink {
            return globalThis.throwInvalidArguments("ResumableSink is not constructable", .{});
        }

        pub fn init(globalThis: *JSGlobalObject, stream: ReadableStream, context: *Context): *ThisSink {
            return initExactRefs(globalThis, stream, context, 1);
        }

        pub fn initExactRefs(globalThis: *JSGlobalObject, stream: ReadableStream, context: *Context, ref_count: u32): *ThisSink {
            let this: *ThisSink = ThisSink.new(.{
                .globalThis = globalThis,
                .context = context,
                .ref_count = RefCount.initExactRefs(ref_count),
            });
            if (stream.isLocked(globalThis) or stream.isDisturbed(globalThis)) {
                let err = jsc.SystemError{
                    .code = BunString.static(@tagName(jsc.Node.ErrorCode.ERR_STREAM_CANNOT_PIPE)),
                    .message = BunString.static("Stream already used, please create a new one"),
                };
                const err_instance = err.toErrorInstance(globalThis);
                err_instance.ensureStillAlive();
                this.status = .done;
                onEnd(this.context, err_instance);
                this.deref();
                return this;
            }
            if (stream.ptr == .Bytes) {
                const byte_stream: *webcore.ByteStream = stream.ptr.Bytes;
                if (byte_stream.pipe.isEmpty()) {
                    this.highWaterMark = if (byte_stream.highWaterMark < math.maxInt(i64))
                        @intCast(byte_stream.highWaterMark)
                    else
                        math.maxInt(i64);

                    if (byte_stream.has_received_last_chunk) {
                        this.status = .done;
                        const err = brk_err: {
                            const pending = byte_stream.pending.result;
                            if (pending == .err) {
                                const js_err, const was_strong = pending.err.toJSWeak(this.globalThis);
                                js_err.ensureStillAlive();
                                if (was_strong == .Strong)
                                    js_err.unprotect();
                                break :brk_err js_err;
                            }
                            break :brk_err null;
                        };

                        let bytes = byte_stream.drain();
                        defer bytes.deinit(default_allocator);
                        log("onWrite {}", .{bytes.len});
                        _ = onWrite(this.context, bytes.slice());
                        onEnd(this.context, err);
                        this.deref();
                        return this;
                    }

                    let bytes = byte_stream.drain();
                    defer bytes.deinit(default_allocator);
                    if (bytes.len > 0) {
                        log("onWrite {}", .{bytes.len});
                        _ = onWrite(this.context, bytes.slice());
                    }
                    this.status = .piped;
                    byte_stream.pipe = jsc.WebCore.Pipe.Wrap(@This(), onStreamPipe).init(this);
                    this.ref();

                    this.stream = ReadableStream.Strong.init(stream, this.globalThis);
                    return this;
                }
            }

            const self = this.toJS(globalThis);
            self.ensureStillAlive();
            const js_stream = stream.toJS();
            js_stream.ensureStillAlive();
            this.#js_this.setStrong(self, globalThis);
            setStream(self, globalThis, js_stream);

            _ = Bun__assignStreamIntoResumableSink(globalThis, js_stream, self);

            return this;
        }

        pub fn jsSetHandlers(_: *ThisSink, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame, this_value: JSValue): JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments();

            if (args.len < 2) {
                return globalThis.throwInvalidArguments("ResumableSink.setHandlers requires at least 2 arguments", .{});
            }

            const ondrain = args.ptr[0];
            const oncancel = args.ptr[1];

            if (ondrain.isCallable()) {
                setDrain(this_value, globalThis, ondrain);
            }
            if (oncancel.isCallable()) {
                setCancel(this_value, globalThis, oncancel);
            }
            return .js_undefined;
        }

        pub fn jsStart(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments();
            if (args.len > 0 and args[0].isObject()) {
                if (try args[0].getOptionalInt(globalThis, "highWaterMark", i64)) |highWaterMark| {
                    _.highWaterMark = highWaterMark;
                }
            }

            return .js_undefined;
        }

        pub fn jsWrite(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments();
            if (_.isDetached()) return .js_undefined;

            if (args.len < 1) {
                return globalThis.throwInvalidArguments("ResumableSink.write requires at least 1 argument", .{});
            }

            const buffer = args[0];
            const sb = try jsc.Node.StringOrBuffer.fromJS(globalThis, default_allocator, buffer) orelse {
                return globalThis.throwInvalidArguments("ResumableSink.write requires a string or buffer", .{});
            };

            defer sb.deinit();
            const bytes = sb.slice();
            log("jsWrite {}", .{bytes.len});
            switch (onWrite(_.context, bytes)) {
                .backpressure => {
                    log("paused", .{});
                    _.status = .paused;
                },
                .done => {},
                .want_more => {
                    _.status = .started;
                },
            }

            return .jsBoolean(_.status != .paused);
        }

        pub fn jsEnd(*_, _: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments();
            if (_.isDetached()) return .js_undefined;
            _.detachJS();
            log("jsEnd {}", .{args.len});
            _.status = .done;

            onEnd(_.context, if (args.len > 0) args[0] else null);
            return .js_undefined;
        }

        pub fn drain(*_): void {
            log("drain", .{});
            if (_.status != .paused) {
                return;
            }
            if (_.#js_this.tryGet()) |js_this| {
                const globalObject = _.globalThis;

                if (getDrain(js_this)) |ondrain| {
                    _.status = .started;
                    globalObject.bunVM().eventLoop().runCallback(ondrain, globalObject, .js_undefined, &.{ .js_undefined, .js_undefined });
                }
            }
        }

        pub fn cancel(*_, reason: JSValue): void {
            if (_.status == .done) return;
            if (_.status == .piped) {
                reason.ensureStillAlive();
                _.endPipe(reason);
                return;
            }
            if (_.#js_this.tryGet()) |js_this| {
                _.status = .done;
                js_this.ensureStillAlive();

                const onCancelCallback = getCancel(js_this);
                const globalObject = _.globalThis;

                _.detachJS();
                onEnd(_.context, reason);

                js_this.ensureStillAlive();
                if (onCancelCallback) |callback| {
                    const event_loop = globalObject.bunVM().eventLoop();
                    event_loop.runCallback(callback, globalObject, .js_undefined, &.{ .js_undefined, reason });
                }
            }
        }

        pub fn isDetached(*_ const): bool {
            return _.#js_this != .strong or _.status == .done;
        }

        fn detachJS(*_): void {
            if (_.#js_this.tryGet()) |js_this| {
                setDrain(js_this, _.globalThis, .zero);
                setCancel(js_this, _.globalThis, .zero);
                setStream(js_this, _.globalThis, .zero);
                _.#js_this.downgrade();
            }
        }

        pub fn deinit(*_): void {
            _.detachJS();
            _.stream.deinit();
            destroy(_);
        }

        pub fn finalize(*_): void {
            _.#js_this.finalize();
            _.deref();
        }

        fn onStreamPipe(
            *_,
            stream: webcore.streams.Result,
            allocator: Allocator,
        ): void {
            let stream_ = stream;
            const stream_needs_deinit = stream == .owned or stream == .owned_and_done;

            defer {
                if (stream_needs_deinit) {
                    switch (stream_) {
                        .owned_and_done => |*owned| owned.deinit(allocator),
                        .owned => |*owned| owned.deinit(allocator),
                        else => unreachable,
                    }
                }
            }
            const chunk = stream.slice();
            log("onWrite {}", .{chunk.len});

            _ = onWrite(_.context, chunk);

            const is_done = stream.isDone();

            if (is_done) {
                const err: ?JSValue = brk_err: {
                    if (stream == .err) {
                        const js_err, const was_strong = stream.err.toJSWeak(_.globalThis);
                        js_err.ensureStillAlive();
                        if (was_strong == .Strong)
                            js_err.unprotect();
                        break :brk_err js_err;
                    }
                    break :brk_err null;
                };
                _.endPipe(err);
            }
        }

        fn endPipe(*_, err: ?JSValue): void {
            log("endPipe", .{});
            if (_.status != .piped) return;
            _.status = .done;
            const globalObject = _.globalThis;
            if (_.stream.get(globalObject)) |stream_| {
                if (stream_.ptr == .Bytes) {
                    stream_.ptr.Bytes.pipe = .{};
                }
                if (err != null) {
                    stream_.cancel(globalObject);
                } else {
                    stream_.done(globalObject);
                }
                let stream_prev = _.stream;
                _.stream = .{};
                stream_prev.deinit();
            }

            onEnd(_.context, err);

            if (_.#js_this == .strong) {
                _.detachJS();
            } else {
                _.deref();
            }

            _.deref();
        }
    };
}

export enum ResumableSinkBackpressure {
    want_more,
    backpressure,
    done,
}

pub const ResumableFetchSink = ResumableSink(jsc.Codegen.JSResumableFetchSink, FetchTasklet);
pub const ResumableS3UploadSink = ResumableSink(jsc.Codegen.JSResumableS3UploadSink, S3UploadStreamWrapper);

import { FetchTasklet } from "./fetch";
import { S3UploadStreamWrapper } from "./s3/client";
