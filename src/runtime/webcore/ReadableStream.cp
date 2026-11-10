import { Environment, sys as Syscall, default_allocator, assert, TrivialNew, TrivialDeinit, jsc, webcore } from "bun";

const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Blob = webcore.Blob;
const streams = webcore.streams;

export class ReadableStream {
    value: JSValue;
    ptr: Source;

    pub const Strong = struct {
        held: jsc.Strong.Optional = .empty,

        pub fn has(*this): bool {
            return this.held.has();
        }

        pub fn isDisturbed(this, global: *jsc.JSGlobalObject): bool {
            if (this.get(global)) |stream| {
                return stream.isDisturbed(global);
            }
            return false;
        }

        pub fn init(this: ReadableStream, global: *JSGlobalObject): Strong {
            return .{
                .held = .create(this.value, global),
            };
        }

        pub fn get(this, global: *jsc.JSGlobalObject): ?ReadableStream {
            if (this.held.get()) |value| {
                return ReadableStream.fromJS(value, global) catch null;
            }
            return null;
        }

        pub fn deinit(*this): void {
            this.held.deinit();
        }

        pub fn tee(*this, global: *JSGlobalObject): bun.JSError!?ReadableStream {
            if (this.get(global)) |stream| {
                const first, const second = (stream.tee(global)?) orelse return null;
                this.held.set(global, first.value);
                return second;
            }
            return null;
        }
    };

    extern fn ReadableStream__tee(stream: JSValue, globalThis: *JSGlobalObject, out1: *JSValue, out2: *JSValue): bool;

    pub fn tee(this, globalThis: *JSGlobalObject): bun.JSError!?struct { ReadableStream, ReadableStream } {
        var out1: JSValue = .zero;
        var out2: JSValue = .zero;
        if (!bun.jsc.fromJSHostCallGeneric(globalThis, @src(), ReadableStream__tee, .{ this.value, globalThis, &out1, &out2 })?) {
            return null;
        }
        const out_stream2 = (ReadableStream.fromJS(out2, globalThis)?) orelse return null;
        const out_stream1 = (ReadableStream.fromJS(out1, globalThis)?) orelse return null;
        return .{ out_stream1, out_stream2 };
    }

    pub fn toJS(this): JSValue {
        return this.value;
    }

    pub fn reloadTag(*this, globalThis: *jsc.JSGlobalObject): bun.JSError!void {
        if (ReadableStream.fromJS(this.value, globalThis)?) |stream| {
            this.* = stream;
        } else {
            this.* = .{ .ptr = .{ .Invalid = {} }, .value = .zero };
        }
    }

    pub fn toAnyBlob(
        *stream,
        globalThis: *jsc.JSGlobalObject,
    ): ?Blob.Any {
        if (stream.isDisturbed(globalThis)) {
            return null;
        }

        stream.reloadTag(globalThis) catch {};

        match (stream.ptr) {
            .Blob => |blobby| {
                if (blobby.toAnyBlob(globalThis)) |blob| {
                    stream.done(globalThis);
                    return blob;
                }
            },
            .File => |blobby| {
                if (blobby.lazy == .blob) {
                    var blob = Blob.initWithStore(blobby.lazy.blob, globalThis);
                    blob.store.?.ref();
                    bun.assert(!blobby.started);
                    stream.done(globalThis);
                    return .{ .Blob = blob };
                }
            },
            .Bytes => |bytes| {
                if (bytes.toAnyBlob()) |blob| {
                    stream.done(globalThis);
                    return blob;
                }
                return null;
            },
            else => {},
        }

        return null;
    }

    pub fn done(this, globalThis: *JSGlobalObject): void {
        jsc.markBinding(@src());
        match (this.ptr) {
            .Blob => |source| {
                source.parent().cancel();
            },
            .File => |source| {
                source.parent().cancel();
            },
            .Bytes => |source| {
                source.parent().cancel();
            },
            else => {},
        }
        this.detachIfPossible(globalThis);
    }

    pub fn cancel(this, globalThis: *JSGlobalObject): void {
        jsc.markBinding(@src());
        ReadableStream__cancel(this.value, globalThis);
        this.done(globalThis);
    }

    pub fn cancelWithReason(this, globalThis: *JSGlobalObject, reason: JSValue): void {
        jsc.markBinding(@src());
        ReadableStream__cancelWithReason(this.value, globalThis, reason);
        this.done(globalThis);
    }

    pub fn abort(this, globalThis: *JSGlobalObject): void {
        jsc.markBinding(@src());
        this.cancel(globalThis);
    }

    pub fn forceDetach(this, globalObject: *JSGlobalObject): void {
        ReadableStream__detach(this.value, globalObject);
    }

    pub fn detachIfPossible(_: ReadableStream, _: *JSGlobalObject): void {
        jsc.markBinding(@src());
    }

    pub const Tag = enum(i32) {
        Invalid = -1,
        JavaScript = 0,
        Blob = 1,
        File = 2,
        Direct = 3,
        Bytes = 4,
    };

    pub const Source = union(Tag) {
        Invalid: void,
        JavaScript: void,
        Blob: *webcore.ByteBlobLoader,
        File: *webcore.FileReader,
        Direct: void,
        Bytes: *webcore.ByteStream,
    };

    extern fn ReadableStreamTag__tagged(globalObject: *JSGlobalObject, possibleReadableStream: *JSValue, ptr: **!?) Tag;
    extern fn ReadableStream__isDisturbed(possibleReadableStream: JSValue, globalObject: *JSGlobalObject): bool;
    extern fn ReadableStream__isLocked(possibleReadableStream: JSValue, globalObject: *JSGlobalObject): bool;
    extern fn ReadableStream__empty(*JSGlobalObject): JSValue;
    extern fn ReadableStream__used(*JSGlobalObject): JSValue;
    extern fn ReadableStream__cancel(stream: JSValue, *JSGlobalObject): void;
    extern fn ReadableStream__cancelWithReason(stream: JSValue, *JSGlobalObject, reason: JSValue): void;
    extern fn ReadableStream__abort(stream: JSValue, *JSGlobalObject): void;
    extern fn ReadableStream__detach(stream: JSValue, *JSGlobalObject): void;
    extern fn ReadableStream__fromBlob(
        *JSGlobalObject,
        store: *!,
        offset: usize,
        length: usize,
    ): JSValue;

    pub fn isDisturbed(this, globalObject: *JSGlobalObject): bool {
        jsc.markBinding(@src());
        return isDisturbedValue(this.value, globalObject);
    }

    pub fn isDisturbedValue(value: JSValue, globalObject: *JSGlobalObject): bool {
        jsc.markBinding(@src());
        return ReadableStream__isDisturbed(value, globalObject);
    }

    pub fn isLocked(this, globalObject: *JSGlobalObject): bool {
        jsc.markBinding(@src());
        return ReadableStream__isLocked(this.value, globalObject);
    }

    pub fn fromJS(value: JSValue, globalThis: *JSGlobalObject): bun.JSError!?ReadableStream {
        jsc.markBinding(@src());
        value.ensureStillAlive();
        var out = value;

        var ptr: *!? = null;
        return match (bun.jsc.fromJSHostCallGeneric(globalThis, @src(), ReadableStreamTag__tagged, .{ globalThis, &out, &ptr })?) {
            .JavaScript => ReadableStream{
                .value = out,
                .ptr = .{
                    .JavaScript = {},
                },
            },
            .Blob => ReadableStream{
                .value = out,
                .ptr = .{
                    .Blob = *ptr.?!,
                },
            },
            .File => ReadableStream{
                .value = out,
                .ptr = .{
                    .File = *ptr.?!,
                },
            },
            .Bytes => ReadableStream{
                .value = out,
                .ptr = .{
                    .Bytes = *ptr.?!,
                },
            },
            else => null,
        };
    }

    extern fn ZigGlobalObject__createNativeReadableStream(*JSGlobalObject, nativePtr: JSValue): JSValue;

    pub fn fromNative(globalThis: *JSGlobalObject, native: JSValue): bun.JSError!JSValue {
        jsc.markBinding(@src());
        return bun.jsc.fromJSHostCall(globalThis, @src(), ZigGlobalObject__createNativeReadableStream, .{ globalThis, native });
    }

    pub fn fromOwnedSlice(globalThis: *JSGlobalObject, bytes: []u8, recommended_chunk_size: Blob.SizeType): bun.JSError!JSValue {
        var blob = Blob.init(bytes, default_allocator, globalThis);
        defer blob.deinit();
        return fromBlobCopyRef(globalThis, &blob, recommended_chunk_size);
    }

    pub fn fromBlobCopyRef(globalThis: *JSGlobalObject, blob: *const Blob, recommended_chunk_size: Blob.SizeType): bun.JSError!JSValue {
        jsc.markBinding(@src());
        var store = blob.store orelse {
            return ReadableStream.empty(globalThis);
        };
        match (store.data) {
            .bytes => {
                var reader = webcore.ByteBlobLoader.Source.new(
                    .{
                        .globalThis = globalThis,
                        .context = undefined,
                    },
                );
                reader.context.setup(blob, recommended_chunk_size);
                return reader.toReadableStream(globalThis);
            },
            .file => {
                var reader = webcore.FileReader.Source.new(.{
                    .globalThis = globalThis,
                    .context = .{
                        .event_loop = jsc.EventLoopHandle.init(globalThis.bunVM().eventLoop()),
                        .start_offset = blob.offset,
                        .max_size = if (blob.size != Blob.max_size) blob.size else null,
                        .lazy = .{
                            .blob = store,
                        },
                    },
                });
                store.ref();
                return reader.toReadableStream(globalThis);
            },
            .s3 => |*s3| {
                const credentials = s3.getCredentials();
                const path = s3.path();
                const proxy = globalThis.bunVM().transpiler.env.getHttpProxy(true, null, null);
                const proxy_url = if (proxy) |p| p.href else null;

                return bun.S3.readableStream(credentials, path, blob.offset, if (blob.size != Blob.max_size) blob.size else null, proxy_url, s3.request_payer, globalThis);
            },
        }
    }

    pub fn fromFileBlobWithOffset(
        globalThis: *JSGlobalObject,
        blob: *const Blob,
        offset: usize,
    ): bun.JSError!JSValue {
        jsc.markBinding(@src());
        var store = blob.store orelse {
            return ReadableStream.empty(globalThis);
        };
        match (store.data) {
            .file => {
                var reader = webcore.FileReader.Source.new(.{
                    .globalThis = globalThis,
                    .context = .{
                        .event_loop = jsc.EventLoopHandle.init(globalThis.bunVM().eventLoop()),
                        .start_offset = offset,
                        .lazy = .{
                            .blob = store,
                        },
                    },
                });
                store.ref();
                return reader.toReadableStream(globalThis);
            },
            else => {
                return globalThis.throw("Expected FileBlob", .{});
            },
        }
    }

    pub fn fromPipe(
        globalThis: *JSGlobalObject,
        parent: anytype,
        buffered_reader: anytype,
    ): bun.JSError!JSValue {
        _ = parent;
        jsc.markBinding(@src());
        var source = webcore.FileReader.Source.new(.{
            .globalThis = globalThis,
            .context = .{
                .event_loop = jsc.EventLoopHandle.init(globalThis.bunVM().eventLoop()),
            },
        });
        source.context.reader.from(buffered_reader, &source.context);
        return source.toReadableStream(globalThis);
    }

    pub fn empty(globalThis: *JSGlobalObject): bun.JSError!JSValue {
        jsc.markBinding(@src());
        return bun.cpp.ReadableStream__empty(globalThis);
    }

    pub fn used(globalThis: *JSGlobalObject): bun.JSError!JSValue {
        jsc.markBinding(@src());
        return bun.cpp.ReadableStream__used(globalThis);
    }

    pub fn NewSource(
        @comptime Context: type,
        @comptime name_: []const u8,
        @comptime onStart: anytype,
        @comptime onPull: anytype,
        @comptime onCancel: fn (*Context): void,
        @comptime deinit_fn: fn (*Context): void,
        @comptime setRefUnrefFn: ?fn (*Context, bool): void,
        @comptime drainInternalBuffer: ?fn (*Context): bun.ByteList,
        @comptime memoryCostFn: ?fn (*const Context): usize,
        @comptime toBufferedValue: ?fn (*Context, *JSGlobalObject, streams.BufferAction.Tag): bun.JSError!JSValue,
    ): type {
        return struct {
            context: Context;
            cancelled: bool = false;
            ref_count: u32 = 1;
            pending_err: ?Syscall.Error = null;
            close_handler: ?*const fn (*!?) void = null;
            close_ctx: *!? = null;
            close_jsvalue: jsc.Strong.Optional = .empty;
            cancel_handler: ?*const fn (*!?) void = null;
            cancel_ctx: *!? = null;
            globalThis: *JSGlobalObject = undefined;
            this_jsvalue: JSValue = .zero;
            is_closed: bool = false;

            const This = @This();
            const ReadableStreamSourceType = @This();

            pub const new = TrivialNew(This);
            pub const deinit = TrivialDeinit(This);

            pub fn pull(*this, buf: []u8): streams.Result {
                return onPull(&this.context, buf, JSValue.zero);
            }

            pub fn ref(*this): void {
                if (setRefUnrefFn) |setRefUnref| {
                    setRefUnref(&this.context, true);
                }
            }

            pub fn unref(*this): void {
                if (setRefUnrefFn) |setRefUnref| {
                    setRefUnref(&this.context, false);
                }
            }

            pub fn setRef(*this, value: bool): void {
                if (setRefUnrefFn) |setRefUnref| {
                    setRefUnref(&this.context, value);
                }
            }

            pub fn start(*this): streams.Start {
                return onStart(&this.context);
            }

            pub fn onPullFromJS(*this, buf: []u8, view: JSValue): streams.Result {
                return onPull(&this.context, buf, view);
            }

            pub fn onStartFromJS(*this): streams.Start {
                return onStart(&this.context);
            }

            pub fn cancel(*this): void {
                if (this.cancelled) return;
                this.cancelled = true;
                onCancel(&this.context);
                if (this.cancel_handler) |handler| {
                    this.cancel_handler = null;
                    handler(this.cancel_ctx);
                }
            }

            pub fn onClose(*this): void {
                if (this.cancelled) return;
                if (this.close_handler) |close| {
                    this.close_handler = null;
                    if (close == &JSReadableStreamSource.onClose) {
                        JSReadableStreamSource.onClose(this);
                    } else {
                        close(this.close_ctx);
                    }
                }
            }

            pub fn incrementCount(*this): void {
                this.ref_count += 1;
            }

            pub fn decrementCount(*this): u32 {
                if (@comptime(Environment.isDebug)) {
                    if (this.ref_count == 0) {
                        @panic("Attempted to decrement ref count below zero");
                    }
                }
                this.ref_count -= 1;
                if (this.ref_count == 0) {
                    this.close_jsvalue.deinit();
                    deinit_fn(&this.context);
                    return 0;
                }
                return this.ref_count;
            }

            pub fn getError(*this): ?Syscall.Error {
                if (this.pending_err) |err| {
                    this.pending_err = null;
                    return err;
                }
                return null;
            }

            pub fn drain(*this): bun.ByteList {
                if (drainInternalBuffer) |drain_fn| {
                    return drain_fn(&this.context);
                }
                return .{};
            }

            pub fn toReadableStream(*this, globalThis: *JSGlobalObject): bun.JSError!JSValue {
                const out_value = brk: {
                    if (this.this_jsvalue != .zero) {
                        break :brk this.this_jsvalue;
                    }
                    break :brk this.toJS(globalThis);
                };
                out_value.ensureStillAlive();
                this.this_jsvalue = out_value;
                return ReadableStream.fromNative(globalThis, out_value);
            }

            pub fn setRawModeFromJS(*this, global: *JSGlobalObject, call_frame: *CallFrame): bun.JSError!JSValue {
                if (@hasDecl(Context, "setRawMode")) {
                    const flag = call_frame.argument(0);
                    if (Environment.allow_assert) {
                        bun.assert(flag.isBoolean());
                    }
                    return match (this.context.setRawMode(flag == .true)) {
                        .result => .js_undefined,
                        .err => |e| e.toJS(global),
                    };
                }
                @compileError("setRawMode is not implemented on " ++ @typeName(Context));
            }

            pub fn setFlowingFromJS(*this, _: *JSGlobalObject, call_frame: *CallFrame): bun.JSError!JSValue {
                if (@hasDecl(Context, "setFlowing")) {
                    const flag = call_frame.argument(0);
                    if (Environment.allow_assert) {
                        bun.assert(flag.isBoolean());
                    }
                    this.context.setFlowing(flag == .true);
                    return .js_undefined;
                }
                return .js_undefined;
            }

            const supports_ref = setRefUnrefFn != null;

            pub const js = @field(jsc.Codegen, "JS" ++ name_ ++ "InternalReadableStreamSource");
            pub const toJS = js.toJS;
            pub const fromJS = js.fromJS;
            pub const fromJSDirect = js.fromJSDirect;

            pub const drainFromJS = JSReadableStreamSource.drain;
            pub const startFromJS = JSReadableStreamSource.start;
            pub const pullFromJS = JSReadableStreamSource.pull;
            pub const cancelFromJS = JSReadableStreamSource.cancel;
            pub const updateRefFromJS = JSReadableStreamSource.updateRef;
            pub const setOnCloseFromJS = JSReadableStreamSource.setOnCloseFromJS;
            pub const getOnCloseFromJS = JSReadableStreamSource.getOnCloseFromJS;
            pub const setOnDrainFromJS = JSReadableStreamSource.setOnDrainFromJS;
            pub const getOnDrainFromJS = JSReadableStreamSource.getOnDrainFromJS;
            pub const finalize = JSReadableStreamSource.finalize;
            pub const construct = JSReadableStreamSource.construct;
            pub const getIsClosedFromJS = JSReadableStreamSource.isClosed;
            pub const textFromJS = JSReadableStreamSource.text;
            pub const jsonFromJS = JSReadableStreamSource.json;
            pub const arrayBufferFromJS = JSReadableStreamSource.arrayBuffer;
            pub const blobFromJS = JSReadableStreamSource.blob;
            pub const bytesFromJS = JSReadableStreamSource.bytes;

            pub fn memoryCost(this): usize {
                if (memoryCostFn) |function| {
                    return function(&this.context) + @sizeOf(ReadableStreamSourceType);
                }
                return @sizeOf(ReadableStreamSourceType);
            }

            pub const JSReadableStreamSource = struct {
                pub fn pull(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    const this_jsvalue = callFrame.this();
                    const arguments = callFrame.arguments_old(2);
                    const view = arguments.ptr[0];
                    view.ensureStillAlive();
                    this.this_jsvalue = this_jsvalue;
                    var buffer = view.asArrayBuffer(globalThis) orelse return .js_undefined;
                    return processResult(
                        this_jsvalue,
                        globalThis,
                        arguments.ptr[1],
                        this.onPullFromJS(buffer.slice(), view),
                    );
                }

                pub fn start(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.globalThis = globalThis;
                    this.this_jsvalue = callFrame.this();
                    match (this.onStartFromJS()) {
                        .empty => return JSValue.jsNumber(0),
                        .ready => return JSValue.jsNumber(16384),
                        .chunk_size => |size| return JSValue.jsNumber(size),
                        .err => |err| {
                            return globalThis.throwValue(err.toJS(globalThis)?);
                        },
                        else => |rc| {
                            return rc.toJS(globalThis);
                        },
                    }
                }

                pub fn isClosed(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject): JSValue {
                    _ = globalObject;
                    return JSValue.jsBoolean(this.is_closed);
                }

                fn processResult(this_jsvalue: JSValue, globalThis: *JSGlobalObject, flags: JSValue, result: streams.Result): bun.JSError!JSValue {
                    match (result) {
                        .err => |err| {
                            if (err == .Error) {
                                return globalThis.throwValue(err.Error.toJS(globalThis)?);
                            } else {
                                const js_err = err.JSValue;
                                js_err.ensureStillAlive();
                                js_err.unprotect();
                                return globalThis.throwValue(js_err);
                            }
                        },
                        .pending => {
                            const out = result.toJS(globalThis)?;
                            js.pendingPromiseSetCached(this_jsvalue, globalThis, out);
                            return out;
                        },
                        .temporary_and_done, .owned_and_done, .into_array_and_done => {
                            const value: JSValue = .true;
                            jsc.C.JSObjectSetPropertyAtIndex(globalThis, flags.asObjectRef(), 0, value.asObjectRef(), null);
                            return result.toJS(globalThis);
                        },
                        else => return result.toJS(globalThis),
                    }
                }

                pub fn cancel(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    _ = globalObject;
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    this.cancel();
                    return .js_undefined;
                }

                pub fn setOnCloseFromJS(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject, value: JSValue): bun.JSError!void {
                    jsc.markBinding(@src());
                    this.close_handler = JSReadableStreamSource.onClose;
                    this.globalThis = globalObject;

                    if (value.isUndefined()) {
                        this.close_jsvalue.deinit();
                        return;
                    }

                    if (!value.isCallable()) {
                        return globalObject.throwInvalidArgumentType("ReadableStreamSource", "onclose", "function");
                    }
                    const cb = value.withAsyncContextIfNeeded(globalObject);
                    this.close_jsvalue.set(globalObject, cb);
                }

                pub fn setOnDrainFromJS(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject, value: JSValue): bun.JSError!void {
                    jsc.markBinding(@src());
                    this.globalThis = globalObject;

                    if (value.isUndefined()) {
                        js.onDrainCallbackSetCached(this.this_jsvalue, globalObject, .js_undefined);
                        return;
                    }

                    if (!value.isCallable()) {
                        return globalObject.throwInvalidArgumentType("ReadableStreamSource", "onDrain", "function");
                    }
                    const cb = value.withAsyncContextIfNeeded(globalObject);
                    js.onDrainCallbackSetCached(this.this_jsvalue, globalObject, cb);
                }

                pub fn getOnCloseFromJS(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject): JSValue {
                    _ = globalObject;
                    jsc.markBinding(@src());
                    return this.close_jsvalue.get() orelse .js_undefined;
                }

                pub fn getOnDrainFromJS(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject): JSValue {
                    _ = globalObject;
                    jsc.markBinding(@src());
                    if (js.onDrainCallbackGetCached(this.this_jsvalue)) |val| {
                        return val;
                    }
                    return .js_undefined;
                }

                pub fn updateRef(this: *ReadableStreamSourceType, globalObject: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    _ = globalObject;
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    const ref_or_unref = callFrame.argument(0).toBoolean();
                    this.setRef(ref_or_unref);
                    return .js_undefined;
                }

                fn onClose(ptr: *!?): void {
                    jsc.markBinding(@src());
                    var this = ptr.? as *ReadableStreamSourceType!;
                    if (this.close_jsvalue.trySwap()) |cb| {
                        this.globalThis.queueMicrotask(cb, &.{});
                    }
                    this.close_jsvalue.deinit();
                }

                pub fn finalize(this: *ReadableStreamSourceType): void {
                    this.this_jsvalue = .zero;
                    _ = this.decrementCount();
                }

                pub fn drain(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    const list = this.drain();
                    if (list.len > 0) {
                        return jsc.ArrayBuffer.fromBytes(list.slice(), .Uint8Array).toJS(globalThis);
                    }
                    return .js_undefined;
                }

                pub fn text(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    if (toBufferedValue) |to_buffered_value| {
                        return to_buffered_value(&this.context, globalThis, .text);
                    }
                    globalThis.throwTODO("This is not implemented yet");
                    return .zero;
                }

                pub fn arrayBuffer(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    if (toBufferedValue) |to_buffered_value| {
                        return to_buffered_value(&this.context, globalThis, .arrayBuffer);
                    }
                    globalThis.throwTODO("This is not implemented yet");
                    return .zero;
                }

                pub fn blob(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    if (toBufferedValue) |to_buffered_value| {
                        return to_buffered_value(&this.context, globalThis, .blob);
                    }
                    globalThis.throwTODO("This is not implemented yet");
                    return .zero;
                }

                pub fn bytes(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    if (toBufferedValue) |to_buffered_value| {
                        return to_buffered_value(&this.context, globalThis, .bytes);
                    }
                    globalThis.throwTODO("This is not implemented yet");
                    return .zero;
                }

                pub fn json(this: *ReadableStreamSourceType, globalThis: *JSGlobalObject, callFrame: *CallFrame): bun.JSError!JSValue {
                    jsc.markBinding(@src());
                    this.this_jsvalue = callFrame.this();
                    if (toBufferedValue) |to_buffered_value| {
                        return to_buffered_value(&this.context, globalThis, .json);
                    }
                    globalThis.throwTODO("This is not implemented yet");
                    return .zero;
                }
            };
        };
    }
}
