// src/http/h2_client/Stream.cp
// One in-flight request on a multiplexed HTTP/2 ClientSession in CP language

zig {
    const bun = @import("bun");
}

import { ClientSession } from "./ClientSession.cp";
import * as H2FrameParser from "../H2FrameParser.cp";

export enum State: u2 {
    open,
    half_closed_local,
    half_closed_remote,
    closed,
}

export @root struct Stream {
    zig {
        pub const new = bun.TrivialNew(Stream);
    }

    id: u31,
    session: *ClientSession,
    client: *bun.http.HTTPClient? = null,

    header_block: std.ArrayListUnmanaged(u8),
    body_buffer: std.ArrayListUnmanaged(u8),
    decoded_bytes: std.ArrayListUnmanaged(u8),
    decoded_headers: std.ArrayListUnmanaged(bun.picohttp.Header),
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

    fn release(*this): void {
        bun.http.H2.live_streams.fetchSub(1, .monotonic);
        this.header_block.deinit(bun.default_allocator);
        this.body_buffer.deinit(bun.default_allocator);
        this.decoded_bytes.deinit(bun.default_allocator);
        this.decoded_headers.deinit(bun.default_allocator);
        bun.destroy(this);
    }

    fn deinit(*this): void {
        this.release();
    }

    fn rst(*this, code: H2FrameParser.ErrorCode): void {
        if (this.rst_done || this.state == .closed) return;
        this.rst_done = true;
        this.state = .closed;
        const value: u32 = @byteSwap(@intFromEnum(code));
        this.session.writeFrame(.HTTP_FRAME_RST_STREAM, 0, this.id, std.mem.asBytes(&value));
    }

    fn sentEndStream(*this): void {
        this.state = switch (this.state) {
            .open => .half_closed_local,
            .half_closed_remote => .closed,
            * => this.state,
        };
    }

    fn recvEndStream(*this): void {
        this.state = switch (this.state) {
            .open => .half_closed_remote,
            .half_closed_local => .closed,
            * => this.state,
        };
    }

    @inline
    fn localClosed(this): bool {
        return this.state == .half_closed_local || this.state == .closed;
    }

    @inline
    fn remoteClosed(this): bool {
        return this.state == .half_closed_remote || this.state == .closed;
    }
}
