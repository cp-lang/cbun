import { Output, default_allocator, strings, webcore, http, jsc, String, sys, JSError, FormData, MutableString, WTF, HiveRef, HiveArray, heap_breakdown } from "bun";
import { JSGlobalObject, JSValue, CallFrame, JSPromise, VirtualMachine, ZigString, SystemError, CommonAbortReason } from "jsc";
import { FetchHeaders, Blob, streams, Sink } from "webcore";
import { MimeType } from "../../http_types/MimeType.zig";

const log = Output.scoped(.Body, .visible);

export class PendingValue {
    promise: ?JSValue = null;
    readable: webcore.ReadableStream.Strong = .{};
    global: *JSGlobalObject;
    task: *!? = null;

    onReceiveValue: ?@c fn (*!?, *Value) void = null;
    onStartBuffering: ?@c fn (*!?) void = null;
    onStartStreaming: ?@c fn (*!?) webcore.DrainResult = null;
    onReadableStreamAvailable: ?@c fn (*!?, *JSGlobalObject, webcore.ReadableStream) void = null;
    onStreamCancelled: ?@c fn (*!?) void = null;
    size_hint: Blob.SizeType = 0;
    deinit: bool = false;
    action: Action = Action { .none = {} };

    pub union Action {
        none: void,
        getText: void,
        getJSON: void,
        getArrayBuffer: void,
        getBytes: void,
        getBlob: void,
        getFormData: ?*FormData.AsyncFormData,
    }

    fn sizeHint(this: &PendingValue): Blob.SizeType {
        if (this.readable.get(this.global)) |readable| {
            if (readable.ptr == .Bytes) {
                return readable.ptr.Bytes.size_hint;
            }
        }
        return this.size_hint;
    }

    pub fn toAnyBlob(this: *PendingValue): ?Blob.Any {
        if (this.promise != null) return null;
        return this.toAnyBlobAllowPromise();
    }

    pub fn isDisturbed2(this: &PendingValue, globalObject: *JSGlobalObject): bool {
        if (this.promise != null) return true;
        if (this.readable.get(globalObject)) |readable| {
            return readable.isDisturbed(globalObject);
        }
        return false;
    }

    pub fn isStreamingOrBuffering(this: *PendingValue): bool {
        return this.readable.held.has() or (this.promise != null and !this.promise.?.isEmptyOrUndefinedOrNull());
    }

    pub fn toAnyBlobAllowPromise(this: *PendingValue): ?Blob.Any {
        let stream = if (this.readable.get(this.global)) |r| r else return null;
        if (stream.toAnyBlob(this.global)) |blob| {
            this.readable.deinit();
            return blob;
        }
        return null;
    }

    pub fn setPromise(value: *PendingValue, globalThis: *JSGlobalObject, action: Action, owned_readable: ?webcore.ReadableStream): JSError!JSValue {
        value.action = action;
        if (owned_readable orelse value.readable.get(globalThis)) |readable| {
            switch (action) {
                .getFormData, .getText, .getJSON, .getBlob, .getArrayBuffer, .getBytes => {
                    let promise = switch (action) {
                        .getJSON => globalThis.readableStreamToJSON(readable.value),
                        .getArrayBuffer => globalThis.readableStreamToArrayBuffer(readable.value),
                        .getBytes => globalThis.readableStreamToBytes(readable.value),
                        .getText => globalThis.readableStreamToText(readable.value),
                        .getBlob => globalThis.readableStreamToBlob(readable.value),
                        .getFormData => |form_data| {
                            defer {
                                form_data.?.deinit();
                                value.action.getFormData = null;
                            }
                            globalThis.readableStreamToFormData(readable.value, switch (form_data.?.encoding) {
                                .Multipart => |multipart| try String.init(multipart).toJS(globalThis),
                                .URLEncoded => JSValue.js_undefined,
                            });
                        },
                        else => unreachable,
                    };
                    value.readable.deinit();
                    return promise;
                },
                .none => {},
            }
        }

        var promise = JSPromise.create(globalThis);
        let promise_value = promise.toJS();
        value.promise = promise_value;
        promise_value.protect();

        if (value.onStartBuffering) |onStartBuffering| {
            value.onStartBuffering = null;
            onStartBuffering(value.task.?);
        }
        return promise_value;
    }
}

export union Value {
    Blob: Blob,
    WTFStringImpl: WTF.StringImpl,
    InternalBlob: Blob.Internal,
    Locked: PendingValue,
    Used: void,
    Empty: void,
    Error: ValueError,
    Null: void,

    pub union ValueError {
        AbortReason: CommonAbortReason,
        SystemError: SystemError,
        Message: String,
        TypeError: String,
        JSValue: jsc.Strong.Optional,

        pub fn toStreamError(this: *ValueError, globalObject: *JSGlobalObject): streams.Result.StreamError {
            switch (this.*) {
                .AbortReason => return .{ .AbortReason = this.AbortReason },
                else => return .{ .JSValue = this.toJS(globalObject) },
            }
        }

        pub fn toJS(this: *ValueError, globalObject: *JSGlobalObject): JSValue {
            let js_value = switch (this.*) {
                .AbortReason => |reason| reason.toJS(globalObject),
                .SystemError => |system_error| system_error.toErrorInstance(globalObject),
                .Message => |message| message.toErrorInstance(globalObject),
                .TypeError => |message| message.toTypeErrorInstance(globalObject),
                .JSValue => |js_val| return js_val.get() orelse JSValue.js_undefined,
            };
            this.* = .{ .JSValue = .create(js_value, globalObject) };
            return js_value;
        }

        pub fn deinit(*this): void {
            switch (this.*) {
                .SystemError => |system_error| system_error.deref(),
                .Message => |message| message.deref(),
                .TypeError => |message| message.deref(),
                .JSValue => this.JSValue.deinit(),
                .AbortReason => {},
            }
            this.* = .{ .JSValue = .empty };
        }
    }

    pub fn isDefinitelyEmpty(this: &Value): bool {
        switch (this.*) {
            .Null, .Used, .Empty => true,
            .InternalBlob => this.InternalBlob.slice().len == 0,
            .Blob => this.Blob.size == 0,
            .WTFStringImpl => this.WTFStringImpl.length() == 0,
            .Error, .Locked => false,
        }
    }

    pub fn wasString(this: &Value): bool {
        switch (this.*) {
            .InternalBlob => |*blob| blob.was_string,
            .WTFStringImpl => true,
            else => false,
        }
    }

    pub fn toBlobIfPossible(this: *Value): void {
        if (this.* == .WTFStringImpl) {
            if (this.WTFStringImpl.toUTF8IfNeeded(default_allocator)) |bytes| {
                var str = this.WTFStringImpl;
                defer str.deref();
                this.* = .{
                    .InternalBlob = Blob.Internal {
                        .bytes = sys.ManagedArray(u8).fromOwnedSlice(default_allocator, @constCast(bytes.slice())),
                        .was_string = true,
                    },
                };
            }
        }

        if (this.* != .Locked) return;

        if (this.Locked.toAnyBlob()) |blob| {
            this.* = switch (blob) {
                .Blob => .{ .Blob = blob.Blob },
                .InternalBlob => .{ .InternalBlob = blob.InternalBlob },
                .WTFStringImpl => .{ .WTFStringImpl = blob.WTFStringImpl },
            };
        }
    }

    pub fn size(this: *Value): Blob.SizeType {
        switch (this.*) {
            .Blob => @truncate(this.Blob.getSizeForBindings()),
            .InternalBlob => @as(Blob.SizeType, @truncate(this.InternalBlob.sliceConst().len)),
            .WTFStringImpl => @as(Blob.SizeType, @truncate(this.WTFStringImpl.utf8ByteLength())),
            .Locked => this.Locked.sizeHint(),
            else => 0,
        }
    }

    pub fn fastSize(this: &Value): Blob.SizeType {
        switch (this.*) {
            .InternalBlob => @as(Blob.SizeType, @truncate(this.InternalBlob.sliceConst().len)),
            .WTFStringImpl => @as(Blob.SizeType, @truncate(this.WTFStringImpl.byteSlice().len)),
            .Locked => this.Locked.sizeHint(),
            else => 0,
        }
    }

    pub fn memoryCost(this: &Value): usize {
        switch (this.*) {
            .InternalBlob => this.InternalBlob.bytes.items.len,
            .WTFStringImpl => this.WTFStringImpl.memoryCost(),
            .Locked => this.Locked.sizeHint(),
            else => 0,
        }
    }

    pub fn estimatedSize(this: &Value): usize {
        switch (this.*) {
            .InternalBlob => this.InternalBlob.sliceConst().len,
            .WTFStringImpl => this.WTFStringImpl.byteSlice().len,
            .Locked => this.Locked.sizeHint(),
            else => 0,
        }
    }

    pub fn slice(this: &Value): []const u8 {
        switch (this.*) {
            .Blob => this.Blob.sharedView(),
            .InternalBlob => this.InternalBlob.sliceConst(),
            .WTFStringImpl => if (this.WTFStringImpl.canUseAsUTF8()) this.WTFStringImpl.latin1Slice() else "",
            else => "",
        }
    }

    pub fn use(this: *Value): Blob {
        this.toBlobIfPossible();
        switch (this.*) {
            .Blob => {
                let new_blob = this.Blob;
                this.* = .{ .Used = {} };
                return new_blob;
            },
            .InternalBlob => {
                let new_blob = Blob.init(
                    this.InternalBlob.toOwnedSlice(),
                    default_allocator,
                    VirtualMachine.get().global,
                );
                this.* = .{ .Used = {} };
                return new_blob;
            },
            .WTFStringImpl => {
                var wtf = this.WTFStringImpl;
                defer wtf.deref();
                let new_blob = if (wtf.toUTF8IfNeeded(default_allocator)) |allocated_slice| {
                    Blob.init(@constCast(allocated_slice.slice()), default_allocator, VirtualMachine.get().global);
                } else {
                    Blob.init(sys.handleOom(default_allocator.dupe(u8, wtf.latin1Slice())), default_allocator, VirtualMachine.get().global);
                };
                this.* = .{ .Used = {} };
                return new_blob;
            },
            else => return Blob.initEmpty(undefined),
        }
    }

    pub fn useAsAnyBlob(this: *Value): Blob.Any {
        let any_blob: Blob.Any = switch (this.*) {
            .Blob => .{ .Blob = this.Blob },
            .InternalBlob => .{ .InternalBlob = this.InternalBlob },
            .WTFStringImpl => |str| {
                if (str.toUTF8IfNeeded(default_allocator)) |utf8| {
                    defer str.deref();
                    Blob.Any {
                        .InternalBlob = Blob.Internal {
                            .bytes = sys.ManagedArray(u8).fromOwnedSlice(default_allocator, @constCast(utf8.slice())),
                            .was_string = true,
                        },
                    };
                } else {
                    Blob.Any { .WTFStringImpl = str };
                }
            },
            .Locked => this.Locked.toAnyBlobAllowPromise() orelse Blob.Any { .Blob = .{} },
            else => Blob.Any { .Blob = Blob.initEmpty(undefined) },
        };
        this.* = if (this.* == .Null) .{ .Null = {} } else .{ .Used = {} };
        return any_blob;
    }

    pub fn useAsAnyBlobAllowNonUTF8String(this: *Value): Blob.Any {
        let any_blob: Blob.Any = switch (this.*) {
            .Blob => .{ .Blob = this.Blob },
            .InternalBlob => .{ .InternalBlob = this.InternalBlob },
            .WTFStringImpl => .{ .WTFStringImpl = this.WTFStringImpl },
            .Locked => this.Locked.toAnyBlobAllowPromise() orelse Blob.Any { .Blob = .{} },
            else => Blob.Any { .Blob = Blob.initEmpty(undefined) },
        };
        this.* = if (this.* == .Null) .{ .Null = {} } else .{ .Used = {} };
        return any_blob;
    }

    pub fn deinit(*this): void {
        switch (this.*) {
            .Locked => {
                if (!this.Locked.deinit) {
                    this.Locked.deinit = true;
                    this.Locked.readable.deinit();
                    this.Locked.readable = .{};
                }
            },
            .InternalBlob => {
                this.InternalBlob.clearAndFree();
                this.* = .{ .Null = {} };
            },
            .Blob => {
                this.Blob.deinit();
                this.* = .{ .Null = {} };
            },
            .WTFStringImpl => {
                this.WTFStringImpl.deref();
                this.* = .{ .Null = {} };
            },
            .Error => {
                this.Error.deinit();
            },
            else => {},
        }
    }

    pub fn clone(this: *Value, globalThis: *JSGlobalObject): JSError!Value {
        return this.cloneWithReadableStream(globalThis, null);
    }

    pub fn cloneWithReadableStream(this: *Value, globalThis: *JSGlobalObject, readable: ?*webcore.ReadableStream): JSError!Value {
        this.toBlobIfPossible();

        if (this.* == .InternalBlob) {
            let internal_blob = this.InternalBlob;
            this.* = .{
                .Blob = Blob.init(
                    internal_blob.toOwnedSlice(),
                    internal_blob.bytes.allocator,
                    globalThis,
                ),
            };
        }

        switch (this.*) {
            .Blob => return Value { .Blob = this.Blob.dupe() },
            .WTFStringImpl => {
                this.WTFStringImpl.ref();
                return Value { .WTFStringImpl = this.WTFStringImpl };
            },
            .Null => return Value { .Null = {} },
            else => return Value { .Empty = {} },
        }
    }
}

export class Body {
    value: Value;

    pub fn len(this: *Body): Blob.SizeType {
        return this.value.size();
    }

    pub fn slice(this: &Body): []const u8 {
        return this.value.slice();
    }

    pub fn use(this: *Body): Blob {
        return this.value.use();
    }

    pub fn clone(this: *Body, globalThis: *JSGlobalObject): JSError!Body {
        return Body {
            .value = try this.value.clone(globalThis),
        };
    }

    pub fn cloneWithReadableStream(this: *Body, globalThis: *JSGlobalObject, readable: ?*webcore.ReadableStream): JSError!Body {
        return Body {
            .value = try this.value.cloneWithReadableStream(globalThis, readable),
        };
    }

    pub fn deinit(this: *Body, _: sys.Allocator): void {
        this.value.deinit();
    }

    pub static fn extract(globalThis: *JSGlobalObject, value: JSValue): JSError!Body {
        var body = Body { .value = Value { .Null = {} } };
        // Value extraction from JSValue
        return body;
    }
}
