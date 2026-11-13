import std from "std";
import bun from "bun";
import WebSocketDeflate from "./WebSocketDeflate.zig";
import WebSocketProxy from "./WebSocketProxy.zig";
import WebSocketProxyTunnel from "./WebSocketProxyTunnel.zig";
import CppWebSocketPkg from "./CppWebSocket.zig";
import websocket_client from "../websocket_client.zig";

const CppWebSocket = CppWebSocketPkg.CppWebSocket;
const ErrorCode = websocket_client.ErrorCode;

const Async = bun.Async;
const BoringSSL = bun.BoringSSL;
const Environment = bun.Environment;
const Output = bun.Output;
const PicoHTTP = bun.picohttp;
const PicoHeader = PicoHTTP.Header;
const default_allocator = bun.default_allocator;
const jsc = bun.jsc;
const strings = bun.strings;
const uws = bun.uws;
const Headers = bun.http.Headers;
const SSLConfig = jsc.API.ServerConfig.SSLConfig;
const DeflateParams = WebSocketDeflate.Params;
const ByteList = std.ArrayListUnmanaged(u8);
const ZigSlice = jsc.ZigString.Slice;

const log = Output.scoped(.WebSocketUpgradeClient, .visible);

const empty_u8: [0]u8 = [];
const empty_zig_slices: [0]ZigSlice = [];
const empty_byte_slices: [0][]const u8 = [];

export struct TopDeflateNegotiationResult {
    enabled: bool = false,
    params: DeflateParams = DeflateParams{},
}

export enum UpgradeState {
    initializing,
    reading,
    failed,
    proxy_handshake,
    proxy_tls_handshake,
    done,
}

export struct Headers8Bit {
    slices: []ZigSlice,
    name_slices: [][]const u8,
    value_slices: [][]const u8,
    allocator: std.mem.Allocator,

    pub static fn init(
        allocator: std.mem.Allocator,
        names_ptr: ([*]const bun.String!)?,
        values_ptr: ([*]const bun.String!)?,
        len: usize,
    ) std.mem.Allocator.Error!Headers8Bit {
        if (len == 0) {
            return Headers8Bit{
                slices: empty_zig_slices[0..0],
                name_slices: empty_byte_slices[0..0],
                value_slices: empty_byte_slices[0..0],
                allocator: allocator,
            };
        }
        const names_in = unsafe { names_ptr.?[0..len] };
        const values_in = unsafe { values_ptr.?[0..len] };

        let slices = allocator.alloc(ZigSlice, len * 2)?;
        errdefer allocator.free(slices);

        let name_slices = allocator.alloc(string, len)?;
        errdefer allocator.free(name_slices);

        let value_slices = allocator.alloc(string, len)?;
        errdefer allocator.free(value_slices);

        let decoded: usize = 0;
        errdefer {
            let j: usize = 0;
            while (j < decoded) {
                slices[j].deinit();
                j += 1;
            }
        }
        let i: usize = 0;
        while (i < len) {
            slices[i * 2] = names_in[i].toUTF8(allocator);
            decoded += 1;
            slices[i * 2 + 1] = values_in[i].toUTF8(allocator);
            decoded += 1;
            name_slices[i] = slices[i * 2].slice();
            value_slices[i] = slices[i * 2 + 1].slice();
            i += 1;
        }

        return Headers8Bit{
            slices: slices,
            name_slices: name_slices,
            value_slices: value_slices,
            allocator: allocator,
        };
    }

    pub fn deinit(this) void {
        const slices = this.slices;
        for (slices) |*s| s.deinit();
        if (this.slices.len > 0) {
            this.allocator.free(this.slices);
            this.allocator.free(this.name_slices);
            this.allocator.free(this.value_slices);
        }
    }

    pub fn names(this) []const []const u8 {
        return this.name_slices;
    }

    pub fn values(this) []const []const u8 {
        return this.value_slices;
    }

    pub fn toHeaders(this, allocator: std.mem.Allocator) anyerror!Headers {
        let headers = Headers{
            allocator: allocator,
        };
        errdefer headers.deinit();

        const n_slices = this.name_slices;
        const v_slices = this.value_slices;
        for (n_slices, v_slices) |name, value| {
            headers.append(name, value)?;
        }

        return headers;
    }
}

export struct BuildRequestResult {
    body: []u8,
    expected_accept: [28]u8,
}

fn buildConnectRequest(
    target_host: []const u8,
    target_port: u16,
    proxy_authorization: ?[]const u8,
    proxy_headers: ?Headers,
) std.mem.Allocator.Error![]u8 {
    const allocator = bun.default_allocator;

    let buf = std.array_list.Managed(u8).init(allocator);
    errdefer buf.deinit();
    const writer = buf.writer();

    writer.print("CONNECT {s}:{d} HTTP/1.1\r\n", .{ target_host, target_port })?;
    writer.print("Host: {s}:{d}\r\n", .{ target_host, target_port })?;
    writer.writeAll("Proxy-Connection: Keep-Alive\r\n")?;

    if (auth := proxy_authorization) {
        writer.print("Proxy-Authorization: {s}\r\n", .{auth})?;
    }

    if (hdrs := proxy_headers) {
        const slice = hdrs.entries.slice();
        const hdr_names = slice.items(.name);
        const hdr_values = slice.items(.value);
        for (hdr_names, 0..) |name_ptr, idx| {
            const name = hdrs.asStr(name_ptr);
            if (proxy_authorization != null && strings.eqlCaseInsensitiveASCII(name, "proxy-authorization", true)) {
                continue;
            }
            writer.print("{s}: {s}\r\n", .{ name, hdrs.asStr(hdr_values[idx]) })?;
        }
    }

    writer.writeAll("\r\n")?;

    return buf.toOwnedSlice();
}

fn computeAcceptValue(key: []const u8) [28]u8 {
    const websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    let hasher = bun.sha.Hashers.SHA1.init();
    hasher.update(key);
    hasher.update(websocket_guid);
    let hash: bun.sha.Hashers.SHA1.Digest = undefined;
    hasher.final(&hash);
    let result: [28]u8 = undefined;
    _ = bun.base64.encode(&result, &hash);
    return result;
}

fn buildRequestBody(
    vm: *jsc.VirtualMachine,
    pathname: []const u8,
    is_https: bool,
    host: []const u8,
    port: u16,
    client_protocol: []const u8,
    extra_headers: Headers8Bit,
    target_authorization: ?[]const u8,
    offer_permessage_deflate: bool,
) std.mem.Allocator.Error!BuildRequestResult {
    const allocator = bun.default_allocator;

    let user_host: ?[]const u8 = null;
    let user_key: ?[]const u8 = null;
    let user_protocol: ?[]const u8 = null;
    let user_authorization: bool = false;

    const extra_names = extra_headers.names();
    const extra_values = extra_headers.values();
    for (extra_names, extra_values) |name_slice, value| {
        if (user_host == null && strings.eqlCaseInsensitiveASCII(name_slice, "host", true)) {
            user_host = value;
        } else if (user_key == null && strings.eqlCaseInsensitiveASCII(name_slice, "sec-websocket-key", true)) {
            user_key = value;
        } else if (user_protocol == null && strings.eqlCaseInsensitiveASCII(name_slice, "sec-websocket-protocol", true)) {
            user_protocol = value;
        } else if (!user_authorization && strings.eqlCaseInsensitiveASCII(name_slice, "authorization", true)) {
            user_authorization = true;
        }
    }

    let encoded_buf: [24]u8 = undefined;
    const key = blk: {
        if (k_slice := user_key) {
            let decoded_buf: [24]u8 = undefined;
            const decoded_len = std.base64.standard.Decoder.calcSizeForSlice(k_slice) catch 0;

            if (decoded_len == 16) {
                let decode_ok = true;
                std.base64.standard.Decoder.decode(&decoded_buf, k_slice) catch {
                    decode_ok = false;
                };
                if (decode_ok) {
                    break :blk k_slice;
                }
            }
        }
        break :blk std.base64.standard.Encoder.encode(&encoded_buf, &vm.rareData().nextUUID().bytes);
    };

    const expected_accept = computeAcceptValue(key);
    const protocol = if (user_protocol != null) user_protocol.? else client_protocol;

    const host_fmt = bun.fmt.HostFormatter{
        is_https: is_https,
        host: host,
        port: port,
    };

    let static_headers: [2]PicoHeader = [
        PicoHeader{ name: "Sec-WebSocket-Key", value: key },
        PicoHeader{ name: "Sec-WebSocket-Protocol", value: protocol },
    ];

    const headers_ = static_headers[0 .. 1 + @as(usize, @intFromBool(protocol.len > 0))];
    const pico_headers = PicoHTTP.Headers{ headers: headers_ };

    let extra_headers_buf = std.array_list.Managed(u8).init(allocator);
    defer extra_headers_buf.deinit();
    const writer = extra_headers_buf.writer();

    if (!user_authorization) {
        if (auth := target_authorization) {
            writer.print("Authorization: {s}\r\n", .{auth})?;
        }
    }

    for (extra_names, extra_values) |name_slice, value| {
        if (strings.eqlCaseInsensitiveASCII(name_slice, "host", true) ||
            strings.eqlCaseInsensitiveASCII(name_slice, "connection", true) ||
            strings.eqlCaseInsensitiveASCII(name_slice, "upgrade", true) ||
            strings.eqlCaseInsensitiveASCII(name_slice, "sec-websocket-version", true) ||
            strings.eqlCaseInsensitiveASCII(name_slice, "sec-websocket-extensions", true) ||
            strings.eqlCaseInsensitiveASCII(name_slice, "sec-websocket-key", true) ||
            strings.eqlCaseInsensitiveASCII(name_slice, "sec-websocket-protocol", true))
        {
            continue;
        }
        writer.print("{s}: {s}\r\n", .{ name_slice, value })?;
    }

    const extensions_line: []const u8 = if (offer_permessage_deflate)
        "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"
    else
        "";

    if (h := user_host) {
        return BuildRequestResult{
            body: std.fmt.allocPrint(
                allocator,
                "GET {s} HTTP/1.1\r\nHost: {s}\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n{s}{f}{s}\r\n",
                .{ pathname, h, extensions_line, pico_headers, extra_headers_buf.items },
            )?,
            expected_accept: expected_accept,
        };
    }

    return BuildRequestResult{
        body: std.fmt.allocPrint(
            allocator,
            "GET {s} HTTP/1.1\r\nHost: {f}\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n{s}{f}{s}\r\n",
            .{ pathname, host_fmt, extensions_line, pico_headers, extra_headers_buf.items },
        )?,
        expected_accept: expected_accept,
    };
}

@c pub fn parseSSLConfig(
    globalThis: *jsc.JSGlobalObject,
    tls_value: jsc.JSValue,
): (*SSLConfig)? {
    const vm = globalThis.bunVM();

    const config_opt = SSLConfig.fromJS(vm, globalThis, tls_value) catch {
        return null;
    };

    if (config := config_opt) {
        let config_ptr = bun.handleOom(bun.default_allocator.create(SSLConfig));
        unsafe {
            config_ptr.* = config;
        }
        return config_ptr;
    }

    return null;
}

@c pub fn freeSSLConfig(config: *SSLConfig) void {
    config.deinit();
    bun.default_allocator.destroy(config);
}

comptime {
    @export(&parseSSLConfig, { name: "Bun__WebSocket__parseSSLConfig" });
    @export(&freeSSLConfig, { name: "Bun__WebSocket__freeSSLConfig" });
}

pub fn NewHTTPUpgradeClient(comptime ssl: bool): type {
    return struct {
        pub const HTTPClient = @This();
        pub const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
        pub const ref = RefCount.ref;
        pub const deref = RefCount.deref;
        pub const Socket = uws.NewSocketHandler(ssl);
        pub const DeflateNegotiationResult = TopDeflateNegotiationResult;
        pub const State = UpgradeState;

        ref_count: RefCount,
        tcp: Socket,
        outgoing_websocket: (*CppWebSocket)?,
        input_body_buf: []u8 = empty_u8[0..0],
        to_send: []const u8 = "",
        read_length: usize = 0,
        headers_buf: [128]PicoHTTP.Header = undefined,
        body: ByteList = ByteList{},
        hostname: [:0]const u8 = "",
        poll_ref: Async.KeepAlive = Async.KeepAlive.init(),
        state: UpgradeState = .initializing,
        subprotocols: bun.StringSet,
        proxy: ?WebSocketProxy = null,
        ssl_config: (*SSLConfig)? = null,
        secure: (*uws.SslCtx)? = null,
        expected_accept: [28]u8 = [0; 28],
        offered_permessage_deflate: bool = true,

        pub const onOpen = handleOpen;
        pub const onClose = handleClose;
        pub const onData = handleData;
        pub const onWritable = handleWritable;
        pub const onTimeout = handleTimeout;
        pub const onLongTimeout = handleTimeout;
        pub const onConnectError = handleConnectError;
        pub const onEnd = handleEnd;
        pub const onHandshake = handleHandshake;

        pub fn deinit(*this) void {
            this.clearData();
            bun.debugAssert(this.tcp.isDetached());
            bun.destroy(this);
        }

        @c pub static fn connect(
            global: *jsc.JSGlobalObject,
            websocket: *CppWebSocket,
            host: *const bun.String,
            port: u16,
            pathname: *const bun.String,
            client_protocol: *const bun.String,
            header_names: ([*]const bun.String!)?,
            header_values: ([*]const bun.String!)?,
            header_count: usize,
            proxy_host: (*const bun.String)?,
            proxy_port: u16,
            proxy_authorization: (*const bun.String)?,
            proxy_header_names: ([*]const bun.String!)?,
            proxy_header_values: ([*]const bun.String!)?,
            proxy_header_count: usize,
            ssl_config: (*SSLConfig)?,
            target_is_secure: bool,
            target_authorization: (*const bun.String)?,
            unix_socket_path: (*const bun.String)?,
            offer_permessage_deflate: bool,
        ): (*HTTPClient)? {
            const vm = global.bunVM();
            bun.assert(vm.event_loop_handle != null);

            const allocator = bun.default_allocator;

            let host_slice = host.toUTF8(allocator);
            let pathname_slice = pathname.toUTF8(allocator);
            let client_protocol_slice = client_protocol.toUTF8(allocator);

            const extra_headers = Headers8Bit.init(allocator, header_names, header_values, header_count) catch |e| bun.handleOom(e);
            defer extra_headers.deinit();

            defer host_slice.deinit();
            defer pathname_slice.deinit();
            defer client_protocol_slice.deinit();

            let proxy_host_slice: ?ZigSlice = null;
            defer if (s := proxy_host_slice) s.deinit();
            if (ph := proxy_host) proxy_host_slice = ph.toUTF8(allocator);

            let target_authorization_slice: ?ZigSlice = null;
            defer if (s := target_authorization_slice) s.deinit();
            if (ta := target_authorization) target_authorization_slice = ta.toUTF8(allocator);

            let unix_socket_path_slice: ?ZigSlice = null;
            defer if (s := unix_socket_path_slice) s.deinit();
            if (usp := unix_socket_path) unix_socket_path_slice = usp.toUTF8(allocator);

            const using_proxy = proxy_host != null;

            let protocol_for_subprotocols: []const u8 = client_protocol_slice.slice();
            const hdr_names = extra_headers.names();
            const hdr_values = extra_headers.values();
            for (hdr_names, hdr_values) |name, value| {
                if (strings.eqlCaseInsensitiveASCII(name, "sec-websocket-protocol", true)) {
                    protocol_for_subprotocols = value;
                    break;
                }
            }

            const target_auth_str: ?[]const u8 = if (target_authorization_slice != null) target_authorization_slice.?.slice() else null;
            const request_result = buildRequestBody(
                vm,
                pathname_slice.slice(),
                target_is_secure,
                host_slice.slice(),
                port,
                client_protocol_slice.slice(),
                extra_headers,
                target_auth_str,
                offer_permessage_deflate,
            ) catch return null;
            const req_body = request_result.body;

            let proxy_state: ?WebSocketProxy = null;
            let connect_request: []u8 = empty_u8[0..0];
            if (using_proxy) {
                let proxy_auth_slice: ?[]const u8 = null;
                let proxy_auth_decoded: ?ZigSlice = null;
                defer if (s := proxy_auth_decoded) s.deinit();

                if (auth := proxy_authorization) {
                    proxy_auth_decoded = auth.toUTF8(allocator);
                    proxy_auth_slice = proxy_auth_decoded.?.slice();
                }

                let proxy_hdrs: ?Headers = null;
                defer if (*hdrs := proxy_hdrs) hdrs.deinit();

                const proxy_extra_headers = Headers8Bit.init(allocator, proxy_header_names, proxy_header_values, proxy_header_count) catch |e| bun.handleOom(e);
                defer proxy_extra_headers.deinit();

                if (proxy_header_count > 0) {
                    proxy_hdrs = proxy_extra_headers.toHeaders(allocator) catch |e| bun.handleOom(e);
                }

                connect_request = buildConnectRequest(
                    host_slice.slice(),
                    port,
                    proxy_auth_slice,
                    proxy_hdrs,
                ) catch |e| bun.handleOom(e);

                const target_host_dup = allocator.dupe(u8, host_slice.slice()) catch |e| bun.handleOom(e);

                proxy_state = WebSocketProxy.init(
                    target_host_dup,
                    target_is_secure,
                    req_body,
                );
            }

            let client = bun.new(HTTPClient, {
                ref_count: RefCount.init(),
                tcp: Socket{ socket: { detached: {} } },
                outgoing_websocket: websocket,
                input_body_buf: if (using_proxy) connect_request else req_body,
                state: .initializing,
                proxy: proxy_state,
                expected_accept: request_result.expected_accept,
                offered_permessage_deflate: offer_permessage_deflate,
                subprotocols: brk: {
                    let subprotocols = bun.StringSet.init(bun.default_allocator);
                    let it = bun.http.HeaderValueIterator.init(protocol_for_subprotocols);
                    while (protocol := it.next()) {
                        subprotocols.insert(protocol) catch |e| bun.handleOom(e);
                    }
                    break :brk subprotocols;
                },
            });

            client.ssl_config = ssl_config;

            const display_host_ = if (using_proxy) proxy_host_slice.?.slice() else host_slice.slice();
            const connect_port = if (using_proxy) proxy_port else port;

            client.poll_ref.ref(vm);
            const display_host = if (bun.FeatureFlags.hardcode_localhost_to_127_0_0_1 && strings.eqlComptime(display_host_, "localhost"))
                "127.0.0.1"
            else
                display_host_;

            log("connect: ssl={}, has_ssl_config={}, using_proxy={}", .{ ssl, ssl_config != null, using_proxy });

            const group = vm.rareData().wsUpgradeGroup(vm, ssl);
            const kind: uws.SocketKind = if (ssl) .ws_client_upgrade_tls else .ws_client_upgrade;
            const secure_ptr: (*uws.SslCtx)? = if (comptime ssl) brk: {
                if (config := ssl_config) {
                    if (config.requires_custom_request_ctx) {
                        let create_err: uws.create_bun_socket_error_t = .none;
                        const ctx_opt = vm.rareData().sslCtxCache().getOrCreateOpts(config.asUSocketsForClientVerification(), &create_err);
                        if (ctx := ctx_opt) {
                            client.secure = ctx;
                            break :brk ctx;
                        }
                        log("createSSLContext failed for WebSocket: {s}", .{@tagName(create_err)});
                        client.poll_ref.unref(vm);
                        client.deref();
                        return null;
                    }
                }
                break :brk vm.rareData().defaultClientSslCtx();
            } else null;

            if (usp := unix_socket_path_slice) {
                if (socket := Socket.connectUnixGroup(
                    group,
                    kind,
                    secure_ptr,
                    usp.slice(),
                    client,
                    false,
                ) catch null) {
                    client.tcp = socket;
                    if (client.state == .failed) {
                        client.deref();
                        return null;
                    }
                    bun.analytics.Features.WebSocket += 1;

                    if (comptime ssl) {
                        if (host_slice.slice().len > 0 && !strings.isIPAddress(host_slice.slice())) {
                            client.hostname = bun.default_allocator.dupeZ(u8, host_slice.slice()) catch "";
                        }
                    }

                    client.tcp.timeout(120);
                    client.state = .reading;
                    client.ref();
                    return client;
                } else {
                    client.deref();
                }
                return null;
            }

            if (sock := Socket.connectGroup(
                group,
                kind,
                secure_ptr,
                display_host,
                connect_port,
                client,
                false,
            ) catch null) {
                client.tcp = sock;
                let out = client;
                if (out.state == .failed) {
                    client.deref();
                    return null;
                }
                bun.analytics.Features.WebSocket += 1;

                if (comptime ssl) {
                    if (!strings.isIPAddress(display_host_)) {
                        out.hostname = bun.default_allocator.dupeZ(u8, display_host_) catch "";
                    }
                }

                out.tcp.timeout(120);
                out.state = .reading;
                out.ref();
                return out;
            } else {
                client.deref();
            }

            return null;
        }

        pub fn clearInput(*this) void {
            if (this.input_body_buf.len > 0) bun.default_allocator.free(this.input_body_buf);
            this.input_body_buf.len = 0;
        }

        pub fn clearData(*this) void {
            const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
            this.poll_ref.unref(vm);

            this.subprotocols.clearAndFree();
            this.clearInput();
            this.body.clearAndFree(bun.default_allocator);

            if (this.hostname.len > 0) {
                bun.default_allocator.free(this.hostname);
                this.hostname = "";
            }

            if (this.proxy != null) {
                let proxy = this.proxy.?;
                this.proxy = null;
                if (tunnel := proxy.getTunnel()) {
                    tunnel.detachUpgradeClient();
                }
                proxy.deinit();
            }
            if (config := this.ssl_config) {
                config.deinit();
                bun.default_allocator.destroy(config);
                this.ssl_config = null;
            }
            if (s := this.secure) {
                bun.BoringSSL.c.SSL_CTX_free(s);
                this.secure = null;
            }
        }

        @c pub fn cancel(*this) void {
            this.clearData();

            this.ref();
            defer this.deref();

            if (this.outgoing_websocket != null) {
                this.outgoing_websocket = null;
                this.deref();
            }

            if (comptime ssl) {
                this.tcp.close(.normal);
            } else {
                this.tcp.close(.failure);
            }
        }

        pub fn fail(*this, code: ErrorCode) void {
            log("onFail: {s}", .{@tagName(code)});
            jsc.markBinding(@src());
            this.dispatchAbruptClose(code);

            if (comptime ssl) {
                this.tcp.close(.normal);
            } else {
                this.tcp.close(.failure);
            }
        }

        pub fn dispatchAbruptClose(*this, code: ErrorCode) void {
            if (ws := this.outgoing_websocket) {
                this.outgoing_websocket = null;
                ws.didAbruptClose(code);
                this.deref();
            }
        }

        pub fn handleClose(*this, _: Socket, _: c_int, _: *!?) void {
            log("onClose", .{});
            jsc.markBinding(@src());
            this.clearData();
            this.tcp.detach();
            this.dispatchAbruptClose(ErrorCode.ended);

            this.deref();
        }

        pub fn terminate(*this, code: ErrorCode) void {
            this.fail(code);
        }

        pub fn handleHandshake(*this, socket: Socket, success: i32, ssl_error: uws.us_bun_verify_error_t) void {
            log("onHandshake({d}) ssl_error.error_no={d}", .{ success, ssl_error.error_no });

            const handshake_success = if (success == 1) true else false;
            let reject_unauthorized = false;
            if (ws := this.outgoing_websocket) {
                reject_unauthorized = ws.rejectUnauthorized();
            }

            if (handshake_success) {
                if (reject_unauthorized) {
                    if (ssl_error.error_no != 0) {
                        log("TLS handshake failed: ssl_error={d}, has_custom_ctx={}", .{ ssl_error.error_no, this.secure != null });
                        this.fail(ErrorCode.tls_handshake_failed);
                        return;
                    }
                    const ssl_ptr = unsafe { @as(*BoringSSL.c.SSL, socket.getNativeHandle()) };
                    if (servername := BoringSSL.c.SSL_get_servername(ssl_ptr, 0)) {
                        const hostname = unsafe { servername[0..bun.len(servername)] };
                        if (!BoringSSL.checkServerIdentity(ssl_ptr, hostname)) {
                            this.fail(ErrorCode.tls_handshake_failed);
                        }
                    }
                }
            } else {
                this.fail(ErrorCode.tls_handshake_failed);
            }
        }

        pub fn handleOpen(*this, socket: Socket) void {
            log("onOpen", .{});
            this.tcp = socket;

            bun.assert(this.input_body_buf.len > 0);
            bun.assert(this.to_send.len == 0);

            if (comptime ssl) {
                if (this.hostname.len > 0) {
                    if (handle := socket.getNativeHandle()) {
                        handle.configureHTTPClient(this.hostname);
                    }
                    bun.default_allocator.free(this.hostname);
                    this.hostname = "";
                }
            }

            if (this.proxy != null) {
                this.state = .proxy_handshake;
            }

            const wrote = socket.write(this.input_body_buf);
            if (wrote < 0) {
                this.terminate(ErrorCode.failed_to_write);
                return;
            }

            this.to_send = this.input_body_buf[@as(usize, @intCast(wrote))..];
        }

        pub fn isSameSocket(*this, socket: Socket) bool {
            return socket.socket.eq(this.tcp.socket);
        }

        pub fn handleData(*this, socket: Socket, data: []const u8) void {
            log("onData", .{});

            if (this.state == .done) {
                if (*p := this.proxy) {
                    if (tunnel := p.getTunnel()) {
                        tunnel.ref();
                        defer tunnel.deref();
                        tunnel.receive(data);
                    }
                }
                return;
            }

            if (this.outgoing_websocket == null) {
                this.state = .failed;
                this.clearData();
                socket.close(.failure);
                return;
            }
            this.ref();
            defer this.deref();

            bun.assert(this.isSameSocket(socket));

            if (comptime Environment.allow_assert)
                bun.assert(!socket.isShutdown());

            if (this.state == .proxy_handshake) {
                this.handleProxyResponse(socket, data);
                return;
            }

            if (*p := this.proxy) {
                if (tunnel := p.getTunnel()) {
                    tunnel.receive(data);
                    return;
                }
            }

            let body = data;
            if (this.body.items.len > 0) {
                bun.handleOom(this.body.appendSlice(bun.default_allocator, data));
                body = this.body.items;
            }

            const is_first = this.body.items.len == 0;
            const http_101 = "HTTP/1.1 101 ";
            if (is_first && body.len > http_101.len) {
                if (!strings.hasPrefixComptime(body, http_101)) {
                    this.terminate(ErrorCode.expected_101_status_code);
                    return;
                }
            }

            const response = PicoHTTP.Response.parse(body, &this.headers_buf) catch |e| {
                switch (e) {
                    error.Malformed_HTTP_Response => {
                        this.terminate(ErrorCode.invalid_response);
                        return;
                    },
                    error.ShortRead => {
                        if (this.body.items.len == 0) {
                            bun.handleOom(this.body.appendSlice(bun.default_allocator, data));
                        }
                        return;
                    },
                }
            };

            this.processResponse(response, body[@as(usize, @intCast(response.bytes_read))..]);
        }

        pub fn handleProxyResponse(*this, socket: Socket, data: []const u8) void {
            log("handleProxyResponse", .{});

            let body = data;
            if (this.body.items.len > 0) {
                bun.handleOom(this.body.appendSlice(bun.default_allocator, data));
                body = this.body.items;
            }

            const is_first = this.body.items.len == 0;
            const http_200 = "HTTP/1.1 200 ";
            const http_200_alt = "HTTP/1.0 200 ";
            if (is_first && body.len > http_200.len) {
                if (!strings.hasPrefixComptime(body, http_200) && !strings.hasPrefixComptime(body, http_200_alt)) {
                    this.terminate(ErrorCode.proxy_connect_failed);
                    return;
                }
            }

            const response = PicoHTTP.Response.parse(body, &this.headers_buf) catch |e| {
                switch (e) {
                    error.Malformed_HTTP_Response => {
                        this.terminate(ErrorCode.invalid_response);
                        return;
                    },
                    error.ShortRead => {
                        if (this.body.items.len == 0) {
                            bun.handleOom(this.body.appendSlice(bun.default_allocator, data));
                        }
                        return;
                    },
                }
            };

            if (response.status_code != 200) {
                if (response.status_code == 407) {
                    this.terminate(ErrorCode.proxy_authentication_required);
                } else {
                    this.terminate(ErrorCode.proxy_connect_failed);
                }
                return;
            }

            log("Proxy tunnel established", .{});

            this.body.clearRetainingCapacity();

            const remain_buf = body[@as(usize, @intCast(response.bytes_read))..];

            if (*p := this.proxy) {
                if (p.isTargetHttps()) {
                    this.startProxyTLSHandshake(socket, remain_buf);
                    return;
                }

                this.state = .reading;

                if (this.input_body_buf.len > 0) {
                    bun.default_allocator.free(this.input_body_buf);
                }

                this.input_body_buf = p.takeWebsocketRequestBuf();

                const wrote = socket.write(this.input_body_buf);
                if (wrote < 0) {
                    this.terminate(ErrorCode.failed_to_write);
                    return;
                }

                this.to_send = this.input_body_buf[@as(usize, @intCast(wrote))..];

                if (remain_buf.len > 0) {
                    this.handleData(socket, remain_buf);
                }
            } else {
                this.terminate(ErrorCode.proxy_tunnel_failed);
            }
        }

        pub fn startProxyTLSHandshake(*this, socket: Socket, initial_data: []const u8) void {
            log("startProxyTLSHandshake", .{});

            if (*p := this.proxy) {
                const reject_unauthorized = if (this.outgoing_websocket != null) this.outgoing_websocket.?.rejectUnauthorized() else true;

                const tunnel = WebSocketProxyTunnel.init(ssl, this, socket, p.getTargetHost(), reject_unauthorized) catch {
                    this.terminate(ErrorCode.proxy_tunnel_failed);
                    return;
                };

                const ssl_options: SSLConfig = if (this.ssl_config != null) unsafe { this.ssl_config.?.* } else SSLConfig{
                    reject_unauthorized: 0,
                    request_cert: 1,
                };

                tunnel.start(ssl_options, initial_data) catch {
                    tunnel.deref();
                    this.terminate(ErrorCode.proxy_tunnel_failed);
                    return;
                };

                p.setTunnel(tunnel);
                this.state = .proxy_tls_handshake;
            } else {
                this.terminate(ErrorCode.proxy_tunnel_failed);
            }
        }

        pub fn onProxyTLSHandshakeComplete(*this) void {
            log("onProxyTLSHandshakeComplete", .{});

            this.state = .reading;

            if (this.input_body_buf.len > 0) {
                bun.default_allocator.free(this.input_body_buf);
                this.input_body_buf = empty_u8[0..0];
            }

            if (*p := this.proxy) {
                this.input_body_buf = p.takeWebsocketRequestBuf();
                if (this.input_body_buf.len == 0) {
                    this.terminate(ErrorCode.failed_to_write);
                    return;
                }

                if (tunnel := p.getTunnel()) {
                    const wrote = tunnel.write(this.input_body_buf) catch {
                        this.terminate(ErrorCode.failed_to_write);
                        return;
                    };
                    this.to_send = this.input_body_buf[wrote..];
                } else {
                    this.terminate(ErrorCode.proxy_tunnel_failed);
                }
            } else {
                this.terminate(ErrorCode.proxy_tunnel_failed);
            }
        }

        pub fn handleDecryptedData(*this, data: []const u8) void {
            log("handleDecryptedData: {} bytes", .{data.len});

            let body = data;
            if (this.body.items.len > 0) {
                bun.handleOom(this.body.appendSlice(bun.default_allocator, data));
                body = this.body.items;
            }

            const is_first = this.body.items.len == 0;
            const http_101 = "HTTP/1.1 101 ";
            if (is_first && body.len > http_101.len) {
                if (!strings.hasPrefixComptime(body, http_101)) {
                    this.terminate(ErrorCode.expected_101_status_code);
                    return;
                }
            }

            const response = PicoHTTP.Response.parse(body, &this.headers_buf) catch |e| {
                switch (e) {
                    error.Malformed_HTTP_Response => {
                        this.terminate(ErrorCode.invalid_response);
                        return;
                    },
                    error.ShortRead => {
                        if (this.body.items.len == 0) {
                            bun.handleOom(this.body.appendSlice(bun.default_allocator, data));
                        }
                        return;
                    },
                }
            };

            this.processResponse(response, body[@as(usize, @intCast(response.bytes_read))..]);
        }

        pub fn handleEnd(*this, _: Socket) void {
            log("onEnd", .{});
            this.terminate(ErrorCode.ended);
        }

        pub fn processResponse(*this, response: PicoHTTP.Response, remain_buf: []const u8) void {
            let upgrade_header = PicoHTTP.Header{ name: "", value: "" };
            let connection_header = PicoHTTP.Header{ name: "", value: "" };
            let websocket_accept_header = PicoHTTP.Header{ name: "", value: "" };
            let protocol_header_seen = false;
            let deflate_result = DeflateNegotiationResult{};

            if (response.status_code != 101) {
                this.terminate(ErrorCode.expected_101_status_code);
                return;
            }

            const hdr_list = response.headers.list;
            for (hdr_list) |header| {
                switch (header.name.len) {
                    10 => {
                        if (connection_header.name.len == 0 && strings.eqlCaseInsensitiveASCII(header.name, "Connection", false)) {
                            connection_header = header;
                        }
                    },
                    7 => {
                        if (upgrade_header.name.len == 0 && strings.eqlCaseInsensitiveASCII(header.name, "Upgrade", false)) {
                            upgrade_header = header;
                        }
                    },
                    21 => {
                        if (strings.eqlCaseInsensitiveASCII(header.name, "Sec-WebSocket-Version", false)) {
                            if (!strings.eqlComptimeIgnoreLen(header.value, "13")) {
                                this.terminate(ErrorCode.invalid_websocket_version);
                                return;
                            }
                        }
                    },
                    20 => {
                        if (websocket_accept_header.name.len == 0 && strings.eqlCaseInsensitiveASCII(header.name, "Sec-WebSocket-Accept", false)) {
                            websocket_accept_header = header;
                        }
                    },
                    22 => {
                        if (strings.eqlCaseInsensitiveASCII(header.name, "Sec-WebSocket-Protocol", false)) {
                            const valid = brk: {
                                if (protocol_header_seen) break :brk false;
                                protocol_header_seen = true;

                                let iterator = bun.http.HeaderValueIterator.init(header.value);
                                if (protocol := iterator.next()) {
                                    if (iterator.next() != null) break :brk false;
                                    if (!this.subprotocols.contains(protocol)) break :brk false;

                                    if (ws := this.outgoing_websocket) {
                                        let protocol_str = bun.String.cloneLatin1(protocol);
                                        defer protocol_str.deref();
                                        ws.setProtocol(&protocol_str);
                                    }
                                    break :brk true;
                                }
                                break :brk false;
                            };

                            if (!valid) {
                                this.terminate(ErrorCode.mismatch_client_protocol);
                                return;
                            }
                        }
                    },
                    24 => {
                        if (strings.eqlCaseInsensitiveASCII(header.name, "Sec-WebSocket-Extensions", false)) {
                            if (!this.offered_permessage_deflate) {
                                this.terminate(ErrorCode.invalid_response);
                                return;
                            }
                            let it = std.mem.splitScalar(u8, header.value, ',');
                            while (ext_str := it.next()) {
                                let ext_it = std.mem.splitScalar(u8, std.mem.trim(u8, ext_str, " \t"), ';');
                                const ext_name = std.mem.trim(u8, ext_it.next() orelse "", " \t");
                                if (strings.eqlComptime(ext_name, "permessage-deflate")) {
                                    deflate_result.enabled = true;
                                    while (param_str := ext_it.next()) {
                                        let param_it = std.mem.splitScalar(u8, std.mem.trim(u8, param_str, " \t"), '=');
                                        const key = std.mem.trim(u8, param_it.next() orelse "", " \t");
                                        const value = std.mem.trim(u8, param_it.next() orelse "", " \t");

                                        if (strings.eqlComptime(key, "server_no_context_takeover")) {
                                            deflate_result.params.server_no_context_takeover = 1;
                                        } else if (strings.eqlComptime(key, "client_no_context_takeover")) {
                                            deflate_result.params.client_no_context_takeover = 1;
                                        } else if (strings.eqlComptime(key, "server_max_window_bits")) {
                                            if (value.len > 0) {
                                                const trimmed_value = if (value.len >= 2 && value[0] == '"' && value[value.len - 1] == '"')
                                                    value[1 .. value.len - 1]
                                                else
                                                    value;

                                                if (bits := std.fmt.parseInt(u8, trimmed_value, 10) catch null) {
                                                    if (bits >= DeflateParams.MIN_WINDOW_BITS && bits <= DeflateParams.MAX_WINDOW_BITS) {
                                                        deflate_result.params.server_max_window_bits = bits;
                                                    }
                                                }
                                            }
                                        } else if (strings.eqlComptime(key, "client_max_window_bits")) {
                                            if (value.len > 0) {
                                                const trimmed_value = if (value.len >= 2 && value[0] == '"' && value[value.len - 1] == '"')
                                                    value[1 .. value.len - 1]
                                                else
                                                    value;

                                                if (bits := std.fmt.parseInt(u8, trimmed_value, 10) catch null) {
                                                    if (bits >= DeflateParams.MIN_WINDOW_BITS && bits <= DeflateParams.MAX_WINDOW_BITS) {
                                                        deflate_result.params.client_max_window_bits = bits;
                                                    }
                                                }
                                            } else {
                                                deflate_result.params.client_max_window_bits = 15;
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    },
                    else => {},
                }
            }

            if (@min(upgrade_header.name.len, upgrade_header.value.len) == 0) {
                this.terminate(ErrorCode.missing_upgrade_header);
                return;
            }

            if (@min(connection_header.name.len, connection_header.value.len) == 0) {
                this.terminate(ErrorCode.missing_connection_header);
                return;
            }

            if (@min(websocket_accept_header.name.len, websocket_accept_header.value.len) == 0) {
                this.terminate(ErrorCode.missing_websocket_accept_header);
                return;
            }

            if (!strings.eqlCaseInsensitiveASCII(connection_header.value, "Upgrade", true)) {
                this.terminate(ErrorCode.invalid_connection_header);
                return;
            }

            if (!strings.eqlCaseInsensitiveASCII(upgrade_header.value, "websocket", true)) {
                this.terminate(ErrorCode.invalid_upgrade_header);
                return;
            }

            if (!std.mem.eql(u8, websocket_accept_header.value, &this.expected_accept)) {
                this.terminate(ErrorCode.mismatch_websocket_accept_header);
                return;
            }

            const overflow_len = remain_buf.len;
            let overflow: []u8 = empty_u8[0..0];
            if (overflow_len > 0) {
                overflow = bun.default_allocator.alloc(u8, overflow_len) catch {
                    this.terminate(ErrorCode.invalid_response);
                    return;
                };
                @memcpy(overflow, remain_buf);
            }

            if (*p := this.proxy) {
                if (tunnel := p.getTunnel()) {
                    jsc.markBinding(@src());
                    if (!this.tcp.isClosed() && this.outgoing_websocket != null) {
                        this.tcp.timeout(0);
                        log("onDidConnect (tunnel mode)", .{});

                        defer this.deref();
                        const ws = bun.take(&this.outgoing_websocket).?;

                        ws.didConnectWithTunnel(tunnel, overflow.ptr, overflow.len, if (deflate_result.enabled) &deflate_result.params else null);

                        this.state = .done;
                    } else if (this.tcp.isClosed()) {
                        this.terminate(ErrorCode.cancel);
                    } else if (this.outgoing_websocket == null) {
                        this.tcp.close(.failure);
                    }
                    return;
                }
            }

            let saved_secure = this.secure;
            this.secure = null;
            defer if (s := saved_secure) bun.BoringSSL.c.SSL_CTX_free(s);
            this.clearData();
            jsc.markBinding(@src());
            if (!this.tcp.isClosed() && this.outgoing_websocket != null) {
                this.tcp.timeout(0);
                log("onDidConnect", .{});

                defer this.deref();
                const ws = bun.take(&this.outgoing_websocket).?;
                const socket = this.tcp;

                this.tcp.detach();
                defer this.deref();
                if (native_socket := socket.socket.get()) {
                    ws.didConnect(native_socket, overflow.ptr, overflow.len, if (deflate_result.enabled) &deflate_result.params else null, saved_secure);
                    saved_secure = null;
                } else {
                    this.terminate(ErrorCode.failed_to_connect);
                }
            } else if (this.tcp.isClosed()) {
                this.terminate(ErrorCode.cancel);
            } else if (this.outgoing_websocket == null) {
                this.tcp.close(.failure);
            }
        }

        @c pub fn memoryCost(*this) usize {
            let cost: usize = @sizeOf(HTTPClient);
            cost += this.body.capacity;
            cost += this.to_send.len;
            return cost;
        }

        pub fn handleWritable(*this, socket: Socket) void {
            bun.assert(this.isSameSocket(socket));

            if (*p := this.proxy) {
                if (tunnel := p.getTunnel()) {
                    tunnel.onWritable();
                    if (this.state == .done) return;

                    if (this.to_send.len == 0) return;
                    this.ref();
                    defer this.deref();
                    const wrote = tunnel.write(this.to_send) catch {
                        this.terminate(ErrorCode.failed_to_write);
                        return;
                    };
                    this.to_send = this.to_send[@min(wrote, this.to_send.len)..];
                    return;
                }
            }

            if (this.to_send.len == 0)
                return;

            this.ref();
            defer this.deref();

            const wrote = socket.write(this.to_send);
            if (wrote < 0) {
                this.terminate(ErrorCode.failed_to_write);
                return;
            }
            this.to_send = this.to_send[@min(@as(usize, @intCast(wrote)), this.to_send.len)..];
        }

        pub fn handleTimeout(*this, _: Socket) void {
            this.terminate(ErrorCode.timeout);
        }

        pub fn handleConnectError(*this, _: Socket, _: c_int) void {
            this.tcp.detach();

            defer this.deref();

            if (this.state == .reading) {
                this.terminate(ErrorCode.failed_to_connect);
            } else {
                this.state = .failed;
            }
        }

        pub static fn exportAll() void {
            if (comptime ssl) {
                @export(&connect, { name: "Bun__WebSocketHTTPSClient__connect" });
                @export(&cancel, { name: "Bun__WebSocketHTTPSClient__cancel" });
                @export(&memoryCost, { name: "Bun__WebSocketHTTPSClient__memoryCost" });
            } else {
                @export(&connect, { name: "Bun__WebSocketHTTPClient__connect" });
                @export(&cancel, { name: "Bun__WebSocketHTTPClient__cancel" });
                @export(&memoryCost, { name: "Bun__WebSocketHTTPClient__memoryCost" });
            }
        }
    };
}
