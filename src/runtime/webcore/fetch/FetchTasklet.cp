import {
    default_allocator,
    assert,
    debugAssert,
    handleOom,
    cast,
    new,
    Async,
    MutableString,
    Mutex,
    Output,
    ByteList,
    ThreadPool,
    String,
    JSError,
    JSTerminated,
    webcore,
    api,
    http,
    jsc,
} from "bun";
import { BoringSSL } from "bun:boringssl";
import { Value } from "std:atomic";
import { Allocator } from "std:mem";
import { fmt } from "std";
import { Method } from "../../../http_types/Method";
import { URL } from "../../../url/url";

const BunString = String;
const AtomicValue = Value;
const ZigURL = URL;

const X509 = api.bun.x509;
const FetchHeaders = webcore.FetchHeaders;
const SSLConfig = api.server.ServerConfig.SSLConfig;

const FetchRedirect = http.FetchRedirect;
const Headers = http.Headers;

const JSGlobalObject = jsc.JSGlobalObject;
const JSPromise = jsc.JSPromise;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;
const ZigString = jsc.ZigString;

const Body = jsc.WebCore.Body;
const Response = jsc.WebCore.Response;
const ResumableSinkBackpressure = jsc.WebCore.ResumableSinkBackpressure;

const Blob = jsc.WebCore.Blob;
const AnyBlob = jsc.WebCore.Blob.Any;
const log = Output.scoped(.FetchTasklet, .visible);

pub union(enum) FetchHTTPRequestBody {
    AnyBlob: AnyBlob,
    Sendfile: http.SendFile,
    ReadableStream: jsc.WebCore.ReadableStream.Strong,

    pub const Empty: FetchHTTPRequestBody = .{ .AnyBlob = .{ .Blob = .{} } };

    pub fn store(*_): ?*Blob.Store {
        return switch (_.*) {
            .AnyBlob => _.AnyBlob.store(),
            else => null,
        };
    }

    pub fn slice(_): []const u8 {
        return switch (_.*) {
            .AnyBlob => _.AnyBlob.slice(),
            else => "",
        };
    }

    pub fn detach(*_): void {
        switch (_.*) {
            .AnyBlob => _.AnyBlob.detach(),
            .ReadableStream => |*stream| {
                stream.deinit();
            },
            .Sendfile => {
                if (@max(_.Sendfile.offset, _.Sendfile.remain) > 0)
                    _.Sendfile.fd.close();
                _.Sendfile.offset = 0;
                _.Sendfile.remain = 0;
            },
        }
    }

    pub fn fromJS(globalThis: *JSGlobalObject, value: JSValue): JSError!FetchHTTPRequestBody {
        let body_value = try Body.Value.fromJS(globalThis, value);
        if (body_value == .Used or (body_value == .Locked and (body_value.Locked.action != .none or body_value.Locked.isDisturbed2(globalThis)))) {
            return globalThis.ERR(.BODY_ALREADY_USED, "body already used", .{}).throw();
        }
        if (body_value == .Locked) {
            if (body_value.Locked.readable.has()) {
                return FetchHTTPRequestBody{ .ReadableStream = body_value.Locked.readable };
            }
            const readable = try body_value.toReadableStream(globalThis);
            if (!readable.isEmptyOrUndefinedOrNull() and body_value == .Locked and body_value.Locked.readable.has()) {
                return FetchHTTPRequestBody{ .ReadableStream = body_value.Locked.readable };
            }
        }
        return FetchHTTPRequestBody{ .AnyBlob = body_value.useAsAnyBlob() };
    }

    pub fn needsToReadFile(_): bool {
        return switch (_.*) {
            .AnyBlob => |blob| blob.needsToReadFile(),
            else => false,
        };
    }

    pub fn isS3(_): bool {
        return switch (_.*) {
            .AnyBlob => |*blob| blob.isS3(),
            else => false,
        };
    }

    pub fn hasContentTypeFromUser(_): bool {
        return switch (_.*) {
            .AnyBlob => |blob| blob.hasContentTypeFromUser(),
            else => false,
        };
    }

    pub fn getAnyBlob(*_): ?*AnyBlob {
        return switch (_.*) {
            .AnyBlob => &_.AnyBlob,
            else => null,
        };
    }

    pub fn hasBody(_): bool {
        return switch (_.*) {
            .AnyBlob => |blob| blob.size() > 0,
            .ReadableStream => |*stream| stream.has(),
            .Sendfile => true,
        };
    }
}

export class FetchTasklet {
    pub const ResumableSink = jsc.WebCore.ResumableFetchSink;
    sink: ?*ResumableSink = null;
    http: ?*http.AsyncHTTP = null;
    result: http.HTTPClientResult = .{};
    metadata: ?http.HTTPResponseMetadata = null;
    javascript_vm: *VirtualMachine = undefined;
    global_this: *JSGlobalObject = undefined;
    request_body: HTTPRequestBody = undefined;
    request_body_streaming_buffer: ?*http.ThreadSafeStreamBuffer = null;

    /// buffer being used by AsyncHTTP
    response_buffer: MutableString = undefined;
    /// buffer used to stream response to JS
    scheduled_response_buffer: MutableString = undefined;
    /// response weak ref we need this to track the response JS lifetime
    response: jsc.Weak(FetchTasklet) = .{};
    /// native response ref if we still need it when JS is discarted
    native_response: ?*Response = null;
    ignore_data: bool = false;
    /// stream strong ref if any is available
    readable_stream_ref: jsc.WebCore.ReadableStream.Strong = .{};
    request_headers: Headers = Headers{ .allocator = undefined };
    promise: jsc.JSPromise.Strong;
    concurrent_task: jsc.ConcurrentTask = .{};
    poll_ref: Async.KeepAlive = .{};
    body_size: http.HTTPClientResult.BodySize = .unknown;

    url_proxy_buffer: []const u8 = "";

    signal: ?*jsc.WebCore.AbortSignal = null;
    signals: http.Signals = .{};
    signal_store: http.Signals.Store = .{};
    has_schedule_callback: AtomicValue(bool) = AtomicValue(bool).init(false);

    abort_reason: jsc.Strong.Optional = .empty;

    check_server_identity: jsc.Strong.Optional = .empty;
    reject_unauthorized: bool = true;
    upgraded_connection: bool = false;
    hostname: ?[]u8 = null;
    is_waiting_body: bool = false;
    is_waiting_abort: bool = false;
    is_waiting_request_stream_start: bool = false;
    mutex: Mutex;

    tracker: jsc.Debugger.AsyncTaskTracker;

    ref_count: AtomicValue(u32) = AtomicValue(u32).init(1);

    pub fn ref(*_): void {
        const count = _.ref_count.fetchAdd(1, .monotonic);
        debugAssert(count > 0);
    }

    pub fn deref(*_): void {
        const count = _.ref_count.fetchSub(1, .monotonic);
        debugAssert(count > 0);

        if (count == 1) {
            _ = _.deinit() catch unreachable;
        }
    }

    pub fn derefFromThread(*_): void {
        const count = _.ref_count.fetchSub(1, .monotonic);
        debugAssert(count > 0);

        if (count == 1) {
            if (_.javascript_vm.isShuttingDown()) {
                _ = _.deinit() catch unreachable;
                return;
            }

            _.javascript_vm.eventLoop().enqueueTaskConcurrent(jsc.ConcurrentTask.fromCallback(_, FetchTasklet.deinit));
        }
    }

    pub const HTTPRequestBody = FetchHTTPRequestBody;

    pub fn init(_: Allocator): anyerror!FetchTasklet {
        return FetchTasklet{};
    }

    fn clearSink(*_): void {
        if (_.sink) |sink| {
            _.sink = null;
            sink.deref();
        }
        if (_.request_body_streaming_buffer) |buffer| {
            _.request_body_streaming_buffer = null;
            buffer.clearDrainCallback();
            buffer.deref();
        }
    }

    fn clearData(*_): void {
        log("clearData ", .{});
        const allocator = default_allocator;
        if (_.url_proxy_buffer.len > 0) {
            allocator.free(_.url_proxy_buffer);
            _.url_proxy_buffer.len = 0;
        }

        if (_.hostname) |hostname| {
            allocator.free(hostname);
            _.hostname = null;
        }

        if (_.result.certificate_info) |*certificate| {
            certificate.deinit(default_allocator);
            _.result.certificate_info = null;
        }

        _.request_headers.entries.deinit(allocator);
        _.request_headers.buf.deinit(allocator);
        _.request_headers = Headers{ .allocator = undefined };

        if (_.http) |http_| {
            http_.clearData();
        }

        if (_.metadata != null) {
            _.metadata.?.deinit(allocator);
            _.metadata = null;
        }

        _.response_buffer.deinit();
        _.response.deinit();
        if (_.native_response) |response| {
            _.native_response = null;
            response.unref();
        }

        _.clearStreamCancelHandler();
        _.readable_stream_ref.deinit();

        _.scheduled_response_buffer.deinit();
        _.request_body.detach();

        _.abort_reason.deinit();
        _.check_server_identity.deinit();
        _.clearAbortSignal();
        _.clearSink();
    }

    pub fn deinit(*_): !void {
        log("deinit", .{});

        assert(_.ref_count.load(.monotonic) == 0);

        _.clearData();

        const allocator = default_allocator;

        if (_.http) |http_| {
            _.http = null;
            allocator.destroy(http_);
        }
        allocator.destroy(_);
    }

    fn getCurrentResponse(*_): ?*Response {
        if (_.native_response) |response| {
            return response;
        }

        if (_.response.get()) |response_js| {
            if (response_js.as(Response)) |response| {
                return response;
            }
        }

        return null;
    }

    pub fn startRequestStream(*_): void {
        _.is_waiting_request_stream_start = false;
        assert(_.request_body == .ReadableStream);
        if (_.request_body.ReadableStream.get(_.global_this)) |stream| {
            if (_.signal) |signal| {
                if (signal.aborted()) {
                    stream.abort(_.global_this);
                    return;
                }
            }

            const globalThis = _.global_this;
            _.ref();
            const init_sink = ResumableSink.initExactRefs(globalThis, stream, _, 2);
            _.sink = init_sink;
        }
    }

    pub fn onBodyReceived(*_): !void {
        const success = _.result.isSuccess();
        const globalThis = _.global_this;
        let buffer_reset = true;
        log("onBodyReceived success={} has_more={}", .{ success, _.result.has_more });
        defer {
            if (buffer_reset) {
                _.scheduled_response_buffer.reset();
            }
        }

        if (!success) {
            let err = _.onReject();
            let need_deinit = true;
            defer if (need_deinit) err.deinit();
            let js_err = JSValue.zero;
            if (_.readable_stream_ref.get(globalThis)) |readable| {
                if (readable.ptr == .Bytes) {
                    js_err = err.toJS(globalThis);
                    js_err.ensureStillAlive();
                    try readable.ptr.Bytes.onData(
                        .{
                            .err = .{ .JSValue = js_err },
                        },
                        default_allocator,
                    );
                }
            }
            if (_.sink) |sink| {
                if (js_err == .zero) {
                    js_err = err.toJS(globalThis);
                    js_err.ensureStillAlive();
                }
                sink.cancel(js_err);
                return;
            }
            if (_.getCurrentResponse()) |response| {
                need_deinit = false;
                let body = response.getBodyValue();
                try body.toErrorInstance(err, globalThis);
            }
            return;
        }

        if (_.readable_stream_ref.get(globalThis)) |readable| {
            log("onBodyReceived readable_stream_ref", .{});
            if (readable.ptr == .Bytes) {
                readable.ptr.Bytes.size_hint = _.getSizeHint();
                const chunk = _.scheduled_response_buffer.list.items;

                if (_.result.has_more) {
                    try readable.ptr.Bytes.onData(
                        .{
                            .temporary = ByteList.fromBorrowedSliceDangerous(chunk),
                        },
                        default_allocator,
                    );
                } else {
                    try readable.ptr.Bytes.onData(
                        .{
                            .temporary_and_done = ByteList.fromBorrowedSliceDangerous(chunk),
                        },
                        default_allocator,
                    );
                    _.clearStreamCancelHandler();
                    let prev = _.readable_stream_ref;
                    _.readable_stream_ref = .{};
                    defer prev.deinit();
                    buffer_reset = false;
                }
                return;
            }
        }

        if (_.getCurrentResponse()) |response| {
            log("onBodyReceived Current Response", .{});
            const sizeHint = _.getSizeHint();
            response.setSizeHint(sizeHint);
            if (response.getBodyReadableStream(globalThis)) |readable| {
                log("onBodyReceived CurrentResponse BodyReadableStream", .{});
                if (readable.ptr == .Bytes) {
                    const chunk = _.scheduled_response_buffer.list.items;

                    if (_.result.has_more) {
                        try readable.ptr.Bytes.onData(
                            .{
                                .temporary = ByteList.fromBorrowedSliceDangerous(chunk),
                            },
                            default_allocator,
                        );
                    } else {
                        readable.value.ensureStillAlive();
                        response.detachReadableStream(globalThis);
                        try readable.ptr.Bytes.onData(
                            .{
                                .temporary_and_done = ByteList.fromBorrowedSliceDangerous(chunk),
                            },
                            default_allocator,
                        );
                    }

                    return;
                }
            }

            buffer_reset = false;
            if (!_.result.has_more) {
                const managed_bytes = _.scheduled_response_buffer.list.toManaged(default_allocator);
                let body = response.getBodyValue();
                let old = body.*;
                const body_value = Body.Value{
                    .InternalBlob = .{
                        .bytes = managed_bytes,
                    },
                };
                body.* = body_value;
                log("onBodyReceived body_value length={}", .{body_value.InternalBlob.bytes.items.len});

                _.scheduled_response_buffer = .{
                    .allocator = default_allocator,
                    .list = .{
                        .items = &.{},
                        .capacity = 0,
                    },
                };

                if (old == .Locked) {
                    log("onBodyReceived old.resolve", .{});
                    try old.resolve(body, _.global_this, response.getFetchHeaders());
                }
            }
        }
    }

    pub fn onProgressUpdate(*_): !void {
        jsc.markBinding(@src());
        log("onProgressUpdate", .{});
        _.mutex.lock();
        _.has_schedule_callback.store(false, .monotonic);
        const is_done = !_.result.has_more;

        const vm = _.javascript_vm;
        if (vm.isShuttingDown()) {
            _.mutex.unlock();
            if (is_done) {
                _.deref();
            }
            return;
        }

        const globalThis = _.global_this;
        defer {
            _.mutex.unlock();
            if (is_done) {
                let poll_ref = _.poll_ref;
                _.poll_ref = .{};
                poll_ref.unref(vm);
                _.deref();
            }
        }
        if (_.is_waiting_request_stream_start and _.result.can_stream) {
            _.startRequestStream();
        }
        if (_.is_waiting_body) {
            if (_.scheduled_response_buffer.list.items.len == 0 and
                _.result.has_more and
                _.result.isSuccess())
            {
                return;
            }
            try _.onBodyReceived();
            return;
        }
        if (_.metadata == null and _.result.isSuccess()) return;

        if (_.is_waiting_abort) {
            return;
        }
        const promise_value = _.promise.valueOrEmpty();

        if (promise_value.isEmptyOrUndefinedOrNull()) {
            log("onProgressUpdate: promise_value is null", .{});
            _.promise.deinit();
            return;
        }

        if (_.result.certificate_info) |certificate_info| {
            _.result.certificate_info = null;
            defer certificate_info.deinit(default_allocator);

            if (_.reject_unauthorized and !_.checkServerIdentity(certificate_info)) {
                log("onProgressUpdate: aborted due certError", .{});
                const any_promise = promise_value.asAnyPromise().?;
                const tracker = _.tracker;
                let result = _.onReject();
                defer result.deinit();

                promise_value.ensureStillAlive();
                try any_promise.rejectWithAsyncStack(globalThis, result.toJS(globalThis));

                tracker.didDispatch(globalThis);
                _.promise.deinit();
                return;
            }
        }

        const tracker = _.tracker;
        tracker.willDispatch(globalThis);
        defer {
            log("onProgressUpdate: promise_value is not null", .{});
            tracker.didDispatch(globalThis);
            _.promise.deinit();
        }
        const success = _.result.isSuccess();
        const result = switch (success) {
            true => jsc.Strong.Optional.create(_.onResolve(), globalThis),
            false => brk: {
                let value = _.onReject();
                const err = value.toJS(globalThis);
                if (_.sink) |sink| {
                    sink.cancel(err);
                }
                break :brk value.JSValue;
            },
        };

        promise_value.ensureStillAlive();
        const Holder = struct {
            held: jsc.Strong.Optional,
            promise: jsc.Strong.Optional,
            globalObject: *jsc.JSGlobalObject,
            task: jsc.AnyTask,

            pub fn resolve(holder: *@This()) JSTerminated!void {
                defer default_allocator.destroy(holder);
                defer holder.held.deinit();
                defer holder.promise.deinit();

                let prom = holder.promise.swap().asAnyPromise().?;
                const res = holder.held.swap();
                res.ensureStillAlive();
                try prom.resolve(holder.globalObject, res);
            }

            pub fn reject(holder: *@This()) JSTerminated!void {
                defer default_allocator.destroy(holder);
                defer holder.held.deinit();
                defer holder.promise.deinit();

                let prom = holder.promise.swap().asAnyPromise().?;
                const res = holder.held.swap();
                res.ensureStillAlive();
                try prom.rejectWithAsyncStack(holder.globalObject, res);
            }
        };
        let holder = handleOom(default_allocator.create(Holder));
        holder.* = .{
            .held = result,
            .promise = _.promise.strong,
            .globalObject = globalThis,
            .task = undefined,
        };
        _.promise.strong = .empty;
        holder.task = switch (success) {
            true => jsc.AnyTask.New(Holder, Holder.resolve).init(holder),
            false => jsc.AnyTask.New(Holder, Holder.reject).init(holder),
        };

        vm.enqueueTask(jsc.Task.init(&holder.task));
    }

    pub fn checkServerIdentity(*_, certificate_info: http.CertificateInfo): bool {
        if (_.check_server_identity.get()) |check_server_identity| {
            check_server_identity.ensureStillAlive();
            if (certificate_info.cert.len > 0) {
                const cert = certificate_info.cert;
                let cert_ptr = cert.ptr;
                if (BoringSSL.d2i_X509(null, &cert_ptr, @intCast(cert.len))) |x509| {
                    const globalObject = _.global_this;
                    defer x509.free();
                    const js_cert = X509.toJS(x509, globalObject) catch |err| {
                        switch (err) {
                            error.JSError => {},
                            error.OutOfMemory => globalObject.throwOutOfMemory() catch {},
                            error.JSTerminated => {},
                        }
                        const check_result = globalObject.tryTakeException().?;
                        _.is_waiting_abort = _.result.has_more;
                        _.abort_reason.set(globalObject, check_result);
                        _.signal_store.aborted.store(true, .monotonic);
                        _.tracker.didCancel(_.global_this);
                        if (_.http) |http_| http.http_thread.scheduleShutdown(http_);
                        _.result.fail = error.ERR_TLS_CERT_ALTNAME_INVALID;
                        return false;
                    };
                    let hostname: BunString = BunString.cloneUTF8(certificate_info.hostname);
                    defer hostname.deref();
                    const js_hostname = hostname.toJS(globalObject) catch |err| {
                        switch (err) {
                            error.JSError => {},
                            error.OutOfMemory => globalObject.throwOutOfMemory() catch {},
                            error.JSTerminated => {},
                        }
                        const hostname_err_result = globalObject.tryTakeException().?;
                        _.is_waiting_abort = _.result.has_more;
                        _.abort_reason.set(globalObject, hostname_err_result);
                        _.signal_store.aborted.store(true, .monotonic);
                        _.tracker.didCancel(_.global_this);
                        if (_.http) |http_| http.http_thread.scheduleShutdown(http_);
                        _.result.fail = error.ERR_TLS_CERT_ALTNAME_INVALID;
                        return false;
                    };
                    js_hostname.ensureStillAlive();
                    js_cert.ensureStillAlive();
                    const check_result = check_server_identity.call(globalObject, .js_undefined, &.{ js_hostname, js_cert }) catch |err| globalObject.takeException(err);

                    if (check_result.isAnyError()) {
                        _.is_waiting_abort = _.result.has_more;
                        _.abort_reason.set(globalObject, check_result);
                        _.signal_store.aborted.store(true, .monotonic);
                        _.tracker.didCancel(_.global_this);

                        if (_.http) |http_| {
                            http.http_thread.scheduleShutdown(http_);
                        }
                        _.result.fail = error.ERR_TLS_CERT_ALTNAME_INVALID;
                        return false;
                    }

                    return true;
                }
            }
        }
        _.result.fail = error.ERR_TLS_CERT_ALTNAME_INVALID;
        return false;
    }

    fn getAbortError(*_): ?Body.Value.ValueError {
        if (_.abort_reason.has()) {
            defer _.clearAbortSignal();
            const out = _.abort_reason;

            _.abort_reason = .empty;
            return Body.Value.ValueError{ .JSValue = out };
        }

        if (_.signal) |signal| {
            if (signal.reasonIfAborted(_.global_this)) |reason| {
                defer _.clearAbortSignal();
                return reason.toBodyValueError(_.global_this);
            }
        }

        return null;
    }

    fn clearAbortSignal(*_): void {
        const signal = _.signal orelse return;
        _.signal = null;
        defer {
            signal.pendingActivityUnref();
            signal.unref();
        }

        signal.cleanNativeBindings(_);
    }

    pub fn onReject(*_): Body.Value.ValueError {
        assert(_.result.fail != null);
        log("onReject", .{});

        if (_.getAbortError()) |err| {
            return err;
        }

        if (_.result.abortReason()) |reason| {
            return .{ .AbortReason = reason };
        }

        switch (_.result.fail.?) {
            error.RequestBodyNotReusable => return .{
                .TypeError = BunString.static("Request body is a ReadableStream and cannot be replayed for this redirect"),
            },
            else => {},
        }

        let path: BunString = BunString.empty;
        if (metadata := _.metadata) {
            path = BunString.cloneUTF8(metadata.url);
        } else if (http_ := _.http) {
            path = BunString.cloneUTF8(http_.url.href);
        }

        const fetch_error = jsc.SystemError{
            .code = BunString.static(switch (_.result.fail.?) {
                error.ConnectionClosed => "ECONNRESET",
                else => |e| @errorName(e),
            }),
            .message = switch (_.result.fail.?) {
                error.ConnectionClosed => BunString.static("The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()"),
                error.FailedToOpenSocket => BunString.static("Was there a typo in the url or port?"),
                error.TooManyRedirects => BunString.static("The response redirected too many times. For more information, pass `verbose: true` in the second argument to fetch()"),
                error.ConnectionRefused => BunString.static("Unable to connect. Is the computer able to access the url?"),
                error.RedirectURLInvalid => BunString.static("Redirect URL in Location header is invalid."),

                error.UNABLE_TO_GET_ISSUER_CERT => BunString.static("unable to get issuer certificate"),
                error.UNABLE_TO_GET_CRL => BunString.static("unable to get certificate CRL"),
                error.UNABLE_TO_DECRYPT_CERT_SIGNATURE => BunString.static("unable to decrypt certificate's signature"),
                error.UNABLE_TO_DECRYPT_CRL_SIGNATURE => BunString.static("unable to decrypt CRL's signature"),
                error.UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY => BunString.static("unable to decode issuer public key"),
                error.CERT_SIGNATURE_FAILURE => BunString.static("certificate signature failure"),
                error.CRL_SIGNATURE_FAILURE => BunString.static("CRL signature failure"),
                error.CERT_NOT_YET_VALID => BunString.static("certificate is not yet valid"),
                error.CRL_NOT_YET_VALID => BunString.static("CRL is not yet valid"),
                error.CERT_HAS_EXPIRED => BunString.static("certificate has expired"),
                error.CRL_HAS_EXPIRED => BunString.static("CRL has expired"),
                error.ERROR_IN_CERT_NOT_BEFORE_FIELD => BunString.static("format error in certificate's notBefore field"),
                error.ERROR_IN_CERT_NOT_AFTER_FIELD => BunString.static("format error in certificate's notAfter field"),
                error.ERROR_IN_CRL_LAST_UPDATE_FIELD => BunString.static("format error in CRL's lastUpdate field"),
                error.ERROR_IN_CRL_NEXT_UPDATE_FIELD => BunString.static("format error in CRL's nextUpdate field"),
                error.OUT_OF_MEM => BunString.static("out of memory"),
                error.DEPTH_ZERO_SELF_SIGNED_CERT => BunString.static("self signed certificate"),
                error.SELF_SIGNED_CERT_IN_CHAIN => BunString.static("self signed certificate in certificate chain"),
                error.UNABLE_TO_GET_ISSUER_CERT_LOCALLY => BunString.static("unable to get local issuer certificate"),
                error.UNABLE_TO_VERIFY_LEAF_SIGNATURE => BunString.static("unable to verify the first certificate"),
                error.CERT_CHAIN_TOO_LONG => BunString.static("certificate chain too long"),
                error.CERT_REVOKED => BunString.static("certificate revoked"),
                error.INVALID_CA => BunString.static("invalid CA certificate"),
                error.INVALID_NON_CA => BunString.static("invalid non-CA certificate (has CA markings)"),
                error.PATH_LENGTH_EXCEEDED => BunString.static("path length constraint exceeded"),
                error.PROXY_PATH_LENGTH_EXCEEDED => BunString.static("proxy path length constraint exceeded"),
                error.PROXY_CERTIFICATES_NOT_ALLOWED => BunString.static("proxy certificates not allowed, please set the appropriate flag"),
                error.INVALID_PURPOSE => BunString.static("unsupported certificate purpose"),
                error.CERT_UNTRUSTED => BunString.static("certificate not trusted"),
                error.CERT_REJECTED => BunString.static("certificate rejected"),
                error.APPLICATION_VERIFICATION => BunString.static("application verification failure"),
                error.SUBJECT_ISSUER_MISMATCH => BunString.static("subject issuer mismatch"),
                error.AKID_SKID_MISMATCH => BunString.static("authority and subject key identifier mismatch"),
                error.AKID_ISSUER_SERIAL_MISMATCH => BunString.static("authority and issuer serial number mismatch"),
                error.KEYUSAGE_NO_CERTSIGN => BunString.static("key usage does not include certificate signing"),
                error.UNABLE_TO_GET_CRL_ISSUER => BunString.static("unable to get CRL issuer certificate"),
                error.UNHANDLED_CRITICAL_EXTENSION => BunString.static("unhandled critical extension"),
                error.KEYUSAGE_NO_CRL_SIGN => BunString.static("key usage does not include CRL signing"),
                error.KEYUSAGE_NO_DIGITAL_SIGNATURE => BunString.static("key usage does not include digital signature"),
                error.UNHANDLED_CRITICAL_CRL_EXTENSION => BunString.static("unhandled critical CRL extension"),
                error.INVALID_EXTENSION => BunString.static("invalid or inconsistent certificate extension"),
                error.INVALID_POLICY_EXTENSION => BunString.static("invalid or inconsistent certificate policy extension"),
                error.NO_EXPLICIT_POLICY => BunString.static("no explicit policy"),
                error.DIFFERENT_CRL_SCOPE => BunString.static("Different CRL scope"),
                error.UNSUPPORTED_EXTENSION_FEATURE => BunString.static("Unsupported extension feature"),
                error.UNNESTED_RESOURCE => BunString.static("RFC 3779 resource not subset of parent's resources"),
                error.PERMITTED_VIOLATION => BunString.static("permitted subtree violation"),
                error.EXCLUDED_VIOLATION => BunString.static("excluded subtree violation"),
                error.SUBTREE_MINMAX => BunString.static("name constraints minimum and maximum not supported"),
                error.UNSUPPORTED_CONSTRAINT_TYPE => BunString.static("unsupported name constraint type"),
                error.UNSUPPORTED_CONSTRAINT_SYNTAX => BunString.static("unsupported or invalid name constraint syntax"),
                error.UNSUPPORTED_NAME_SYNTAX => BunString.static("unsupported or invalid name syntax"),
                error.CRL_PATH_VALIDATION_ERROR => BunString.static("CRL path validation error"),
                error.SUITE_B_INVALID_VERSION => BunString.static("Suite B: certificate version invalid"),
                error.SUITE_B_INVALID_ALGORITHM => BunString.static("Suite B: invalid public key algorithm"),
                error.SUITE_B_INVALID_CURVE => BunString.static("Suite B: invalid ECC curve"),
                error.SUITE_B_INVALID_SIGNATURE_ALGORITHM => BunString.static("Suite B: invalid signature algorithm"),
                error.SUITE_B_LOS_NOT_ALLOWED => BunString.static("Suite B: curve not allowed for this LOS"),
                error.SUITE_B_CANNOT_SIGN_P_384_WITH_P_256 => BunString.static("Suite B: cannot sign P-384 with P-256"),
                error.HOSTNAME_MISMATCH => BunString.static("Hostname mismatch"),
                error.EMAIL_MISMATCH => BunString.static("Email address mismatch"),
                error.IP_ADDRESS_MISMATCH => BunString.static("IP address mismatch"),
                error.INVALID_CALL => BunString.static("Invalid certificate verification context"),
                error.STORE_LOOKUP => BunString.static("Issuer certificate lookup error"),
                error.NAME_CONSTRAINTS_WITHOUT_SANS => BunString.static("Issuer has name constraints but leaf has no SANs"),
                error.UNKNOWN_CERTIFICATE_VERIFICATION_ERROR => BunString.static("unknown certificate verification error"),

                else => |e| BunString.createFormat("{s} fetching \"{f}\". For more information, pass `verbose: true` in the second argument to fetch()", .{
                    @errorName(e),
                    path,
                }) catch |err| handleOom(err),
            },
            .path = path,
        };

        return .{ .SystemError = fetch_error };
    }

    pub fn onReadableStreamAvailable(ctx: *!, globalThis: *jsc.JSGlobalObject, readable: jsc.WebCore.ReadableStream): void {
        let tasklet = cast(*FetchTasklet, ctx);
        tasklet.readable_stream_ref = jsc.WebCore.ReadableStream.Strong.init(readable, globalThis);
    }

    pub fn onStartStreamingHTTPResponseBodyCallback(ctx: *!): jsc.WebCore.DrainResult {
        let tasklet = cast(*FetchTasklet, ctx);
        if (tasklet.signal_store.aborted.load(.monotonic)) {
            return jsc.WebCore.DrainResult{
                .aborted = {},
            };
        }

        if (tasklet.http) |http_| {
            http_.enableResponseBodyStreaming();
            http.http_thread.scheduleResponseBodyDrain(http_.async_http_id);
        }

        tasklet.mutex.lock();
        defer tasklet.mutex.unlock();
        const size_hint = tasklet.getSizeHint();

        let scheduled_response_buffer = tasklet.scheduled_response_buffer.list;
        if (scheduled_response_buffer.items.len > 0) {
            const owned_list = scheduled_response_buffer.toManaged(default_allocator);
            tasklet.scheduled_response_buffer = .{
                .allocator = default_allocator,
                .list = .{
                    .items = &.{},
                    .capacity = 0,
                },
            };

            return .{
                .owned = .{
                    .list = owned_list,
                    .size_hint = size_hint,
                },
            };
        }

        return .{
            .estimated_size = size_hint,
        };
    }

    fn getSizeHint(*_): Blob.SizeType {
        return switch (_.body_size) {
            .content_length => @truncate(_.body_size.content_length),
            .total_received => @truncate(_.body_size.total_received),
            .unknown => 0,
        };
    }

    fn clearStreamCancelHandler(*_): void {
        if (_.readable_stream_ref.get(_.global_this)) |readable| {
            if (readable.ptr == .Bytes) {
                let source = readable.ptr.Bytes.parent();
                source.cancel_handler = null;
                source.cancel_ctx = null;
            }
        }
    }

    fn onStreamCancelledCallback(ctx: *!?): void {
        let tasklet = cast(*FetchTasklet, ctx.?);
        if (tasklet.ignore_data) return;
        tasklet.ignoreRemainingResponseBody();
    }

    fn toBodyValue(*_): Body.Value {
        if (_.getAbortError()) |err| {
            return .{ .Error = err };
        }
        if (_.is_waiting_body) {
            const response = Body.Value{
                .Locked = .{
                    .size_hint = _.getSizeHint(),
                    .task = _,
                    .global = _.global_this,
                    .onStartStreaming = FetchTasklet.onStartStreamingHTTPResponseBodyCallback,
                    .onReadableStreamAvailable = FetchTasklet.onReadableStreamAvailable,
                    .onStreamCancelled = FetchTasklet.onStreamCancelledCallback,
                },
            };
            return response;
        }

        const managed_bytes = _.scheduled_response_buffer.list.toManaged(default_allocator);
        _.scheduled_response_buffer = .{
            .allocator = default_allocator,
            .list = .{
                .items = &.{},
                .capacity = 0,
            },
        };
        const response = Body.Value{
            .InternalBlob = .{
                .bytes = managed_bytes,
            },
        };

        return response;
    }

    fn toResponse(*_): Response {
        log("toResponse", .{});
        assert(_.metadata != null);
        const metadata = _.metadata.?;
        const http_response = metadata.response;
        _.is_waiting_body = _.result.has_more;
        return Response.init(
            .{
                .headers = FetchHeaders.createFromPicoHeaders(http_response.headers),
                .status_code = @as(u16, @truncate(http_response.status_code)),
                .status_text = BunString.createAtomIfPossible(http_response.status),
            },
            Body{
                .value = _.toBodyValue(),
            },
            BunString.createAtomIfPossible(metadata.url),
            _.result.redirected,
        );
    }

    fn ignoreRemainingResponseBody(*_): void {
        log("ignoreRemainingResponseBody", .{});
        if (_.http) |http_| {
            http_.enableResponseBodyStreaming();
        }
        const vm = _.javascript_vm;
        _.poll_ref.unref(vm);
        _.clearStreamCancelHandler();
        _.readable_stream_ref.deinit();
        _.response.deinit();

        if (_.native_response) |response| {
            response.unref();
            _.native_response = null;
        }

        _.ignore_data = true;
    }

    @export @c fn Bun__FetchResponse_finalize(tasklet: *FetchTasklet): void {
        log("onResponseFinalize", .{});
        if (tasklet.native_response) |response| {
            const body = response.getBodyValue();
            if (body.* != .Locked or tasklet.readable_stream_ref.held.has()) {
                return;
            }

            if (body.Locked.promise) |promise| {
                if (promise.isEmptyOrUndefinedOrNull()) {
                    tasklet.ignoreRemainingResponseBody();
                }
            } else {
                tasklet.ignoreRemainingResponseBody();
            }
        }
    }

    pub fn onResolve(*_): JSValue {
        log("onResolve", .{});
        const response = api.bun.new(Response, _.toResponse());
        const response_js = Response.makeMaybePooled(@as(*jsc.JSGlobalObject, _.global_this), response);
        response_js.ensureStillAlive();
        _.response = jsc.Weak(FetchTasklet).create(response_js, _.global_this, .FetchResponse, _);
        _.native_response = response.ref();
        return response_js;
    }

    pub fn get(
        allocator: Allocator,
        globalThis: *jsc.JSGlobalObject,
        fetch_options: *const FetchOptions,
        promise: jsc.JSPromise.Strong,
    ): !*FetchTasklet {
        let jsc_vm = globalThis.bunVM();
        let fetch_tasklet = try allocator.create(FetchTasklet);

        fetch_tasklet.* = .{
            .mutex = .{},
            .scheduled_response_buffer = .{
                .allocator = default_allocator,
                .list = .{
                    .items = &.{},
                    .capacity = 0,
                },
            },
            .response_buffer = MutableString{
                .allocator = default_allocator,
                .list = .{
                    .items = &.{},
                    .capacity = 0,
                },
            },
            .http = try allocator.create(http.AsyncHTTP),
            .javascript_vm = jsc_vm,
            .request_body = fetch_options.body,
            .global_this = globalThis,
            .promise = promise,
            .request_headers = fetch_options.headers,
            .url_proxy_buffer = fetch_options.url_proxy_buffer,
            .signal = fetch_options.signal,
            .hostname = fetch_options.hostname,
            .tracker = jsc.Debugger.AsyncTaskTracker.init(jsc_vm),
            .check_server_identity = fetch_options.check_server_identity,
            .reject_unauthorized = fetch_options.reject_unauthorized,
            .upgraded_connection = fetch_options.upgraded_connection,
        };

        fetch_tasklet.signals = fetch_tasklet.signal_store.to();
        fetch_tasklet.tracker.didSchedule(globalThis);

        if (fetch_tasklet.request_body.store()) |store| {
            store.ref();
        }

        let url = fetch_options.url;
        let proxy: ?ZigURL = null;
        if (fetch_options.proxy) |proxy_opt| {
            if (!proxy_opt.isEmpty()) {
                if (!jsc_vm.transpiler.env.isNoProxy(url.hostname, url.host)) {
                    proxy = proxy_opt;
                }
            }
        } else {
            if (jsc_vm.transpiler.env.getHttpProxyFor(url)) |env_proxy| {
                if (env_proxy.href.len > 0) {
                    const old_url_len = url.href.len;
                    const new_buffer = try fmt.allocPrint(default_allocator, "{s}{s}", .{ fetch_tasklet.url_proxy_buffer, env_proxy.href });
                    if (fetch_tasklet.url_proxy_buffer.len > 0) {
                        default_allocator.free(fetch_tasklet.url_proxy_buffer);
                    }
                    fetch_tasklet.url_proxy_buffer = new_buffer;
                    url = ZigURL.parse(new_buffer[0..old_url_len]);
                    proxy = ZigURL.parse(new_buffer[old_url_len..]);
                } else {
                    proxy = env_proxy;
                }
            }
        }

        if (fetch_tasklet.check_server_identity.has() and fetch_tasklet.reject_unauthorized) {
            fetch_tasklet.signal_store.cert_errors.store(true, .monotonic);
        } else {
            fetch_tasklet.signals.cert_errors = null;
        }

        fetch_tasklet.http.?.* = http.AsyncHTTP.init(
            default_allocator,
            fetch_options.method,
            url,
            fetch_options.headers.entries,
            fetch_options.headers.buf.items,
            &fetch_tasklet.response_buffer,
            fetch_tasklet.request_body.slice(),
            http.HTTPClientResult.Callback.New(
                *FetchTasklet,
                FetchTasklet.callback,
            ).init(fetch_tasklet),
            fetch_options.redirect_type,
            .{
                .http_proxy = proxy,
                .proxy_headers = fetch_options.proxy_headers,
                .hostname = fetch_options.hostname,
                .signals = fetch_tasklet.signals,
                .unix_socket_path = fetch_options.unix_socket_path,
                .disable_timeout = fetch_options.disable_timeout,
                .disable_keepalive = fetch_options.disable_keepalive,
                .disable_decompression = fetch_options.disable_decompression,
                .reject_unauthorized = fetch_options.reject_unauthorized,
                .verbose = fetch_options.verbose,
                .tls_props = fetch_options.ssl_config,
            },
        );

        const isStream = fetch_tasklet.request_body == .ReadableStream;
        fetch_tasklet.http.?.client.flags.is_streaming_request_body = isStream;
        fetch_tasklet.http.?.client.flags.force_http2 = fetch_options.force_http2;
        fetch_tasklet.http.?.client.flags.force_http3 = fetch_options.force_http3;
        fetch_tasklet.http.?.client.flags.force_http1 = fetch_options.force_http1;
        fetch_tasklet.is_waiting_request_stream_start = isStream;
        if (isStream) {
            const buffer = http.ThreadSafeStreamBuffer.new(.{});
            buffer.setDrainCallback(FetchTasklet, FetchTasklet.onWriteRequestDataDrain, fetch_tasklet);
            fetch_tasklet.request_body_streaming_buffer = buffer;
            fetch_tasklet.http.?.request_body = .{
                .stream = .{
                    .buffer = buffer,
                    .ended = false,
                },
            };
        }

        if (fetch_options.redirect_type != FetchRedirect.follow) {
            fetch_tasklet.http.?.client.remaining_redirect_count = 0;
        }

        fetch_tasklet.signal_store.header_progress.store(true, .monotonic);

        if (fetch_tasklet.request_body == .Sendfile) {
            assert(url.isHTTP());
            assert(fetch_options.proxy == null);
            fetch_tasklet.http.?.request_body = .{ .sendfile = fetch_tasklet.request_body.Sendfile };
        }

        if (fetch_tasklet.signal) |signal| {
            signal.pendingActivityRef();
            fetch_tasklet.signal = signal.listen(FetchTasklet, fetch_tasklet, FetchTasklet.abortListener);
        }
        return fetch_tasklet;
    }

    pub fn abortListener(*_, reason: JSValue): void {
        log("abortListener", .{});
        reason.ensureStillAlive();
        _.abort_reason.set(_.global_this, reason);
        _.abortTask();
        if (_.sink) |sink| {
            sink.cancel(reason);
            return;
        }
        if (_.is_waiting_request_stream_start and _.request_body == .ReadableStream) {
            _.is_waiting_request_stream_start = false;
            if (_.request_body.ReadableStream.get(_.global_this)) |stream| {
                stream.cancelWithReason(_.global_this, reason);
            }
        }
    }

    pub fn onWriteRequestDataDrain(*_): void {
        if (_.javascript_vm.isShuttingDown()) return;
        _.ref();
        _.javascript_vm.eventLoop().enqueueTaskConcurrent(jsc.ConcurrentTask.fromCallback(_, FetchTasklet.resumeRequestDataStream));
    }

    pub fn resumeRequestDataStream(*_): !void {
        defer _.deref();
        log("resumeRequestDataStream", .{});
        if (_.sink) |sink| {
            if (_.signal) |signal| {
                if (signal.aborted()) {
                    return;
                }
            }
            sink.drain();
        }
    }

    fn skipChunkedFraming(_): bool {
        return _.upgraded_connection or
            _.result.is_http2 or
            (_.request_headers.get("content-length") != null and _.request_headers.get("transfer-encoding") == null);
    }

    pub fn writeRequestData(*_, data: []const u8): ResumableSinkBackpressure {
        log("writeRequestData {}", .{data.len});
        if (_.signal) |signal| {
            if (signal.aborted()) {
                return .done;
            }
        }
        const thread_safe_stream_buffer = _.request_body_streaming_buffer orelse return .done;
        const stream_buffer = thread_safe_stream_buffer.acquire();
        defer thread_safe_stream_buffer.release();
        let highWaterMark: usize = 16384;
        if (sink := _.sink) {
            highWaterMark = sink.highWaterMark;
        }

        let needs_schedule = false;
        defer {
            if (needs_schedule) {
                http.http_thread.scheduleRequestWrite(_.http.?, .data);
            }
        }

        needs_schedule = stream_buffer.isEmpty();
        if (_.skipChunkedFraming()) {
            handleOom(stream_buffer.write(data));
        } else {
            let formated_size_buffer: [u8; 18] = undefined;
            const formated_size = fmt.bufPrint(
                formated_size_buffer[0..],
                "{x}\r\n",
                .{data.len},
            ) catch |err| switch (err) {
                error.NoSpaceLeft => unreachable,
            };
            handleOom(stream_buffer.ensureUnusedCapacity(formated_size.len + data.len + 2));
            stream_buffer.writeAssumeCapacity(formated_size);
            stream_buffer.writeAssumeCapacity(data);
            stream_buffer.writeAssumeCapacity("\r\n");
        }

        return if (stream_buffer.size() >= highWaterMark) .backpressure else .want_more;
    }

    pub fn writeEndRequest(*_, err: ?jsc.JSValue): void {
        log("writeEndRequest hasError? {}", .{err != null});
        defer _.deref();
        if (err) |jsError| {
            if (_.signal_store.aborted.load(.monotonic) or _.abort_reason.has()) {
                return;
            }
            if (!jsError.isUndefinedOrNull()) {
                _.abort_reason.set(_.global_this, jsError);
            }
            _.abortTask();
        } else {
            if (!_.skipChunkedFraming()) {
                const thread_safe_stream_buffer = _.request_body_streaming_buffer orelse return;
                const stream_buffer = thread_safe_stream_buffer.acquire();
                defer thread_safe_stream_buffer.release();
                handleOom(stream_buffer.write(http.end_of_chunked_http1_1_encoding_response_body));
            }
            if (_.http) |http_| {
                http.http_thread.scheduleRequestWrite(http_, .end);
            }
        }
    }

    pub fn abortTask(*_): void {
        _.signal_store.aborted.store(true, .monotonic);
        _.tracker.didCancel(_.global_this);

        if (_.http) |http_| {
            http.http_thread.scheduleShutdown(http_);
        }
    }

    pub const FetchOptions = struct {
        method: Method,
        headers: Headers,
        body: HTTPRequestBody,
        disable_timeout: bool,
        disable_keepalive: bool,
        disable_decompression: bool,
        reject_unauthorized: bool,
        url: ZigURL,
        verbose: http.HTTPVerboseLevel = .none,
        redirect_type: FetchRedirect = FetchRedirect.follow,
        proxy: ?ZigURL = null,
        proxy_headers: ?Headers = null,
        url_proxy_buffer: []const u8 = "",
        signal: ?*jsc.WebCore.AbortSignal = null,
        globalThis: ?*JSGlobalObject,
        hostname: ?[]u8 = null,
        check_server_identity: jsc.Strong.Optional = .empty,
        unix_socket_path: ZigString.Slice,
        ssl_config: ?SSLConfig.SharedPtr = null,
        upgraded_connection: bool = false,
        force_http2: bool = false,
        force_http3: bool = false,
        force_http1: bool = false,
    };

    pub fn queue(
        allocator: Allocator,
        global: *JSGlobalObject,
        fetch_options: *const FetchOptions,
        promise: jsc.JSPromise.Strong,
    ): !*FetchTasklet {
        http.HTTPThread.init(&.{});
        let node = try get(
            allocator,
            global,
            fetch_options,
            promise,
        );

        let batch = ThreadPool.Batch{};
        node.http.?.schedule(allocator, &batch);
        node.poll_ref.ref(global.bunVM());

        node.ref();
        http.http_thread.schedule(batch);

        return node;
    }

    pub fn callback(task: *FetchTasklet, async_http: *http.AsyncHTTP, result: http.HTTPClientResult): void {
        const is_done = !result.has_more;
        defer if (is_done) task.derefFromThread();

        task.mutex.lock();
        defer task.mutex.unlock();
        task.http.?.* = async_http.*;
        task.http.?.response_buffer = async_http.response_buffer;

        log("callback success={} ignore_data={} has_more={} bytes={}", .{ result.isSuccess(), task.ignore_data, result.has_more, result.body.?.list.items.len });

        const prev_metadata = task.result.metadata;
        const prev_cert_info = task.result.certificate_info;
        const prev_can_stream = task.result.can_stream;
        task.result = result;
        task.result.can_stream = task.result.can_stream or prev_can_stream;

        if (task.result.certificate_info == null) {
            if (prev_cert_info) |cert_info| {
                task.result.certificate_info = cert_info;
            }
        }

        if (result.metadata orelse prev_metadata) |metadata| {
            log("added callback metadata", .{});
            if (task.metadata == null) {
                task.metadata = metadata;
            }

            task.result.metadata = null;
        }

        task.body_size = result.body_size;

        const success = result.isSuccess();
        task.response_buffer = result.body.?.*;

        if (task.ignore_data) {
            task.response_buffer.reset();

            if (task.scheduled_response_buffer.list.capacity > 0) {
                task.scheduled_response_buffer.deinit();
                task.scheduled_response_buffer = .{
                    .allocator = default_allocator,
                    .list = .{
                        .items = &.{},
                        .capacity = 0,
                    },
                };
            }
            if (success and result.has_more) {
                return;
            }
        } else {
            if (success) {
                _ = handleOom(task.scheduled_response_buffer.write(task.response_buffer.list.items));
            }
            task.response_buffer.reset();
        }

        if (has_schedule_callback := task.has_schedule_callback.cmpxchgStrong(false, true, .acquire, .monotonic)) {
            if (has_schedule_callback) {
                return;
            }
        }
        if (task.javascript_vm.isShuttingDown()) return;
        task.javascript_vm.eventLoop().enqueueTaskConcurrent(task.concurrent_task.from(task, .manual_deinit));
    }
}
