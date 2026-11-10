const std = @import("std");

const c = struct {
    extern fn us_quic_socket_make_stream(s: *Socket) void;
    extern fn us_quic_socket_streams_avail(s: *Socket) c_uint;
    extern fn us_quic_socket_status(s: *Socket, buf: [*]u8, len: c_uint) c_int;
    extern fn us_quic_socket_close(s: *Socket) void;
    extern fn us_quic_socket_ext(s: *Socket) *anyopaque;
};

pub const Socket = opaque {
    pub fn status(self: *Socket, buf: []u8) i32 {
        return c.us_quic_socket_status(self, buf.ptr, @intCast(buf.len));
    }
    pub const makeStream = c.us_quic_socket_make_stream;
    pub const streamsAvail = c.us_quic_socket_streams_avail;
    pub const close = c.us_quic_socket_close;
    pub fn ext(self: *Socket, comptime T: type) *?*T {
        return @ptrCast(c.us_quic_socket_ext(self));
    }
};
