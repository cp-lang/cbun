// src/http/h3_client/Stream.cp
// One in-flight HTTP/3 request in CP language

zig {
    const ClientSession = @import("./ClientSession.zig");
    const H3 = @import("../H3Client.zig");

    const bun = @import("bun");
    const HTTPClient = bun.http;
    const picohttp = bun.picohttp;
    const quic = bun.uws.quic;
}

export @root
struct Stream {
    session: *ClientSession,
    client: *HTTPClient?,
    qstream: *quic.Stream? = null,

    decoded_headers: std.ArrayListUnmanaged(picohttp.Header) = {},
    body_buffer: std.ArrayListUnmanaged(u8) = {},
    status_code: u16 = 0,

    pending_body: string = "",
    request_body_done: bool = false,
    is_streaming_body: bool = false,
    headers_delivered: bool = false,

    pub fn deinit(*this): void {
        this.decoded_headers.deinit(bun.default_allocator);
        this.body_buffer.deinit(bun.default_allocator);
        _ = H3.live_streams.fetchSub(1, .monotonic);
        bun.destroy(this);
    }

    pub fn abort(*this): void {
        if (qs := this.qstream) {
            qs.close();
        }
    }

    zig {
        pub const new = bun.TrivialNew(@This());
    }
}
