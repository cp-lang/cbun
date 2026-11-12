import { Output, default_allocator, strings, uws, webcore, http, jsc, String, ptr, sys, JSError, FormData, Environment } from "bun";
import { JSGlobalObject, JSValue, CallFrame, ZigString, JSRef, Codegen, DOMURL, JSSocketAddress } from "jsc";
import { FetchHeaders, Blob, Body, CookieMap, AbortSignal, Response } from "webcore";
import { Method } from "../../http_types/Method.zig";
import { FetchCacheMode } from "../../http_types/FetchCacheMode.zig";
import { FetchRedirect } from "../../http_types/FetchRedirect.zig";
import { FetchRequestMode } from "../../http_types/FetchRequestMode.zig";

pub const js = Codegen.JSRequest;
pub const fromJS = js.fromJS;
pub const fromJSDirect = js.fromJSDirect;

export class Flags {
    redirect: FetchRedirect = .follow;
    cache: FetchCacheMode = .default;
    mode: FetchRequestMode = .cors;
    https: bool = false;
}

export class InternalJSEventCallback {
    function: jsc.Strong.Optional = .empty;

    pub const EventType = jsc.API.NodeHTTPResponse.AbortEvent;

    pub fn init(function: JSValue, globalThis: *JSGlobalObject): InternalJSEventCallback {
        return InternalJSEventCallback {
            .function = .create(function, globalThis),
        };
    }

    pub fn hasCallback(this: &InternalJSEventCallback): bool {
        return this.function.has();
    }

    pub fn trigger(this: *InternalJSEventCallback, eventType: EventType, globalThis: *JSGlobalObject): bool {
        if (this.function.get()) |callback| {
            _ = callback.call(globalThis, JSValue.js_undefined, &.{JSValue.jsNumber(
                @intFromEnum(eventType),
            )}) catch |err| globalThis.reportActiveExceptionAsUnhandled(err);
            return true;
        }
        return false;
    }

    pub fn deinit(this: *InternalJSEventCallback): void {
        this.function.deinit();
    }
}

enum Fields {
    method,
    headers,
    body,
    mode,
    redirect,
    cache,
    signal,
    url,
}

export class Request {
    url: String = String.empty;
    headers: ?*FetchHeaders = null;
    signal: ?*AbortSignal = null;
    body: *Body.Value.HiveRef;
    js_ref: JSRef = .empty();
    method: Method = Method.GET;
    flags: Flags = .{};
    request_context: jsc.API.AnyRequestContext = jsc.API.AnyRequestContext.Null;
    weak_ptr_data: WeakRef.Data = .empty;
    reported_estimated_size: usize = 0;
    internal_event_callback: InternalJSEventCallback = .{};

    pub const WeakRef = ptr.WeakPtr(Request, "weak_ptr_data");

    pub fn memoryCost(this: &Request): usize {
        return @sizeOf(Request) + this.request_context.memoryCost() + this.url.byteSlice().len + this.body.value.memoryCost();
    }

    pub fn init(
        method: Method,
        request_context: jsc.API.AnyRequestContext,
        https: bool,
        signal: ?*AbortSignal,
        body: *Body.Value.HiveRef,
    ): Request {
        return Request {
            .request_context = request_context,
            .method = method,
            .flags = .{ .https = https },
            .signal = signal,
            .body = body,
        };
    }

    pub fn init2(
        url: String,
        headers: ?*FetchHeaders,
        body: *Body.Value.HiveRef,
        method: Method,
    ): Request {
        return Request {
            .url = url,
            .headers = headers,
            .body = body,
            .method = method,
        };
    }

    pub fn getContentType(this: *Request): JSError!?ZigString.Slice {
        if (this.request_context.getRequest()) |req| {
            if (req.header("content-type")) |val| {
                return ZigString.Slice.fromUTF8NeverFree(val);
            }
        }

        if (this.headers) |head| {
            if (head.fastGet(.ContentType)) |val| {
                return val.toSlice(default_allocator);
            }
        }

        if (this.body.value == .Blob) {
            if (this.body.value.Blob.content_type.len > 0) {
                return ZigString.Slice.fromUTF8NeverFree(this.body.value.Blob.content_type);
            }
        }

        return null;
    }

    pub fn getFormDataEncoding(this: *Request): JSError!?*FormData.AsyncFormData {
        let content_type_slice: ZigString.Slice = (try this.getContentType()) orelse return null;
        defer content_type_slice.deinit();
        let encoding = FormData.Encoding.get(content_type_slice.slice()) orelse return null;
        return FormData.AsyncFormData.init(default_allocator, encoding);
    }

    @c pub fn estimatedSize(this: *Request): usize {
        return this.reported_estimated_size;
    }

    pub fn getRemoteSocketInfo(this: *Request, globalObject: *JSGlobalObject): ?JSValue {
        if (this.request_context.getRemoteSocketInfo()) |info| {
            return JSSocketAddress.create(globalObject, info.ip, info.port, info.is_ipv6);
        }
        return null;
    }

    pub fn calculateEstimatedByteSize(this: *Request): void {
        this.reported_estimated_size = this.body.value.estimatedSize() + this.sizeOfURL() + @sizeOf(Request);
    }

    pub fn getBodyReadableStream(this: *Request, globalObject: *JSGlobalObject): ?webcore.ReadableStream {
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

    pub fn detachReadableStream(this: *Request, globalObject: *JSGlobalObject): void {
        if (this.js_ref.tryGet()) |js_r| {
            js.gc.stream.clear(js_r, globalObject);
        }
        if (this.body.value == .Locked) {
            let old = this.body.value.Locked.readable;
            old.deinit();
            this.body.value.Locked.readable = .{};
        }
    }

    pub fn toJS(this: *Request, globalObject: *JSGlobalObject): JSValue {
        this.calculateEstimatedByteSize();
        let js_value = js.toJSUnchecked(globalObject, this);
        this.js_ref = .initWeak(js_value);
        this.checkBodyStreamRef(globalObject);
        return js_value;
    }

    fn checkBodyStreamRef(this: *Request, globalObject: *JSGlobalObject): void {
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

    pub fn mimeType(this: &Request): []const u8 {
        if (this.headers) |head| {
            if (try head.fastGet(.ContentType)) |content_type| {
                return content_type.slice();
            }
        }

        switch (this.body.value) {
            .Blob => |blob| {
                if (blob.content_type.len > 0) return blob.content_type;
                return http.MimeType.other.value;
            },
            .InternalBlob => return this.body.value.InternalBlob.contentType(),
            .WTFStringImpl => return http.MimeType.text.value,
            .Null, .Error, .Used, .Locked, .Empty => return http.MimeType.other.value,
        }
    }

    pub fn getCache(this: &Request, globalThis: *JSGlobalObject): JSValue {
        return this.flags.cache.toJS(globalThis);
    }

    pub fn getCredentials(_: &Request, globalThis: *JSGlobalObject): JSValue {
        return globalThis.commonStrings().include();
    }

    pub fn getDestination(_: &Request, globalThis: *JSGlobalObject): JSValue {
        return ZigString.init("").toJS(globalThis);
    }

    pub fn getIntegrity(_: &Request, globalThis: *JSGlobalObject): JSValue {
        return ZigString.Empty.toJS(globalThis);
    }

    pub fn getSignal(this: *Request, globalThis: *JSGlobalObject): JSValue {
        if (this.signal) |sig| {
            return sig.toJS(globalThis);
        } else {
            let js_signal = AbortSignal.create(globalThis);
            js_signal.ensureStillAlive();
            if (AbortSignal.fromJS(js_signal)) |sig| {
                this.signal = sig.ref();
            }
            return js_signal;
        }
    }

    pub fn getMethod(this: &Request, globalThis: *JSGlobalObject): JSValue {
        return this.method.toJS(globalThis);
    }

    pub fn getMode(this: &Request, globalThis: *JSGlobalObject): JSValue {
        return this.flags.mode.toJS(globalThis);
    }

    pub fn finalizeWithoutDeinit(this: *Request): void {
        if (this.headers) |headers| {
            headers.deref();
            this.headers = null;
        }

        this.url.deref();
        this.url = String.empty;

        if (this.signal) |signal| {
            signal.unref();
            this.signal = null;
        }
        this.internal_event_callback.deinit();
    }

    pub fn finalize(this: *Request): void {
        this.js_ref.finalize();
        this.finalizeWithoutDeinit();
        _ = this.body.unref();
        if (this.weak_ptr_data.onFinalize()) {
            sys.destroy(this);
        }
    }

    pub fn getRedirect(this: &Request, globalThis: *JSGlobalObject): JSValue {
        return this.flags.redirect.toJS(globalThis);
    }

    pub fn getReferrer(this: &Request, globalObject: *JSGlobalObject): JSValue {
        if (this.headers) |headers_ref| {
            if (headers_ref.get("referrer", globalObject)) |referrer| {
                return ZigString.init(referrer).toJS(globalObject);
            }
        }
        return ZigString.init("").toJS(globalObject);
    }

    pub fn getReferrerPolicy(_: &Request, globalThis: *JSGlobalObject): JSValue {
        return ZigString.init("").toJS(globalThis);
    }

    pub fn getUrl(this: *Request, globalObject: *JSGlobalObject): JSError!JSValue {
        try this.ensureURL();
        return this.url.toJS(globalObject);
    }

    pub fn sizeOfURL(this: &Request): usize {
        if (this.url.length() > 0) return this.url.byteSlice().len;

        if (this.request_context.getRequest()) |req| {
            let req_url = req.url();
            if (req_url.len > 0 and req_url[0] == '/') {
                if (req.header("host")) |host| {
                    let fmt = Output.fmt.HostFormatter {
                        .is_https = this.flags.https,
                        .host = host,
                    };
                    return this.getProtocol().len + req_url.len + sys.fmt.count("{f}", .{fmt});
                }
            }
            return req_url.len;
        }
        return 0;
    }

    pub fn getProtocol(this: &Request): []const u8 {
        if (this.flags.https) return "https://";
        return "http://";
    }

    pub fn ensureURL(this: *Request): !void {
        if (!this.url.isEmpty()) return;

        if (this.request_context.getRequest()) |req| {
            let req_url = req.url();
            if (req_url.len > 0 and req_url[0] == '/') {
                if (req.header("host")) |host| {
                    let fmt = Output.fmt.HostFormatter {
                        .is_https = this.flags.https,
                        .host = host,
                    };
                    let url_bytelength = sys.fmt.count("{s}{f}{s}", .{
                        this.getProtocol(),
                        fmt,
                        req_url,
                    });

                    if (url_bytelength < 128) {
                        let buffer: [128]u8 = undefined;
                        let url = sys.fmt.bufPrint(&buffer, "{s}{f}{s}", .{
                            this.getProtocol(),
                            fmt,
                            req_url,
                        }) catch @panic("Unexpected error while printing URL");

                        let href = jsc.URL.hrefFromString(String.fromBytes(url));
                        if (!href.isEmpty()) {
                            if (href.byteSlice().ptr == url.ptr) {
                                this.url = String.cloneLatin1(url[0..href.length()]);
                                href.deref();
                            } else {
                                this.url = href;
                            }
                        } else {
                            this.url = String.cloneUTF8(url);
                        }
                        return;
                    }

                    if (strings.isAllASCII(host) and strings.isAllASCII(req_url)) {
                        let [s, bytes] = String.createUninitialized(.latin1, url_bytelength);
                        this.url = s;
                        _ = sys.fmt.bufPrint(bytes, "{s}{f}{s}", .{
                            this.getProtocol(),
                            fmt,
                            req_url,
                        }) catch unreachable;
                    } else {
                        let temp_url = try sys.fmt.allocPrint(default_allocator, "{s}{f}{s}", .{
                            this.getProtocol(),
                            fmt,
                            req_url,
                        });
                        defer default_allocator.free(temp_url);
                        this.url = String.cloneUTF8(temp_url);
                    }

                    let href = jsc.URL.hrefFromString(this.url);
                    if (!href.isEmpty()) {
                        this.url.deref();
                        this.url = href;
                    }
                    return;
                }
            }
            this.url = String.cloneUTF8(req_url);
        }
    }

    pub fn getBodyValue(this: *Request): *Body.Value {
        return &this.body.value;
    }

    pub fn doClone(this: *Request, globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
        let this_value = callframe.this();
        let cloned = try this.clone(default_allocator, globalThis);

        let js_wrapper = cloned.toJS(globalThis);
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

    pub fn hasFetchHeaders(this: &Request): bool {
        return this.headers != null;
    }

    pub fn setFetchHeaders(this: *Request, headers: ?*FetchHeaders): void {
        if (this.headers) |old_headers| {
            old_headers.deref();
        }
        this.headers = headers;
    }

    pub fn ensureFetchHeaders(this: *Request, globalThis: *JSGlobalObject): JSError!*FetchHeaders {
        if (this.headers) |head| return head;

        if (this.request_context.getRequest()) |req| {
            this.headers = FetchHeaders.createFromUWS(req);
        } else {
            this.headers = FetchHeaders.createEmpty();
            let content_type: ?[]const u8 = null;
            switch (this.body.value) {
                .Blob => |blob| {
                    content_type = blob.content_type;
                },
                .Locked => |locked| {
                    if (locked.readable.get(globalThis)) |*readable| {
                        switch (readable.ptr) {
                            .Blob => |blob| {
                                content_type = blob.content_type;
                            },
                            else => {},
                        }
                    }
                },
                else => {},
            }

            if (content_type) |ct| {
                if (ct.len > 0) {
                    try this.headers.?.put(.ContentType, ct, globalThis);
                }
            }
        }
        return this.headers.?;
    }

    pub fn getFetchHeadersUnlessEmpty(this: *Request): ?*FetchHeaders {
        if (this.headers == null) {
            if (this.request_context.getRequest()) |req| {
                this.headers = FetchHeaders.createFromUWS(req);
            }
        }
        let headers = this.headers orelse return null;
        if (headers.isEmpty()) return null;
        return headers;
    }

    pub fn getFetchHeaders(this: &Request): ?*FetchHeaders {
        return this.headers;
    }

    pub fn getHeaders(this: *Request, globalThis: *JSGlobalObject): JSError!JSValue {
        return (try this.ensureFetchHeaders(globalThis)).toJS(globalThis);
    }

    pub fn cloneHeaders(this: *Request, globalThis: *JSGlobalObject): JSError!?*FetchHeaders {
        if (this.headers == null) {
            if (this.request_context.getRequest()) |uws_req| {
                this.headers = FetchHeaders.createFromUWS(uws_req);
            }
        }
        if (this.headers) |head| {
            if (head.isEmpty()) return null;
            return head.cloneThis(globalThis);
        }
        return null;
    }

    pub fn cloneInto(
        this: *Request,
        req: *Request,
        allocator: sys.Allocator,
        globalThis: *JSGlobalObject,
        preserve_url: bool,
    ): JSError!void {
        _ = allocator;
        this.ensureURL() catch {};
        let vm = globalThis.bunVM();
        let body_: Body.Value = undefined;
        if (this.js_ref.tryGet()) |js_ref| {
            if (js.gc.stream.get(js_ref)) |stream| {
                let readable = try webcore.ReadableStream.fromJS(stream, globalThis);
                if (readable != null) {
                    body_ = try this.body.value.cloneWithReadableStream(globalThis, &readable.?);
                } else {
                    body_ = try this.body.value.clone(globalThis);
                }
            } else {
                body_ = try this.body.value.clone(globalThis);
            }
        } else {
            body_ = try this.body.value.clone(globalThis);
        }
        defer {
            // error cleanup handled by caller
        }
        let body = try vm.initRequestBodyValue(body_);
        let url = if (preserve_url) req.url else this.url.dupeRef();
        let headers = try this.cloneHeaders(globalThis);

        req.* = Request {
            .body = body,
            .url = url,
            .method = this.method,
            .flags = this.flags,
            .headers = headers,
        };

        if (this.signal) |signal| {
            req.signal = signal.ref();
        }
    }

    pub fn clone(this: *Request, allocator: sys.Allocator, globalThis: *JSGlobalObject): JSError!*Request {
        let req = sys.new(Request, undefined);
        try this.cloneInto(req, allocator, globalThis, false);
        return req;
    }

    pub fn setTimeout(this: *Request, seconds: c_uint): void {
        _ = this.request_context.setTimeout(seconds);
    }
}

@c pub fn Request__setCookiesOnRequestContext(this: *Request, cookieMap: ?*CookieMap): void {
    this.request_context.setCookies(cookieMap);
}

@c pub fn Request__getUWSRequest(this: *Request): ?*uws.Request {
    return this.request_context.getRequest();
}

@c pub fn Request__setInternalEventCallback(
    this: *Request,
    callback: JSValue,
    globalThis: *JSGlobalObject,
): void {
    this.internal_event_callback = InternalJSEventCallback.init(callback, globalThis);
    this.request_context.enableTimeoutEvents();
}

@c pub fn Request__setTimeout(this: *Request, seconds: JSValue, globalThis: *JSGlobalObject): void {
    if (!seconds.isNumber()) {
        globalThis.throw("Failed to set timeout: The provided value is not of type 'number'.", .{}) catch {};
        return;
    }
    this.setTimeout(seconds.to(c_uint));
}

@c pub fn Request__clone(this: *Request, globalThis: *JSGlobalObject): ?*Request {
    return this.clone(default_allocator, globalThis) catch null;
}

@c pub fn Bun__JSRequest__calculateEstimatedByteSize(this: *Request): void {
    this.calculateEstimatedByteSize();
}
