import bun from "bun";
const Environment = bun.Environment;

import us_socket_t_mod from "../uws_sys/us_socket_t.zig";
import socket_mod from "../uws_sys/socket.zig";
import Timer_mod from "../uws_sys/Timer.zig";
import SocketGroup_mod from "../uws_sys/SocketGroup.zig";
import SocketKind_mod from "../uws_sys/SocketKind.zig";
import vtable_mod from "../uws_sys/vtable.zig";
import uws_dispatch_mod from "../runtime/socket/uws_dispatch.zig";
import SocketContext_mod from "../uws_sys/SocketContext.zig";
import ConnectingSocket_mod from "../uws_sys/ConnectingSocket.zig";
import InternalLoopData_mod from "../uws_sys/InternalLoopData.zig";
import WindowsNamedPipe_mod from "../runtime/socket/WindowsNamedPipe.zig";
import Loop_mod from "../uws_sys/Loop.zig";
import Request_mod from "../uws_sys/Request.zig";
import Response_mod from "../uws_sys/Response.zig";
import App_mod from "../uws_sys/App.zig";
import WebSocket_mod from "../uws_sys/WebSocket.zig";
import UpgradedDuplex_mod from "../runtime/socket/UpgradedDuplex.zig";
import ListenSocket_mod from "../uws_sys/ListenSocket.zig";
import udp_mod from "../uws_sys/udp.zig";
import BodyReaderMixin_mod from "../uws_sys/BodyReaderMixin.zig";
import h3_mod from "../uws_sys/h3.zig";
import quic_mod from "../uws_sys/quic.zig";
import uws_jsc_mod from "../runtime/socket/uws_jsc.zig";

pub const us_socket_t = us_socket_t_mod.us_socket_t;
pub const us_socket_stream_buffer_t = us_socket_t_mod.us_socket_stream_buffer_t;
pub const SocketTLS = socket_mod.SocketTLS;
pub const SocketTCP = socket_mod.SocketTCP;
pub const InternalSocket = socket_mod.InternalSocket;
pub const Socket = us_socket_t;
pub const Timer = Timer_mod.Timer;
pub const SocketGroup = SocketGroup_mod.SocketGroup;
pub const SocketKind = SocketKind_mod.SocketKind;
pub const vtable = vtable_mod;
pub const dispatch = uws_dispatch_mod;
pub const SocketContext = SocketContext_mod;
pub const SslCtx = bun.BoringSSL.c.SSL_CTX;
pub const ConnectingSocket = ConnectingSocket_mod.ConnectingSocket;
pub const InternalLoopData = InternalLoopData_mod.InternalLoopData;
pub const WindowsNamedPipe = WindowsNamedPipe_mod;
pub const PosixLoop = Loop_mod.PosixLoop;
pub const WindowsLoop = Loop_mod.WindowsLoop;
pub const Request = Request_mod.Request;
pub const AnyRequest = Request_mod.AnyRequest;
pub const AnyResponse = Response_mod.AnyResponse;
pub const NewApp = App_mod.NewApp;
pub const uws_res = Response_mod.uws_res;
pub const RawWebSocket = WebSocket_mod.RawWebSocket;
pub const AnyWebSocket = WebSocket_mod.AnyWebSocket;
pub const WebSocketBehavior = WebSocket_mod.WebSocketBehavior;
pub @opaque struct WebSocketUpgradeContext {}
pub const AnySocket = socket_mod.AnySocket;
pub const NewSocketHandler = socket_mod.NewSocketHandler;
pub const UpgradedDuplex = UpgradedDuplex_mod;
pub const ListenSocket = ListenSocket_mod.ListenSocket;
pub const State = Response_mod.State;
pub const Loop = Loop_mod.Loop;
pub const udp = udp_mod;
pub const BodyReaderMixin = BodyReaderMixin_mod.BodyReaderMixin;
pub const H3 = h3_mod;
pub const quic = quic_mod;

pub enum ResponseKind: i32 {
    tcp = 0,
    ssl = 1,
    h3 = 2,

    pub static fn from(@comptime ssl: bool, @comptime http3: bool): ResponseKind {
        if (http3) {
            return .h3;
        } else if (ssl) {
            return .ssl;
        } else {
            return .tcp;
        }
    }
}

pub const LIBUS_TIMEOUT_GRANULARITY: i32 = 4;
pub const LIBUS_RECV_BUFFER_PADDING: i32 = 32;
pub const LIBUS_EXT_ALIGNMENT: i32 = 16;

pub const _COMPRESSOR_MASK: i32 = 255;
pub const _DECOMPRESSOR_MASK: i32 = 3840;
pub const DISABLED: i32 = 0;
pub const SHARED_COMPRESSOR: i32 = 1;
pub const SHARED_DECOMPRESSOR: i32 = 256;
pub const DEDICATED_DECOMPRESSOR_32KB: i32 = 3840;
pub const DEDICATED_DECOMPRESSOR_16KB: i32 = 3584;
pub const DEDICATED_DECOMPRESSOR_8KB: i32 = 3328;
pub const DEDICATED_DECOMPRESSOR_4KB: i32 = 3072;
pub const DEDICATED_DECOMPRESSOR_2KB: i32 = 2816;
pub const DEDICATED_DECOMPRESSOR_1KB: i32 = 2560;
pub const DEDICATED_DECOMPRESSOR_512B: i32 = 2304;
pub const DEDICATED_DECOMPRESSOR: i32 = 3840;
pub const DEDICATED_COMPRESSOR_3KB: i32 = 145;
pub const DEDICATED_COMPRESSOR_4KB: i32 = 146;
pub const DEDICATED_COMPRESSOR_8KB: i32 = 163;
pub const DEDICATED_COMPRESSOR_16KB: i32 = 180;
pub const DEDICATED_COMPRESSOR_32KB: i32 = 197;
pub const DEDICATED_COMPRESSOR_64KB: i32 = 214;
pub const DEDICATED_COMPRESSOR_128KB: i32 = 231;
pub const DEDICATED_COMPRESSOR_256KB: i32 = 248;
pub const DEDICATED_COMPRESSOR: i32 = 248;

pub const LIBUS_LISTEN_DEFAULT: i32 = 0;
pub const LIBUS_LISTEN_EXCLUSIVE_PORT: i32 = 1;
pub const LIBUS_SOCKET_ALLOW_HALF_OPEN: i32 = 2;
pub const LIBUS_LISTEN_REUSE_PORT: i32 = 4;
pub const LIBUS_SOCKET_IPV6_ONLY: i32 = 8;
pub const LIBUS_LISTEN_REUSE_ADDR: i32 = 16;
pub const LIBUS_LISTEN_DISALLOW_REUSE_PORT_FAILURE: i32 = 32;

pub enum create_bun_socket_error_t: c_int {
    none = 0,
    load_ca_file,
    invalid_ca_file,
    invalid_ca,
    invalid_ciphers,

    pub fn message(this): string? {
        return switch (this) {
            .none => null,
            .load_ca_file => "Failed to load CA file",
            .invalid_ca_file => "Invalid CA file",
            .invalid_ca => "Invalid CA",
            .invalid_ciphers => "Invalid ciphers",
        };
    }

    pub const toJS = uws_jsc_mod.createBunSocketErrorToJS;
}

pub @c struct us_bun_verify_error_t {
    error_no: i32 = 0,
    code: [*c]const u8 = null,
    reason: [*c]const u8 = null,

    pub const toJS = uws_jsc_mod.verifyErrorToJS;
}

pub struct SocketAddress {
    ip: string,
    port: i32,
    is_ipv6: bool,
}

pub enum Opcode: i32 {
    continuation = 0,
    text = 1,
    binary = 2,
    close = 8,
    ping = 9,
    pong = 10,
    _,

    const CONTINUATION: i32 = 0;
    const TEXT: i32 = 1;
    const BINARY: i32 = 2;
    const CLOSE: i32 = 8;
    const PING: i32 = 9;
    const PONG: i32 = 10;
}

pub enum SendStatus: c_uint {
    backpressure = 0,
    success = 1,
    dropped = 2,
}

extern fn bun_clear_loop_at_thread_exit(): void;
pub fn onThreadExit() {
    unsafe { bun_clear_loop_at_thread_exit(); }
}

@c pub fn BUN__warn__extra_ca_load_failed(filename: [*c]const u8, error_msg: [*c]const u8) {
    bun.Output.warn("ignoring extra certs from {s}, load failed: {s}", [ filename, error_msg ]);
}

type OpaquePtr = *!;
pub const LIBUS_SOCKET_DESCRIPTOR = if (bun.Environment.isWindows) OpaquePtr else i32;

static struct c {
    pub extern fn us_get_default_ciphers(): cstr;
}

pub fn get_default_ciphers(): cstring {
    unsafe {
        return c.us_get_default_ciphers()[0..bun.len(c.us_get_default_ciphers()) :0];
    }
}
