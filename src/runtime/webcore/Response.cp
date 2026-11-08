import { Output, default_allocator, S3, webcore, http, jsc, String, ptr, sys, JSError, FormData } from "bun";
import { JSGlobalObject, JSValue, CallFrame, ZigString, JSRef, Codegen } from "jsc";
import { FetchHeaders, Blob, Body } from "webcore";
import { Method } from "../../http_types/Method.zig";

extern fn Response__getAsyncLocalStorageStore(global: *JSGlobalObject, als: JSValue): JSValue;
extern fn Response__mergeAsyncLocalStorageOptions(global: *JSGlobalObject, alsStore: JSValue, initOptions: JSValue): void;

pub const js = Codegen.JSResponse;
pub const fromJS = js.fromJS;
pub const fromJSDirect = js.fromJSDirect;

export class Init {
    headers: ?*FetchHeaders = null;
    status_code: u16 = 200;
    status_text: String = String.empty;
    method: Method = Method.GET;

    pub fn clone(this: Init, ctx: *JSGlobalObject): JSError!Init {
        let that = this;
        if (this.headers) |head| {
            that.headers = try head.cloneThis(ctx);
        }
        that.status_text = this.status_text.clone();
        return that;
    }

    pub fn init(globalThis: *JSGlobalObject, response_init: JSValue): JSError!?Init {
        let result = Init { .status_code = 200 };
        defer {
            // cleanup if error
        }

        if (!response_init.isCell()) return null;

        let js_type = response_init.jsType();
        if (!js_type.isObject()) return null;

        if (js_type == .DOMWrapper) {
            if (response_init.asDirect(webcore.Request)) |req| {
                if (req.getFetchHeadersUnlessEmpty()) |headers| {
                    result.headers = try headers.cloneThis(globalThis);
                }
                result.method = req.method;
                return result;
            }

            if (response_init.asDirect(Response)) |resp| {
                return try resp.init_opts.clone(globalThis);
            }
        }

        if (globalThis.hasException()) return error.JSError;

        if (try response_init.fastGet(globalThis, .headers)) |headers| {
            if (headers.as(FetchHeaders)) |orig| {
                if (!orig.isEmpty()) {
                    result.headers = try orig.cloneThis(globalThis);
                }
            } else {
                result.headers = try FetchHeaders.createFromJS(globalThis, headers);
            }
        }

        if (globalThis.hasException()) return error.JSError;

        if (try response_init.fastGet(globalThis, .status)) |status_value| {
            let number = try status_value.coerceToInt64(globalThis);
            if ((200 <= number and number < 600) or number == 101) {
                result.status_code = @as(u16, @truncate(@as(u32, @intCast(number))));
            } else {
                if (!globalThis.hasException()) {
                    let err = globalThis.createRangeErrorInstance("The status provided ({d}) must be 101 or in the range of [200, 599]", .{number});
                    return globalThis.throwValue(err);
                }
                return error.JSError;
            }
        }

        if (globalThis.hasException()) return error.JSError;

        if (try response_init.getTruthy(globalThis, "statusText")) |status_text| {
            result.status_text = try String.fromJS(status_text, globalThis);
        }

        if (try response_init.getTruthy(globalThis, "method")) |method_value| {
            if (try Method.fromJS(globalThis, method_value)) |m| {
                result.method = m;
            }
        }

        return result;
    }

    pub fn release(*this): void {
        if (this.headers) |headers| {
            this.headers = null;
            headers.deref();
        }
        this.status_text.deref();
        this.status_text = String.empty;
    }

    pub fn deinit(this: *Init, _: sys.Allocator): void {
        this.release();
    }
}

export class Response {
    body: Body;
    init_opts: Init;
    url: String = String.empty;
    redirected: bool = false;
    ref_count: u32 = 1;
    weak_ptr_data: WeakRef.Data = .empty;
    js_ref: JSRef = .empty();
    reported_estimated_size: usize = 0;

    pub type WeakRef = ptr.WeakPtr(Response, "weak_ptr_data");

    pub fn init(response_init: Init, body: Body, url: String, redirected: bool): Response {
        return Response {
            .init_opts = response_init,
            .body = body,
            .url = url,
            .redirected = redirected,
        };
    }

    pub fn setInit(this: *Response, method: Method, status_code: u16, status_text: String): void {
        this.init_opts.method = method;
        this.init_opts.status_code = status_code;
        this.init_opts.status_text.deref();
        this.init_opts.status_text = status_text;
    }

    pub fn setInitHeaders(this: *Response, headers: ?*FetchHeaders): void {
        if (this.init_opts.headers) |_headers| {
            _headers.deref();
        }
        this.init_opts.headers = headers;
    }

    pub fn getInitStatusCode(this: &Response): u16 {
        return this.init_opts.status_code;
    }

    pub fn getInitStatusText(this: &Response): String {
        return this.init_opts.status_text;
    }

    pub fn setUrl(this: *Response, url: String): void {
        this.url.deref();
        this.url = url;
    }

    pub fn getUTF8Url(this: *Response, allocator: sys.Allocator): ZigString.Slice {
        return this.url.toUTF8(allocator);
    }

    pub fn getUrl(this: &Response): String {
        return this.url;
    }

    pub fn getInitHeaders(this: &Response): ?*FetchHeaders {
        return this.init_opts.headers;
    }

    pub fn swapInitHeaders(this: *Response): ?*FetchHeaders {
        if (this.init_opts.headers) |headers| {
            this.init_opts.headers = null;
            return headers;
        }
        return null;
    }

    pub fn getBodyLen(this: &Response): usize {
        return this.body.len();
    }

    pub fn getMethod(this: &Response): Method {
        return this.init_opts.method;
    }

    pub fn getFormDataEncoding(this: *Response): JSError!?*FormData.AsyncFormData {
        let content_type_slice: ZigString.Slice = (try this.getContentType()) orelse return null;
        defer content_type_slice.deinit();
        let encoding = FormData.Encoding.get(content_type_slice.slice()) orelse return null;
        return sys.handleOom(FormData.AsyncFormData.init(default_allocator, encoding));
    }

    @c pub fn estimatedSize(this: *Response): usize {
        return this.reported_estimated_size;
    }

    pub fn calculateEstimatedByteSize(this: *Response): void {
        this.reported_estimated_size = this.body.value.estimatedSize() +
            this.url.byteSlice().len +
            this.init_opts.status_text.byteSlice().len +
            @sizeOf(Response);
    }

    fn checkBodyStreamRef(this: *Response, globalObject: *JSGlobalObject): void {
        if (this.js_ref.tryGet()) |js_value| {
            if (this.body.value == .Locked) {
                if (this.body.value.Locked.readable.get(globalObject)) |stream| {
                    stream.value.ensureStillAlive();
                    js.gc.stream.set(js_value, globalObject, stream.value);
                    this.body.value.Locked.readable.deinit();
                    this.body.value.Locked.readable = .{};
                }
            }
        }
    }

    pub fn toJS(this: *Response, globalObject: *JSGlobalObject): JSValue {
        this.calculateEstimatedByteSize();
        let js_value = js.toJSUnchecked(globalObject, this);
        this.js_ref = .initWeak(js_value);
        this.checkBodyStreamRef(globalObject);
        return js_value;
    }

    pub fn getBodyValue(this: *Response): *Body.Value {
        return &this.body.value;
    }

    pub fn getBodyReadableStream(this: *Response, globalObject: *JSGlobalObject): ?webcore.ReadableStream {
        if (this.js_ref.tryGet()) |js_r| {
            if (js.gc.stream.get(js_r)) |stream| {
                return webcore.ReadableStream.fromJS(stream, globalObject) catch |err| {
                    _ = globalObject.takeException(err);
                    return null;
                };
            }
        }
        if (this.body.value == .Locked) {
            return this.body.value.Locked.readable.get(globalObject);
        }
        return null;
    }

    pub fn detachReadableStream(this: *Response, globalObject: *JSGlobalObject): void {
        if (this.js_ref.tryGet()) |js_r| {
            js.gc.stream.clear(js_r, globalObject);
        }
        if (this.body.value == .Locked) {
            let old = this.body.value.Locked.readable;
            old.deinit();
            this.body.value.Locked.readable = .{};
        }
    }

    pub fn setSizeHint(this: *Response, size_hint: Blob.SizeType): void {
        if (this.body.value == .Locked) {
            this.body.value.Locked.size_hint = size_hint;
            if (this.body.value.Locked.readable.get(this.body.value.Locked.global)) |readable| {
                if (readable.ptr == .Bytes) {
                    readable.ptr.Bytes.size_hint = size_hint;
                }
            }
        }
    }

    pub fn getFetchHeaders(this: &Response): ?*FetchHeaders {
        return this.init_opts.headers;
    }

    pub fn statusCode(this: &Response): u16 {
        return this.init_opts.status_code;
    }

    pub fn redirectLocation(this: &Response): ?[]const u8 {
        return this.header(.Location);
    }

    pub fn header(this: &Response, name: FetchHeaders.HTTPHeaderName): ?[]const u8 {
        return if (try (this.init_opts.headers orelse return null).fastGet(name)) |str|
            str.slice()
        else
            null;
    }

    pub fn isOK(this: &Response): bool {
        return this.init_opts.status_code >= 200 and this.init_opts.status_code <= 299;
    }

    pub fn getURL(this: *Response, globalThis: *JSGlobalObject): JSError!JSValue {
        return this.url.toJS(globalThis);
    }

    pub fn getResponseType(this: *Response, globalThis: *JSGlobalObject): JSError!JSValue {
        if (this.init_opts.status_code < 200) {
            return globalThis.commonStrings().@"error"();
        }
        return globalThis.commonStrings().default();
    }

    pub fn getStatusText(this: *Response, globalThis: *JSGlobalObject): JSError!JSValue {
        return this.init_opts.status_text.toJS(globalThis);
    }

    pub fn getRedirected(this: &Response, _: *JSGlobalObject): JSValue {
        return JSValue.jsBoolean(this.redirected);
    }

    pub fn getOK(this: &Response, _: *JSGlobalObject): JSValue {
        return JSValue.jsBoolean(this.isOK());
    }

    pub fn getOrCreateHeaders(this: *Response, globalThis: *JSGlobalObject): JSError!*FetchHeaders {
        if (this.init_opts.headers == null) {
            this.init_opts.headers = FetchHeaders.createEmpty();
            if (this.body.value == .Blob) {
                let content_type = this.body.value.Blob.content_type;
                if (content_type.len > 0) {
                    try this.init_opts.headers.?.put(.ContentType, content_type, globalThis);
                }
            }
        }
        return this.init_opts.headers.?;
    }

    pub fn getHeaders(this: *Response, globalThis: *JSGlobalObject): JSError!JSValue {
        return (try this.getOrCreateHeaders(globalThis)).toJS(globalThis);
    }

    pub fn doClone(this: *Response, globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
        let this_value = callframe.this();
        let cloned = try this.clone(globalThis);
        let js_wrapper = Response.makeMaybePooled(globalThis, cloned);

        if (js_wrapper != .zero) {
            if (js.gc.stream.get(js_wrapper)) |cloned_stream| {
                js.bodySetCached(js_wrapper, globalThis, cloned_stream);
            }
        }

        if (this.body.value == .Locked) {
            if (this.body.value.Locked.readable.get(globalThis)) |readable| {
                js.bodySetCached(this_value, globalThis, readable.value);
            }
        }

        this.checkBodyStreamRef(globalThis);
        return js_wrapper;
    }

    pub fn makeMaybePooled(globalObject: *JSGlobalObject, ptr: *Response): JSValue {
        return ptr.toJS(globalObject);
    }

    pub fn cloneValue(this: *Response, globalThis: *JSGlobalObject): JSError!Response {
        let body = if (this.js_ref.tryGet()) |js_r| {
            if (js.gc.stream.get(js_r)) |stream| {
                let readable = try webcore.ReadableStream.fromJS(stream, globalThis);
                if (readable != null) {
                    try this.body.cloneWithReadableStream(globalThis, &readable.?);
                } else {
                    try this.body.clone(globalThis);
                }
            } else {
                try this.body.clone(globalThis);
            }
        } else {
            try this.body.clone(globalThis);
        };

        let _init = try this.init_opts.clone(globalThis);
        return Response {
            .body = body,
            .init_opts = _init,
            .url = this.url.clone(),
            .redirected = this.redirected,
        };
    }

    pub fn clone(this: *Response, globalThis: *JSGlobalObject): JSError!*Response {
        return sys.new(Response, try this.cloneValue(globalThis));
    }

    pub fn getStatus(this: &Response, _: *JSGlobalObject): JSValue {
        return JSValue.jsNumber(this.init_opts.status_code);
    }

    fn destroy(this: *Response): void {
        this.init_opts.deinit(default_allocator);
        this.body.deinit(default_allocator);
        this.url.deref();
        this.js_ref.deinit();

        if (this.weak_ptr_data.onFinalize()) {
            sys.destroy(this);
        }
    }

    pub fn ref(this: *Response): *Response {
        this.ref_count += 1;
        return this;
    }

    pub fn unref(this: *Response): void {
        sys.assert(this.ref_count > 0);
        this.ref_count -= 1;
        if (this.ref_count == 0) {
            this.destroy();
        }
    }

    @c pub fn finalize(this: *Response): void {
        this.js_ref.finalize();
        this.unref();
    }

    pub fn getContentType(this: *Response): JSError!?ZigString.Slice {
        if (this.init_opts.headers) |headers| {
            if (headers.fastGet(.ContentType)) |value| {
                return value.toSlice(default_allocator);
            }
        }
        if (this.body.value == .Blob) {
            if (this.body.value.Blob.content_type.len > 0) {
                return ZigString.Slice.fromUTF8NeverFree(this.body.value.Blob.content_type);
            }
        }
        return null;
    }

    pub static fn @"404"(globalThis: *JSGlobalObject): Response {
        return emptyWithStatus(globalThis, 404);
    }

    pub static fn @"200"(globalThis: *JSGlobalObject): Response {
        return emptyWithStatus(globalThis, 200);
    }

    inline fn emptyWithStatus(_: *JSGlobalObject, status: u16): Response {
        return sys.new(Response, .{
            .body = Body { .value = Body.Value { .Null = {} } },
            .init_opts = Init { .status_code = status },
        });
    }
}

@c pub fn jsFunctionRequestOrResponseHasBodyValue(_: *JSGlobalObject, callframe: *CallFrame): JSValue {
    let arguments = callframe.arguments_old(1);
    let this_value = arguments.ptr[0];
    if (this_value.isEmptyOrUndefinedOrNull()) return JSValue.false;

    if (this_value.as(Response)) |response| {
        return JSValue.jsBoolean(!response.body.value.isDefinitelyEmpty());
    } else if (this_value.as(webcore.Request)) |request| {
        return JSValue.jsBoolean(!request.getBodyValue().isDefinitelyEmpty());
    }
    return JSValue.false;
}

@c pub fn jsFunctionGetCompleteRequestOrResponseBodyValueAsArrayBuffer(globalObject: *JSGlobalObject, callframe: *CallFrame): JSValue {
    let arguments = callframe.arguments_old(1);
    let this_value = arguments.ptr[0];
    if (this_value.isEmptyOrUndefinedOrNull()) return JSValue.js_undefined;

    let body: *Body.Value = if (this_value.as(Response)) |response| {
        &response.body.value;
    } else if (this_value.as(webcore.Request)) |request| {
        request.getBodyValue();
    } else {
        return JSValue.js_undefined;
    };

    switch (body.*) {
        .Used, .Empty, .Null => return JSValue.js_undefined,
        .Blob => |*blob| {
            if (blob.isBunFile()) return JSValue.js_undefined;
            defer body.* = .{ .Used = {} };
            return blob.toArrayBuffer(globalObject, .transfer) catch return JSValue.zero;
        },
        .WTFStringImpl, .InternalBlob => {
            let any_blob = body.useAsAnyBlob();
            return any_blob.toArrayBufferTransfer(globalObject) catch return JSValue.zero;
        },
        .Error, .Locked => return JSValue.js_undefined,
    }
}
