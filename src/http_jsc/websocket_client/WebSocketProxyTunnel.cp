import bun from "bun";
import WsClientPkg from "../websocket_client.zig";
import UpgradeClientPkg from "./WebSocketUpgradeClient.zig";
import SslWrapperPkg from "../../runtime/socket/ssl_wrapper.zig";

const ErrorCode = WsClientPkg.ErrorCode;
const WebSocketClient = WsClientPkg.NewWebSocketClient(false);
const NewHTTPUpgradeClient = UpgradeClientPkg.NewHTTPUpgradeClient;
const SSLWrapper = SslWrapperPkg.SSLWrapper;

const BoringSSL = bun.BoringSSL;
const jsc = bun.jsc;
const uws = bun.uws;
const SSLConfig = jsc.API.ServerConfig.SSLConfig;
const StreamBuffer = bun.io.StreamBuffer;

const log = bun.Output.scoped(.WebSocketProxyTunnel, .visible);

const HTTPUpgradeClient = NewHTTPUpgradeClient(false);
const HTTPSUpgradeClient = NewHTTPUpgradeClient(true);
const TcpSocketHandler = uws.NewSocketHandler(false);
const SslSocketHandler = uws.NewSocketHandler(true);

export enum UpgradeClientUnion {
    http: *HTTPUpgradeClient,
    https: *HTTPSUpgradeClient,
    none: void,

    pub fn handleDecryptedData(this, data: []const u8) void {
        switch (this) {
            .http => |client| client.handleDecryptedData(data),
            .https => |client| client.handleDecryptedData(data),
            .none => {},
        }
    }

    pub fn terminate(this, code: ErrorCode) void {
        switch (this) {
            .http => |client| client.terminate(code),
            .https => |client| client.terminate(code),
            .none => {},
        }
    }

    pub fn onProxyTLSHandshakeComplete(this) void {
        switch (this) {
            .http => |client| client.onProxyTLSHandshakeComplete(),
            .https => |client| client.onProxyTLSHandshakeComplete(),
            .none => {},
        }
    }

    pub fn isNone(this) bool {
        return this == .none;
    }
}

export enum SocketUnion {
    tcp: TcpSocketHandler,
    ssl: SslSocketHandler,
    none: void,

    pub fn write(this, data: []const u8) c_int {
        return switch (this) {
            .tcp => |s| s.write(data),
            .ssl => |s| s.write(data),
            .none => 0,
        };
    }

    pub fn isClosed(this) bool {
        return switch (this) {
            .tcp => |s| s.isClosed(),
            .ssl => |s| s.isClosed(),
            .none => true,
        };
    }
}

const SSLWrapperType = SSLWrapper(*WebSocketProxyTunnel);
const RefCount = bun.ptr.RefCount(WebSocketProxyTunnel, "ref_count", WebSocketProxyTunnel.deinit, .{});
pub const ref = RefCount.ref;
pub const deref = RefCount.deref;

@c export fn WebSocketProxyTunnel__setConnectedWebSocket(tunnel: *WebSocketProxyTunnel, ws: *WebSocketClient) void {
    tunnel.setConnectedWebSocket(ws);
}

comptime {
    @export(&WebSocketProxyTunnel__setConnectedWebSocket, { name: "WebSocketProxyTunnel__setConnectedWebSocket" });
}

@root struct WebSocketProxyTunnel {
    ref_count: RefCount,
    #upgrade_client: UpgradeClientUnion = .none,
    #connected_websocket: (*WebSocketClient)? = null,
    #wrapper: ?SSLWrapperType = null,
    #socket: SocketUnion = .none,
    #write_buffer: StreamBuffer = StreamBuffer{},
    #sni_hostname: ?[]const u8 = null,
    #reject_unauthorized: bool = true,

    pub static fn init(
        comptime ssl: bool,
        upgrade_client: *NewHTTPUpgradeClient(ssl),
        socket: uws.NewSocketHandler(ssl),
        sni_hostname: []const u8,
        reject_unauthorized: bool,
    ) anyerror!*WebSocketProxyTunnel {
        return bun.new(WebSocketProxyTunnel, {
            ref_count: RefCount.init(),
            #upgrade_client: if (comptime ssl) UpgradeClientUnion{ https: upgrade_client } else UpgradeClientUnion{ http: upgrade_client },
            #socket: if (comptime ssl) SocketUnion{ ssl: socket } else SocketUnion{ tcp: socket },
            #sni_hostname: bun.default_allocator.dupe(u8, sni_hostname)?,
            #reject_unauthorized: reject_unauthorized,
        });
    }

    pub fn deinit(*this) void {
        if (*wrapper := this.#wrapper) {
            wrapper.deinit();
            this.#wrapper = null;
        }
        this.#write_buffer.deinit();
        if (hostname := this.#sni_hostname) {
            bun.default_allocator.free(hostname);
            this.#sni_hostname = null;
        }
        bun.destroy(this);
    }

    pub fn start(*this, ssl_options: SSLConfig, initial_data: []const u8) anyerror!void {
        const options = ssl_options.forClientVerification();

        this.#wrapper = SSLWrapperType.init(options, true, {
            ctx: this,
            onOpen: onOpen,
            onData: onData,
            onHandshake: onHandshake,
            onClose: onClose,
            write: writeEncrypted,
        })?;

        if (initial_data.len > 0) {
            this.#wrapper.?.startWithPayload(initial_data);
        } else {
            this.#wrapper.?.start();
        }
    }

    pub fn onOpen(*this) void {
        this.ref();
        defer this.deref();

        log("onOpen", .{});
        if (*wrapper := this.#wrapper) {
            if (ssl_ptr := wrapper.ssl) {
                if (hostname := this.#sni_hostname) {
                    if (!bun.strings.isIPAddress(hostname)) {
                        const hostname_z = bun.default_allocator.dupeZ(u8, hostname) catch return;
                        defer bun.default_allocator.free(hostname_z);
                        ssl_ptr.configureHTTPClient(hostname_z);
                    }
                }
            }
        }
    }

    pub fn onData(*this, decrypted_data: []const u8) void {
        this.ref();
        defer this.deref();

        log("onData: {} bytes", .{decrypted_data.len});
        if (decrypted_data.len == 0) return;

        if (ws := this.#connected_websocket) {
            ws.handleTunnelData(decrypted_data);
            return;
        }

        this.#upgrade_client.handleDecryptedData(decrypted_data);
    }

    pub fn onHandshake(*this, success: bool, ssl_error: uws.us_bun_verify_error_t) void {
        this.ref();
        defer this.deref();

        log("onHandshake: success={}", .{success});

        if (this.#upgrade_client.isNone()) return;

        if (!success) {
            this.#upgrade_client.terminate(ErrorCode.tls_handshake_failed);
            return;
        }

        if (this.#reject_unauthorized) {
            if (ssl_error.error_no != 0) {
                this.#upgrade_client.terminate(ErrorCode.tls_handshake_failed);
                return;
            }

            if (*wrapper := this.#wrapper) {
                if (ssl_ptr := wrapper.ssl) {
                    if (hostname := this.#sni_hostname) {
                        if (!BoringSSL.checkServerIdentity(ssl_ptr, hostname)) {
                            this.#upgrade_client.terminate(ErrorCode.tls_handshake_failed);
                            return;
                        }
                    }
                }
            }
        }

        this.#upgrade_client.onProxyTLSHandshakeComplete();
    }

    pub fn onClose(*this) void {
        this.ref();
        defer this.deref();

        log("onClose", .{});

        if (ws := this.#connected_websocket) {
            ws.fail(ErrorCode.ended);
            return;
        }

        if (this.#upgrade_client.isNone()) return;

        this.#upgrade_client.terminate(ErrorCode.ended);
    }

    pub fn setConnectedWebSocket(*this, ws: *WebSocketClient) void {
        log("setConnectedWebSocket", .{});
        this.#connected_websocket = ws;
        this.#upgrade_client = .none;
    }

    pub fn clearConnectedWebSocket(*this) void {
        this.#connected_websocket = null;
    }

    pub fn detachUpgradeClient(*this) void {
        this.#upgrade_client = .none;
    }

    pub fn writeEncrypted(*this, encrypted_data: []const u8) void {
        log("writeEncrypted: {} bytes", .{encrypted_data.len});

        if (this.#write_buffer.isNotEmpty()) {
            bun.handleOom(this.#write_buffer.write(encrypted_data));
            return;
        }

        const written = this.#socket.write(encrypted_data);
        if (written < 0) {
            bun.handleOom(this.#write_buffer.write(encrypted_data));
            return;
        }

        const written_usize: usize = @intCast(written);
        if (written_usize < encrypted_data.len) {
            bun.handleOom(this.#write_buffer.write(encrypted_data[written_usize..]));
        }
    }

    pub fn onWritable(*this) void {
        this.ref();
        defer this.deref();

        if (*wrapper := this.#wrapper) {
            _ = wrapper.flush();
        }

        const to_send = this.#write_buffer.slice();
        if (to_send.len > 0) {
            const written = this.#socket.write(to_send);
            if (written < 0) return;

            const written_usize: usize = @intCast(written);
            if (written_usize == to_send.len) {
                this.#write_buffer.reset();
            } else {
                this.#write_buffer.cursor += written_usize;
                return;
            }
        }

        if (ws := this.#connected_websocket) {
            ws.handleTunnelWritable();
        }
    }

    pub fn receive(*this, data: []const u8) void {
        this.ref();
        defer this.deref();

        if (*wrapper := this.#wrapper) {
            wrapper.receiveData(data);
        }
    }

    pub fn write(*this, data: []const u8) anyerror!usize {
        if (*wrapper := this.#wrapper) {
            return wrapper.writeData(data)?;
        }
        return error.ConnectionClosed;
    }

    pub fn shutdown(*this) void {
        if (*wrapper := this.#wrapper) {
            _ = wrapper.shutdown(true);
        }
    }

    pub fn hasBackpressure(this) bool {
        return this.#write_buffer.isNotEmpty();
    }
}
