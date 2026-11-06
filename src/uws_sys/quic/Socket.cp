zig {
    const c = struct {
        extern fn us_quic_socket_make_stream(s: *Socket) void;
        extern fn us_quic_socket_streams_avail(s: *Socket) c_uint;
        extern fn us_quic_socket_status(s: *Socket, buf: [*]u8, len: c_uint) c_int;
        extern fn us_quic_socket_close(s: *Socket) void;
        extern fn us_quic_socket_ext(s: *Socket) *anyopaque;
    };
}

export @opaque class Socket {
    zig {
        pub const makeStream = c.us_quic_socket_make_stream;
        pub const streamsAvail = c.us_quic_socket_streams_avail;
        pub const close = c.us_quic_socket_close;
    }

    pub fn status(*this, buf: &mut u8[]): i32 {
        zig {
            return c.us_quic_socket_status(self, buf.ptr, @intCast(buf.len));
        }
    }

    zig {
        pub fn ext(self: *Socket, comptime T: type) *?*T {
            const PtrT: type = *?*T;
            return @as(PtrT, @ptrCast(@alignCast(c.us_quic_socket_ext(self))));
        }
    }
}
