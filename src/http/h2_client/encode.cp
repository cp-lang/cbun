// src/http/h2_client/encode.cp
// Outbound request encoding for the fetch() HTTP/2 client in CP language

import { ClientSession } from "./ClientSession.cp";
import { Stream } from "./Stream.cp";
import * as H2FrameParser from "../H2FrameParser.cp";

zig {
const bun = @import("bun");
const HPACK = @import("../lshpack.zig").HPACK;
const RequestHeader = enum {
    drop,
    host,
    te,
    content_length,
    expect,
    sensitive,

    const map = bun.ComptimeStringMap(RequestHeader, .{
        .{ "connection", .drop },
        .{ "keep-alive", .drop },
        .{ "proxy-connection", .drop },
        .{ "transfer-encoding", .drop },
        .{ "upgrade", .drop },
        .{ "host", .host },
        .{ "te", .te },
        .{ "content-length", .content_length },
        .{ "expect", .expect },
        .{ "authorization", .sensitive },
        .{ "cookie", .sensitive },
        .{ "set-cookie", .sensitive },
    });
};
}

export fn writePreface(session: *ClientSession): void {
    session.queue(H2FrameParser.client_preface);

    let mut settings: [u8; 18] = [0; 18];
    H2FrameParser.SettingsPayloadUnit.encode(settings[0..6], .SETTINGS_ENABLE_PUSH, 0);
    H2FrameParser.SettingsPayloadUnit.encode(settings[6..12], .SETTINGS_INITIAL_WINDOW_SIZE, bun.http.H2.local_initial_window_size);
    H2FrameParser.SettingsPayloadUnit.encode(settings[12..18], .SETTINGS_MAX_HEADER_LIST_SIZE, bun.http.H2.local_max_header_list_size);
    session.writeFrame(.HTTP_FRAME_SETTINGS, 0, 0, &settings);

    session.writeWindowUpdate(0, bun.http.H2.local_initial_window_size - H2FrameParser.DEFAULT_WINDOW_SIZE);
    session.preface_sent = true;
}

export fn writeRequest(session: *ClientSession, client: *bun.http.HTTPClient, stream: *Stream, request: bun.picohttp.Request): !void {
    session.encode_scratch.clearRetainingCapacity();

    if (cap := session.pending_hpack_enc_capacity) {
        session.pending_hpack_enc_capacity = null;
        session.hpack.setEncoderMaxCapacity(cap);
        session.encode_scratch.ensureUnusedCapacity(bun.default_allocator, 8)?;
        encodeHpackTableSizeUpdate(&session.encode_scratch, cap);
    }

    let authority: string = client.url.host;
    let has_expect_continue = false;
    for (h of request.headers) {
        if (kind := RequestHeader.map.getAnyCase(h.name)) {
            switch (kind) {
                .host => authority = h.value,
                .expect => has_expect_continue = bun.strings.eqlCaseInsensitiveASCIIICheckLength(h.value, "100-continue"),
                * => {},
            }
        }
    }

    let path_str: string = "/";
    if (request.path.len > 0) {
        path_str = request.path;
    }

    encodeHeader(session.hpack, &session.encode_scratch, ":method", request.method, false)?;
    encodeHeader(session.hpack, &session.encode_scratch, ":scheme", "https", false)?;
    encodeHeader(session.hpack, &session.encode_scratch, ":authority", authority, false)?;
    encodeHeader(session.hpack, &session.encode_scratch, ":path", path_str, false)?;

    let mut lower_buf: [u8; 256] = [0; 256];
    for (h of request.headers) {
        let mut heap: u8[]? = null;
        defer if (buf := heap) bun.default_allocator.free(buf);
        let name: string = "";
        if (h.name.len <= lower_buf.len) {
            name = bun.strings.copyLowercaseIfNeeded(h.name, &lower_buf);
        } else {
            const hbuf = bun.handleOom(bun.default_allocator.alloc(u8, h.name.len));
            name = bun.strings.copyLowercaseIfNeeded(h.name, hbuf);
            heap = hbuf;
        }
        let never_index = false;
        if (kind := RequestHeader.map.get(name)) {
            let mut should_skip = false;
            zig {
                switch (kind) {
                    .drop => should_skip = true,
                    .host => should_skip = true,
                    .te => {
                        if (!bun.strings.eqlCaseInsensitiveASCIIICheckLength(bun.strings.trim(h.value, " \t"), "trailers")) {
                            should_skip = true;
                        }
                    },
                    .content_length => {
                        if (has_expect_continue) {
                            should_skip = true;
                        }
                    },
                    .sensitive => never_index = true,
                    .expect => {},
                }
            }
            if (should_skip) continue;
        }
        encodeHeader(session.hpack, &session.encode_scratch, name, h.value, never_index)?;
    }

    const body = client.state.request_body;
    const has_inline_body = client.state.original_request_body == .bytes && body.len > 0;
    const is_streaming = client.state.original_request_body == .stream;

    if (has_expect_continue && (has_inline_body || is_streaming)) stream.awaiting_continue = true;

    writeHeaderBlock(session, stream.id, session.encode_scratch.items, !has_inline_body && !is_streaming);
    if (session.encode_scratch.capacity > 64 * 1024) session.encode_scratch.clearAndFree(bun.default_allocator);
    if (has_inline_body) {
        stream.pending_body = body;
        drainSendBody(session, stream, std.math.maxInt(usize));
    } else if (!is_streaming) {
        stream.sentEndStream();
    }
}

export fn writeHeaderBlock(session: *ClientSession, stream_id: u31, block: string, end_stream: bool): void {
    const max: usize = session.remote_max_frame_size;
    let remaining = block;
    let first = true;
    while (true) {
        const chunk = remaining[0..@min(remaining.len, max)];
        remaining = remaining[chunk.len..];
        const last = remaining.len == 0;
        let flags: u8 = 0;
        if (last) flags |= @intFromEnum(H2FrameParser.HeadersFrameFlags.END_HEADERS);
        if (first && end_stream) flags |= @intFromEnum(H2FrameParser.HeadersFrameFlags.END_STREAM);
        let mut frame_kind: H2FrameParser.FrameType = .HTTP_FRAME_CONTINUATION;
        if (first) {
            frame_kind = .HTTP_FRAME_HEADERS;
        }
        session.writeFrame(frame_kind, flags, stream_id, chunk);
        first = false;
        if (last) break;
    }
}

export fn writeDataWindowed(session: *ClientSession, stream: *Stream, data: string, end_stream: bool, cap: usize): usize {
    let remaining = data;
    let consumed: usize = 0;
    while (true) {
        const window: usize = @intCast(@max(0, @min(stream.send_window, session.conn_send_window)));
        if (remaining.len > 0 && window == 0) break;
        if (remaining.len > 0 && session.write_buffer.size() >= bun.http.H2.write_buffer_high_water) break;
        if (consumed >= cap && remaining.len > 0) break;
        const chunk_len = @min(remaining.len, @as(usize, session.remote_max_frame_size), window);
        const last = chunk_len == remaining.len;
        let flags: u8 = 0;
        if (last && end_stream) {
            flags = @intFromEnum(H2FrameParser.DataFrameFlags.END_STREAM);
        }
        session.writeFrame(.HTTP_FRAME_DATA, flags, stream.id, remaining[0..chunk_len]);
        stream.send_window -= @as(i32, @intCast(chunk_len));
        session.conn_send_window -= @as(i32, @intCast(chunk_len));
        consumed += chunk_len;
        remaining = remaining[chunk_len..];
        if (last) break;
    }
    return consumed;
}

export fn drainSendBody(session: *ClientSession, stream: *Stream, cap: usize): void {
    if (stream.localClosed() || stream.awaiting_continue || stream.fatal_error != null) return;
    if (client := stream.client) {
        zig {
            switch (client.state.original_request_body) {
                .bytes => {
                    const sent = writeDataWindowed(session, stream, stream.pending_body, true, cap);
                    stream.pending_body = stream.pending_body[sent..];
                    if (stream.pending_body.len == 0) {
                        stream.sentEndStream();
                        client.state.request_stage = .done;
                    }
                },
                .stream => |*body| {
                    if (body.buffer) |sb| {
                        const buffer = sb.acquire();
                        const data = buffer.slice();
                        if (data.len == 0 and !body.ended) {
                            sb.release();
                            return;
                        }
                        const sent = writeDataWindowed(session, stream, data, body.ended, cap);
                        buffer.cursor += sent;
                        const drained = buffer.isEmpty();
                        if (drained) buffer.reset();
                        if (drained and body.ended) {
                            stream.sentEndStream();
                            client.state.request_stage = .done;
                        } else if (drained and data.len > 0) {
                            sb.reportDrain();
                        }
                        sb.release();
                        if (stream.localClosed()) body.detach();
                    } else {
                        return;
                    }
                },
                .sendfile => {
                    @panic("unreachable");
                },
            }
        }
    }
}

export fn drainSendBodies(session: *ClientSession): void {
    const slice: usize = session.remote_max_frame_size;
    while (session.conn_send_window > 0 && session.write_buffer.size() < bun.http.H2.write_buffer_high_water) {
        let progressed = false;
        for (stream of session.streams.values()) {
            if (stream.localClosed() || stream.send_window <= 0) continue;
            const before = session.conn_send_window;
            drainSendBody(session, stream, slice);
            if (session.conn_send_window != before || stream.localClosed()) progressed = true;
        }
        if (!progressed) break;
    }
}

export fn encodeHeader(hpack: *HPACK, encoded: *std.ArrayListUnmanaged(u8), name: string, value: string, never_index: bool): !void {
    const required = encoded.items.len + name.len + value.len + 32;
    encoded.ensureTotalCapacity(bun.default_allocator, required)?;
    const written = hpack.encode(name, value, never_index, encoded.allocatedSlice(), encoded.items.len)?;
    encoded.items.len += written;
}

export fn encodeHpackTableSizeUpdate(encoded: *std.ArrayListUnmanaged(u8), value: u32): void {
    if (value < 31) {
        encoded.appendAssumeCapacity(0x20 | @as(u8, @intCast(value)));
        return;
    }
    encoded.appendAssumeCapacity(0x20 | 31);
    let rest = value - 31;
    while (rest >= 128) {
        encoded.appendAssumeCapacity(@as(u8, @truncate(rest)) | 0x80);
        rest >>= 7;
    }
    encoded.appendAssumeCapacity(@as(u8, @truncate(rest)));
}
