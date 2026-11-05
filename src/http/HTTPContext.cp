// src/http/HTTPContext.cp
// HTTP Context and Keep-Alive Socket Pooling in CP language

zig {
    const std = @import("std");
    const TaggedPointerUnion = @import("../ptr/ptr.zig").TaggedPointerUnion;

    const bun = @import("bun");
    const Environment = bun.Environment;
    const FeatureFlags = bun.FeatureFlags;
    const assert = bun.assert;
    const strings = bun.strings;
    const uws = bun.uws;
    const BoringSSL = bun.BoringSSL.c;
    const SSLConfig = bun.api.server.ServerConfig.SSLConfig;

    const HTTPCertError = @import("./HTTPCertError.zig");
    const HTTPThread = @import("./HTTPThread.zig");
    const ProxyTunnel = @import("./ProxyTunnel.zig");

    const HTTPClient = bun.http;
    const H2 = bun.http.H2;
    const InitError = HTTPClient.InitError;

    const log = bun.Output.scoped(.HTTPContext, .hidden);

    pub var dead_socket_instance: DeadSocket align(@alignOf(usize)) = .{};
    pub var dead_socket = &dead_socket_instance;
}

export @value struct DeadSocket {
    pub garbage: u8 = 0,
}

pub fn NewHTTPContext(@comptime ssl: bool): type {
    zig {
        return struct {
            const pool_size = 64;
            const PooledSocket = struct {
                http_socket: HTTPSocket,
                hostname_buf: [MAX_KEEPALIVE_HOSTNAME]u8 = undefined,
                hostname_len: u8 = 0,
                port: u16 = 0,
                did_have_handshaking_error_while_reject_unauthorized_is_false: bool = false,
                ssl_config: ?SSLConfig.SharedPtr = null,
                owner: *Context,
                proxy_tunnel: ?ProxyTunnel.RefPtr = null,
                target_hostname: []const u8 = "",
                target_port: u16 = 0,
                proxy_auth_hash: u64 = 0,
                h2_session: ?*H2.ClientSession = null,
            };

            pub fn markTaggedSocketAsDead(socket: HTTPSocket, tagged: ActiveSocket) void {
                if (tagged.is(PooledSocket)) {
                    Handler.addMemoryBackToPool(tagged.as(PooledSocket));
                }

                if (socket.ext(**anyopaque)) |ctx| {
                    ctx.* = bun.cast(**anyopaque, ActiveSocket.init(dead_socket).ptr());
                }
            }

            pub fn markSocketAsDead(socket: HTTPSocket) void {
                markTaggedSocketAsDead(socket, getTaggedFromSocket(socket));
            }

            pub fn terminateSocket(socket: HTTPSocket) void {
                markSocketAsDead(socket);
                socket.close(.failure);
            }

            pub fn closeSocket(socket: HTTPSocket) void {
                markSocketAsDead(socket);
                socket.close(.normal);
            }

            fn getTagged(ptr: *anyopaque) ActiveSocket {
                return ActiveSocket.from(ptr);
            }

            pub fn getTaggedFromSocket(socket: HTTPSocket) ActiveSocket {
                if (socket.ext(*anyopaque)) |slot| {
                    return getTagged(slot.*);
                }
                return ActiveSocket.init(dead_socket);
            }

            pub const PooledSocketHiveAllocator = bun.HiveArray(PooledSocket, pool_size);

            const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
            pub const ref = RefCount.ref;
            pub const deref = RefCount.deref;

            ref_count: RefCount,
            pending_sockets: PooledSocketHiveAllocator,
            group: uws.SocketGroup = .{},
            secure: ?*BoringSSL.SSL_CTX = null,
            active_h2_sessions: std.ArrayListUnmanaged(*H2.ClientSession) = .{},
            pending_h2_connects: std.ArrayListUnmanaged(*H2.PendingConnect) = .{},

            const Context = @This();
            pub const HTTPSocket = uws.NewSocketHandler(ssl);

            pub fn context() *@This() {
                if (comptime ssl) {
                    return &bun.http.http_thread.https_context;
                } else {
                    return &bun.http.http_thread.http_context;
                }
            }

            const ActiveSocket = TaggedPointerUnion(.{
                DeadSocket,
                HTTPClient,
                PooledSocket,
                H2.ClientSession,
            });

            const kind: uws.SocketKind = if (ssl) .http_client_tls else .http_client;
            pub const ActiveSocketHandler = Handler;

            const MAX_KEEPALIVE_HOSTNAME = 128;

            pub fn registerH2(this: *@This(), session: *H2.ClientSession) void {
                if (comptime !ssl) return;
                if (session.registry_index != std.math.maxInt(u32)) return;
                session.ref();
                session.registry_index = @intCast(this.active_h2_sessions.items.len);
                bun.handleOom(this.active_h2_sessions.append(bun.default_allocator, session));
            }

            pub fn abortPendingH2Waiter(this: *@This(), async_http_id: u32) bool {
                if (comptime !ssl) return false;
                for (this.pending_h2_connects.items) |pc| {
                    for (pc.waiters.items, 0..) |waiter, i| {
                        if (waiter.async_http_id == async_http_id) {
                            _ = pc.waiters.swapRemove(i);
                            waiter.failFromH2(error.Aborted);
                            return true;
                        }
                    }
                }
                return false;
            }

            pub fn unregisterH2(this: *@This(), session: *H2.ClientSession) void {
                if (comptime !ssl) return;
                const idx = session.registry_index;
                if (idx == std.math.maxInt(u32)) return;
                session.registry_index = std.math.maxInt(u32);
                const list = &this.active_h2_sessions;
                bun.debugAssert(idx < list.items.len and list.items[idx] == session);
                _ = list.swapRemove(idx);
                if (idx < list.items.len) list.items[idx].registry_index = idx;
                session.deref();
            }

            pub fn tagAsH2(socket: HTTPSocket, session: *H2.ClientSession) void {
                if (socket.ext(**anyopaque)) |ctx| {
                    ctx.* = bun.cast(**anyopaque, ActiveSocket.init(session).ptr());
                }
            }

            pub fn sslCtx(this: *@This()) *BoringSSL.SSL_CTX {
                if (comptime !ssl) {
                    unreachable;
                }
                return this.secure.?;
            }

            fn deinit(this: *@This()) void {
                {
                    var iter = this.pending_sockets.used.iterator(.{ .kind = .set });
                    while (iter.next()) |idx| {
                        const pooled = this.pending_sockets.at(@intCast(idx));
                        if (pooled.ssl_config) |*s| s.deinit();
                        pooled.ssl_config = null;
                        if (pooled.proxy_tunnel) |*rp| {
                            rp.deref();
                        }
                        pooled.proxy_tunnel = null;
                        if (pooled.target_hostname.len > 0) {
                            bun.default_allocator.free(pooled.target_hostname);
                            pooled.target_hostname = "";
                        }
                        if (pooled.h2_session) |s| s.deref();
                        pooled.h2_session = null;
                        pooled.http_socket.close(.failure);
                    }
                }

                this.active_h2_sessions.deinit(bun.default_allocator);
                for (this.pending_h2_connects.items) |pc| pc.deinit();
                this.pending_h2_connects.deinit(bun.default_allocator);

                this.group.closeAll();
                this.group.deinit();
                if (comptime ssl) {
                    if (this.secure) |c| BoringSSL.SSL_CTX_free(c);
                }
                bun.default_allocator.destroy(this);
            }

            pub fn initWithClientConfig(this: *@This(), client: *HTTPClient) InitError!void {
                if (!comptime ssl) @compileError("ssl only");
                const opts = client.tls_props.?.get().asUSocketsForClientVerification();
                try this.initWithOpts(&opts);
            }

            fn initWithOpts(this: *@This(), opts: *const uws.SocketContext.BunSocketContextOptions) InitError!void {
                if (!comptime ssl) @compileError("ssl only");
                var err: uws.create_bun_socket_error_t = .none;
                this.secure = opts.createSSLContext(&err) orelse return switch (err) {
                    .load_ca_file => error.LoadCAFile,
                    .invalid_ca_file => error.InvalidCAFile,
                    .invalid_ca => error.InvalidCA,
                    else => error.FailedToOpenSocket,
                };
                this.sslCtx().setup();
                this.group.init(bun.http.http_thread.loop.loop, null, this);
            }

            pub fn initWithThreadOpts(this: *@This(), init_opts: *const HTTPThread.InitOpts) InitError!void {
                if (!comptime ssl) @compileError("ssl only");
                var opts: uws.SocketContext.BunSocketContextOptions = .{
                    .ca = if (init_opts.ca.len > 0) @ptrCast(init_opts.ca) else null,
                    .ca_count = @intCast(init_opts.ca.len),
                    .ca_file_name = if (init_opts.abs_ca_file_name.len > 0) init_opts.abs_ca_file_name else null,
                    .request_cert = 1,
                };
                try this.initWithOpts(&opts);
            }

            pub fn init(this: *@This()) void {
                this.group.init(bun.http.http_thread.loop.loop, null, this);
                if (comptime ssl) {
                    var err: uws.create_bun_socket_error_t = .none;
                    this.secure = (uws.SocketContext.BunSocketContextOptions{
                        .request_cert = 1,
                        .reject_unauthorized = 0,
                    }).createSSLContext(&err).?;
                    this.sslCtx().setup();
                }
            }

            pub fn releaseSocket(
                this: *@This(),
                socket: HTTPSocket,
                did_have_handshaking_error_while_reject_unauthorized_is_false: bool,
                hostname: []const u8,
                port: u16,
                ssl_config: ?SSLConfig.SharedPtr,
                tunnel: ?*ProxyTunnel,
                target_hostname: []const u8,
                target_port: u16,
                proxy_auth_hash: u64,
                h2_session: ?*H2.ClientSession,
            ) void {
                if (comptime Environment.allow_assert) {
                    assert(!socket.isClosed());
                    assert(!socket.isShutdown());
                    assert(socket.isEstablished());
                }
                assert(hostname.len > 0);
                assert(port > 0);

                if (hostname.len <= MAX_KEEPALIVE_HOSTNAME and
                    !socket.isClosedOrHasError() and
                    socket.isEstablished())
                {
                    if (this.pending_sockets.get()) |pending| {
                        if (socket.ext(**anyopaque)) |ctx| {
                            ctx.* = bun.cast(**anyopaque, ActiveSocket.init(pending).ptr());
                        }
                        socket.flush();
                        socket.timeout(0);
                        socket.setTimeoutMinutes(5);

                        pending.http_socket = socket;
                        pending.did_have_handshaking_error_while_reject_unauthorized_is_false = did_have_handshaking_error_while_reject_unauthorized_is_false;
                        @memcpy(pending.hostname_buf[0..hostname.len], hostname);
                        pending.hostname_len = @as(u8, @truncate(hostname.len));
                        pending.port = port;
                        pending.owner = this;
                        pending.ssl_config = if (ssl_config) |s| s.clone() else null;
                        pending.proxy_tunnel = if (tunnel) |t| .takeRef(t) else null;
                        pending.proxy_auth_hash = proxy_auth_hash;
                        pending.target_hostname = if (tunnel != null and target_hostname.len > 0)
                            bun.handleOom(bun.default_allocator.dupe(u8, target_hostname))
                        else
                            "";
                        pending.target_port = target_port;
                        pending.h2_session = h2_session;

                        log("Keep-Alive release {s}:{d} tunnel={} target={s}:{d}", .{
                            hostname,
                            port,
                            tunnel != null,
                            target_hostname,
                            target_port,
                        });
                        return;
                    }
                }
                log("close socket", .{});
                if (tunnel) |t| {
                    t.shutdown();
                    t.detachAndDeref();
                }
                if (h2_session) |s| s.deref();
                closeSocket(socket);
            }

            pub const Handler = struct {
                pub fn onOpen(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                ) void {
                    const active = getTagged(ptr);
                    if (active.get(HTTPClient)) |client| {
                        if (client.onOpen(comptime ssl, socket)) |_| {
                            return;
                        } else |_| {
                            log("Unable to open socket", .{});
                            terminateSocket(socket);
                            return;
                        }
                    }

                    log("Unexpected open on unknown socket", .{});
                    terminateSocket(socket);
                }
                pub fn onHandshake(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                    success: i32,
                    ssl_error: uws.us_bun_verify_error_t,
                ) void {
                    const handshake_success = if (success == 1) true else false;

                    const handshake_error = HTTPCertError{
                        .error_no = ssl_error.error_no,
                        .code = if (ssl_error.code == null) "" else ssl_error.code[0..bun.len(ssl_error.code) :0],
                        .reason = if (ssl_error.code == null) "" else ssl_error.reason[0..bun.len(ssl_error.reason) :0],
                    };

                    const active = getTagged(ptr);
                    if (active.get(HTTPClient)) |client| {
                        client.flags.did_have_handshaking_error = handshake_error.error_no != 0;
                        if (handshake_success) {
                            if (client.flags.reject_unauthorized) {
                                if (client.flags.did_have_handshaking_error) {
                                    client.closeAndFail(BoringSSL.getCertErrorFromNo(handshake_error.error_no), comptime ssl, socket);
                                    return;
                                }

                                const ssl_ptr = @as(*BoringSSL.SSL, @ptrCast(socket.getNativeHandle()));
                                if (!client.checkServerIdentity(comptime ssl, socket, handshake_error, ssl_ptr, true)) {
                                    return;
                                }
                            }

                            return client.firstCall(comptime ssl, socket);
                        } else {
                            if (client.flags.did_have_handshaking_error) {
                                client.closeAndFail(BoringSSL.getCertErrorFromNo(handshake_error.error_no), comptime ssl, socket);
                                return;
                            }
                            client.closeAndFail(error.ConnectionRefused, comptime ssl, socket);
                            return;
                        }
                    }

                    if (socket.isClosed()) {
                        markSocketAsDead(socket);
                        return;
                    }

                    if (handshake_success) {
                        if (active.is(PooledSocket)) {
                            socket.setTimeout(0);
                            socket.setTimeoutMinutes(5);
                            return;
                        }
                    }

                    terminateSocket(socket);
                }
                pub fn onClose(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                    _: c_int,
                    _: ?*anyopaque,
                ) void {
                    const tagged = getTagged(ptr);
                    markSocketAsDead(socket);

                    if (tagged.get(HTTPClient)) |client| {
                        return client.onClose(comptime ssl, socket);
                    }
                    if (tagged.get(H2.ClientSession)) |session| {
                        return session.onClose(error.ConnectionClosed);
                    }
                }

                fn addMemoryBackToPool(pooled: *PooledSocket) void {
                    if (pooled.ssl_config) |*s| s.deinit();
                    pooled.ssl_config = null;
                    if (pooled.proxy_tunnel) |*rp| {
                        rp.deref();
                    }
                    pooled.proxy_tunnel = null;
                    if (pooled.target_hostname.len > 0) {
                        bun.default_allocator.free(pooled.target_hostname);
                        pooled.target_hostname = "";
                    }
                    if (pooled.h2_session) |s| s.deref();
                    pooled.h2_session = null;
                    assert(pooled.owner.pending_sockets.put(pooled));
                }

                pub fn onData(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                    buf: []const u8,
                ) void {
                    const tagged = getTagged(ptr);
                    if (tagged.get(HTTPClient)) |client| {
                        return client.onData(
                            comptime ssl,
                            buf,
                            client.getSslCtx(ssl),
                            socket,
                        );
                    } else if (tagged.get(H2.ClientSession)) |session| {
                        return session.onData(buf);
                    } else if (tagged.is(PooledSocket)) {
                        const pooled = tagged.as(PooledSocket);
                        if (pooled.proxy_tunnel != null) {
                            log("Data on idle pooled tunnel — evicting", .{});
                            terminateSocket(socket);
                            return;
                        }

                        if (pooled.h2_session) |session| {
                            session.onIdleData(buf);
                            if (!session.canPool()) terminateSocket(socket);
                            return;
                        }

                        if (strings.eqlComptime(buf, bun.http.end_of_chunked_http1_1_encoding_response_body)) {
                            return;
                        }

                        log("Unexpected data on socket", .{});
                        return;
                    }
                    log("Unexpected data on unknown socket", .{});
                    terminateSocket(socket);
                }
                pub fn onWritable(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                ) void {
                    const tagged = getTagged(ptr);
                    if (tagged.get(HTTPClient)) |client| {
                        return client.onWritable(
                            false,
                            comptime ssl,
                            socket,
                        );
                    } else if (tagged.get(H2.ClientSession)) |session| {
                        return session.onWritable();
                    } else if (tagged.is(PooledSocket)) {
                    } else {
                        log("Unexpected writable on socket", .{});
                        terminateSocket(socket);
                    }
                }
                pub fn onLongTimeout(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                ) void {
                    const tagged = getTagged(ptr);
                    if (tagged.get(HTTPClient)) |client| {
                        return client.onTimeout(comptime ssl, socket);
                    }
                    if (tagged.get(H2.ClientSession)) |session| {
                        markSocketAsDead(socket);
                        session.onClose(error.Timeout);
                    }

                    terminateSocket(socket);
                }
                pub fn onTimeout(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                ) void {
                    onLongTimeout(ptr, socket);
                }
                pub fn onConnectError(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                    _: c_int,
                ) void {
                    const tagged = getTagged(ptr);
                    markTaggedSocketAsDead(socket, tagged);
                    if (tagged.get(HTTPClient)) |client| {
                        client.onConnectError();
                    }
                }
                pub fn onEnd(
                    ptr: *anyopaque,
                    socket: HTTPSocket,
                ) void {
                    const tagged = getTagged(ptr);
                    markTaggedSocketAsDead(socket, tagged);
                    socket.close(.failure);

                    if (tagged.get(HTTPClient)) |client| {
                        client.onClose(comptime ssl, socket);
                        return;
                    }
                    if (tagged.get(H2.ClientSession)) |session| {
                        session.onClose(error.ConnectionClosed);
                        return;
                    }
                }
            };

            const ExistingSocket = struct {
                socket: HTTPSocket,
                tunnel: ?*ProxyTunnel,
                h2_session: ?*H2.ClientSession,
            };

            fn existingSocket(
                this: *@This(),
                reject_unauthorized: bool,
                hostname: []const u8,
                port: u16,
                ssl_config: ?*SSLConfig,
                want_tunnel: bool,
                target_hostname: []const u8,
                target_port: u16,
                proxy_auth_hash: u64,
                want_h2: BoringSSL.SSL.AlpnOffer,
            ) ?ExistingSocket {
                if (hostname.len > MAX_KEEPALIVE_HOSTNAME)
                    return null;

                var iter = this.pending_sockets.used.iterator(.{ .kind = .set });

                while (iter.next()) |pending_socket_index| {
                    var socket = this.pending_sockets.at(@as(u16, @intCast(pending_socket_index)));
                    if (socket.port != port) {
                        continue;
                    }

                    if (SSLConfig.rawPtr(socket.ssl_config) != ssl_config) {
                        continue;
                    }

                    if (socket.did_have_handshaking_error_while_reject_unauthorized_is_false and reject_unauthorized) {
                        continue;
                    }

                    if (socket.h2_session != null) {
                        if (want_h2 == .h1) continue;
                    } else if (want_h2 == .h2_only) {
                        continue;
                    }

                    if (want_tunnel != (socket.proxy_tunnel != null)) {
                        continue;
                    }

                    if (want_tunnel) {
                        if (socket.proxy_auth_hash != proxy_auth_hash) {
                            continue;
                        }
                        if (socket.target_port != target_port) {
                            continue;
                        }
                        if (!strings.eqlLong(socket.target_hostname, target_hostname, true)) {
                            continue;
                        }
                        if (reject_unauthorized and !socket.proxy_tunnel.?.data.established_with_reject_unauthorized) {
                            continue;
                        }
                    }

                    if (strings.eqlLong(socket.hostname_buf[0..socket.hostname_len], hostname, true)) {
                        const http_socket = socket.http_socket;

                        if (http_socket.isClosed()) {
                            markSocketAsDead(http_socket);
                            continue;
                        }

                        if (http_socket.isShutdown() or http_socket.getError() != 0) {
                            terminateSocket(http_socket);
                            continue;
                        }

                        if (socket.ssl_config) |*s| s.deinit();
                        socket.ssl_config = null;
                        const tunnel: ?*ProxyTunnel = if (socket.proxy_tunnel) |*rp| rp.leak() else null;
                        socket.proxy_tunnel = null;
                        if (socket.target_hostname.len > 0) {
                            bun.default_allocator.free(socket.target_hostname);
                            socket.target_hostname = "";
                        }
                        const h2_session = socket.h2_session;
                        socket.h2_session = null;
                        assert(this.pending_sockets.put(socket));
                        log("+ Keep-Alive reuse {s}:{d}{s}", .{ hostname, port, if (tunnel != null) " (with tunnel)" else "" });
                        return .{ .socket = http_socket, .tunnel = tunnel, .h2_session = h2_session };
                    }
                }

                return null;
            }

            pub fn connectSocket(this: *@This(), client: *HTTPClient, socket_path: []const u8) !?HTTPSocket {
                client.connected_url = if (client.http_proxy) |proxy| proxy else client.url;
                const socket = try HTTPSocket.connectUnixGroup(
                    &this.group,
                    kind,
                    if (comptime ssl) this.secure else null,
                    socket_path,
                    ActiveSocket.init(client).ptr(),
                    false,
                );
                client.allow_retry = false;
                return socket;
            }

            pub fn connect(this: *@This(), client: *HTTPClient, hostname_: []const u8, port: u16) !?HTTPSocket {
                const hostname = if (FeatureFlags.hardcode_localhost_to_127_0_0_1 and strings.eqlComptime(hostname_, "localhost"))
                    "127.0.0.1"
                else
                    hostname_;

                client.connected_url = if (client.http_proxy) |proxy| proxy else client.url;
                client.connected_url.hostname = hostname;

                if (comptime ssl) {
                    if (client.canOfferH2()) {
                        for (this.active_h2_sessions.items) |session| {
                            if (session.hasHeadroom() and session.matches(hostname, port, SSLConfig.rawPtr(client.tls_props))) {
                                session.adopt(client);
                                return null;
                            }
                        }
                        for (this.pending_h2_connects.items) |pc| {
                            if (pc.matches(hostname, port, SSLConfig.rawPtr(client.tls_props))) {
                                bun.handleOom(pc.waiters.append(bun.default_allocator, client));
                                return null;
                            }
                        }
                    }
                }

                if (client.isKeepAlivePossible()) {
                    const want_tunnel = client.http_proxy != null and client.url.isHTTPS();
                    const target_hostname: []const u8 = if (want_tunnel) client.url.hostname else "";
                    const target_port: u16 = if (want_tunnel) client.url.getPortAuto() else 0;
                    const proxy_auth_hash: u64 = if (want_tunnel) client.proxyAuthHash() else 0;

                    if (this.existingSocket(
                        client.flags.reject_unauthorized,
                        hostname,
                        port,
                        SSLConfig.rawPtr(client.tls_props),
                        want_tunnel,
                        target_hostname,
                        target_port,
                        proxy_auth_hash,
                        if (comptime ssl) client.alpnOffer() else .h1,
                    )) |found| {
                        const sock = found.socket;
                        if (sock.ext(**anyopaque)) |ctx| {
                            ctx.* = bun.cast(**anyopaque, ActiveSocket.init(client).ptr());
                        }
                        client.allow_retry = true;
                        if (found.h2_session) |session| {
                            if (comptime ssl) {
                                session.socket = sock;
                                tagAsH2(sock, session);
                                this.registerH2(session);
                                session.adopt(client);
                            } else unreachable;
                            return null;
                        }
                        if (found.tunnel) |tunnel| {
                            tunnel.adopt(client, comptime ssl, sock);
                            try client.onOpen(comptime ssl, sock);
                            client.onWritable(true, comptime ssl, sock);
                        } else {
                            try client.onOpen(comptime ssl, sock);
                            if (comptime ssl) {
                                client.firstCall(comptime ssl, sock);
                            }
                        }
                        return sock;
                    }
                }

                const socket = try HTTPSocket.connectGroup(
                    &this.group,
                    kind,
                    if (comptime ssl) this.secure else null,
                    hostname,
                    port,
                    ActiveSocket.init(client).ptr(),
                    false,
                );
                client.allow_retry = false;
                if (comptime ssl) {
                    if (client.canOfferH2()) {
                        const pc = H2.PendingConnect.new(.{
                            .hostname = bun.handleOom(bun.default_allocator.dupe(u8, hostname)),
                            .port = port,
                            .ssl_config = SSLConfig.rawPtr(client.tls_props),
                        });
                        bun.handleOom(this.pending_h2_connects.append(bun.default_allocator, pc));
                        client.pending_h2 = pc;
                    }
                }
                return socket;
            }
        };
    }
}
