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
    const MAX_KEEPALIVE_HOSTNAME = 128;
    const pool_size = 64;

    pub var dead_socket_instance: DeadSocket align(@alignOf(usize)) = .{};
    pub var dead_socket = &dead_socket_instance;

    pub fn ActiveSocketType(comptime ssl: bool) type {
        return NewHTTPContext(ssl).ActiveSocket;
    }
}

export @value class DeadSocket {
    pub garbage: u8 = 0;
}

export class SocketEventHandler {
    pub static fn markTaggedSocketAsDead(@comptime ssl: bool, socket: anytype, tagged: anytype): void {
        zig {
            const Context = NewHTTPContext(ssl);
            if (tagged.is(Context.PooledSocket)) {
                Context.Handler.addMemoryBackToPool(tagged.as(Context.PooledSocket));
            }

            if (socket.ext(**anyopaque)) |ctx| {
                const ActiveSocket = TaggedPointerUnion(.{
                    DeadSocket,
                    HTTPClient,
                    Context.PooledSocket,
                    H2.ClientSession,
                });
                ctx.* = bun.cast(**anyopaque, ActiveSocket.init(dead_socket).ptr());
            }
        }
    }

    pub static fn markSocketAsDead(@comptime ssl: bool, socket: anytype): void {
        markTaggedSocketAsDead(ssl, socket, getTaggedFromSocket(ssl, socket));
    }

    pub static fn terminateSocket(@comptime ssl: bool, socket: anytype): void {
        markSocketAsDead(ssl, socket);
        socket.close(.failure);
    }

    pub static fn closeSocket(@comptime ssl: bool, socket: anytype): void {
        markSocketAsDead(ssl, socket);
        socket.close(.normal);
    }

    pub static fn getTagged(@comptime ssl: bool, ptr: *anyopaque): ActiveSocketType(ssl) {
        zig {
            return ActiveSocketType(ssl).from(ptr);
        }
    }

    pub static fn getTaggedFromSocket(@comptime ssl: bool, socket: anytype): ActiveSocketType(ssl) {
        zig {
            if (socket.ext(*anyopaque)) |slot| {
                return getTagged(ssl, slot.*);
            }
            return ActiveSocketType(ssl).init(dead_socket);
        }
    }

    pub static fn tagAsH2(socket: anytype, session: *H2.ClientSession): void {
        zig {
            const Context = NewHTTPContext(true);
            const ActiveSocket = TaggedPointerUnion(.{
                DeadSocket,
                HTTPClient,
                Context.PooledSocket,
                H2.ClientSession,
            });
            if (socket.ext(**anyopaque)) |ctx| {
                ctx.* = bun.cast(**anyopaque, ActiveSocket.init(session).ptr());
            }
        }
    }

    pub static fn addMemoryBackToPool(pooled: anytype): void {
        zig {
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
    }

    pub static fn onOpen(@comptime ssl: bool, ptr: *anyopaque, socket: anytype): void {
        zig {
            const active = getTagged(ssl, ptr);
            if (active.get(HTTPClient)) |client| {
                if (client.onOpen(comptime ssl, socket)) |_| {
                    return;
                } else |_| {
                    log("Unable to open socket", .{});
                    terminateSocket(ssl, socket);
                    return;
                }
            }

            log("Unexpected open on unknown socket", .{});
            terminateSocket(ssl, socket);
        }
    }

    pub static fn onHandshake(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
        success: i32,
        ssl_error: uws.us_bun_verify_error_t,
    ): void {
        zig {
            const handshake_success = if (success == 1) true else false;

            const handshake_error = HTTPCertError{
                .error_no = ssl_error.error_no,
                .code = if (ssl_error.code == null) "" else ssl_error.code[0..bun.len(ssl_error.code) :0],
                .reason = if (ssl_error.code == null) "" else ssl_error.reason[0..bun.len(ssl_error.reason) :0],
            };

            const active = getTagged(ssl, ptr);
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
                markSocketAsDead(ssl, socket);
                return;
            }

            if (handshake_success) {
                const Context = NewHTTPContext(ssl);
                if (active.is(Context.PooledSocket)) {
                    socket.setTimeout(0);
                    socket.setTimeoutMinutes(5);
                    return;
                }
            }

            terminateSocket(ssl, socket);
        }
    }

    pub static fn onClose(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
        code: c_int,
        ext: anytype,
    ): void {
        zig {
            _ = code;
            _ = ext;
            const tagged = getTagged(ssl, ptr);
            markSocketAsDead(ssl, socket);

            if (tagged.get(HTTPClient)) |client| {
                return client.onClose(comptime ssl, socket);
            }
            if (tagged.get(H2.ClientSession)) |session| {
                return session.onClose(error.ConnectionClosed);
            }
        }
    }

    pub static fn onData(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
        buf: string,
    ): void {
        zig {
            const tagged = getTagged(ssl, ptr);
            if (tagged.get(HTTPClient)) |client| {
                return client.onData(
                    comptime ssl,
                    buf,
                    client.getSslCtx(ssl),
                    socket,
                );
            } else if (tagged.get(H2.ClientSession)) |session| {
                return session.onData(buf);
            } else {
                const Context = NewHTTPContext(ssl);
                if (tagged.is(Context.PooledSocket)) {
                    const pooled = tagged.as(Context.PooledSocket);
                    if (pooled.proxy_tunnel != null) {
                        log("Data on idle pooled tunnel — evicting", .{});
                        terminateSocket(ssl, socket);
                        return;
                    }

                    if (pooled.h2_session) |session| {
                        session.onIdleData(buf);
                        if (!session.canPool()) terminateSocket(ssl, socket);
                        return;
                    }

                    if (strings.eqlComptime(buf, bun.http.end_of_chunked_http1_1_encoding_response_body)) {
                        return;
                    }

                    log("Unexpected data on socket", .{});
                    return;
                }
            }
            log("Unexpected data on unknown socket", .{});
            terminateSocket(ssl, socket);
        }
    }

    pub static fn onWritable(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
    ): void {
        zig {
            const tagged = getTagged(ssl, ptr);
            if (tagged.get(HTTPClient)) |client| {
                return client.onWritable(
                    false,
                    comptime ssl,
                    socket,
                );
            } else if (tagged.get(H2.ClientSession)) |session| {
                return session.onWritable();
            } else {
                const Context = NewHTTPContext(ssl);
                if (tagged.is(Context.PooledSocket)) {
                    // it's a keep-alive socket
                } else {
                    log("Unexpected writable on socket", .{});
                    terminateSocket(ssl, socket);
                }
            }
        }
    }

    pub static fn onLongTimeout(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
    ): void {
        zig {
            const tagged = getTagged(ssl, ptr);
            if (tagged.get(HTTPClient)) |client| {
                return client.onTimeout(comptime ssl, socket);
            }
            if (tagged.get(H2.ClientSession)) |session| {
                markSocketAsDead(ssl, socket);
                session.onClose(error.Timeout);
            }

            terminateSocket(ssl, socket);
        }
    }

    pub static fn onTimeout(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
    ): void {
        onLongTimeout(ssl, ptr, socket);
    }

    pub static fn onConnectError(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
        code: c_int,
    ): void {
        zig {
            _ = code;
            const tagged = getTagged(ssl, ptr);
            markTaggedSocketAsDead(ssl, socket, tagged);
            if (tagged.get(HTTPClient)) |client| {
                client.onConnectError();
            }
        }
    }

    pub static fn onEnd(
        @comptime ssl: bool,
        ptr: *anyopaque,
        socket: anytype,
    ): void {
        zig {
            const tagged = getTagged(ssl, ptr);
            markTaggedSocketAsDead(ssl, socket, tagged);
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
    }
}

export class HTTPContext {
    pub secure: *BoringSSL.SSL_CTX? = null;

    pub static fn init(ctx: anytype, @comptime ssl: bool): void {
        zig {
            ctx.group.init(bun.http.http_thread.loop.loop, null, ctx);
            if (comptime ssl) {
                var err: uws.create_bun_socket_error_t = .none;
                ctx.secure = (uws.SocketContext.BunSocketContextOptions{
                    .request_cert = 1,
                    .reject_unauthorized = 0,
                }).createSSLContext(&err).?;
                ctx.sslCtx().setup();
            }
        }
    }

    pub static fn release(ctx: anytype, @comptime ssl: bool): void {
        zig {
            {
                var iter = ctx.pending_sockets.used.iterator(.{ .kind = .set });
                while (iter.next()) |idx| {
                    const pooled = ctx.pending_sockets.at(@intCast(idx));
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

            ctx.active_h2_sessions.deinit(bun.default_allocator);
            for (ctx.pending_h2_connects.items) |pc| pc.deinit();
            ctx.pending_h2_connects.deinit(bun.default_allocator);

            ctx.group.closeAll();
            ctx.group.deinit();
            if (comptime ssl) {
                if (ctx.secure) |c| BoringSSL.SSL_CTX_free(c);
            }
            bun.default_allocator.destroy(ctx);
        }
    }

    pub static fn sslCtx(ctx: anytype): *BoringSSL.SSL_CTX {
        zig {
            return ctx.secure.?;
        }
    }

    pub static fn registerH2(ctx: anytype, session: *H2.ClientSession): void {
        zig {
            if (session.registry_index != std.math.maxInt(u32)) return;
            session.ref();
            session.registry_index = @intCast(ctx.active_h2_sessions.items.len);
            bun.handleOom(ctx.active_h2_sessions.append(bun.default_allocator, session));
        }
    }

    pub static fn unregisterH2(ctx: anytype, session: *H2.ClientSession): void {
        zig {
            const idx = session.registry_index;
            if (idx == std.math.maxInt(u32)) return;
            session.registry_index = std.math.maxInt(u32);
            const list = &ctx.active_h2_sessions;
            bun.debugAssert(idx < list.items.len and list.items[idx] == session);
            _ = list.swapRemove(idx);
            if (idx < list.items.len) list.items[idx].registry_index = idx;
            session.deref();
        }
    }

    pub static fn abortPendingH2Waiter(ctx: anytype, async_http_id: u32): bool {
        zig {
            for (ctx.pending_h2_connects.items) |pc| {
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
    }

    pub static fn initWithClientConfig(ctx: anytype, client: *HTTPClient): !void {
        zig {
            const opts = client.tls_props.?.get().asUSocketsForClientVerification();
            try initWithOpts(ctx, &opts);
        }
    }

    pub static fn initWithOpts(ctx: anytype, opts: anytype): !void {
        zig {
            var err: uws.create_bun_socket_error_t = .none;
            ctx.secure = opts.createSSLContext(&err) orelse return switch (err) {
                .load_ca_file => error.LoadCAFile,
                .invalid_ca_file => error.InvalidCAFile,
                .invalid_ca => error.InvalidCA,
                else => error.FailedToOpenSocket,
            };
            ctx.sslCtx().setup();
            ctx.group.init(bun.http.http_thread.loop.loop, null, ctx);
        }
    }

    pub static fn initWithThreadOpts(ctx: anytype, init_opts: anytype): !void {
        zig {
            var opts: uws.SocketContext.BunSocketContextOptions = .{
                .ca = if (init_opts.ca.len > 0) @ptrCast(init_opts.ca) else null,
                .ca_count = @intCast(init_opts.ca.len),
                .ca_file_name = if (init_opts.abs_ca_file_name.len > 0) init_opts.abs_ca_file_name else null,
                .request_cert = 1,
            };
            try initWithOpts(ctx, &opts);
        }
    }

    pub static fn releaseSocket(
        ctx: anytype,
        @comptime ssl: bool,
        socket: anytype,
        did_have_handshaking_error_while_reject_unauthorized_is_false: bool,
        hostname: string,
        port: u16,
        ssl_config: anytype,
        tunnel: *ProxyTunnel?,
        target_hostname: string,
        target_port: u16,
        proxy_auth_hash: u64,
        h2_session: *H2.ClientSession?,
    ): void {
        zig {
            const Context = NewHTTPContext(ssl);
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
                if (ctx.pending_sockets.get()) |pending| {
                    if (socket.ext(**anyopaque)) |slot| {
                        const ActiveSocket = TaggedPointerUnion(.{
                            DeadSocket,
                            HTTPClient,
                            Context.PooledSocket,
                            H2.ClientSession,
                        });
                        slot.* = bun.cast(**anyopaque, ActiveSocket.init(pending).ptr());
                    }
                    socket.flush();
                    socket.timeout(0);
                    socket.setTimeoutMinutes(5);

                    pending.http_socket = socket;
                    pending.did_have_handshaking_error_while_reject_unauthorized_is_false = did_have_handshaking_error_while_reject_unauthorized_is_false;
                    @memcpy(pending.hostname_buf[0..hostname.len], hostname);
                    pending.hostname_len = @as(u8, @truncate(hostname.len));
                    pending.port = port;
                    pending.owner = ctx;
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
            SocketEventHandler.closeSocket(ssl, socket);
        }
    }

    pub static fn existingSocket(
        ctx: anytype,
        @comptime ssl: bool,
        reject_unauthorized: bool,
        hostname: string,
        port: u16,
        ssl_config: *SSLConfig?,
        want_tunnel: bool,
        target_hostname: string,
        target_port: u16,
        proxy_auth_hash: u64,
        want_h2: BoringSSL.SSL.AlpnOffer,
    ): NewHTTPContext(ssl).ExistingSocket? {
        zig {
            const Context = NewHTTPContext(ssl);
            if (hostname.len > MAX_KEEPALIVE_HOSTNAME)
                return null;

            var iter = ctx.pending_sockets.used.iterator(.{ .kind = .set });

            while (iter.next()) |pending_socket_index| {
                var socket = ctx.pending_sockets.at(@as(u16, @intCast(pending_socket_index)));
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
                        SocketEventHandler.markSocketAsDead(ssl, http_socket);
                        continue;
                    }

                    if (http_socket.isShutdown() or http_socket.getError() != 0) {
                        SocketEventHandler.terminateSocket(ssl, http_socket);
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
                    assert(ctx.pending_sockets.put(socket));
                    log("+ Keep-Alive reuse {s}:{d}{s}", .{ hostname, port, if (tunnel != null) " (with tunnel)" else "" });
                    return Context.ExistingSocket{ .socket = http_socket, .tunnel = tunnel, .h2_session = h2_session };
                }
            }

            return null;
        }
    }

    pub static fn connectSocket(
        ctx: anytype,
        @comptime ssl: bool,
        client: *HTTPClient,
        socket_path: string,
    ): !NewHTTPContext(ssl).HTTPSocket? {
        zig {
            const Context = NewHTTPContext(ssl);
            const kind: uws.SocketKind = if (ssl) .http_client_tls else .http_client;
            const ActiveSocket = TaggedPointerUnion(.{
                DeadSocket,
                HTTPClient,
                Context.PooledSocket,
                H2.ClientSession,
            });

            client.connected_url = if (client.http_proxy) |proxy| proxy else client.url;
            const socket = try Context.HTTPSocket.connectUnixGroup(
                &ctx.group,
                kind,
                if (comptime ssl) ctx.secure else null,
                socket_path,
                ActiveSocket.init(client).ptr(),
                false,
            );
            client.allow_retry = false;
            return socket;
        }
    }

    pub static fn connect(
        ctx: anytype,
        @comptime ssl: bool,
        client: *HTTPClient,
        hostname_: string,
        port: u16,
    ): !NewHTTPContext(ssl).HTTPSocket? {
        zig {
            const Context = NewHTTPContext(ssl);
            const kind: uws.SocketKind = if (ssl) .http_client_tls else .http_client;
            const ActiveSocket = TaggedPointerUnion(.{
                DeadSocket,
                HTTPClient,
                Context.PooledSocket,
                H2.ClientSession,
            });

            const hostname = if (FeatureFlags.hardcode_localhost_to_127_0_0_1 and strings.eqlComptime(hostname_, "localhost"))
                "127.0.0.1"
            else
                hostname_;

            client.connected_url = if (client.http_proxy) |proxy| proxy else client.url;
            client.connected_url.hostname = hostname;

            if (comptime ssl) {
                if (client.canOfferH2()) {
                    for (ctx.active_h2_sessions.items) |session| {
                        if (session.hasHeadroom() and session.matches(hostname, port, SSLConfig.rawPtr(client.tls_props))) {
                            session.adopt(client);
                            return null;
                        }
                    }
                    for (ctx.pending_h2_connects.items) |pc| {
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

                if (existingSocket(
                    ctx,
                    ssl,
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
                    if (sock.ext(**anyopaque)) |slot| {
                        slot.* = bun.cast(**anyopaque, ActiveSocket.init(client).ptr());
                    }
                    client.allow_retry = true;
                    if (found.h2_session) |session| {
                        if (comptime ssl) {
                            session.socket = sock;
                            SocketEventHandler.tagAsH2(sock, session);
                            registerH2(ctx, session);
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

            const socket = try Context.HTTPSocket.connectGroup(
                &ctx.group,
                kind,
                if (comptime ssl) ctx.secure else null,
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
                    bun.handleOom(ctx.pending_h2_connects.append(bun.default_allocator, pc));
                    client.pending_h2 = pc;
                }
            }
            return socket;
        }
    }
}

pub fn NewHTTPContext(@comptime ssl: bool): type {
    zig {
        return struct {
            pub const is_ssl = ssl;
            pub const HTTPSocket = uws.NewSocketHandler(ssl);
            pub const PooledSocket = struct {
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

                pub fn isTunnel(this: *const @This()) bool {
                    return this.proxy_tunnel != null;
                }
                pub fn isH2(this: *const @This()) bool {
                    return this.h2_session != null;
                }
            };
            pub const PooledSocketHiveAllocator = bun.HiveArray(PooledSocket, pool_size);
            const Context = @This();
            const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
            pub const ref = RefCount.ref;
            pub const deref = RefCount.deref;

            ref_count: RefCount,
            pending_sockets: PooledSocketHiveAllocator,
            group: uws.SocketGroup = .{},
            secure: ?*BoringSSL.SSL_CTX = null,
            active_h2_sessions: std.ArrayListUnmanaged(*H2.ClientSession) = .{},
            pending_h2_connects: std.ArrayListUnmanaged(*H2.PendingConnect) = .{},

            pub const ActiveSocketHandler = Handler;
            pub const Handler = struct {
                pub fn onOpen(ptr: *anyopaque, socket: HTTPSocket) void {
                    SocketEventHandler.onOpen(ssl, ptr, socket);
                }
                pub fn onHandshake(ptr: *anyopaque, socket: HTTPSocket, success: i32, ssl_error: uws.us_bun_verify_error_t) void {
                    SocketEventHandler.onHandshake(ssl, ptr, socket, success, ssl_error);
                }
                pub fn onClose(ptr: *anyopaque, socket: HTTPSocket, code: c_int, ext: ?*anyopaque) void {
                    SocketEventHandler.onClose(ssl, ptr, socket, code, ext);
                }
                pub fn addMemoryBackToPool(pooled: *PooledSocket) void {
                    SocketEventHandler.addMemoryBackToPool(pooled);
                }
                pub fn onData(ptr: *anyopaque, socket: HTTPSocket, buf: []const u8) void {
                    SocketEventHandler.onData(ssl, ptr, socket, buf);
                }
                pub fn onWritable(ptr: *anyopaque, socket: HTTPSocket) void {
                    SocketEventHandler.onWritable(ssl, ptr, socket);
                }
                pub fn onLongTimeout(ptr: *anyopaque, socket: HTTPSocket) void {
                    SocketEventHandler.onLongTimeout(ssl, ptr, socket);
                }
                pub fn onTimeout(ptr: *anyopaque, socket: HTTPSocket) void {
                    SocketEventHandler.onTimeout(ssl, ptr, socket);
                }
                pub fn onConnectError(ptr: *anyopaque, socket: HTTPSocket, code: c_int) void {
                    SocketEventHandler.onConnectError(ssl, ptr, socket, code);
                }
                pub fn onEnd(ptr: *anyopaque, socket: HTTPSocket) void {
                    SocketEventHandler.onEnd(ssl, ptr, socket);
                }
            };

            pub const ExistingSocket = struct {
                socket: HTTPSocket,
                tunnel: ?*ProxyTunnel,
                h2_session: ?*H2.ClientSession,

                pub fn hasTunnel(this: @This()) bool {
                    return this.tunnel != null;
                }
                pub fn hasH2(this: @This()) bool {
                    return this.h2_session != null;
                }
            };

            pub const ActiveSocket = TaggedPointerUnion(.{
                DeadSocket,
                HTTPClient,
                PooledSocket,
                H2.ClientSession,
            });

            pub fn markTaggedSocketAsDead(socket: HTTPSocket, tagged: ActiveSocket) void {
                SocketEventHandler.markTaggedSocketAsDead(ssl, socket, tagged);
            }
            pub fn markSocketAsDead(socket: HTTPSocket) void {
                SocketEventHandler.markSocketAsDead(ssl, socket);
            }
            pub fn terminateSocket(socket: HTTPSocket) void {
                SocketEventHandler.terminateSocket(ssl, socket);
            }
            pub fn closeSocket(socket: HTTPSocket) void {
                SocketEventHandler.closeSocket(ssl, socket);
            }
            pub fn getTaggedFromSocket(socket: HTTPSocket) ActiveSocket {
                return SocketEventHandler.getTaggedFromSocket(ssl, socket);
            }
            pub fn tagAsH2(socket: HTTPSocket, session: *H2.ClientSession) void {
                SocketEventHandler.tagAsH2(socket, session);
            }
            pub fn context() *Context {
                if (comptime ssl) return &bun.http.http_thread.https_context else return &bun.http.http_thread.http_context;
            }

            pub fn init(this: *Context) void {
                HTTPContext.init(this, ssl);
            }
            pub fn deinit(this: *Context) void {
                HTTPContext.release(this, ssl);
            }
            pub fn sslCtx(this: *Context) *BoringSSL.SSL_CTX {
                if (comptime !ssl) unreachable;
                return HTTPContext.sslCtx(this);
            }
            pub fn registerH2(this: *Context, session: *H2.ClientSession) void {
                if (comptime !ssl) return;
                HTTPContext.registerH2(this, session);
            }
            pub fn unregisterH2(this: *Context, session: *H2.ClientSession) void {
                if (comptime !ssl) return;
                HTTPContext.unregisterH2(this, session);
            }
            pub fn abortPendingH2Waiter(this: *Context, async_http_id: u32) bool {
                if (comptime !ssl) return false;
                return HTTPContext.abortPendingH2Waiter(this, async_http_id);
            }
            pub fn initWithClientConfig(this: *Context, client: *HTTPClient) InitError!void {
                if (comptime !ssl) @compileError("ssl only");
                return HTTPContext.initWithClientConfig(this, client);
            }
            pub fn initWithOpts(this: *Context, opts: *const uws.SocketContext.BunSocketContextOptions) InitError!void {
                if (comptime !ssl) @compileError("ssl only");
                return HTTPContext.initWithOpts(this, opts);
            }
            pub fn initWithThreadOpts(this: *Context, init_opts: *const HTTPThread.InitOpts) InitError!void {
                if (comptime !ssl) @compileError("ssl only");
                return HTTPContext.initWithThreadOpts(this, init_opts);
            }
            pub fn releaseSocket(this: *Context, socket: HTTPSocket, did_have_handshaking_error_while_reject_unauthorized_is_false: bool, hostname: []const u8, port: u16, ssl_config: ?SSLConfig.SharedPtr, tunnel: ?*ProxyTunnel, target_hostname: []const u8, target_port: u16, proxy_auth_hash: u64, h2_session: ?*H2.ClientSession) void {
                HTTPContext.releaseSocket(this, ssl, socket, did_have_handshaking_error_while_reject_unauthorized_is_false, hostname, port, ssl_config, tunnel, target_hostname, target_port, proxy_auth_hash, h2_session);
            }
            pub fn existingSocket(this: *Context, reject_unauthorized: bool, hostname: []const u8, port: u16, ssl_config: ?*SSLConfig, want_tunnel: bool, target_hostname: []const u8, target_port: u16, proxy_auth_hash: u64, want_h2: BoringSSL.SSL.AlpnOffer) ?ExistingSocket {
                return HTTPContext.existingSocket(this, ssl, reject_unauthorized, hostname, port, ssl_config, want_tunnel, target_hostname, target_port, proxy_auth_hash, want_h2);
            }
            pub fn connectSocket(this: *Context, client: *HTTPClient, socket_path: []const u8) !?HTTPSocket {
                return HTTPContext.connectSocket(this, ssl, client, socket_path);
            }
            pub fn connect(this: *Context, client: *HTTPClient, hostname_: []const u8, port: u16) !?HTTPSocket {
                return HTTPContext.connect(this, ssl, client, hostname_, port);
            }
        };
    }
}
