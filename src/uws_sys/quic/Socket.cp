@c {
    fn us_quic_socket_make_stream(s: *Socket): void;
    fn us_quic_socket_streams_avail(s: *Socket): u32;
    fn us_quic_socket_status(s: *Socket, buf: [*]u8!, len: u32): i32;
    fn us_quic_socket_close(s: *Socket): void;
    fn us_quic_socket_ext(s: *Socket): *any!;
}

export @opaque class Socket {
    pub fn makeStream(*this): void {
        us_quic_socket_make_stream(this);
    }

    pub fn streamsAvail(*this): u32 {
        return us_quic_socket_streams_avail(this);
    }

    pub fn close(*this): void {
        us_quic_socket_close(this);
    }

    pub fn status(*this, buf: *u8[]): i32 {
        return us_quic_socket_status(this, buf.ptr, buf.len as u32);
    }

    pub fn ext<T>(*this): **T?! {
        return us_quic_socket_ext(this) as **T?!;
    }
}
