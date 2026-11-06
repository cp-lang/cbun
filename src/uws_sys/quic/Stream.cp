import { Header } from "./Header.cp";
import { Socket } from "./Socket.cp";

@c {
    fn us_quic_stream_socket(s: *Stream): *Socket?!;
    fn us_quic_stream_shutdown(s: *Stream): void;
    fn us_quic_stream_close(s: *Stream): void;
    fn us_quic_stream_reset(s: *Stream): void;
    fn us_quic_stream_header_count(s: *Stream): u32;
    fn us_quic_stream_header(s: *Stream, i: u32): *const Header?!;
    fn us_quic_stream_ext(s: *Stream): *any!;
    fn us_quic_stream_write(s: *Stream, data: [*]u8!, len: u32): i32;
    fn us_quic_stream_want_write(s: *Stream, want: i32): void;
    fn us_quic_stream_send_headers(s: *Stream, h: [*]const Header!, n: u32, end_stream: i32): i32;
}

export @opaque class Stream {
    pub fn socket(*this): *Socket?! {
        return us_quic_stream_socket(this);
    }

    pub fn shutdown(*this): void {
        us_quic_stream_shutdown(this);
    }

    pub fn close(*this): void {
        us_quic_stream_close(this);
    }

    pub fn reset(*this): void {
        us_quic_stream_reset(this);
    }

    pub fn headerCount(*this): u32 {
        return us_quic_stream_header_count(this);
    }

    pub fn header(*this, i: u32): *const Header?! {
        return us_quic_stream_header(this, i);
    }

    pub fn ext<T>(*this): *T?! {
        return us_quic_stream_ext(this) as *T?!;
    }

    pub fn write(*this, data: string): i32 {
        return us_quic_stream_write(this, data.ptr, data.len as u32);
    }

    pub fn wantWrite(*this, want: bool): void {
        us_quic_stream_want_write(this, if (want) 1 else 0);
    }

    pub fn sendHeaders(*this, headers: Header[], end_stream: bool): i32 {
        return us_quic_stream_send_headers(this, headers.ptr, headers.len as u32, if (end_stream) 1 else 0);
    }
}
