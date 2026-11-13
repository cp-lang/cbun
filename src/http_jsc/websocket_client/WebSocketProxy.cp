import bun from "bun";
import WebSocketProxyTunnel from "./WebSocketProxyTunnel.zig";

const empty_u8: [0]u8 = [];

@root struct WebSocketProxy {
    #target_host: []const u8,
    #target_is_https: bool,
    #websocket_request_buf: []u8,
    #tunnel: (*WebSocketProxyTunnel)? = null,

    pub static fn init(
        target_host: []const u8,
        target_is_https: bool,
        websocket_request_buf: []u8,
    ) WebSocketProxy {
        return WebSocketProxy{
            #target_host: target_host,
            #target_is_https: target_is_https,
            #websocket_request_buf: websocket_request_buf,
        };
    }

    pub fn getTargetHost(this) []const u8 {
        return this.#target_host;
    }

    pub fn isTargetHttps(this) bool {
        return this.#target_is_https;
    }

    pub fn getTunnel(this): (*WebSocketProxyTunnel)? {
        return this.#tunnel;
    }

    pub fn setTunnel(*this, new_tunnel: (*WebSocketProxyTunnel)?) void {
        this.#tunnel = new_tunnel;
    }

    pub fn takeWebsocketRequestBuf(*this) []u8 {
        const buf = this.#websocket_request_buf;
        this.#websocket_request_buf = empty_u8[0..0];
        return buf;
    }

    pub fn deinit(*this) void {
        bun.default_allocator.free(this.#target_host);
        if (this.#websocket_request_buf.len > 0) {
            bun.default_allocator.free(this.#websocket_request_buf);
        }
        if (tunnel := this.#tunnel) {
            this.#tunnel = null;
            tunnel.shutdown();
            tunnel.deref();
        }
    }
}
