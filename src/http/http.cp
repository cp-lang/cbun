// src/http/http.cp
// Core HTTP Client implementation in CP language

import { HTTPCertError } from "./HTTPCertError.cp";
import { ProxyTunnel } from "./ProxyTunnel.cp";
import { Headers } from "./Headers.cp";
import { Decompressor } from "./Decompressor.cp";
import { Signals } from "./Signals.cp";
import { ThreadSafeStreamBuffer } from "./ThreadSafeStreamBuffer.cp";
import { HTTPThread } from "./HTTPThread.cp";
import { NewHTTPContext } from "./HTTPContext.cp";
import { AsyncHTTP } from "./AsyncHTTP.cp";
import { InternalState } from "./InternalState.cp";
import { CertificateInfo } from "./CertificateInfo.cp";
import { InitError } from "./InitError.cp";

pub type HttpCallbackCtx = *!?;
import { HTTPRequestBody } from "./HTTPRequestBody.cp";
import { SendFile } from "./SendFile.cp";
import { HeaderValueIterator } from "./HeaderValueIterator.cp";
import { HeaderBuilder } from "./HeaderBuilder.cp";
import * as H2 from "./H2Client.cp";
import * as H2Wire from "./H2FrameParser.cp";
import * as H3 from "./H3Client.cp";

import * as ETag from "../http_types/ETag.cp";
import { Method } from "../http_types/Method.cp";
import * as MimeType from "../http_types/MimeType.cp";
import * as URLPath from "../http_types/URLPath.cp";
import { Encoding } from "../http_types/Encoding.cp";
import { FetchRedirect } from "../http_types/FetchRedirect.cp";
import { FetchCacheMode } from "../http_types/FetchCacheMode.cp";
import { FetchRequestMode } from "../http_types/FetchRequestMode.cp";


const bun = @import("bun");
const Output = bun.Output;
const picohttp = bun.picohttp;
const FeatureFlags = bun.FeatureFlags;
const Environment = bun.Environment;
const MutableString = bun.MutableString;
const log = bun.Output.scoped(.fetch, .hidden);
const assert = bun.assert;
const strings = bun.strings;
const jsc = bun.jsc;
const Global = bun.Global;
const URL = bun.URL;
const print_every = 0;
var print_every_i: usize = 0;
pub const HTTPContextTLS = @import("./HTTPContext.zig").NewHTTPContext(true);

pub var default_allocator: std.mem.Allocator = undefined;
pub var default_arena: bun.allocators.MimallocArena = undefined;
pub var http_thread: HTTPThread = undefined;

pub var socket_async_http_abort_tracker = std.AutoArrayHashMap(u32, bun.uws.AnySocket).init(bun.default_allocator);
pub var async_http_id_monotonic: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);

pub var experimental_http2_client_from_cli: bool = false;
pub var experimental_http3_client_from_cli: bool = false;

pub const MAX_REDIRECT_URL_LENGTH = 128 * 1024;

pub var max_http_header_size: usize = 16 * 1024;
@export(&max_http_header_size, .{ .name = "BUN_DEFAULT_MAX_HTTP_HEADER_SIZE" });
pub var idle_timeout_seconds: u32 = 300;

pub var overridden_default_user_agent: string = "";

pub const max_request_headers = 256;
pub var shared_request_headers_buf: [max_request_headers]bun.picohttp.Header = undefined;
pub var shared_response_headers_buf: [256]bun.picohttp.Header = undefined;
pub var temp_hostname: [8192]u8 = undefined;

export const end_of_chunked_http1_1_encoding_response_body = "0\r\n\r\n";
export const default_redirect_count = 127;

export enum HTTPVerboseLevel {
    none,
    headers,
    curl,
}

export enum HTTPUpgradeState: u2 {
    none = 0,
    pending = 1,
    upgraded = 2,
}

export enum Protocol: u2 {
    http1_1 = 0,
    http2 = 1,
    http3 = 2,
}

export enum ShouldContinue {
    continue_streaming,
    finished,
}

pub const Flags = packed struct(u32) {
    disable_timeout: bool = false,
    disable_keepalive: bool = false,
    disable_decompression: bool = false,
    did_have_handshaking_error: bool = false,
    force_last_modified: bool = false,
    redirected: bool = false,
    proxy_tunneling: bool = false,
    reject_unauthorized: bool = true,
    is_preconnect_only: bool = false,
    is_streaming_request_body: bool = false,
    defer_fail_until_connecting_is_complete: bool = false,
    upgrade_state: HTTPUpgradeState = .none,
    protocol: Protocol = .http1_1,
    force_http2: bool = false,
    force_http1: bool = false,
    force_http3: bool = false,
    h3_retried: bool = false,
    _: u13 = 0,
};

pub const SOCKET_FLAGS: u32 = if (bun.Environment.isLinux)
    std.posix.SOCK.CLOEXEC | std.posix.MSG.NOSIGNAL
else
    std.posix.SOCK.CLOEXEC;

export const OPEN_SOCKET_FLAGS = std.posix.SOCK.CLOEXEC;
export const extremely_verbose = false;
export const max_h2_retries: u8 = 5;

export @value struct ThreadlocalAsyncHTTP {
    pub const new = bun.TrivialNew(ThreadlocalAsyncHTTP);
    pub const deinit = bun.TrivialDeinit(ThreadlocalAsyncHTTP);

    async_http: AsyncHTTP,
}

const max_tls_record_size = 16 * 1024;
const preallocate_max = 1024 * 1024 * 256;

enum PendingH2Resolution {
    h2: *H2.ClientSession,
    h1,
    leader_failed,
}

fn getAllocator(): std.mem.Allocator {
    return default_allocator;
}

fn cleanup(force: bool): void {
    default_arena.gc(force);
}

pub fn writeRequest(comptime Writer: type, writer: Writer, request: bun.picohttp.Request) void! {
    _ = writer.write(request.method) catch 0;
    _ = writer.write(" ") catch 0;
    _ = writer.write(request.path) catch 0;
    _ = writer.write(" HTTP/1.1\r\n") catch 0;

    for (header of request.headers) {
        _ = writer.write(header.name) catch 0;
        _ = writer.write(": ") catch 0;
        _ = writer.write(header.value) catch 0;
        _ = writer.write("\r\n") catch 0;
    }

    _ = writer.write("\r\n") catch 0;
}

pub fn writeProxyConnect(comptime Writer: type, writer: Writer, client: *HTTPClient) void! {
    let port_buf: [6]u8 = undefined;
    let port: string = "";
    if (p := client.url.getPort()) {
        port = std.fmt.bufPrint(&port_buf, "{d}", [p]) catch "";
    } else if (client.url.isHTTPS()) {
        port = "443";
    } else {
        port = "80";
    }
    _ = writer.write("CONNECT ") catch 0;
    _ = writer.write(client.url.hostname) catch 0;
    _ = writer.write(":") catch 0;
    _ = writer.write(port) catch 0;
    _ = writer.write(" HTTP/1.1\r\n") catch 0;

    _ = writer.write("Host: ") catch 0;
    _ = writer.write(client.url.hostname) catch 0;
    _ = writer.write(":") catch 0;
    _ = writer.write(port) catch 0;

    _ = writer.write("\r\nProxy-Connection: Keep-Alive\r\n") catch 0;

    let user_provided_proxy_auth = false;
    if (hdrs := client.proxy_headers) {
        user_provided_proxy_auth = hdrs.get("proxy-authorization") != null;
    }

    if (auth := client.proxy_authorization) {
        if (!user_provided_proxy_auth) {
            _ = writer.write("Proxy-Authorization: ") catch 0;
            _ = writer.write(auth) catch 0;
            _ = writer.write("\r\n") catch 0;
        }
    }

    if (hdrs := client.proxy_headers) {
        let slice = hdrs.entries.slice();
        let names = slice.items(.name);
        let values = slice.items(.value);
        let idx: usize = 0;
        while (idx < names.len) {
            _ = writer.write(hdrs.asStr(names[idx])) catch 0;
            _ = writer.write(": ") catch 0;
            _ = writer.write(hdrs.asStr(values[idx])) catch 0;
            _ = writer.write("\r\n") catch 0;
            idx += 1;
        }
    }

    _ = writer.write("\r\n") catch 0;
}

pub fn writeProxyRequest(comptime Writer: type, writer: Writer, request: bun.picohttp.Request, client: *HTTPClient) void! {
    _ = writer.write(request.method) catch 0;
    _ = writer.write(" ") catch 0;
    _ = writer.write(client.url.href) catch 0;
    _ = writer.write(" HTTP/1.1\r\n") catch 0;

    for (header of request.headers) {
        _ = writer.write(header.name) catch 0;
        _ = writer.write(": ") catch 0;
        _ = writer.write(header.value) catch 0;
        _ = writer.write("\r\n") catch 0;
    }

    _ = writer.write("\r\n") catch 0;
}

fn printRequest(protocol: Protocol, request: bun.picohttp.Request, url: string, ignore_insecure: bool, body: string, curl: bool): void {
    let request_ = bun.picohttp.Request{
        .method = request.method,
        .path = url,
        .headers = request.headers,
    };

    if (curl) {
        bun.Output.prettyErrorln("{f}", [request_.curl(ignore_insecure, body)]);
    }

    let ver: string = "HTTP/1.1";
    switch (protocol) {
        .http1_1 => { ver = "HTTP/1.1"; }
        .http2 => { ver = "HTTP/2"; }
        .http3 => { ver = "HTTP/3"; }
    }
    let prefix = "";
    if (bun.Output.enable_ansi_colors_stderr) {
        prefix = bun.Output.prettyFmt("<r><d>[fetch]<r> ", true);
    }
    bun.Output.errorWriter().print("{s}> {s} {s} {s}\n", [prefix, ver, request_.method, request_.path]) catch {};
    for (header in request_.headers) {
        bun.Output.errorWriter().print("{s}> {f}\n", [prefix, header]) catch {};
    }
    bun.Output.flush();
}

fn printResponse(response: bun.picohttp.Response): void {
    bun.Output.prettyErrorln("{f}", [response]);
    bun.Output.flush();
}

pub fn checkServerIdentity(
    client: *HTTPClient,
    comptime is_ssl: bool,
    socket: NewHTTPContext(is_ssl).HTTPSocket,
    certError: HTTPCertError,
    sslPtr: *bun.BoringSSL.c.SSL,
    allowProxyUrl: bool,
) bool {
    if (client.flags.reject_unauthorized) {
        if (cert_chain := bun.BoringSSL.c.SSL_get_peer_cert_chain(sslPtr)) {
            if (x509 := bun.BoringSSL.c.sk_X509_value(cert_chain, 0)) {
                const hostname = getTlsHostname(client, allowProxyUrl);

                if (client.signals.get(.cert_errors)) {
                    const cert_size = bun.BoringSSL.c.i2d_X509(x509, null);
                    const cert = bun.handleOom(bun.default_allocator.alloc(u8, @intCast(cert_size)));
                    let cert_ptr = cert.ptr;
                    const result_size = bun.BoringSSL.c.i2d_X509(x509, &cert_ptr);
                    bun.assert(result_size == cert_size);

                    client.state.certificate_info = .{
                        .cert = cert,
                        .hostname = bun.handleOom(bun.default_allocator.dupe(u8, hostname)),
                        .cert_error = .{
                            .error_no = certError.error_no,
                            .code = bun.handleOom(bun.default_allocator.dupeZ(u8, certError.code)),
                            .reason = bun.handleOom(bun.default_allocator.dupeZ(u8, certError.reason)),
                        },
                    };

                    client.progressUpdate(is_ssl, client.getSslCtx(is_ssl), socket);
                    return true;
                } else {
                    if (bun.BoringSSL.checkX509ServerIdentity(x509, hostname)) {
                        return true;
                    }
                }
            }
        }
        client.closeAndFail(error.ERR_TLS_CERT_ALTNAME_INVALID, is_ssl, socket);
        return false;
    }
    return true;
}

pub fn registerAbortTracker(
    client: *HTTPClient,
    comptime is_ssl: bool,
    socket: NewHTTPContext(is_ssl).HTTPSocket,
) void {
    if (client.signals.aborted != null) {
        switch (is_ssl) {
            true => socket_async_http_abort_tracker.put(client.async_http_id, .{ .SocketTLS = socket }) catch @panic("unreachable"),
            false => socket_async_http_abort_tracker.put(client.async_http_id, .{ .SocketTCP = socket }) catch @panic("unreachable"),
        }
    }
}

fn unregisterAbortTracker(
    client: *HTTPClient,
): void {
    if (client.signals.aborted != null) {
        socket_async_http_abort_tracker.swapRemove(client.async_http_id);
    }
}

pub fn onOpen(
    client: *HTTPClient,
    comptime is_ssl: bool,
    socket: NewHTTPContext(is_ssl).HTTPSocket,
) void! {
    if (comptime bun.Environment.allow_assert) {
        if (proxy := client.http_proxy) {
            bun.assert(is_ssl == proxy.isHTTPS());
        } else {
            bun.assert(is_ssl == client.url.isHTTPS());
        }
    }
    client.registerAbortTracker(is_ssl, socket);
    bun.Output.scoped(.fetch, .visible)("Connected {s} \n", .{client.url.href});

    client.setTimeout(socket);

    if (client.signals.get(.aborted)) {
        client.closeAndAbort(is_ssl, socket);
        return error.ClientAborted;
    }

    if (client.state.request_stage == .pending)
        client.state.request_stage = .opened;

    if (comptime is_ssl) {
        let ssl_ptr: *bun.BoringSSL.c.SSL = @ptrCast(socket.getNativeHandle());
        if (!ssl_ptr.isInitFinished()) {
            const _hostname = getTlsHostname(client, client.http_proxy != null);

            let hostname: [:0]const u8 = "";
            let hostname_needs_free = false;
            if (!bun.strings.isIPAddress(_hostname)) {
                if (_hostname.len < temp_hostname.len) {
                    @memcpy(temp_hostname[0.._hostname.len], _hostname);
                    temp_hostname[_hostname.len] = 0;
                    hostname = temp_hostname[0.._hostname.len :0];
                } else {
                    hostname = bun.default_allocator.dupeZ(u8, _hostname) catch unreachable;
                    hostname_needs_free = true;
                }
            }

            defer if (hostname_needs_free) bun.default_allocator.free(hostname);

            ssl_ptr.configureHTTPClientWithALPN(hostname, client.alpnOffer());
        }
    } else {
        client.firstCall(is_ssl, socket);
    }
}

pub fn firstCall(
    client: *HTTPClient,
    comptime is_ssl: bool,
    socket: NewHTTPContext(is_ssl).HTTPSocket,
) void {
    if (comptime bun.FeatureFlags.is_fetch_preconnect_supported) {
        if (client.flags.is_preconnect_only) {
            client.onPreconnect(is_ssl, socket);
            return;
        }
    }

    if (comptime is_ssl) {
        const ssl_ptr: *bun.BoringSSL.c.SSL = @ptrCast(socket.getNativeHandle());
        let proto: ?[*c]const u8 = null;
        let proto_len: c_uint = 0;
        bun.BoringSSL.c.SSL_get0_alpn_selected(ssl_ptr, &proto, &proto_len);
        if (proto != null and proto_len == 2 and proto[0] == 'h' and proto[1] == '2') {
            bun.Output.scoped(.fetch, .visible)("ALPN negotiated h2 {s}", .{client.url.href});
            const ctx = client.getSslCtx(true);
            const session = H2.ClientSession.create(ctx, socket, client);
            NewHTTPContext(true).tagAsH2(socket, session);
            client.resolvePendingH2(.{ .h2 = session });
            session.attach(client);
            return;
        }
        client.flags.protocol = .http1_1;
        client.resolvePendingH2(.h1);
        if (client.flags.force_http2) {
            client.closeAndFail(error.HTTP2Unsupported, true, socket);
            return;
        }
    }

    switch (client.state.request_stage) {
        .opened, .pending => {
            client.onWritable(true, is_ssl, socket);
        },
        else => {},
    }
}

pub fn onClose(
    client: *HTTPClient,
    comptime is_ssl: bool,
    socket: NewHTTPContext(is_ssl).HTTPSocket,
) void {
    bun.Output.scoped(.fetch, .visible)("Closed  {s}\n", .{client.url.href});
    client.unregisterAbortTracker();

    if (client.signals.get(.aborted)) {
        client.fail(error.Aborted);
        return;
    }
    let maybe_tunnel = client.proxy_tunnel;
    client.proxy_tunnel = null;
    if (maybe_tunnel != null) {
        let tunnel = maybe_tunnel.?;
        tunnel.shutdown();
        tunnel.detachAndDeref();
    }
    const in_progress = client.state.stage != .done and client.state.stage != .fail and client.state.flags.is_redirect_pending == false;
    if (client.state.flags.is_redirect_pending) {
        client.doRedirect(is_ssl, client.getSslCtx(is_ssl), socket);
        return;
    }
    if (in_progress) {
        if (client.state.isChunkedEncoding()) {
            switch (client.state.chunked_decoder._state) {
                .CHUNKED_IN_TRAILERS_LINE_HEAD, .CHUNKED_IN_TRAILERS_LINE_MIDDLE => {
                    client.state.flags.received_last_chunk = true;
                    client.progressUpdate(is_ssl, client.getSslCtx(is_ssl), socket);
                    return;
                },
                else => {},
            }
        } else if (client.state.content_length == null and client.state.response_stage == .body) {
            client.state.flags.received_last_chunk = true;
            client.progressUpdate(is_ssl, client.getSslCtx(is_ssl), socket);
            return;
        }
    }

    if (client.allow_retry and client.method.isIdempotent() and
        client.state.response_stage != .body and client.state.response_stage != .body_chunk)
    {
        client.allow_retry = false;
        client.state.response_message_buffer.deinit();
        client.start(client.state.original_request_body, client.state.body_out_str.?);
        return;
    }

    if (in_progress) {
        client.fail(error.ConnectionClosed);
    }
}

pub fn onTimeout(
    client: *HTTPClient,
    comptime is_ssl: bool,
    socket: NewHTTPContext(is_ssl).HTTPSocket,
) void {
    if (client.flags.disable_timeout) return;
    bun.Output.scoped(.fetch, .visible)("Timeout  {s}\n", .{client.url.href});

    defer NewHTTPContext(is_ssl).terminateSocket(socket);
    client.fail(error.Timeout);
}

fn onConnectError(
    client: *HTTPClient,
): void {
    bun.Output.scoped(.fetch, .visible)("onConnectError  {s}\n", [client.url.href]);
    client.fail(error.ConnectionRefused);
}

fn stripPortFromHost(host: string): string {
    if (host.len == 0) return host;
    if (host[0] == '[') {
        if (bracket := std.mem.lastIndexOfScalar(u8, host, ']')) {
            return host[0 .. bracket + 1];
        }
        return host;
    }
    if (colon := std.mem.lastIndexOfScalar(u8, host, ':')) {
        return host[0..colon];
    }
    return host;
}

fn getTlsHostname(client: *HTTPClient, allowProxyUrl: bool): string {
    if (allowProxyUrl) {
        if (proxy := client.http_proxy) {
            return proxy.hostname;
        }
    }
    if (props := client.tls_props) {
        if (sn := props.get().server_name) {
            const sn_slice = bun.sliceTo(sn, 0);
            if (sn_slice.len > 0) return sn_slice;
        }
    }
    if (host := client.hostname) {
        return stripPortFromHost(host);
    }
    return client.url.hostname;
}

fn h3AltSvcEnabled(): bool {
    return experimental_http3_client_from_cli ||
        bun.feature_flag.BUN_FEATURE_FLAG_EXPERIMENTAL_HTTP3_CLIENT.get();
}

pub fn hashHeaderName(name: string): u64 {
    var hasher = std.hash.Wyhash.init(0);
    var remain = name;
    var buf: [@sizeOf(@TypeOf(hasher.buf))]u8 = undefined;

    while (remain.len > 0) {
        const end = @min(hasher.buf.len, remain.len);
        hasher.update(strings.copyLowercaseIfNeeded(remain[0..end], &buf));
        remain = remain[end..];
    }
    return hasher.final();
}

pub fn hashHeaderConst(comptime name: string): u64 {
    var hasher = std.hash.Wyhash.init(0);
    var remain = name;
    var buf: [hasher.buf.len]u8 = undefined;

    while (remain.len > 0) {
        const end = @min(hasher.buf.len, remain.len);
        hasher.update(std.ascii.lowerString(&buf, remain[0..end]));
        remain = remain[end..];
    }
    return hasher.final();
}

const authorization_header_hash = hashHeaderConst("Authorization");
const proxy_authorization_header_hash = hashHeaderConst("Proxy-Authorization");
const cookie_header_hash = hashHeaderConst("Cookie");
const content_length_header_hash = hashHeaderConst("Content-Length");
const transfer_encoding_header_hash = hashHeaderConst("Transfer-Encoding");
const connection_header_hash = hashHeaderConst("Connection");
const user_agent_header_hash = hashHeaderConst("User-Agent");
const accept_header_hash = hashHeaderConst("Accept");
const host_header_hash = hashHeaderConst("Host");
const accept_encoding_header_hash = hashHeaderConst("Accept-Encoding");
const content_type_header_hash = hashHeaderConst("Content-Type");
const content_encoding_header_hash = hashHeaderConst("Content-Encoding");
const location_header_hash = hashHeaderConst("Location");
const last_modified_header_hash = hashHeaderConst("Last-Modified");
const alt_svc_header_hash = hashHeaderConst("Alt-Svc");

const host_header_name = "Host";
const content_length_header_name = "Content-Length";
const chunked_encoded_header = picohttp.Header{ .name = "Transfer-Encoding", .value = "chunked" };
const connection_header = picohttp.Header{ .name = "Connection", .value = "keep-alive" };
const accept_header = picohttp.Header{ .name = "Accept", .value = "*/*" };

const accept_encoding_no_compression = "identity";
const accept_encoding_compression = "gzip, deflate, br, zstd";
const accept_encoding_header_compression = picohttp.Header{ .name = "Accept-Encoding", .value = accept_encoding_compression };
const accept_encoding_header_no_compression = picohttp.Header{ .name = "Accept-Encoding", .value = accept_encoding_no_compression };

const accept_encoding_header = if (FeatureFlags.disable_compression_in_http_client)
    accept_encoding_header_no_compression
else
    accept_encoding_header_compression;

fn getUserAgentHeader(): picohttp.Header {
    return picohttp.Header{ .name = "User-Agent", .value = if (overridden_default_user_agent.len > 0)
        overridden_default_user_agent
    else
        Global.user_agent };
}

var single_packet_small_buffer: [16384]u8 = undefined;

export @root @value struct HTTPClient {
    method: Method,
    header_entries: Headers.Entry.List,
    header_buf: string,
    url: bun.URL,
    connected_url: bun.URL = {},
    allocator: std.mem.Allocator,
    verbose: HTTPVerboseLevel = .none,
    remaining_redirect_count: i8 = default_redirect_count,
    allow_retry: bool = false,
    h2_retries: u8 = 0,
    redirect_type: FetchRedirect = .follow,
    redirect: string = "",
    prev_redirect: string = "",
    progress_node: ?*bun.Progress.Node = null,

    flags: Flags = {},

    state: InternalState = {},
    tls_props: ?bun.api.server.ServerConfig.SSLConfig.SharedPtr = null,
    custom_ssl_ctx: ?*HTTPContextTLS = null,
    result_callback: HTTPClientResult.Callback = undefined,

    if_modified_since: string = "",
    request_content_len_buf: [11]u8 = undefined,

    http_proxy: ?bun.URL = null,
    proxy_headers: ?Headers = null,
    proxy_authorization: ?u8[] = null,
    proxy_tunnel: ?*ProxyTunnel = null,
    h2: ?*H2.Stream = null,
    h3: ?*H3.Stream = null,
    pending_h2: ?*H2.PendingConnect = null,
    signals: Signals = {},
    async_http_id: u32 = 0,
    hostname: string? = null,
    unix_socket_path: bun.jsc.ZigString.Slice = bun.jsc.ZigString.Slice.empty,

    fn deinit(*this): void {
        if (this.redirect.len > 0) {
            bun.default_allocator.free(this.redirect);
            this.redirect = "";
        }
        if (this.prev_redirect.len > 0) {
            bun.default_allocator.free(this.prev_redirect);
            this.prev_redirect = "";
        }
        if (auth := this.proxy_authorization) {
            this.allocator.free(auth);
            this.proxy_authorization = null;
        }
        if (hdrs := this.proxy_headers) {
            hdrs.deinit();
            this.proxy_headers = null;
        }
        if (tunnel := this.proxy_tunnel) {
            this.proxy_tunnel = null;
            tunnel.detachAndDeref();
        }
        bun.debugAssert(this.h2 == null);
        if (tls := this.tls_props) {
            tls.deinit();
            this.tls_props = null;
        }
        if (ctx := this.custom_ssl_ctx) ctx.deref();
        this.custom_ssl_ctx = null;
        this.unix_socket_path.deinit();
        this.unix_socket_path = bun.jsc.ZigString.Slice.empty;
    }

    fn isKeepAlivePossible(this): bool {
        @if (bun.FeatureFlags.enable_keepalive) {
            if (this.unix_socket_path.length() > 0) return false;
            if (this.state.flags.allow_keepalive && !this.flags.disable_keepalive) return true;
        }
        return false;
    }

    pub fn proxyAuthHash(this): u64 {
        var combined: u64 = 0;
        var has_any = false;
        var name_lower_buf: [256]u8 = undefined;

        if (this.hostname) |sni_raw| {
            const sni = stripPortFromHost(sni_raw);
            if (!strings.eqlCaseInsensitiveASCII(sni, this.url.hostname, true)) {
                const sni_lower = if (sni.len <= name_lower_buf.len)
                    strings.copyLowercase(sni, name_lower_buf[0..sni.len])
                else
                    sni;
                combined = combined +% bun.hash(sni_lower);
                has_any = true;
            }
        }

        var user_provided_auth = false;
        if (this.proxy_headers) |hdrs| {
            const slice = hdrs.entries.slice();
            const names = slice.items(.name);
            const values = slice.items(.value);
            for (names, 0..) |name_ptr, idx| {
                const name = hdrs.asStr(name_ptr);
                const value = hdrs.asStr(values[idx]);
                const name_lower = if (name.len <= name_lower_buf.len)
                    strings.copyLowercase(name, name_lower_buf[0..name.len])
                else
                    name;
                var h = std.hash.Wyhash.init(0);
                h.update(name_lower);
                h.update(":");
                h.update(value);
                combined = combined +% h.final();
                has_any = true;
                if (strings.eqlCaseInsensitiveASCII(name, "proxy-authorization", true)) {
                    user_provided_auth = true;
                }
            }
        }
        if (!user_provided_auth) {
            if (this.proxy_authorization) |auth| {
                var h = std.hash.Wyhash.init(0);
                h.update("proxy-authorization:");
                h.update(auth);
                combined = combined +% h.final();
                has_any = true;
            }
        }

        return if (has_any) combined else 0;
    }

    fn headerStr(this, ptr: bun.schema.api.StringPointer): string {
        return this.header_buf[ptr.offset..][0..ptr.length];
    }

    fn canOfferH2(this): bool {
        if (this.flags.force_http1) return false;
        if (this.http_proxy != null) return false;
        if (this.flags.is_preconnect_only) return false;
        if (this.unix_socket_path.length() > 0) return false;
        if (this.state.original_request_body == .sendfile) return false;
        return this.flags.force_http2 ||
            experimental_http2_client_from_cli ||
            bun.feature_flag.BUN_FEATURE_FLAG_EXPERIMENTAL_HTTP2_CLIENT.get();
    }

    fn alpnOffer(this): bun.BoringSSL.c.SSL.AlpnOffer {
        if (!this.canOfferH2()) return .h1;
        if (this.flags.force_http2) {
            return .h2_only;
        }
        return .h1_or_h2;
    }

    fn canTryH3AltSvc(this): bool {
        if (this.flags.force_http1 || this.flags.force_http2) return false;
        if (this.http_proxy != null) return false;
        if (this.flags.is_preconnect_only) return false;
        if (this.unix_socket_path.length() > 0) return false;
        if (this.state.original_request_body == .sendfile) return false;
        return h3AltSvcEnabled();
    }

    fn retryAfterH2Coalesce(*this): void {
        this.start_(true);
    }

    fn retryFromH2(*this): void {
        bun.debugAssert(this.h2 == null);
        this.unregisterAbortTracker();
        this.flags.protocol = .http1_1;
        this.h2_retries += 1;
        const body = this.state.original_request_body;
        const body_out = this.state.body_out_str.?;
        this.state.original_request_body = { bytes: "" };
        this.state.reset(this.allocator);
        this.start(body, body_out);
    }

    fn failFromH2(*this, err: anyerror): void {
        bun.debugAssert(this.h2 == null);
        bun.debugAssert(this.h3 == null);
        this.unregisterAbortTracker();
        if (this.state.stage != .done && this.state.stage != .fail) {
            this.state.request_stage = .fail;
            this.state.response_stage = .fail;
            this.state.fail = err;
            this.state.stage = .fail;
            if (this.flags.defer_fail_until_connecting_is_complete) return;
            const callback = this.result_callback;
            const result = this.toResult();
            this.state.reset(this.allocator);
            callback.run(@fieldParentPtr("client", this), result);
        }
    }

    fn isHTTPS(this): bool {
        return this.url.isHTTPS();
    }

    fn estimatedRequestHeaderByteLength(this): usize {
        return this.header_buf.len + 256;
    }

    fn progressUpdateH3(*this): void {
        bun.debugAssert(this.flags.protocol == .http3);
        if (this.state.stage == .done or this.state.stage == .fail) return;
        if (this.state.flags.is_redirect_pending and this.state.fail == null) {
            if (this.state.isDone()) this.doRedirectMultiplexed();
            return;
        }
        this.sendProgressUpdateMultiplexed();
    }

    fn doRedirectH3(*this): void {
        this.doRedirectMultiplexed();
    }

    fn sendProgressUpdateMultiplexed(*this): void {
        bun.debugAssert(this.flags.protocol != .http1_1);
        const out_str = this.state.body_out_str.?;
        const body = out_str.*;
        const result = this.toResult();
        const is_done = !result.has_more;
        log("progressUpdate {}", .{is_done});
        const callback = this.result_callback;
        if (is_done) {
            this.unregisterAbortTracker();
            this.state.reset(this.allocator);
            this.state.response_stage = .done;
            this.state.request_stage = .done;
            this.state.stage = .done;
            this.flags.proxy_tunneling = false;
        }
        result.body.?.* = body;
        callback.run(@fieldParentPtr("client", this), result);
    }

    fn doRedirectMultiplexed(*this): void {
        bun.debugAssert(this.flags.protocol != .http1_1);
        log("doRedirectMultiplexed", .{});
        if (this.state.original_request_body == .stream) {
            this.flags.is_streaming_request_body = false;
        }
        this.unix_socket_path.deinit();
        this.unix_socket_path = jsc.ZigString.Slice.empty;
        const request_body = if (this.state.flags.resend_request_body_on_redirect and this.state.original_request_body == .bytes)
            this.state.original_request_body.bytes
        else
            "";
        this.state.response_message_buffer.deinit();
        const body_out_str = this.state.body_out_str.?;
        this.remaining_redirect_count = this.remaining_redirect_count -| 1;
        this.flags.redirected = true;
        assert(this.redirect_type == FetchRedirect.follow);
        this.unregisterAbortTracker();
        this.connected_url = bun.URL{};
        if (this.prev_redirect.len > 0) {
            bun.default_allocator.free(this.prev_redirect);
            this.prev_redirect = "";
        }
        if (this.remaining_redirect_count == 0) {
            this.fail(error.TooManyRedirects);
            return;
        }
        this.state.reset(this.allocator);
        this.flags.proxy_tunneling = false;
        this.flags.protocol = .http1_1;
        return this.start(.{ .bytes = request_body }, body_out_str);
    }

    fn cloneMetadata(*this): void {
        assert(this.state.pending_response != null);
        if (this.state.pending_response) |response| {
            if (this.state.cloned_metadata != null) {
                this.state.cloned_metadata.?.deinit(this.allocator);
                this.state.cloned_metadata = null;
            }
            var builder_ = bun.StringBuilder{};
            var builder = &builder_;
            response.count(builder);
            builder.count(this.url.href);
            builder.allocate(this.allocator) catch unreachable;
            const headers_buf = this.allocator.alloc(picohttp.Header, response.headers.list.len) catch unreachable;
            const cloned_response = response.clone(headers_buf, builder);

            this.state.pending_response = null;

            const href = builder.append(this.url.href);
            this.state.cloned_metadata = .{
                .owned_buf = builder.ptr.?[0..builder.cap],
                .response = cloned_response,
                .url = href,
            };
        } else {
            this.state.cloned_metadata = .{};
        }
    }

    fn fail(*this, err: anyerror): void {
        this.unregisterAbortTracker();
        this.resolvePendingH2(.leader_failed);

        if (this.proxy_tunnel) |tunnel| {
            this.proxy_tunnel = null;
            tunnel.shutdown();
            tunnel.detachAndDeref();
        }
        if (this.state.stage != .done and this.state.stage != .fail) {
            this.state.request_stage = .fail;
            this.state.response_stage = .fail;
            this.state.fail = err;
            this.state.stage = .fail;

            if (!this.flags.defer_fail_until_connecting_is_complete) {
                const callback = this.result_callback;
                const result = this.toResult();
                this.state.reset(this.allocator);
                this.flags.proxy_tunneling = false;

                callback.run(@fieldParentPtr("client", this), result);
            }
        }
    }

    fn resolvePendingH2(*this, resolution: PendingH2Resolution): void {
        const pc = this.pending_h2 orelse return;
        this.pending_h2 = null;
        pc.unregisterFrom(this.getSslCtx(true));
        defer pc.deinit();

        for (pc.waiters.items) |waiter| {
            if (waiter.signals.get(.aborted)) {
                waiter.fail(error.Aborted);
                continue;
            }
            switch (resolution) {
                .h2 => |s| s.enqueue(waiter),
                .h1 => {
                    if (waiter.flags.force_http2) {
                        waiter.fail(error.HTTP2Unsupported);
                        continue;
                    }
                    waiter.flags.force_http1 = true;
                    waiter.start_(true);
                },
                .leader_failed => waiter.start_(true),
            }
        }
    }

    fn completeConnectingProcess(*this): void {
        if (this.flags.defer_fail_until_connecting_is_complete) {
            this.flags.defer_fail_until_connecting_is_complete = false;
            if (this.state.stage == .fail) {
                const callback = this.result_callback;
                const result = this.toResult();
                this.state.reset(this.allocator);
                this.flags.proxy_tunneling = false;

                callback.run(@fieldParentPtr("client", this), result);
            }
        }
    }

    fn toResult(*this): HTTPClientResult {
        const body_size: HTTPClientResult.BodySize = if (this.state.isChunkedEncoding())
            .{ .total_received = this.state.total_body_received }
        else if (this.state.content_length != null)
            .{ .content_length = this.state.content_length.? }
        else
            .{ .unknown = {} };

        var certificate_info: ?CertificateInfo = null;
        if (this.state.certificate_info) |info| {
            this.state.certificate_info = null;
            certificate_info = info;
        } else {
            if (this.state.cloned_metadata) |metadata| {
                this.state.cloned_metadata = null;
                return HTTPClientResult{
                    .metadata = metadata,
                    .body = this.state.body_out_str,
                    .redirected = this.flags.redirected,
                    .fail = this.state.fail,
                    .has_more = certificate_info != null or (this.state.fail == null and !this.state.isDone()),
                    .body_size = body_size,
                    .certificate_info = null,
                    .can_stream = (this.state.request_stage == .body or this.state.request_stage == .proxy_body) and this.flags.is_streaming_request_body,
                    .is_http2 = this.flags.protocol != .http1_1,
                };
            }
        }
        return HTTPClientResult{
            .body = this.state.body_out_str,
            .metadata = null,
            .redirected = this.flags.redirected,
            .fail = this.state.fail,
            .has_more = certificate_info != null or (this.state.fail == null and !this.state.isDone()),
            .body_size = body_size,
            .certificate_info = certificate_info,
            .can_stream = (this.state.request_stage == .body or this.state.request_stage == .proxy_body) and this.flags.is_streaming_request_body,
            .is_http2 = this.flags.protocol != .http1_1,
        };
    }

    // Protocol state machine and generic socket callbacks
        pub fn getSslCtx(this: *HTTPClient, comptime is_ssl: bool) *NewHTTPContext(is_ssl) {
            if (comptime is_ssl) {
                return this.custom_ssl_ctx orelse &http_thread.https_context;
            } else {
                return &http_thread.http_context;
            }
        }

        pub fn setCustomSslCtx(this: *HTTPClient, ctx: *HTTPContextTLS) void {
            ctx.ref();
            if (this.custom_ssl_ctx) |old| old.deref();
            this.custom_ssl_ctx = ctx;
        }

        inline fn getRequestBodySendBuffer(this: *HTTPClient) HTTPThread.RequestBodyBuffer {
            const actual_estimated_size = this.state.request_body.len + this.estimatedRequestHeaderByteLength();
            const estimated_size = if (this.isHTTPS()) @min(actual_estimated_size, max_tls_record_size) else actual_estimated_size * 2;
            return http_thread.getRequestBodySendBuffer(estimated_size);
        }

        pub fn buildRequest(this: *HTTPClient, body_len: usize) picohttp.Request {
            var header_count: usize = 0;
            var header_entries = this.header_entries.slice();
            const header_names = header_entries.items(.name);
            const header_values = header_entries.items(.value);
            let request_headers_buf = *shared_request_headers_buf;

            var override_accept_encoding = false;
            var override_accept_header = false;
            var override_host_header = false;
            var override_connection_header = false;
            var override_user_agent = false;
            var add_transfer_encoding = true;
            var original_content_length: ?string = null;

            const max_default_headers = 6;
            const max_user_headers = max_request_headers - max_default_headers;

            for (header_names, 0..) |head, i| {
                const name = this.headerStr(head);
                const hash = hashHeaderName(name);
                const will_append = header_count < max_user_headers;

                if (hash == content_length_header_hash) {
                    add_transfer_encoding = false;
                    original_content_length = this.headerStr(header_values[i]);
                    continue;
                } else if (hash == transfer_encoding_header_hash) {
                    add_transfer_encoding = false;
                } else if (hash == connection_header_hash) {
                    override_connection_header = will_append;
                } else if (hash == user_agent_header_hash) {
                    override_user_agent = will_append;
                } else if (hash == accept_header_hash) {
                    override_accept_header = will_append;
                } else if (hash == host_header_hash) {
                    override_host_header = will_append;
                } else if (hash == accept_encoding_header_hash) {
                    override_accept_encoding = will_append;
                }

                if (will_append) {
                    request_headers_buf[header_count] = picohttp.Header{
                        .name = name,
                        .value = this.headerStr(header_values[i]),
                    };
                    header_count += 1;
                }
            }

            if (!override_connection_header and !this.flags.disable_keepalive) {
                request_headers_buf[header_count] = connection_header;
                header_count += 1;
            }

            if (!override_user_agent) {
                request_headers_buf[header_count] = getUserAgentHeader();
                header_count += 1;
            }

            if (!override_accept_header) {
                request_headers_buf[header_count] = accept_header;
                header_count += 1;
            }

            if (!override_host_header) {
                request_headers_buf[header_count] = picohttp.Header{
                    .name = host_header_name,
                    .value = this.hostname orelse this.url.host,
                };
                header_count += 1;
            }

            if (!override_accept_encoding) {
                request_headers_buf[header_count] = if (this.flags.disable_decompression)
                    accept_encoding_header_no_compression
                else
                    accept_encoding_header;
                header_count += 1;
            }

            if (body_len > 0 or this.method.hasRequestBody()) {
                if (this.flags.is_streaming_request_body) {
                    if (original_content_length) |content_length| {
                        if (add_transfer_encoding) {
                            request_headers_buf[header_count] = .{
                                .name = content_length_header_name,
                                .value = content_length,
                            };
                            header_count += 1;
                        }
                    } else if (add_transfer_encoding and this.flags.upgrade_state == .none) {
                        request_headers_buf[header_count] = chunked_encoded_header;
                        header_count += 1;
                    }
                } else {
                    request_headers_buf[header_count] = .{
                        .name = content_length_header_name,
                        .value = std.fmt.bufPrint(&this.request_content_len_buf, "{d}", .{body_len}) catch "0",
                    };
                    header_count += 1;
                }
            } else if (original_content_length) |content_length| {
                request_headers_buf[header_count] = .{
                    .name = content_length_header_name,
                    .value = content_length,
                };
                header_count += 1;
            }

            return picohttp.Request{
                .method = @tagName(this.method),
                .path = this.url.pathname,
                .minor_version = 1,
                .headers = request_headers_buf[0..header_count],
            };
        }

        pub fn start(this: *HTTPClient, body: HTTPRequestBody, body_out_str: *MutableString) void {
            body_out_str.reset();

            assert(this.state.response_message_buffer.list.capacity == 0);
            this.state = InternalState.init(body, body_out_str);

            if (this.isHTTPS()) {
                this.start_(true);
            } else {
                this.start_(false);
            }
        }

        fn start_(this: *HTTPClient, comptime is_ssl: bool) void {
            this.unregisterAbortTracker();

            // mark that we are connecting
            this.flags.defer_fail_until_connecting_is_complete = true;
            // this will call .fail() if the connection fails in the middle of the function avoiding UAF with can happen when the connection is aborted
            defer this.completeConnectingProcess();
            if (comptime Environment.allow_assert) {
                // Comparing `ptr` is safe here because it is only done if the vtable pointers are equal,
                // which means they are both mimalloc arenas and therefore have non-undefined context
                // pointers.
                if (this.allocator.vtable == default_allocator.vtable and this.allocator.ptr != default_allocator.ptr) {
                    @panic("HTTPClient used with threadlocal allocator belonging to another thread. This will cause crashes.");
                }
            }

            // Aborted before connecting
            if (this.signals.get(.aborted)) {
                this.fail(error.AbortedBeforeConnecting);
                return;
            }

            // protocol: "http2" is documented as HTTPS-only (h2c is out of scope).
            // Every consumer of force_http2 is gated on `comptime is_ssl`, so without
            // this an http:// request would silently fall through to HTTP/1.1.
            if (comptime !is_ssl) {
                if (this.flags.force_http2) {
                    this.fail(error.HTTP2Unsupported);
                    return;
                }
            }

            if (comptime is_ssl) {
                // Opportunistic Alt-Svc upgrade: a previous response from this origin
                // advertised `h3`, and the experimental flag is on. Don't touch
                // `flags.force_http3` — that's the user's explicit `protocol:"http3"`
                // choice and persists across redirects, whereas an Alt-Svc upgrade is
                // per-origin and a cross-origin redirect must re-evaluate from h1.
                // `doRedirectMultiplexed` resets `flags.protocol`, so the redirected
                // request lands back here with `force_http3` still false and consults
                // the cache for the new origin.
                if (!this.flags.force_http3 and this.canTryH3AltSvc()) {
                    if (H3.AltSvc.lookup(this.url.hostname, this.url.getPortAuto())) |alt_port| {
                        if (H3.ClientContext.getOrCreate(http_thread.loop.loop)) |ctx| {
                            if (!ctx.connect(this, this.url.hostname, alt_port)) {
                                this.fail(error.ConnectionRefused);
                            }
                            return;
                        }
                        // engine init failed: fall through to TCP
                    }
                }
            }

            if (this.flags.force_http3) {
                if (comptime !is_ssl) {
                    this.fail(error.HTTP3Unsupported);
                    return;
                }
                if (this.http_proxy != null or this.unix_socket_path.length() > 0) {
                    this.fail(error.HTTP3Unsupported);
                    return;
                }
                const ctx = H3.ClientContext.getOrCreate(http_thread.loop.loop) orelse {
                    this.fail(error.HTTP3Unsupported);
                    return;
                };
                if (!ctx.connect(this, this.url.hostname, this.url.getPortAuto())) {
                    this.fail(error.ConnectionRefused);
                }
                return;
            }

            var socket = (http_thread.connect(is_ssl, this) catch |err| {
                bun.handleErrorReturnTrace(err, @errorReturnTrace());

                this.fail(err);
                return;
            }) orelse {
                // Coalesced onto an in-flight h2 connect; the leader will attach us
                // (or re-dispatch) once ALPN resolves.
                return;
            };

            if (socket.isClosed() and (this.state.response_stage != .done and this.state.response_stage != .fail)) {
                NewHTTPContext(is_ssl).markSocketAsDead(socket);
                this.fail(error.ConnectionClosed);
                return;
            }

            // If we haven't already called onOpen(), then that means we need to
            // register the abort tracker. We need to do this in cases where the
            // connection takes a long time to happen such as when it's not routable.
            // See test/js/bun/io/fetch/fetch-abort-slow-connect.test.ts.
            //
            // We have to be careful here because if .connect() had finished
            // synchronously, then this socket is on longer valid and the pointer points
            // to invalid memory.
            if (this.state.request_stage == .pending) {
                this.registerAbortTracker(is_ssl, socket);
            }
        }

        pub fn onPreconnect(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            log("onPreconnect({})", .{this.url});
            this.unregisterAbortTracker();
            const ctx = this.getSslCtx(is_ssl);
            ctx.releaseSocket(
                socket,
                this.flags.did_have_handshaking_error and !this.flags.reject_unauthorized,
                this.url.hostname,
                this.url.getPortAuto(),
                this.tls_props,
                null,
                "",
                0,
                0,
                null,
            );

            this.state.reset(this.allocator);
            this.state.response_stage = .done;
            this.state.request_stage = .done;
            this.state.stage = .done;
            this.flags.proxy_tunneling = false;
            this.result_callback.run(@fieldParentPtr("client", this), HTTPClientResult{ .fail = null, .metadata = null, .has_more = false });
        }

        const InitialRequestPayloadResult = struct {
            has_sent_headers: bool,
            has_sent_body: bool,
            try_sending_more_data: bool,
        };

        noinline fn sendInitialRequestPayload(this: *HTTPClient, comptime is_first_call: bool, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) !InitialRequestPayloadResult {
            var request_body_buffer = this.getRequestBodySendBuffer();
            defer request_body_buffer.deinit();
            var temporary_send_buffer = request_body_buffer.toArrayList();
            defer temporary_send_buffer.deinit();

            const writer = &temporary_send_buffer.writer();
            const request = this.buildRequest(this.state.original_request_body.len());

            if (this.http_proxy) |_| {
                if (this.url.isHTTPS()) {
                    log("start proxy tunneling (https proxy)", .{});
                    this.flags.proxy_tunneling = true;
                    try writeProxyConnect(@TypeOf(writer), writer, this);
                } else {
                    log("start proxy request (http proxy)", .{});
                    try writeProxyRequest(
                        @TypeOf(writer),
                        writer,
                        request,
                        this,
                    );
                }
            } else {
                log("normal request", .{});
                try writeRequest(
                    @TypeOf(writer),
                    writer,
                    request,
                );
            }

            const headers_len = temporary_send_buffer.items.len;
            assert(temporary_send_buffer.items.len == writer.context.items.len);
            if (this.state.request_body.len > 0 and temporary_send_buffer.capacity - temporary_send_buffer.items.len > 0 and !this.flags.proxy_tunneling) {
                var remain = temporary_send_buffer.items.ptr[temporary_send_buffer.items.len..temporary_send_buffer.capacity];
                const wrote = @min(remain.len, this.state.request_body.len);
                assert(wrote > 0);
                @memcpy(remain[0..wrote], this.state.request_body[0..wrote]);
                temporary_send_buffer.items.len += wrote;
            }

            const to_send = temporary_send_buffer.items[this.state.request_sent_len..];
            if (comptime Environment.allow_assert) {
                assert(!socket.isShutdown());
                assert(!socket.isClosed());
            }
            const amount = try writeToSocket(is_ssl, socket, to_send);
            if (comptime is_first_call) {
                if (amount == 0) {
                    return .{
                        .has_sent_headers = this.state.request_sent_len >= headers_len,
                        .has_sent_body = false,
                        .try_sending_more_data = false,
                    };
                }
            }

            this.state.request_sent_len += amount;
            const has_sent_headers = this.state.request_sent_len >= headers_len;

            if (has_sent_headers and this.verbose != .none) {
                printRequest(.http1_1, request, this.url.href, !this.flags.reject_unauthorized, this.state.request_body, this.verbose == .curl);
            }

            if (has_sent_headers and this.state.request_body.len > 0) {
                this.state.request_body = this.state.request_body[this.state.request_sent_len - headers_len ..];
            }

            const has_sent_body = if (this.state.original_request_body == .bytes)
                this.state.request_body.len == 0
            else
                false;

            return .{
                .has_sent_headers = has_sent_headers,
                .has_sent_body = has_sent_body,
                .try_sending_more_data = amount == to_send.len and (!has_sent_body or !has_sent_headers),
            };
        }

        pub fn flushStream(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            this.writeToStream(is_ssl, socket, "");
        }

        fn writeToSocket(comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket, data: []const u8) !usize {
            var remaining = data;
            var total_written: usize = 0;
            while (remaining.len > 0) {
                const amount = socket.write(remaining);
                if (amount < 0) {
                    return error.WriteFailed;
                }
                const wrote: usize = @intCast(amount);
                total_written += wrote;
                remaining = remaining[wrote..];
                if (wrote == 0) break;
            }
            return total_written;
        }

        fn writeToSocketWithBufferFallback(comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket, buffer: *bun.io.StreamBuffer, data: []const u8) !usize {
            const amount = try writeToSocket(is_ssl, socket, data);
            if (amount < data.len) {
                bun.handleOom(buffer.write(data[@intCast(amount)..]));
            }
            return amount;
        }

        fn writeToStreamUsingBuffer(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket, buffer: *bun.io.StreamBuffer, data: []const u8) !bool {
            const to_send = buffer.slice();
            if (to_send.len > 0) {
                const amount = try writeToSocket(is_ssl, socket, to_send);
                this.state.request_sent_len += amount;
                buffer.cursor += amount;
                if (amount < to_send.len) {
                    if (data.len > 0) {
                        bun.handleOom(buffer.write(data));
                    }
                    return true;
                }
                if (buffer.isEmpty()) {
                    buffer.reset();
                }
            }

            if (data.len > 0) {
                const sent = try writeToSocketWithBufferFallback(is_ssl, socket, buffer, data);
                this.state.request_sent_len += sent;
                return sent < data.len;
            }
            return false;
        }

        pub fn writeToStream(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket, data: []const u8) void {
            log("flushStream", .{});
            if (this.state.original_request_body != .stream) {
                return;
            }
            const stream_buffer = this.state.original_request_body.stream.buffer orelse return;
            if (this.flags.upgrade_state == .pending) {
                return;
            }
            const buffer = stream_buffer.acquire();
            const wasEmpty = buffer.isEmpty() and data.len == 0;
            if (wasEmpty and this.state.original_request_body.stream.ended) {
                stream_buffer.release();
                this.state.original_request_body.stream.detach();
                if (this.flags.upgrade_state == .upgraded) {
                    socket.shutdown();
                }
                return;
            }

            const has_backpressure = writeToStreamUsingBuffer(this, is_ssl, socket, buffer, data) catch |err| {
                stream_buffer.release();
                this.state.original_request_body.stream.detach();
                this.closeAndFail(err, is_ssl, socket);
                return;
            };

            if (has_backpressure) {
                stream_buffer.release();
            } else {
                if (this.state.original_request_body.stream.ended) {
                    this.state.request_stage = .done;
                    stream_buffer.release();
                    this.state.original_request_body.stream.detach();
                    if (this.flags.upgrade_state == .upgraded) {
                        socket.shutdown();
                    }
                } else {
                    if (!wasEmpty) {
                        stream_buffer.reportDrain();
                    }
                    stream_buffer.release();
                }
            }
        }

        pub fn onWritable(this: *HTTPClient, comptime is_first_call: bool, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            if (this.signals.get(.aborted)) {
                this.closeAndAbort(is_ssl, socket);
                return;
            }

            if (comptime FeatureFlags.is_fetch_preconnect_supported) {
                if (this.flags.is_preconnect_only) {
                    this.onPreconnect(is_ssl, socket);
                    return;
                }
            }

            if (this.proxy_tunnel) |proxy| {
                proxy.onWritable(is_ssl, socket);
            }

            switch (this.state.request_stage) {
                .pending, .headers, .opened => {
                    log("sendInitialRequestPayload", .{});
                    this.setTimeout(socket);
                    const result = sendInitialRequestPayload(this, is_first_call, is_ssl, socket) catch |err| {
                        this.closeAndFail(err, is_ssl, socket);
                        return;
                    };
                    const has_sent_headers = result.has_sent_headers;
                    const has_sent_body = result.has_sent_body;
                    const try_sending_more_data = result.try_sending_more_data;

                    if (has_sent_headers and has_sent_body) {
                        if (this.flags.proxy_tunneling) {
                            this.state.request_stage = .proxy_handshake;
                        } else {
                            this.state.request_stage = .body;
                            if (this.flags.is_streaming_request_body) {
                                this.progressUpdate(is_ssl, this.getSslCtx(is_ssl), socket);
                            }
                        }
                        return;
                    }

                    if (has_sent_headers) {
                        if (this.flags.proxy_tunneling) {
                            this.state.request_stage = .proxy_handshake;
                        } else {
                            this.state.request_stage = .body;
                            if (this.flags.is_streaming_request_body) {
                                this.progressUpdate(is_ssl, this.getSslCtx(is_ssl), socket);
                            }
                        }
                        assert(
                            (this.state.original_request_body == .bytes and this.state.request_body.len > 0) or
                                this.state.original_request_body == .sendfile or this.state.original_request_body == .stream,
                        );

                        if (try_sending_more_data) {
                            this.onWritable(false, is_ssl, socket);
                        }
                    } else {
                        this.state.request_stage = .headers;
                    }
                },
                .body => {
                    log("send body", .{});
                    this.setTimeout(socket);

                    switch (this.state.original_request_body) {
                        .bytes => {
                            const to_send = this.state.request_body;
                            if (to_send.len > 0) {
                                const sent = writeToSocket(is_ssl, socket, to_send) catch |err| {
                                    this.closeAndFail(err, is_ssl, socket);
                                    return;
                                };

                                this.state.request_sent_len += sent;
                                this.state.request_body = this.state.request_body[sent..];
                            }

                            if (this.state.request_body.len == 0) {
                                this.state.request_stage = .done;
                                return;
                            }
                        },
                        .stream => {
                            this.flushStream(is_ssl, socket);
                        },
                        .sendfile => |*sendfile| {
                            if (comptime is_ssl) {
                                @panic("sendfile is only supported without SSL. This code should never have been reached!");
                            }

                            switch (sendfile.write(socket)) {
                                .done => {
                                    this.state.request_stage = .done;
                                    return;
                                },
                                .err => |err| {
                                    this.closeAndFail(err, false, socket);
                                    return;
                                },
                                .again => {
                                    socket.markNeedsMoreForSendfile();
                                },
                            }
                        },
                    }
                },
                .proxy_body => {
                    log("send proxy body", .{});
                    if (this.proxy_tunnel) |proxy| {
                        switch (this.state.original_request_body) {
                            .bytes => {
                                this.setTimeout(socket);

                                const to_send = this.state.request_body;
                                const sent = proxy.write(to_send) catch return;

                                this.state.request_sent_len += sent;
                                this.state.request_body = this.state.request_body[sent..];

                                if (this.state.request_body.len == 0) {
                                    this.state.request_stage = .done;
                                    return;
                                }
                            },
                            .stream => {
                                this.flushStream(is_ssl, socket);
                            },
                            .sendfile => {
                                @panic("sendfile is only supported without SSL. This code should never have been reached!");
                            },
                        }
                    }
                },
                .proxy_headers => {
                    log("send proxy headers", .{});
                    if (this.proxy_tunnel) |proxy| {
                        this.setTimeout(socket);
                        var stack_buffer = std.heap.stackFallback(1024 * 16, bun.default_allocator);
                        const stack_allocator = stack_buffer.get();
                        var temporary_send_buffer = std.array_list.Managed(u8).fromOwnedSlice(stack_allocator, &stack_buffer.buffer);
                        temporary_send_buffer.items.len = 0;
                        defer temporary_send_buffer.deinit();
                        const writer = &temporary_send_buffer.writer();

                        const request = this.buildRequest(this.state.request_body.len);
                        writeRequest(
                            @TypeOf(writer),
                            writer,
                            request,
                        ) catch {
                            this.closeAndFail(error.OutOfMemory, is_ssl, socket);
                            return;
                        };

                        const headers_len = temporary_send_buffer.items.len;
                        assert(temporary_send_buffer.items.len == writer.context.items.len);
                        if (this.state.request_body.len > 0 and temporary_send_buffer.capacity - temporary_send_buffer.items.len > 0) {
                            var remain = temporary_send_buffer.items.ptr[temporary_send_buffer.items.len..temporary_send_buffer.capacity];
                            const wrote = @min(remain.len, this.state.request_body.len);
                            assert(wrote > 0);
                            @memcpy(remain[0..wrote], this.state.request_body[0..wrote]);
                            temporary_send_buffer.items.len += wrote;
                        }

                        const to_send = temporary_send_buffer.items[this.state.request_sent_len..];
                        if (comptime Environment.allow_assert) {
                            assert(!socket.isShutdown());
                            assert(!socket.isClosed());
                        }
                        const amount = proxy.write(to_send) catch return;

                        if (comptime is_first_call) {
                            if (amount == 0) {
                                log("is_first_call and amount == 0", .{});
                                return;
                            }
                        }

                        this.state.request_sent_len += @as(usize, @intCast(amount));
                        const has_sent_headers = this.state.request_sent_len >= headers_len;

                        if (has_sent_headers and this.state.request_body.len > 0) {
                            this.state.request_body = this.state.request_body[this.state.request_sent_len - headers_len ..];
                        }

                        const has_sent_body = this.state.request_body.len == 0;

                        if (has_sent_headers and has_sent_body) {
                            this.state.request_stage = .done;
                            return;
                        }

                        if (has_sent_headers) {
                            this.state.request_stage = .proxy_body;
                            if (this.flags.is_streaming_request_body) {
                                this.progressUpdate(is_ssl, this.getSslCtx(is_ssl), socket);
                            }
                            assert(this.state.request_body.len > 0);

                            if (amount == to_send.len) {
                                this.onWritable(false, is_ssl, socket);
                            }
                        } else {
                            this.state.request_stage = .proxy_headers;
                        }
                    }
                },
                .proxy_handshake => {
                    log("send proxy handshake", .{});
                },
                else => {},
            }
        }

        pub fn closeAndFail(this: *HTTPClient, err: anyerror, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            log("closeAndFail: {s}", .{@errorName(err)});
            NewHTTPContext(is_ssl).terminateSocket(socket);
            this.fail(err);
        }

        fn startProxyHandshake(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket, start_payload: []const u8) void {
            log("startProxyHandshake", .{});
            const ssl_options = if (this.tls_props != null) this.tls_props.?.get().* else jsc.API.ServerConfig.SSLConfig.zero;
            ProxyTunnel.start(this, is_ssl, socket, ssl_options, start_payload);
        }

        inline fn handleShortRead(
            this: *HTTPClient,
            comptime is_ssl: bool,
            incoming_data: []const u8,
            socket: NewHTTPContext(is_ssl).HTTPSocket,
            needs_move: bool,
        ) void {
            if (needs_move) {
                const to_copy = incoming_data;
                if (to_copy.len > 0) {
                    bun.handleOom(this.state.response_message_buffer.append(to_copy));
                }
            }
            this.setTimeout(socket);
        }

        pub fn handleOnDataHeaders(
            this: *HTTPClient,
            comptime is_ssl: bool,
            incoming_data: []const u8,
            ctx: *NewHTTPContext(is_ssl),
            socket: NewHTTPContext(is_ssl).HTTPSocket,
        ) void {
            log("handleOnDataHeader data: {s}", .{incoming_data});
            var to_read = incoming_data;
            var needs_move = true;
            if (this.state.response_message_buffer.list.items.len > 0) {
                bun.handleOom(this.state.response_message_buffer.appendSliceExact(incoming_data));
                to_read = this.state.response_message_buffer.list.items;
                needs_move = false;
            }

            while (true) {
                var amount_read: usize = 0;
                this.state.pending_response = picohttp.Response{};

                if (to_read.len < 16) {
                    log("handleShortRead", .{});
                    this.handleShortRead(is_ssl, incoming_data, socket, needs_move);
                    return;
                }

                const response = picohttp.Response.parseParts(
                    to_read,
                    &shared_response_headers_buf,
                    &amount_read,
                ) catch |err| {
                    switch (err) {
                        error.ShortRead => {
                            this.handleShortRead(is_ssl, incoming_data, socket, needs_move);
                        },
                        else => {
                            this.closeAndFail(err, is_ssl, socket);
                        },
                    }
                    return;
                };

                this.state.pending_response = response;
                to_read = to_read[@min(@as(usize, @intCast(response.bytes_read)), to_read.len)..];

                if (response.status_code == 101) {
                    if (this.flags.upgrade_state == .none) {
                        this.closeAndFail(error.UnrequestedUpgrade, is_ssl, socket);
                        return;
                    }
                    this.flags.upgrade_state = .upgraded;
                    if (this.signals.upgraded) |upgraded| {
                        upgraded.store(true, .monotonic);
                    }
                    this.flushStream(is_ssl, socket);
                    break;
                }

                if (response.status_code >= 100 and response.status_code < 200) {
                    log("information headers", .{});
                    this.state.pending_response = null;
                    if (to_read.len == 0) {
                        return;
                    }
                    continue;
                }

                break;
            }

            var response = this.state.pending_response.?;
            const should_continue = this.handleResponseMetadata(
                &response,
            ) catch |err| {
                this.closeAndFail(err, is_ssl, socket);
                return;
            };

            if (this.state.content_encoding_i < response.headers.list.len and !this.state.flags.did_set_content_encoding) {
                const mutable_headers: std.ArrayListUnmanaged(picohttp.Header) = .{ .items = response.headers.list, .capacity = response.headers.list.len };
                this.state.flags.did_set_content_encoding = true;
                response.headers = .{ .list = mutable_headers.items };
                this.state.content_encoding_i = std.math.maxInt(@TypeOf(this.state.content_encoding_i));
                this.state.pending_response = response;
            }

            if (should_continue == .finished) {
                if (this.state.flags.is_redirect_pending) {
                    this.doRedirect(is_ssl, ctx, socket);
                    return;
                }
                this.cloneMetadata();
                this.state.flags.received_last_chunk = true;
                this.state.content_length = 0;
                this.progressUpdate(is_ssl, ctx, socket);
                return;
            }

            if (this.flags.proxy_tunneling and this.proxy_tunnel == null) {
                this.startProxyHandshake(is_ssl, socket, to_read);
                return;
            }

            this.cloneMetadata();

            if (to_read.len == 0) {
                if (this.signals.get(.header_progress)) {
                    this.progressUpdate(is_ssl, ctx, socket);
                }
                return;
            }

            if (this.state.response_stage == .body) {
                const report_progress = this.handleResponseBody(to_read, true) catch |err| {
                    this.closeAndFail(err, is_ssl, socket);
                    return;
                };

                if (report_progress) {
                    this.progressUpdate(is_ssl, ctx, socket);
                    return;
                }
            } else if (this.state.response_stage == .body_chunk) {
                this.setTimeout(socket);
                const report_progress = this.handleResponseBodyChunkedEncoding(to_read) catch |err| {
                    this.closeAndFail(err, is_ssl, socket);
                    return;
                };

                if (report_progress) {
                    this.progressUpdate(is_ssl, ctx, socket);
                    return;
                }
            }

            if (this.signals.get(.header_progress)) {
                this.progressUpdate(is_ssl, ctx, socket);
                return;
            }
        }

        pub fn onData(
            this: *HTTPClient,
            comptime is_ssl: bool,
            incoming_data: []const u8,
            ctx: *NewHTTPContext(is_ssl),
            socket: NewHTTPContext(is_ssl).HTTPSocket,
        ) void {
            log("onData {}", .{incoming_data.len});
            if (this.signals.get(.aborted)) {
                this.closeAndAbort(is_ssl, socket);
                return;
            }

            if (this.proxy_tunnel) |proxy| {
                this.setTimeout(socket);
                proxy.receive(incoming_data);
                return;
            }

            switch (this.state.response_stage) {
                .pending, .headers => {
                    this.handleOnDataHeaders(is_ssl, incoming_data, ctx, socket);
                },
                .body => {
                    this.setTimeout(socket);

                    const report_progress = this.handleResponseBody(incoming_data, false) catch |err| {
                        this.closeAndFail(err, is_ssl, socket);
                        return;
                    };

                    if (report_progress) {
                        this.progressUpdate(is_ssl, ctx, socket);
                        return;
                    }
                },

                .body_chunk => {
                    this.setTimeout(socket);

                    const report_progress = this.handleResponseBodyChunkedEncoding(incoming_data) catch |err| {
                        this.closeAndFail(err, is_ssl, socket);
                        return;
                    };

                    if (report_progress) {
                        this.progressUpdate(is_ssl, ctx, socket);
                        return;
                    }
                },

                .fail => {},
                else => {
                    this.state.pending_response = null;
                    this.closeAndFail(error.UnexpectedData, is_ssl, socket);
                    return;
                },
            }
        }

        pub fn closeAndAbort(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            this.closeAndFail(error.Aborted, is_ssl, socket);
        }

        pub fn setTimeout(this: *HTTPClient, socket: anytype) void {
            if (this.flags.disable_timeout) return;
            socket.setTimeout(idle_timeout_seconds);
        }

        pub fn drainResponseBody(this: *HTTPClient, comptime is_ssl: bool, socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            switch (this.state.stage) {
                .done, .fail => return;
                else => {},
            }

            if (this.state.fail != null) {
                return;
            }

            if (this.state.flags.is_redirect_pending) {
                return;
            }

            const body_out_str = this.state.body_out_str orelse return;
            if (body_out_str.list.items.len == 0) {
                return;
            }

            this.sendProgressUpdateWithoutStageCheck(is_ssl, this.getSslCtx(is_ssl), socket);
        }

        fn sendProgressUpdateWithoutStageCheck(this: *HTTPClient, comptime is_ssl: bool, ctx: *NewHTTPContext(is_ssl), socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            if (this.flags.protocol != .http1_1) return this.sendProgressUpdateMultiplexed();
            const out_str = this.state.body_out_str.?;
            const body = out_str.*;
            const result = this.toResult();
            const is_done = !result.has_more;

            log("progressUpdate {}", .{is_done});

            const callback = this.result_callback;

            if (is_done) {
                this.unregisterAbortTracker();
                var tunnel_poolable = true;
                if (this.proxy_tunnel) |t| {
                    var wrapper_ok = false;
                    if (t.wrapper) |*w| {
                        wrapper_ok = !w.isShutdown();
                    }
                    tunnel_poolable = this.state.request_stage == .done and
                        t.write_buffer.isEmpty() and
                        wrapper_ok;
                }

                if (this.isKeepAlivePossible() and !socket.isClosedOrHasError() and tunnel_poolable) {
                    log("release socket", .{});
                    const tunnel = this.proxy_tunnel;
                    this.proxy_tunnel = null;
                    if (tunnel) |t| t.detachOwner(this);
                    ctx.releaseSocket(
                        socket,
                        this.flags.did_have_handshaking_error and !this.flags.reject_unauthorized,
                        this.connected_url.hostname,
                        this.connected_url.getPortAuto(),
                        this.tls_props,
                        tunnel,
                        if (tunnel != null) this.url.hostname else "",
                        if (tunnel != null) this.url.getPortAuto() else 0,
                        if (tunnel != null) this.proxyAuthHash() else 0,
                        null,
                    );
                } else {
                    if (this.proxy_tunnel) |tunnel| {
                        log("close the tunnel", .{});
                        this.proxy_tunnel = null;
                        tunnel.shutdown();
                        tunnel.detachAndDeref();
                    }
                    NewHTTPContext(is_ssl).closeSocket(socket);
                }

                this.state.reset(this.allocator);
                this.state.response_stage = .done;
                this.state.request_stage = .done;
                this.state.stage = .done;
                this.flags.proxy_tunneling = false;
                log("done", .{});
            }

            result.body.?.* = body;
            callback.run(@fieldParentPtr("client", this), result);

            if (comptime print_every > 0) {
                print_every_i += 1;
                if (print_every_i % print_every == 0) {
                    Output.prettyln("Heap stats for HTTP thread\n", .{});
                    Output.flush();
                    default_arena.dumpThreadStats();
                    print_every_i = 0;
                }
            }
        }

        pub fn progressUpdate(this: *HTTPClient, comptime is_ssl: bool, ctx: *NewHTTPContext(is_ssl), socket: NewHTTPContext(is_ssl).HTTPSocket) void {
            if (this.state.stage != .done and this.state.stage != .fail) {
                if (this.state.flags.is_redirect_pending and this.state.fail == null) {
                    if (this.state.isDone()) {
                        this.doRedirect(is_ssl, ctx, socket);
                    }
                    return;
                }

                this.sendProgressUpdateWithoutStageCheck(is_ssl, ctx, socket);
            }
        }

        pub fn doRedirect(
            this: *HTTPClient,
            comptime is_ssl: bool,
            ctx: *NewHTTPContext(is_ssl),
            socket: NewHTTPContext(is_ssl).HTTPSocket,
        ) void {
            if (this.flags.protocol != .http1_1) return this.doRedirectMultiplexed();
            log("doRedirect", .{});
            if (this.state.original_request_body == .stream) {
                this.flags.is_streaming_request_body = false;
            }

            this.unix_socket_path.deinit();
            this.unix_socket_path = jsc.ZigString.Slice.empty;
            const request_body = if (this.state.flags.resend_request_body_on_redirect and this.state.original_request_body == .bytes)
                this.state.original_request_body.bytes
            else
                "";

            this.state.response_message_buffer.deinit();

            const body_out_str = this.state.body_out_str.?;
            this.remaining_redirect_count = this.remaining_redirect_count -| 1;
            this.flags.redirected = true;
            assert(this.redirect_type == FetchRedirect.follow);
            this.unregisterAbortTracker();

            if (this.proxy_tunnel) |tunnel| {
                log("close the tunnel in redirect", .{});
                this.proxy_tunnel = null;
                tunnel.shutdown();
                tunnel.detachAndDeref();
                NewHTTPContext(is_ssl).closeSocket(socket);
            } else if (this.state.request_stage == .done and this.isKeepAlivePossible() and !socket.isClosedOrHasError()) {
                log("Keep-Alive release in redirect", .{});
                assert(this.connected_url.hostname.len > 0);
                ctx.releaseSocket(
                    socket,
                    this.flags.did_have_handshaking_error and !this.flags.reject_unauthorized,
                    this.connected_url.hostname,
                    this.connected_url.getPortAuto(),
                    this.tls_props,
                    null,
                    "",
                    0,
                    0,
                    null,
                );
            } else {
                NewHTTPContext(is_ssl).closeSocket(socket);
            }
            this.connected_url = bun.URL{};
            if (this.prev_redirect.len > 0) {
                bun.default_allocator.free(this.prev_redirect);
                this.prev_redirect = "";
            }

            if (this.remaining_redirect_count == 0) {
                this.fail(error.TooManyRedirects);
                return;
            }
            this.state.reset(this.allocator);
            log("doRedirect state reset", .{});
            this.flags.proxy_tunneling = false;
            if (this.proxy_tunnel) |tunnel| {
                this.proxy_tunnel = null;
                tunnel.detachAndDeref();
            }
            this.flags.protocol = .http1_1;

            return this.start(.{ .bytes = request_body }, body_out_str);
        }

        pub fn handleResponseBody(this: *HTTPClient, incoming_data: []const u8, is_only_buffer: bool) !bool {
            assert(this.state.transfer_encoding == .identity);
            const content_length = this.state.content_length;
            // is it exactly as much as we need?
            if (is_only_buffer and content_length != null and incoming_data.len >= content_length.?) {
                try handleResponseBodyFromSinglePacket(this, incoming_data[0..content_length.?]);
                return true;
            } else {
                return handleResponseBodyFromMultiplePackets(this, incoming_data);
            }
        }

        fn handleResponseBodyFromSinglePacket(this: *HTTPClient, incoming_data: []const u8) !void {
            if (!this.state.isChunkedEncoding()) {
                this.state.total_body_received += incoming_data.len;
                log("handleResponseBodyFromSinglePacket {d}", .{this.state.total_body_received});
            }
            defer {
                if (this.progress_node) |progress| {
                    progress.activate();
                    progress.setCompletedItems(incoming_data.len);
                    progress.context.maybeRefresh();
                }
            }
            // we can ignore the body data in redirects
            if (this.state.flags.is_redirect_pending) return;

            if (this.state.encoding.isCompressed()) {
                try this.state.decompressBytes(incoming_data, this.state.body_out_str.?, true);
            } else {
                try this.state.getBodyBuffer().appendSliceExact(incoming_data);
            }

            if (this.state.response_message_buffer.owns(incoming_data)) {
                if (comptime Environment.allow_assert) {
                    // i'm not sure why this would happen and i haven't seen it happen
                    // but we should check
                    assert(this.state.getBodyBuffer().list.items.ptr != this.state.response_message_buffer.list.items.ptr);
                }

                this.state.response_message_buffer.deinit();
            }
        }

        fn handleResponseBodyFromMultiplePackets(this: *HTTPClient, incoming_data: []const u8) !bool {
            var buffer = this.state.getBodyBuffer();
            const content_length = this.state.content_length;

            var remainder: []const u8 = undefined;
            if (content_length != null) {
                const remaining_content_length = content_length.? -| this.state.total_body_received;
                remainder = incoming_data[0..@min(incoming_data.len, remaining_content_length)];
            } else {
                remainder = incoming_data;
            }

            // we can ignore the body data in redirects
            if (!this.state.flags.is_redirect_pending) {
                if (buffer.list.items.len == 0 and incoming_data.len < preallocate_max) {
                    buffer.list.ensureTotalCapacityPrecise(buffer.allocator, incoming_data.len) catch {};
                }

                _ = try buffer.write(remainder);
            }

            this.state.total_body_received += remainder.len;
            log("handleResponseBodyFromMultiplePackets {d}", .{this.state.total_body_received});
            if (this.progress_node) |progress| {
                progress.activate();
                progress.setCompletedItems(this.state.total_body_received);
                progress.context.maybeRefresh();
            }

            // done or streaming
            const is_done = content_length != null and this.state.total_body_received >= content_length.?;
            if (is_done or this.signals.get(.response_body_streaming) or content_length == null) {
                const is_final_chunk = is_done;
                const processed = try this.state.processBodyBuffer(buffer.*, is_final_chunk);

                // We can only use the libdeflate fast path when we are not streaming
                // If we ever call processBodyBuffer again, it cannot go through the fast path.
                this.state.flags.is_libdeflate_fast_path_disabled = true;

                if (this.progress_node) |progress| {
                    progress.activate();
                    progress.setCompletedItems(this.state.total_body_received);
                    progress.context.maybeRefresh();
                }
                return is_done or processed;
            }
            return false;
        }

        pub fn handleResponseBodyChunkedEncoding(
            this: *HTTPClient,
            incoming_data: []const u8,
        ) !bool {
            if (incoming_data.len <= single_packet_small_buffer.len and this.state.getBodyBuffer().list.items.len == 0) {
                return try this.handleResponseBodyChunkedEncodingFromSinglePacket(incoming_data);
            } else {
                return try this.handleResponseBodyChunkedEncodingFromMultiplePackets(incoming_data);
            }
        }

        fn handleResponseBodyChunkedEncodingFromMultiplePackets(
            this: *HTTPClient,
            incoming_data: []const u8,
        ) !bool {
            let decoder = *this.state.chunked_decoder;
            let buffer_ptr = this.state.getBodyBuffer();
            var buffer = buffer_ptr.*;
            try buffer.appendSlice(incoming_data);

            // set consume_trailer to 1 to discard the trailing header
            // using content-encoding per chunk is not supported
            decoder.consume_trailer = 1;

            var bytes_decoded = incoming_data.len;
            // phr_decode_chunked mutates in-place
            const pret = picohttp.phr_decode_chunked(
                decoder,
                buffer.list.items.ptr + (buffer.list.items.len -| incoming_data.len),
                &bytes_decoded,
            );
            buffer.list.items.len = buffer.list.items.len -| (incoming_data.len - bytes_decoded);
            this.state.total_body_received += bytes_decoded;
            log("handleResponseBodyChunkedEncodingFromMultiplePackets {d}", .{this.state.total_body_received});

            buffer_ptr.* = buffer;

            switch (pret) {
                // Invalid HTTP response body
                -1 => return error.InvalidHTTPResponse,
                // Needs more data
                -2 => {
                    if (this.progress_node) |progress| {
                        progress.activate();
                        progress.setCompletedItems(buffer.list.items.len);
                        progress.context.maybeRefresh();
                    }
                    // streaming chunks
                    if (this.signals.get(.response_body_streaming)) {
                        // If we're streaming, we cannot use the libdeflate fast path
                        this.state.flags.is_libdeflate_fast_path_disabled = true;
                        return try this.state.processBodyBuffer(buffer, false);
                    }

                    return false;
                },
                // Done
                else => {
                    this.state.flags.received_last_chunk = true;
                    _ = try this.state.processBodyBuffer(
                        buffer,
                        true,
                    );

                    if (this.progress_node) |progress| {
                        progress.activate();
                        progress.setCompletedItems(buffer.list.items.len);
                        progress.context.maybeRefresh();
                    }

                    return true;
                },
            }

            unreachable;
        }

        fn handleResponseBodyChunkedEncodingFromSinglePacket(
            this: *HTTPClient,
            incoming_data: []const u8,
        ) !bool {
            let decoder = *this.state.chunked_decoder;
            assert(incoming_data.len <= single_packet_small_buffer.len);

            // set consume_trailer to 1 to discard the trailing header
            // using content-encoding per chunk is not supported
            decoder.consume_trailer = 1;

            var buffer: []u8 = undefined;

            if (
                // if we've already copied the buffer once, we can avoid copying it again.
                this.state.response_message_buffer.owns(incoming_data)
            ) {
                buffer = @constCast(incoming_data);
            } else {
                buffer = single_packet_small_buffer[0..incoming_data.len];
                @memcpy(buffer[0..incoming_data.len], incoming_data);
            }

            var bytes_decoded = incoming_data.len;
            // phr_decode_chunked mutates in-place
            const pret = picohttp.phr_decode_chunked(
                decoder,
                buffer.ptr + (buffer.len -| incoming_data.len),
                &bytes_decoded,
            );
            buffer.len = buffer.len -| (incoming_data.len - bytes_decoded);
            this.state.total_body_received += bytes_decoded;
            log("handleResponseBodyChunkedEncodingFromSinglePacket {d}", .{this.state.total_body_received});
            switch (pret) {
                // Invalid HTTP response body
                -1 => {
                    return error.InvalidHTTPResponse;
                },
                // Needs more data
                -2 => {
                    if (this.progress_node) |progress| {
                        progress.activate();
                        progress.setCompletedItems(buffer.len);
                        progress.context.maybeRefresh();
                    }
                    const body_buffer = this.state.getBodyBuffer();
                    try body_buffer.appendSliceExact(buffer);

                    // streaming chunks
                    if (this.signals.get(.response_body_streaming)) {
                        // If we're streaming, we cannot use the libdeflate fast path
                        this.state.flags.is_libdeflate_fast_path_disabled = true;

                        return try this.state.processBodyBuffer(body_buffer.*, true);
                    }

                    return false;
                },
                // Done
                else => {
                    this.state.flags.received_last_chunk = true;
                    try this.handleResponseBodyFromSinglePacket(buffer);
                    assert(this.state.body_out_str.?.list.items.ptr != buffer.ptr);
                    if (this.progress_node) |progress| {
                        progress.activate();
                        progress.setCompletedItems(buffer.len);
                        progress.context.maybeRefresh();
                    }

                    return true;
                },
            }

            unreachable;
        }

        pub fn handleResponseMetadata(
            this: *HTTPClient,
            response: *picohttp.Response,
        ) !ShouldContinue {
            var location: string = "";
            var pretend_304 = false;
            var is_server_sent_events = false;
            for (response.headers.list, 0..) |header, header_i| {
                const header_hash = hashHeaderName(header.name);
                if (header_hash == content_length_header_hash) {
                    const content_length = std.fmt.parseInt(usize, header.value, 10) catch 0;
                    if (this.method.hasBody()) {
                        this.state.content_length = content_length;
                    } else {
                        // ignore body size for HEAD requests
                        this.state.content_length = 0;
                    }
                } else if (header_hash == content_type_header_hash) {
                    if (strings.contains(header.value, "text/event-stream")) {
                        is_server_sent_events = true;
                    }
                } else if (header_hash == content_encoding_header_hash) {
                    if (!this.flags.disable_decompression) {
                        if (strings.eqlComptime(header.value, "gzip")) {
                            this.state.encoding = Encoding.gzip;
                            this.state.content_encoding_i = @as(u8, @truncate(header_i));
                        } else if (strings.eqlComptime(header.value, "deflate")) {
                            this.state.encoding = Encoding.deflate;
                            this.state.content_encoding_i = @as(u8, @truncate(header_i));
                        } else if (strings.eqlComptime(header.value, "br")) {
                            this.state.encoding = Encoding.brotli;
                            this.state.content_encoding_i = @as(u8, @truncate(header_i));
                        } else if (strings.eqlComptime(header.value, "zstd")) {
                            this.state.encoding = Encoding.zstd;
                            this.state.content_encoding_i = @as(u8, @truncate(header_i));
                        }
                    }
                } else if (header_hash == transfer_encoding_header_hash) {
                    if (strings.eqlComptime(header.value, "gzip")) {
                        if (!this.flags.disable_decompression) {
                            this.state.transfer_encoding = Encoding.gzip;
                        }
                    } else if (strings.eqlComptime(header.value, "deflate")) {
                        if (!this.flags.disable_decompression) {
                            this.state.transfer_encoding = Encoding.deflate;
                        }
                    } else if (strings.eqlComptime(header.value, "br")) {
                        if (!this.flags.disable_decompression) {
                            this.state.transfer_encoding = .brotli;
                        }
                    } else if (strings.eqlComptime(header.value, "zstd")) {
                        if (!this.flags.disable_decompression) {
                            this.state.transfer_encoding = .zstd;
                        }
                    } else if (strings.eqlComptime(header.value, "identity")) {
                        this.state.transfer_encoding = Encoding.identity;
                    } else if (strings.eqlComptime(header.value, "chunked")) {
                        this.state.transfer_encoding = Encoding.chunked;
                    } else {
                        return error.UnsupportedTransferEncoding;
                    }
                } else if (header_hash == location_header_hash) {
                    location = header.value;
                } else if (header_hash == connection_header_hash) {
                    if (response.status_code >= 200 and response.status_code <= 299) {
                        // HTTP headers are case-insensitive (RFC 7230)
                        if (std.ascii.eqlIgnoreCase(header.value, "close")) {
                            this.state.flags.allow_keepalive = false;
                        } else if (std.ascii.eqlIgnoreCase(header.value, "keep-alive")) {
                            this.state.flags.allow_keepalive = true;
                        }
                    }
                } else if (header_hash == last_modified_header_hash) {
                    pretend_304 = this.flags.force_last_modified and response.status_code > 199 and response.status_code < 300 and this.if_modified_since.len > 0 and strings.eql(this.if_modified_since, header.value);
                } else if (header_hash == alt_svc_header_hash) {
                    // Record regardless of *this* request's shape — a future
                    // request to the same origin may be h3-eligible even if this
                    // one was pinned/proxied/sendfile.
                    if (this.isHTTPS() and this.unix_socket_path.length() == 0 and h3AltSvcEnabled()) {
                        H3.AltSvc.record(this.url.hostname, this.url.getPortAuto(), header.value);
                    }
                }
            }

            if (this.verbose != .none) {
                printResponse(response.*);
            }

            if (pretend_304) {
                response.status_code = 304;
            }

            // According to RFC 7230 section 3.3.3:
            //   1. Any response to a HEAD request and any response with a 1xx (Informational),
            //      204 (No Content), or 304 (Not Modified) status code
            //      [...] cannot contain a message body or trailer section.
            // Therefore in these cases set content-length to 0, so the response body is always ignored
            // and is not waited for (which could cause a timeout).
            // This applies regardless of whether we're using a proxy tunnel or not,
            // since these status codes NEVER have a body per the HTTP spec.
            if ((response.status_code >= 100 and response.status_code < 200) or response.status_code == 204 or response.status_code == 304) {
                this.state.content_length = 0;
            }

            // Don't do this for proxies because those connections will be open for awhile.
            if (!this.flags.proxy_tunneling) {
                //
                // according to RFC 7230 section 6.3:
                //   In order to remain persistent, all messages on a connection need to
                //   have a self-defined message length (i.e., one not defined by closure
                //   of the connection)
                // therefore, if response has no content-length header and is not chunked, implicitly disable
                // the keep-alive behavior (keep-alive being the default behavior for HTTP/1.1 and not for HTTP/1.0)
                //
                // but, we must only do this IF the status code allows it to contain a body.
                if (this.state.content_length == null and this.state.transfer_encoding != .chunked) {
                    this.state.flags.allow_keepalive = false;
                }
            }

            if (this.flags.proxy_tunneling and this.proxy_tunnel == null) {
                if (response.status_code == 200) {
                    // signal to continue the proxing
                    return ShouldContinue.continue_streaming;
                }

                // proxy denied connection so return proxy result (407, 403 etc)
                this.flags.proxy_tunneling = false;
                this.flags.disable_keepalive = true;
            }

            const status_code = response.status_code;

            if (status_code == 407) {
                // If the request is being proxied and passes through the 407 status code, then let's also not do HTTP Keep-Alive.
                this.flags.disable_keepalive = true;
            }

            // if is no redirect or if is redirect == "manual" just proceed
            const is_redirect = status_code >= 300 and status_code <= 399;
            if (is_redirect) {
                if (this.redirect_type == FetchRedirect.follow and location.len > 0 and this.remaining_redirect_count > 0) {
                    switch (status_code) {
                        302, 301, 307, 308, 303 => {
                            // https://fetch.spec.whatwg.org/#http-redirect-fetch step 11:
                            // "If internalResponse's status is not 303, request's body
                            // is non-null, and request's body's source is null, then
                            // return a network error." A ReadableStream body has no
                            // source to replay from, so only 303 (which drops the body
                            // and switches to GET) may be followed.
                            if (status_code != 303 and this.state.original_request_body == .stream) {
                                return error.RequestBodyNotReusable;
                            }
                            var is_same_origin = true;

                            {
                                var url_arena = std.heap.ArenaAllocator.init(bun.default_allocator);
                                defer url_arena.deinit();
                                var fba = std.heap.stackFallback(4096, url_arena.allocator());
                                const url_allocator = fba.get();
                                if (strings.indexOf(location, "://")) |i| {
                                    var string_builder = bun.StringBuilder{};

                                    const is_protocol_relative = i == 0;
                                    const protocol_name = if (is_protocol_relative) this.url.displayProtocol() else location[0..i];
                                    const is_http = strings.eqlComptime(protocol_name, "http");
                                    if (is_http or strings.eqlComptime(protocol_name, "https")) {} else {
                                        return error.UnsupportedRedirectProtocol;
                                    }

                                    if ((protocol_name.len * @as(usize, @intFromBool(is_protocol_relative))) + location.len > MAX_REDIRECT_URL_LENGTH) {
                                        return error.RedirectURLTooLong;
                                    }

                                    string_builder.count(location);

                                    if (is_protocol_relative) {
                                        if (is_http) {
                                            string_builder.count("http");
                                        } else {
                                            string_builder.count("https");
                                        }
                                    }

                                    try string_builder.allocate(url_allocator);

                                    if (is_protocol_relative) {
                                        if (is_http) {
                                            _ = string_builder.append("http");
                                        } else {
                                            _ = string_builder.append("https");
                                        }
                                    }

                                    _ = string_builder.append(location);

                                    if (comptime Environment.allow_assert)
                                        assert(string_builder.cap == string_builder.len);

                                    const normalized_url = jsc.URL.hrefFromString(bun.String.fromBytes(string_builder.allocatedSlice()));
                                    defer normalized_url.deref();
                                    if (normalized_url.tag == .Dead) {
                                        // URL__getHref failed, dont pass dead tagged string to toOwnedSlice.
                                        return error.RedirectURLInvalid;
                                    }
                                    const normalized_url_str = try normalized_url.toOwnedSlice(bun.default_allocator);

                                    const new_url = URL.parse(normalized_url_str);
                                    is_same_origin = strings.eqlCaseInsensitiveASCII(strings.withoutTrailingSlash(new_url.origin), strings.withoutTrailingSlash(this.url.origin), true);
                                    this.url = new_url;
                                    // connected_url still borrows from the previous hop's buffer
                                    // until doRedirect releases the socket, so park it in
                                    // prev_redirect for doRedirect to free instead of leaking it.
                                    bun.debugAssert(this.prev_redirect.len == 0);
                                    this.prev_redirect = this.redirect;
                                    this.redirect = normalized_url_str;
                                } else if (strings.hasPrefixComptime(location, "//")) {
                                    var string_builder = bun.StringBuilder{};

                                    const protocol_name = this.url.displayProtocol();

                                    if (protocol_name.len + 1 + location.len > MAX_REDIRECT_URL_LENGTH) {
                                        return error.RedirectURLTooLong;
                                    }

                                    const is_http = strings.eqlComptime(protocol_name, "http");

                                    if (is_http) {
                                        string_builder.count("http:");
                                    } else {
                                        string_builder.count("https:");
                                    }

                                    string_builder.count(location);

                                    try string_builder.allocate(url_allocator);

                                    if (is_http) {
                                        _ = string_builder.append("http:");
                                    } else {
                                        _ = string_builder.append("https:");
                                    }

                                    _ = string_builder.append(location);

                                    if (comptime Environment.allow_assert)
                                        assert(string_builder.cap == string_builder.len);

                                    const normalized_url = jsc.URL.hrefFromString(bun.String.fromBytes(string_builder.allocatedSlice()));
                                    defer normalized_url.deref();
                                    const normalized_url_str = try normalized_url.toOwnedSlice(bun.default_allocator);

                                    const new_url = URL.parse(normalized_url_str);
                                    is_same_origin = strings.eqlCaseInsensitiveASCII(strings.withoutTrailingSlash(new_url.origin), strings.withoutTrailingSlash(this.url.origin), true);
                                    this.url = new_url;
                                    bun.debugAssert(this.prev_redirect.len == 0);
                                    this.prev_redirect = this.redirect;
                                    this.redirect = normalized_url_str;
                                } else {
                                    const original_url = this.url;

                                    const new_url_ = bun.jsc.URL.join(
                                        bun.String.fromBytes(original_url.href),
                                        bun.String.fromBytes(location),
                                    );
                                    defer new_url_.deref();

                                    if (new_url_.isEmpty()) {
                                        return error.InvalidRedirectURL;
                                    }

                                    const new_url = new_url_.toOwnedSlice(bun.default_allocator) catch {
                                        return error.RedirectURLTooLong;
                                    };
                                    this.url = URL.parse(new_url);
                                    is_same_origin = strings.eqlCaseInsensitiveASCII(strings.withoutTrailingSlash(this.url.origin), strings.withoutTrailingSlash(original_url.origin), true);
                                    bun.debugAssert(this.prev_redirect.len == 0);
                                    this.prev_redirect = this.redirect;
                                    this.redirect = new_url;
                                }
                            }

                            // If one of the following is true
                            // - internalResponse’s status is 301 or 302 and request’s method is `POST`
                            // - internalResponse’s status is 303 and request’s method is not `GET` or `HEAD`
                            // then:
                            if (((status_code == 301 or status_code == 302) and this.method == .POST) or
                                (status_code == 303 and this.method != .GET and this.method != .HEAD))
                            {
                                // - Set request’s method to `GET` and request’s body to null.
                                this.method = .GET;

                                // https://github.com/oven-sh/bun/issues/6053
                                if (this.header_entries.len > 0) {
                                    // A request-body-header name is a header name that is a byte-case-insensitive match for one of:
                                    // - `Content-Encoding`
                                    // - `Content-Language`
                                    // - `Content-Location`
                                    // - `Content-Type`
                                    const @"request-body-header" = &.{
                                        "Content-Encoding",
                                        "Content-Language",
                                        "Content-Location",
                                    };
                                    var i: usize = 0;

                                    // - For each headerName of request-body-header name, delete headerName from request’s header list.
                                    const names = this.header_entries.items(.name);
                                    var len = names.len;
                                    outer: while (i < len) {
                                        const name = this.headerStr(names[i]);
                                        if (name.len == 12) {
                                            const hash = hashHeaderName(name);
                                            if (hash == content_type_header_hash) {
                                                _ = this.header_entries.orderedRemove(i);
                                                len = this.header_entries.len;
                                                continue :outer;
                                            }
                                        } else if (name.len == 16) {
                                            const hash = hashHeaderName(name);
                                            inline for (@"request-body-header") |hash_value| {
                                                if (hash == hashHeaderConst(hash_value)) {
                                                    _ = this.header_entries.orderedRemove(i);
                                                    len = this.header_entries.len;
                                                    continue :outer;
                                                }
                                            }
                                        }
                                        i += 1;
                                    }
                                }
                            }

                            // https://fetch.spec.whatwg.org/#concept-http-redirect-fetch
                            // If request’s current URL’s origin is not same origin with
                            // locationURL’s origin, then for each headerName of CORS
                            // non-wildcard request-header name, delete headerName from
                            // request’s header list.
                            if (!is_same_origin and this.header_entries.len > 0) {
                                const hashes_to_remove = [
                                    authorization_header_hash,
                                    proxy_authorization_header_hash,
                                    cookie_header_hash,
                                ];
                                inline for (hashes_to_remove) |target_hash| {
                                    const names = this.header_entries.items(.name);

                                    for (names, 0..) |name_ptr, i| {
                                        const name = this.headerStr(name_ptr);
                                        const hash = hashHeaderName(name);
                                        if (hash == target_hash) {
                                            this.header_entries.orderedRemove(i);
                                            break;
                                        }
                                    }
                                }
                            }
                            this.state.flags.is_redirect_pending = true;
                            if (this.method.hasRequestBody()) {
                                this.state.flags.resend_request_body_on_redirect = true;
                            }
                        },
                        else => {},
                    }
                } else if (this.redirect_type == FetchRedirect.@"error") {
                    // error out if redirect is not allowed
                    return error.UnexpectedRedirect;
                }
            }

            this.state.response_stage = if (this.state.transfer_encoding == .chunked) .body_chunk else .body;
            const content_length = this.state.content_length;
            if (content_length) |length| {
                log("handleResponseMetadata: content_length is {} and transfer_encoding {}", .{ length, this.state.transfer_encoding });
            } else {
                log("handleResponseMetadata: content_length is null and transfer_encoding {}", .{ this.state.transfer_encoding });
            }
            if (this.flags.upgrade_state == .upgraded) {
                this.state.content_length = null;
                this.state.flags.allow_keepalive = false;
                return ShouldContinue.continue_streaming;
            }

            if (this.method.hasBody() and (content_length == null or content_length.? > 0 or !this.state.flags.allow_keepalive or this.state.transfer_encoding == .chunked or is_server_sent_events)) {
                return ShouldContinue.continue_streaming;
            } else {
                return ShouldContinue.finished;
            }
        }
}

export @value struct HTTPResponseMetadata {
    url: string = "",
    owned_buf: u8[] = "",
    response: bun.picohttp.Response = {},

    fn deinit(*this, allocator: std.mem.Allocator): void {
        if (this.owned_buf.len > 0) allocator.free(this.owned_buf);
        if (this.response.headers.list.len > 0) allocator.free(this.response.headers.list);
        this.owned_buf = "";
        this.url = "";
        this.response.headers = {};
        this.response.status = "";
    }
}

export @value struct HTTPClientResult {
    body: *bun.MutableString? = null,
    has_more: bool = false,
    redirected: bool = false,
    can_stream: bool = false,
    is_http2: bool = false,

    fail: anyerror? = null,

    metadata: HTTPResponseMetadata? = null,
    body_size: BodySize = .unknown,
    certificate_info: CertificateInfo? = null,

    fn abortReason(this): bun.jsc.CommonAbortReason? {
        if (this.isTimeout()) {
            return .Timeout;
        }

        if (this.isAbort()) {
            return .UserAbort;
        }

        return null;
    }

    fn isSuccess(this): bool {
        return this.fail == null;
    }

    fn isTimeout(this): bool {
        if (e := this.fail) {
            return e == error.Timeout;
        }
        return false;
    }

    fn isAbort(this): bool {
        if (e := this.fail) {
            return e == error.Aborted || e == error.AbortedBeforeConnecting;
        }
        return false;
    }

    pub const BodySize = union(enum) {
        total_received: usize,
        content_length: usize,
        unknown: void,
    };

    pub const Callback = struct {
        ctx: HttpCallbackCtx,
        function: Function,

        pub const Function = *const fn (ctx: HttpCallbackCtx, async_http: *AsyncHTTP, result: HTTPClientResult) void;

        pub fn run(self: Callback, async_http: *AsyncHTTP, result: HTTPClientResult) void {
            self.function(self.ctx, async_http, result);
        }

        pub fn New(comptime Type: type, comptime callback: anytype) type {
            return struct {
                pub fn init(this: Type) Callback {
                    return Callback{
                        .ctx = this,
                        .function = wrapped_callback,
                    };
                }

                fn wrapped_callback(ptr: HttpCallbackCtx, async_http: *AsyncHTTP, result: HTTPClientResult) void {
                    const casted: Type = @ptrCast(@alignCast(ptr));
                    @call(bun.callmod_inline, callback, .{ casted, async_http, result });
                }
            };
        }
    };
}
