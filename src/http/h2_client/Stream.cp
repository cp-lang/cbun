// src/http/h2_client/Stream.cp
// One in-flight request on a multiplexed HTTP/2 ClientSession in CP language

zig {
    const ClientSession = @import("./ClientSession.zig");
    const H2 = @import("../H2Client.zig");
    const wire = @import("../H2FrameParser.zig");

    const bun = @import("bun");
    const HTTPClient = bun.http;
    const picohttp = bun.picohttp;

    const ByteList = std.ArrayListUnmanaged(u8);
    const HeaderList = std.ArrayListUnmanaged(picohttp.Header);
}

export @root struct Stream {
    id: u31,
    session: *ClientSession,
    client: *HTTPClient?,

    header_block: ByteList,
    body_buffer: ByteList,
    decoded_bytes: ByteList,
    decoded_headers: HeaderList,
    status_code: u32 = 0,

    state: State,
    rst_done: bool = false,
    headers_ready: bool = false,
    headers_end_stream: bool = false,
    awaiting_continue: bool = false,
    fatal_error: anyerror? = null,
    unacked_bytes: u32 = 0,
    data_bytes_received: u64 = 0,
    send_window: i32,
    pending_body: string = "",

    zig {
        pub const new = bun.TrivialNew(@This());

        pub const State = enum(u2) {
            open,
            half_closed_local,
            half_closed_remote,
            closed,
        };
    }

    pub fn deinit(*this): void {
        zig {
            _ = H2.live_streams.fetchSub(1, .monotonic);
            self.header_block.deinit(bun.default_allocator);
            self.body_buffer.deinit(bun.default_allocator);
            self.decoded_bytes.deinit(bun.default_allocator);
            self.decoded_headers.deinit(bun.default_allocator);
            bun.destroy(self);
        }
    }

    pub fn rst(*this, code: wire.ErrorCode): void {
        if (this.rst_done || this.state == .closed) return;
        this.rst_done = true;
        this.state = .closed;
        zig {
            var value: u32 = @byteSwap(@intFromEnum(code));
            self.session.writeFrame(.HTTP_FRAME_RST_STREAM, 0, self.id, std.mem.asBytes(&value));
        }
    }

    pub fn sentEndStream(*this): void {
        zig {
            self.state = switch (self.state) {
                .open => .half_closed_local,
                .half_closed_remote => .closed,
                else => self.state,
            };
        }
    }

    pub fn recvEndStream(*this): void {
        zig {
            self.state = switch (self.state) {
                .open => .half_closed_remote,
                .half_closed_local => .closed,
                else => self.state,
            };
        }
    }

    @inline
    pub fn localClosed(this): bool {
        return this.state == .half_closed_local || this.state == .closed;
    }

    @inline
    pub fn remoteClosed(this): bool {
        return this.state == .half_closed_remote || this.state == .closed;
    }
}
