import { Loop } from "../Loop.cp";
import { PendingConnect } from "./PendingConnect.cp";
import { Socket } from "./Socket.cp";
import { Stream } from "./Stream.cp";

zig {
    const c = struct {
        extern fn us_create_quic_client_context(loop: *Loop, ext_size: c_uint, conn_ext: c_uint, stream_ext: c_uint) ?*Context;
        extern fn us_quic_socket_context_loop(ctx: *Context) *Loop;
        extern fn us_quic_socket_context_connect(ctx: *Context, host: [*:0]const u8, port: c_int, sni: [*:0]const u8, reject_unauthorized: c_int, out_qs: *?*Socket, out_pending: *?*PendingConnect, user: *anyopaque) c_int;
        extern fn us_quic_socket_context_on_hsk_done(ctx: *Context, cb: *const fn (*Socket, c_int) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_goaway(ctx: *Context, cb: *const fn (*Socket) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_close(ctx: *Context, cb: *const fn (*Socket) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_stream_open(ctx: *Context, cb: *const fn (*Stream, c_int) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_stream_headers(ctx: *Context, cb: *const fn (*Stream) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_stream_data(ctx: *Context, cb: *const fn (*Stream, [*]const u8, c_uint, c_int) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_stream_writable(ctx: *Context, cb: *const fn (*Stream) callconv(.c) void) void;
        extern fn us_quic_socket_context_on_stream_close(ctx: *Context, cb: *const fn (*Stream) callconv(.c) void) void;
    };
}

export enum ConnectResult {
    socket(*Socket),
    pending(*PendingConnect),
    err,
}

export @opaque class Context {
    zig {
        pub const createClient = c.us_create_quic_client_context;
        pub const loop = c.us_quic_socket_context_loop;

        pub const onHskDone = c.us_quic_socket_context_on_hsk_done;
        pub const onGoaway = c.us_quic_socket_context_on_goaway;
        pub const onClose = c.us_quic_socket_context_on_close;
        pub const onStreamOpen = c.us_quic_socket_context_on_stream_open;
        pub const onStreamHeaders = c.us_quic_socket_context_on_stream_headers;
        pub const onStreamData = c.us_quic_socket_context_on_stream_data;
        pub const onStreamWritable = c.us_quic_socket_context_on_stream_writable;
        pub const onStreamClose = c.us_quic_socket_context_on_stream_close;
    }

    pub fn connect(*this, host: anytype, port: u16, sni: anytype, reject_unauthorized: bool, user: *anyopaque): ConnectResult {
        zig {
            var qs: ?*Socket = null;
            var pc: ?*PendingConnect = null;
            return switch (c.us_quic_socket_context_connect(self, host, port, sni, @intFromBool(reject_unauthorized), &qs, &pc, user)) {
                1 => .{ .socket = qs.? },
                0 => .{ .pending = pc.? },
                else => .err,
            };
        }
    }
}
