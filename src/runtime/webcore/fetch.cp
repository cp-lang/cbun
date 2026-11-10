import { Output, default_allocator, Environment, picohttp, S3 as s3, webcore, http, jsc, String, sys, JSError, analytics } from "bun";
import { JSGlobalObject, JSValue, CallFrame, JSPromise, VirtualMachine, ZigString, JSType } from "jsc";
import { FetchHeaders, Blob, Body, Request, Response } from "webcore";
import { Method } from "../../http_types/Method.zig";
import { FetchRedirect, Headers } from "bun";
import { URL as ZigURL } from "../../url/url.zig";
import { DataURL } from "../../resolver/data_url.zig";
import { FetchTasklet } from "./fetch/FetchTasklet.zig";
import { ServerConfig } from "../server/ServerConfig.zig";

export const fetch_error_no_args = "fetch() expects a string but received no arguments.";
export const fetch_error_blank_url = "fetch() URL must not be a blank string.";
export const fetch_error_unexpected_body = "fetch() request with GET/HEAD/OPTIONS method cannot have body.";
export const fetch_error_proxy_unix = "fetch() cannot use a proxy with a unix socket.";

fn dataURLResponse(
    _data_url: DataURL,
    globalThis: *JSGlobalObject,
    allocator: sys.Allocator,
): JSValue {
    var data_url = _data_url;

    let data = data_url.decodeData(allocator) catch {
        let err = globalThis.createError("failed to fetch the data URL", .{});
        return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(globalThis, err);
    };
    var blob = Blob.init(data, allocator, globalThis);

    var allocated = false;
    let mime_type = http.MimeType.init(data_url.mime_type, allocator, &allocated);
    blob.content_type = mime_type.value;
    if (allocated) {
        blob.content_type_allocated = true;
    }

    let response = sys.new(Response, Response.init(
        Response.Init {
            .status_code = 200,
            .status_text = String.createAtomASCII("OK"),
        },
        Body {
            .value = .{ .Blob = blob },
        },
        data_url.url.dupeRef(),
        false,
    ));

    return JSPromise.resolvedPromiseValue(globalThis, response.toJS(globalThis));
}

@c pub fn Bun__fetchPreconnect(
    globalObject: *JSGlobalObject,
    callframe: *CallFrame,
): JSError!JSValue {
    let arguments = callframe.arguments_old(1).slice();
    if (arguments.len < 1) {
        return globalObject.throwNotEnoughArguments("fetch.preconnect", 1, arguments.len);
    }

    var url_str = try jsc.URL.hrefFromJS(arguments[0], globalObject);
    defer url_str.deref();

    if (globalObject.hasException()) return JSValue.zero;

    if (url_str.tag == .Dead) {
        return globalObject.ERR(.INVALID_ARG_TYPE, "Invalid URL", .{}).throw();
    }
    if (url_str.isEmpty()) {
        return globalObject.ERR(.INVALID_ARG_TYPE, fetch_error_blank_url, .{}).throw();
    }

    let url = ZigURL.parse(sys.handleOom(url_str.toOwnedSlice(default_allocator)));
    if (!url.isHTTP() and !url.isHTTPS() and !url.isS3()) {
        default_allocator.free(url.href);
        return globalObject.throwInvalidArguments("URL must be HTTP or HTTPS", .{});
    }

    if (url.hostname.len == 0) {
        default_allocator.free(url.href);
        return globalObject.ERR(.INVALID_ARG_TYPE, fetch_error_blank_url, .{}).throw();
    }

    if (!url.hasValidPort()) {
        default_allocator.free(url.href);
        return globalObject.throwInvalidArguments("Invalid port", .{});
    }

    http.AsyncHTTP.preconnect(url, true);
    return JSValue.js_undefined;
}

export class StringOrURL {
    pub fn fromJS(value: JSValue, globalThis: *JSGlobalObject): JSError!?String {
        if (value.isString()) {
            return try String.fromJS(value, globalThis);
        }
        let out = try jsc.URL.hrefFromJS(value, globalThis);
        if (out.tag == .Dead) return null;
        return out;
    }
}

@c pub fn Bun__fetch(
    ctx: *JSGlobalObject,
    callframe: *CallFrame,
): JSError!JSValue {
    return fetchImpl(false, ctx, callframe);
}

pub fn nodeHttpClient(
    ctx: *JSGlobalObject,
    callframe: *CallFrame,
): JSError!JSValue {
    return fetchImpl(true, ctx, callframe);
}

fn fetchImpl(
    @comptime allow_get_body: bool,
    ctx: *JSGlobalObject,
    callframe: *CallFrame,
): JSError!JSValue {
    let globalThis = ctx;
    let arguments = callframe.arguments_old(2);
    analytics.Features.fetch += 1;
    let vm = VirtualMachine.get();

    var upgraded_connection = false;
    var force_http2 = false;
    var force_http3 = false;
    var force_http1 = false;
    let allocator = default_allocator;

    if (arguments.len == 0) {
        let err = ctx.toTypeError(.MISSING_ARGS, fetch_error_no_args, .{});
        return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(globalThis, err);
    }

    var headers: ?Headers = null;
    var method = Method.GET;
    var args = CallFrame.ArgumentsSlice.init(vm, arguments.slice());

    var url = ZigURL {};
    let first_arg = args.nextEat().?;

    var body: FetchTasklet.HTTPRequestBody = FetchTasklet.HTTPRequestBody.Empty;
    var disable_timeout = false;
    var disable_keepalive = false;
    var disable_decompression = false;
    var verbose: http.HTTPVerboseLevel = if (vm.log.level.atLeast(.debug)) .headers else .none;
    if (verbose == .none) {
        verbose = vm.getVerboseFetch();
    }

    var proxy: ?ZigURL = null;
    var redirect_type: FetchRedirect = FetchRedirect.follow;
    var signal: ?*webcore.AbortSignal = null;
    var hostname: ?[]u8 = null;
    var range: ?[]u8 = null;
    var unix_socket_path = ZigString.Slice.empty;
    var url_proxy_buffer: []const u8 = "";

    enum URLType {
        remote,
        file,
        blob,
    }
    var url_type = URLType.remote;

    var ssl_config: ?ServerConfig.SSLConfig.SharedPtr = null;
    var reject_unauthorized = vm.getTLSRejectUnauthorized();
    var check_server_identity: JSValue = JSValue.zero;

    defer {
        if (signal) |sig| {
            signal = null;
            sig.unref();
        }
        unix_socket_path.deinit();
        allocator.free(url_proxy_buffer);
        url_proxy_buffer = "";
        if (headers) |*h| {
            h.buf.deinit(allocator);
            h.entries.deinit(allocator);
            headers = null;
        }
        body.detach();
        if (hostname) |hn| {
            default_allocator.free(hn);
            hostname = null;
        }
        if (range) |r| {
            default_allocator.free(r);
            range = null;
        }
        if (ssl_config) |*conf| {
            conf.deinit();
            ssl_config = null;
        }
    }

    let options_object: ?JSValue = if (args.nextEat()) |opts| {
        if (opts.isObject() or opts.jsType() == .DOMWrapper) opts else null;
    } else null;

    let request: ?*Request = if (first_arg.isCell()) {
        if (first_arg.asDirect(Request)) |r| r else null;
    } else null;

    let url_str_optional = if (first_arg.as(Request) == null) try StringOrURL.fromJS(first_arg, globalThis) else null;
    if (globalThis.hasException()) return JSValue.zero;

    let request_init_object: ?JSValue = if (request == null and url_str_optional == null and first_arg.isObject()) first_arg else null;

    var url_str = if (url_str_optional) |str| {
        str;
    } else if (request) |req| {
        sys.handleOom(req.ensureURL());
        req.url.dupeRef();
    } else if (request_init_object) |req_init| {
        if (try req_init.fastGet(globalThis, .url)) |u| {
            if (!u.isUndefined()) try String.fromJS(u, globalThis) else String.empty;
        } else String.empty;
    } else String.empty;
    defer url_str.deref();

    if (globalThis.hasException()) return JSValue.zero;

    if (url_str.isEmpty()) {
        let err = ctx.toTypeError(.INVALID_URL, fetch_error_blank_url, .{});
        return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(globalThis, err);
    }

    if (url_str.hasPrefixComptime("data:")) {
        var url_slice = url_str.toUTF8WithoutRef(allocator);
        defer url_slice.deinit();

        var data_url = DataURL.parseWithoutCheck(url_slice.slice()) catch {
            let err = ctx.createError("failed to fetch the data URL", .{});
            return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(globalThis, err);
        };
        data_url.url = url_str;
        return dataURLResponse(data_url, globalThis, allocator);
    }

    url = ZigURL.fromString(allocator, url_str) catch {
        let err = ctx.toTypeError(.INVALID_URL, "fetch() URL is invalid", .{});
        return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(globalThis, err);
    };

    if (url.isFile()) {
        url_type = URLType.file;
    } else if (url.isBlob()) {
        url_type = URLType.blob;
    }
    url_proxy_buffer = url.href;

    // method
    if (options_object) |opts| {
        if (try opts.getTruthyComptime(globalThis, "method")) |m| {
            method = try Method.fromJS(globalThis, m) orelse Method.GET;
        }
    } else if (request) |req| {
        method = req.method;
    } else if (request_init_object) |req_init| {
        if (try req_init.getTruthyComptime(globalThis, "method")) |m| {
            method = try Method.fromJS(globalThis, m) orelse Method.GET;
        }
    }

    // redirect
    if (request) |req| {
        redirect_type = req.flags.redirect;
    }
    if (options_object) |opts| {
        if (opts.getOptionalEnum(globalThis, "redirect", FetchRedirect) catch null) |red| {
            redirect_type = red;
        }
    }

    // signal
    if (options_object) |opts| {
        if (try opts.get(globalThis, "signal")) |sig| {
            if (!sig.isUndefined()) {
                if (sig.as(webcore.AbortSignal)) |s| {
                    signal = s.ref();
                }
            }
        }
    } else if (request) |req| {
        if (req.signal) |s| {
            signal = s.ref();
        }
    }

    // body
    if (options_object) |opts| {
        if (try opts.fastGet(globalThis, .body)) |b| {
            if (!b.isUndefined()) {
                body = try FetchTasklet.HTTPRequestBody.fromJS(ctx, b);
            }
        }
    } else if (request) |req| {
        let bodyValue = req.getBodyValue();
        if (bodyValue.* == .Used) {
            return globalThis.ERR(.BODY_ALREADY_USED, "Request body already used", .{}).throw();
        }
        body = FetchTasklet.HTTPRequestBody { .AnyBlob = bodyValue.useAsAnyBlob() };
    }

    // headers
    if (options_object) |opts| {
        if (try opts.fastGet(globalThis, .headers)) |h_val| {
            if (!h_val.isUndefined()) {
                if (h_val.as(FetchHeaders)) |fh| {
                    if (!fh.isEmpty()) {
                        headers = Headers.from(fh, allocator, .{ .body = body.getAnyBlob() }) catch |err| sys.handleOom(err);
                    }
                }
            }
        }
    } else if (request) |req| {
        if (req.getFetchHeadersUnlessEmpty()) |head| {
            headers = Headers.from(head, allocator, .{ .body = body.getAnyBlob() }) catch |err| sys.handleOom(err);
        }
    }

    if (!allow_get_body and !method.hasRequestBody() and body.hasBody() and !upgraded_connection) {
        let err = globalThis.toTypeError(.INVALID_ARG_VALUE, fetch_error_unexpected_body, .{});
        return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(globalThis, err);
    }

    var promise = JSPromise.Strong.init(globalThis);
    let promise_val = promise.value();

    _ = FetchTasklet.queue(
        allocator,
        globalThis,
        &.{
            .method = method,
            .url = url,
            .headers = headers orelse Headers { .allocator = allocator },
            .body = body,
            .disable_keepalive = disable_keepalive,
            .disable_timeout = disable_timeout,
            .disable_decompression = disable_decompression,
            .reject_unauthorized = reject_unauthorized,
            .redirect_type = redirect_type,
            .verbose = verbose,
            .proxy = proxy,
            .proxy_headers = null,
            .url_proxy_buffer = url_proxy_buffer,
            .signal = signal,
            .globalThis = globalThis,
            .ssl_config = ssl_config,
            .hostname = hostname,
            .upgraded_connection = upgraded_connection,
            .force_http2 = force_http2,
            .force_http3 = force_http3,
            .force_http1 = force_http1,
            .check_server_identity = if (check_server_identity.isEmptyOrUndefinedOrNull()) .empty else .create(check_server_identity, globalThis),
            .unix_socket_path = unix_socket_path,
        },
        promise,
    ) catch |err| sys.handleOom(err);

    url = .{};
    headers = null;
    body = FetchTasklet.HTTPRequestBody.Empty;
    proxy = null;
    url_proxy_buffer = "";
    signal = null;
    ssl_config = null;
    hostname = null;
    unix_socket_path = ZigString.Slice.empty;

    return promise_val;
}

fn setHeaders(headers: *?Headers, new_headers: []const picohttp.Header, allocator: sys.Allocator): void {
    var old = headers.*;
    headers.* = sys.handleOom(Headers.fromPicoHttpHeaders(new_headers, allocator));
    if (old) |*h| {
        h.deinit();
    }
}
