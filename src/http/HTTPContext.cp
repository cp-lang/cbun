// src/http/HTTPContext.cp
// HTTP Context and Keep-Alive Socket Pooling in CP language

import bun from "bun";
import { SocketHandler } from "../uws_sys/socket.cp";
import { SocketGroup } from "../uws_sys/SocketGroup.cp";
import { BunSocketContextOptions } from "../uws_sys/SocketContext.cp";

const MAX_KEEPALIVE_HOSTNAME: usize = 128;
const pool_size: usize = 64;

export struct DeadSocket {
    garbage: u8 = 0,
}

export var dead_socket_instance: DeadSocket = DeadSocket {};
export var dead_socket: *DeadSocket = &dead_socket_instance;

export static class SocketEventHandler {
    fn markTaggedSocketAsDead<const ssl: bool>(socket: anytype, tagged: anytype): void {
        let Context = HTTPContext<ssl>;
        if (tagged.is(Context.PooledSocket)) {
            SocketEventHandler.addMemoryBackToPool(tagged.as(Context.PooledSocket));
        }

        let ctx = socket.ext<*any!>();
        if (ctx != null) {
            let ActiveSocket = Context.ActiveSocket;
            ctx!.* = ActiveSocket.init(dead_socket).ptr() as *any!;
        }
    }

    fn markSocketAsDead<const ssl: bool>(socket: anytype): void {
        SocketEventHandler.markTaggedSocketAsDead<ssl>(socket, SocketEventHandler.getTaggedFromSocket<ssl>(socket));
    }

    fn terminateSocket<const ssl: bool>(socket: anytype): void {
        SocketEventHandler.markSocketAsDead<ssl>(socket);
        socket.close(bun.uws.CloseCode.failure);
    }

    fn closeSocket<const ssl: bool>(socket: anytype): void {
        SocketEventHandler.markSocketAsDead<ssl>(socket);
        socket.close(bun.uws.CloseCode.normal);
    }

    fn getTagged<const ssl: bool>(ptr: *any!): HTTPContext<ssl>.ActiveSocket {
        return HTTPContext<ssl>.ActiveSocket.from(ptr);
    }

    fn getTaggedFromSocket<const ssl: bool>(socket: anytype): HTTPContext<ssl>.ActiveSocket {
        let slot = socket.ext<*any!>();
        if (slot != null) {
            return SocketEventHandler.getTagged<ssl>(slot!.*);
        }
        return HTTPContext<ssl>.ActiveSocket.init(dead_socket);
    }

    fn tagAsH2(socket: anytype, session: *bun.http.H2.ClientSession): void {
        let Context = HTTPContext<true>;
        let ActiveSocket = Context.ActiveSocket;
        let ctx = socket.ext<*any!>();
        if (ctx != null) {
            ctx!.* = ActiveSocket.init(session).ptr() as *any!;
        }
    }

    fn addMemoryBackToPool(pooled: anytype): void {
        if (pooled.ssl_config != null) {
            pooled.ssl_config!.release();
            pooled.ssl_config = null;
        }
        if (pooled.proxy_tunnel != null) {
            pooled.proxy_tunnel!.deref();
            pooled.proxy_tunnel = null;
        }
        if (pooled.target_hostname.len > 0) {
            bun.default_allocator.free(pooled.target_hostname);
            pooled.target_hostname = "";
        }
        if (pooled.h2_session != null) {
            pooled.h2_session!.deref();
            pooled.h2_session = null;
        }
        bun.assert(pooled.owner.pending_sockets.put(pooled));
    }

    fn onOpen<const ssl: bool>(ptr: *any!, socket: anytype): void {
        let active = SocketEventHandler.getTagged<ssl>(ptr);
        let client = active.get(bun.http);
        if (client != null) {
            if (client!.onOpen(ssl, socket)) {
                return;
            } else {
                SocketEventHandler.terminateSocket<ssl>(socket);
                return;
            }
        }

        SocketEventHandler.terminateSocket<ssl>(socket);
    }

    fn onHandshake<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
        success: i32,
        ssl_error: bun.uws.us_bun_verify_error_t,
    ): void {
        let handshake_success = success == 1;

        let handshake_error = bun.http.HTTPCertError {
            error_no = ssl_error.error_no,
            code = if (ssl_error.code == null) "" else bun.sliceTo(ssl_error.code!, 0),
            reason = if (ssl_error.code == null) "" else bun.sliceTo(ssl_error.reason!, 0),
        };

        let active = SocketEventHandler.getTagged<ssl>(ptr);
        let client = active.get(bun.http);
        if (client != null) {
            let c = client!;
            c.flags.did_have_handshaking_error = handshake_error.error_no != 0;
            if (handshake_success) {
                if (c.flags.reject_unauthorized) {
                    if (c.flags.did_have_handshaking_error) {
                        c.closeAndFail(bun.BoringSSL.c.getCertErrorFromNo(handshake_error.error_no), ssl, socket);
                        return;
                    }

                    let ssl_ptr = socket.getNativeHandle() as *bun.BoringSSL.c.SSL;
                    if (!c.checkServerIdentity(ssl, socket, handshake_error, ssl_ptr, true)) {
                        return;
                    }
                }

                return c.firstCall(ssl, socket);
            } else {
                if (c.flags.did_have_handshaking_error) {
                    c.closeAndFail(bun.BoringSSL.c.getCertErrorFromNo(handshake_error.error_no), ssl, socket);
                    return;
                }
                c.closeAndFail(error.ConnectionRefused, ssl, socket);
                return;
            }
        }

        if (socket.isClosed()) {
            SocketEventHandler.markSocketAsDead<ssl>(socket);
            return;
        }

        if (handshake_success) {
            let Context = HTTPContext<ssl>;
            if (active.is(Context.PooledSocket)) {
                socket.setTimeout(0);
                socket.setTimeoutMinutes(5);
                return;
            }
        }

        SocketEventHandler.terminateSocket<ssl>(socket);
    }

    fn onClose<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
        code: i32,
        ext: *any?!,
    ): void {
        let tagged = SocketEventHandler.getTagged<ssl>(ptr);
        SocketEventHandler.markSocketAsDead<ssl>(socket);

        let client = tagged.get(bun.http);
        if (client != null) {
            return client!.onClose(ssl, socket);
        }
        let session = tagged.get(bun.http.H2.ClientSession);
        if (session != null) {
            return session!.onClose(error.ConnectionClosed);
        }
    }

    fn onData<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
        buf: string,
    ): void {
        let tagged = SocketEventHandler.getTagged<ssl>(ptr);
        let client = tagged.get(bun.http);
        if (client != null) {
            return client!.onData(
                ssl,
                buf,
                client!.getSslCtx(ssl),
                socket,
            );
        }
        let session = tagged.get(bun.http.H2.ClientSession);
        if (session != null) {
            return session!.onData(buf);
        }

        let Context = HTTPContext<ssl>;
        if (tagged.is(Context.PooledSocket)) {
            let pooled = tagged.as(Context.PooledSocket);
            if (pooled.proxy_tunnel != null) {
                SocketEventHandler.terminateSocket<ssl>(socket);
                return;
            }

            if (pooled.h2_session != null) {
                let h2 = pooled.h2_session!;
                h2.onIdleData(buf);
                if (!h2.canPool()) SocketEventHandler.terminateSocket<ssl>(socket);
                return;
            }

            if (buf == bun.http.end_of_chunked_http1_1_encoding_response_body) {
                return;
            }
            return;
        }

        SocketEventHandler.terminateSocket<ssl>(socket);
    }

    fn onWritable<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
    ): void {
        let tagged = SocketEventHandler.getTagged<ssl>(ptr);
        let client = tagged.get(bun.http);
        if (client != null) {
            return client!.onWritable(
                false,
                ssl,
                socket,
            );
        }
        let session = tagged.get(bun.http.H2.ClientSession);
        if (session != null) {
            return session!.onWritable();
        }

        let Context = HTTPContext<ssl>;
        if (!tagged.is(Context.PooledSocket)) {
            SocketEventHandler.terminateSocket<ssl>(socket);
        }
    }

    fn onLongTimeout<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
    ): void {
        let tagged = SocketEventHandler.getTagged<ssl>(ptr);
        let client = tagged.get(bun.http);
        if (client != null) {
            return client!.onTimeout(ssl, socket);
        }
        let session = tagged.get(bun.http.H2.ClientSession);
        if (session != null) {
            SocketEventHandler.markSocketAsDead<ssl>(socket);
            session!.onClose(error.Timeout);
        }

        SocketEventHandler.terminateSocket<ssl>(socket);
    }

    fn onTimeout<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
    ): void {
        SocketEventHandler.onLongTimeout<ssl>(ptr, socket);
    }

    fn onConnectError<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
        code: i32,
    ): void {
        let tagged = SocketEventHandler.getTagged<ssl>(ptr);
        SocketEventHandler.markTaggedSocketAsDead<ssl>(socket, tagged);
        let client = tagged.get(bun.http);
        if (client != null) {
            client!.onConnectError();
        }
    }

    fn onEnd<const ssl: bool>(
        ptr: *any!,
        socket: anytype,
    ): void {
        let tagged = SocketEventHandler.getTagged<ssl>(ptr);
        SocketEventHandler.markTaggedSocketAsDead<ssl>(socket, tagged);
        socket.close(bun.uws.CloseCode.failure);

        let client = tagged.get(bun.http);
        if (client != null) {
            client!.onClose(ssl, socket);
            return;
        }
        let session = tagged.get(bun.http.H2.ClientSession);
        if (session != null) {
            session!.onClose(error.ConnectionClosed);
            return;
        }
    }
}

export class HTTPContext<const ssl: bool> {
    pub const is_ssl: bool = ssl;
    pub type HTTPSocket = SocketHandler<ssl>;

    pub struct PooledSocket {
        http_socket: HTTPSocket,
        hostname_buf: u8[MAX_KEEPALIVE_HOSTNAME],
        hostname_len: u8 = 0,
        port: u16 = 0,
        did_have_handshaking_error_while_reject_unauthorized_is_false: bool = false,
        ssl_config: bun.api.server.ServerConfig.SSLConfig.SharedPtr? = null,
        owner: *HTTPContext<ssl>,
        proxy_tunnel: bun.http.ProxyTunnel.RefPtr? = null,
        target_hostname: string = "",
        target_port: u16 = 0,
        proxy_auth_hash: u64 = 0,
        h2_session: *bun.http.H2.ClientSession? = null,

        fn isTunnel(this): bool {
            return this.proxy_tunnel != null;
        }
        fn isH2(this): bool {
            return this.h2_session != null;
        }
    }

    pub struct ExistingSocket {
        socket: HTTPSocket,
        tunnel: *bun.http.ProxyTunnel?,
        h2_session: *bun.http.H2.ClientSession?,

        fn hasTunnel(this): bool {
            return this.tunnel != null;
        }
        fn hasH2(this): bool {
            return this.h2_session != null;
        }
    }

    pub type ActiveSocket = bun.ptr.TaggedPointerUnion([
        DeadSocket,
        bun.http,
        PooledSocket,
        bun.http.H2.ClientSession,
    ]);

    ref_count: bun.ptr.RefCount,
    pending_sockets: bun.HiveArray<PooledSocket, pool_size>,
    group: SocketGroup,
    secure: *bun.BoringSSL.c.SSL_CTX? = null,
    active_h2_sessions: bun.ArrayList<*bun.http.H2.ClientSession>,
    pending_h2_connects: bun.ArrayList<*bun.http.H2.PendingConnect>,

    pub fn init(*this): void {
        this.group.init(bun.http.http_thread.loop.loop, null, this as *any!);
        if (ssl) {
            let err = bun.uws.create_bun_socket_error_t.none;
            let opts = BunSocketContextOptions {
                request_cert = 1,
                reject_unauthorized = 0,
            };
            this.secure = BunSocketContextOptions.createSSLContext(opts, &err)!;
        }
    }

    pub fn release(*this): void {
        this.group.closeAll();
        this.group.deinit();
        if (ssl && this.secure != null) {
            bun.BoringSSL.c.SSL_CTX_free(this.secure!);
        }
        bun.default_allocator.destroy(this);
    }

    pub fn sslCtx(*this): *bun.BoringSSL.c.SSL_CTX {
        return this.secure!;
    }
}
