import { Loop } from "../Loop.cp";
import { PendingConnect } from "./PendingConnect.cp";
import { Socket } from "./Socket.cp";
import { Stream } from "./Stream.cp";

@c {
    fn us_create_quic_client_context(loop: *Loop, ext_size: u32, conn_ext: u32, stream_ext: u32): *Context?;
    fn us_quic_socket_context_loop(ctx: *Context): *Loop;
    fn us_quic_socket_context_connect(ctx: *Context, host: cstr, port: i32, sni: cstr, reject_unauthorized: i32, out_qs: **Socket?!, out_pending: **PendingConnect?!, user: *any!): i32;
    fn us_quic_socket_context_on_hsk_done(ctx: *Context, cb: @c fn(*Socket, i32) void): void;
    fn us_quic_socket_context_on_goaway(ctx: *Context, cb: @c fn(*Socket) void): void;
    fn us_quic_socket_context_on_close(ctx: *Context, cb: @c fn(*Socket) void): void;
    fn us_quic_socket_context_on_stream_open(ctx: *Context, cb: @c fn(*Stream, i32) void): void;
    fn us_quic_socket_context_on_stream_headers(ctx: *Context, cb: @c fn(*Stream) void): void;
    fn us_quic_socket_context_on_stream_data(ctx: *Context, cb: @c fn(*Stream, [*]u8!, u32, i32) void): void;
    fn us_quic_socket_context_on_stream_writable(ctx: *Context, cb: @c fn(*Stream) void): void;
    fn us_quic_socket_context_on_stream_close(ctx: *Context, cb: @c fn(*Stream) void): void;
}

pub union(enum) QuicConnectResult {
    socket: *Socket,
    pending: *PendingConnect,
    err: void,
}

export @opaque class Context {
    pub const ConnectResult = QuicConnectResult;

    pub static fn createClient(event_loop: *Loop, ext_size: u32, conn_ext: u32, stream_ext: u32): *Context? {
        return us_create_quic_client_context(event_loop, ext_size, conn_ext, stream_ext);
    }

    pub fn loop(*this): *Loop {
        return us_quic_socket_context_loop(this);
    }

    pub fn onHskDone(*this, cb: @c fn(*Socket, i32) void): void {
        us_quic_socket_context_on_hsk_done(this, cb);
    }

    pub fn onGoaway(*this, cb: @c fn(*Socket) void): void {
        us_quic_socket_context_on_goaway(this, cb);
    }

    pub fn onClose(*this, cb: @c fn(*Socket) void): void {
        us_quic_socket_context_on_close(this, cb);
    }

    pub fn onStreamOpen(*this, cb: @c fn(*Stream, i32) void): void {
        us_quic_socket_context_on_stream_open(this, cb);
    }

    pub fn onStreamHeaders(*this, cb: @c fn(*Stream) void): void {
        us_quic_socket_context_on_stream_headers(this, cb);
    }

    pub fn onStreamData(*this, cb: @c fn(*Stream, [*]u8!, u32, i32) void): void {
        us_quic_socket_context_on_stream_data(this, cb);
    }

    pub fn onStreamWritable(*this, cb: @c fn(*Stream) void): void {
        us_quic_socket_context_on_stream_writable(this, cb);
    }

    pub fn onStreamClose(*this, cb: @c fn(*Stream) void): void {
        us_quic_socket_context_on_stream_close(this, cb);
    }

    pub fn connect(*this, host: cstr, port: u16, sni: cstr, reject_unauthorized: bool, user: *any!): QuicConnectResult {
        let qs: ?*Socket = null;
        let pc: ?*PendingConnect = null;
        let reject_int = if (reject_unauthorized) 1 else 0;
        let res = us_quic_socket_context_connect(this, host, port as i32, sni, reject_int, &qs, &pc, user);
        return switch (res) {
            1 => .{ .socket = qs.? },
            0 => .{ .pending = pc.? },
            else => .err,
        };
    }
}
