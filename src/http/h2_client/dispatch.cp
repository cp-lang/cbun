zig {
    const log = bun.Output.scoped(.h2_client, .hidden);
    const ClientSession = @import("./ClientSession.zig");
    const Stream = @import("./Stream.zig");
    const wire = @import("../H2FrameParser.zig");

    const H2 = @import("../H2Client.zig");
    const local_max_header_list_size = H2.local_max_header_list_size;
    const write_buffer_control_limit = H2.write_buffer_control_limit;

    const bun = @import("bun");
    const strings = bun.strings;

    const forbidden_response_fields = bun.ComptimeStringMap(void, .{
        .{ "connection", {} },
        .{ "keep-alive", {} },
        .{ "proxy-connection", {} },
        .{ "te", {} },
        .{ "transfer-encoding", {} },
        .{ "upgrade", {} },
    });
}

pub fn parseFrames(session: *ClientSession, buf: string): usize {
    let mut consumed: usize = 0;
    while (true) {
        const remaining = buf[consumed..];
        if (remaining.len < wire.FrameHeader.byteSize) {
            break;
        }
        let mut header: wire.FrameHeader = { flags: 0 };
        wire.FrameHeader.from(&mut header, remaining[0..wire.FrameHeader.byteSize], 0, true);
        header.streamIdentifier = wire.UInt31WithReserved.from(header.streamIdentifier).uint31;
        if (header.length > wire.DEFAULT_MAX_FRAME_SIZE) {
            session.fatal_error = error.HTTP2FrameSizeError;
            break;
        }
        const frame_len = wire.FrameHeader.byteSize + @as(usize, header.length);
        if (remaining.len < frame_len) {
            break;
        }
        dispatchFrame(session, header, remaining[wire.FrameHeader.byteSize..frame_len]);
        consumed += frame_len;
        if (session.fatal_error != null) {
            break;
        }
    }
    return consumed;
}

pub fn dispatchFrame(session: *ClientSession, header: wire.FrameHeader, payload: string): void {
    log("frame type={d} len={d} flags={d} stream={d}", [header.type, header.length, header.flags, header.streamIdentifier]);

    if (session.expecting_continuation != 0 && header.type != @intFromEnum(wire.FrameType.HTTP_FRAME_CONTINUATION)) {
        session.fatal_error = error.HTTP2ProtocolError;
        return;
    }
    if (!session.settings_received && header.type != @intFromEnum(wire.FrameType.HTTP_FRAME_SETTINGS)) {
        session.fatal_error = error.HTTP2ProtocolError;
        return;
    }

    switch (@as(wire.FrameType, @enumFromInt(header.type))) {
        .HTTP_FRAME_SETTINGS => {
            if (header.streamIdentifier != 0) {
                session.fatal_error = error.HTTP2ProtocolError;
                return;
            }
            if ((header.flags & @intFromEnum(wire.SettingsFlags.ACK)) != 0) {
                if (header.length != 0) {
                    session.fatal_error = error.HTTP2FrameSizeError;
                }
                return;
            }
            if ((header.length % wire.SettingsPayloadUnit.byteSize) != 0) {
                session.fatal_error = error.HTTP2FrameSizeError;
                return;
            }
            let mut i: usize = 0;
            while (i + wire.SettingsPayloadUnit.byteSize <= payload.len) {
                let mut unit: wire.SettingsPayloadUnit = undefined;
                wire.SettingsPayloadUnit.from(&mut unit, payload[i..(i + wire.SettingsPayloadUnit.byteSize)], 0, true);
                switch (@as(wire.SettingsType, @enumFromInt(unit.type))) {
                    .SETTINGS_MAX_FRAME_SIZE => {
                        if (unit.value < wire.DEFAULT_MAX_FRAME_SIZE || unit.value > wire.MAX_FRAME_SIZE) {
                            session.fatal_error = error.HTTP2ProtocolError;
                            return;
                        }
                        session.remote_max_frame_size = @truncate(unit.value);
                    },
                    .SETTINGS_MAX_CONCURRENT_STREAMS => {
                        session.remote_max_concurrent_streams = unit.value;
                    },
                    .SETTINGS_HEADER_TABLE_SIZE => {
                        session.pending_hpack_enc_capacity = @min(session.pending_hpack_enc_capacity ?? unit.value, unit.value);
                    },
                    .SETTINGS_INITIAL_WINDOW_SIZE => {
                        if (unit.value > wire.MAX_WINDOW_SIZE) {
                            session.fatal_error = error.HTTP2FlowControlError;
                            return;
                        }
                        const delta = @as(i64, unit.value) - @as(i64, session.remote_initial_window_size);
                        session.remote_initial_window_size = unit.value;
                        for (s of session.streams.values()) {
                            const next = @as(i64, s.send_window) + delta;
                            if (next > wire.MAX_WINDOW_SIZE) {
                                session.fatal_error = error.HTTP2FlowControlError;
                                return;
                            }
                            s.send_window = @intCast(next);
                        }
                    },
                    else => {}
                }
                i += wire.SettingsPayloadUnit.byteSize;
            }
            if (session.write_buffer.size() >= write_buffer_control_limit) {
                session.fatal_error = error.HTTP2EnhanceYourCalm;
                return;
            }
            session.writeFrame(.HTTP_FRAME_SETTINGS, @intFromEnum(wire.SettingsFlags.ACK), 0, "");
            session.settings_received = true;
        },
        .HTTP_FRAME_WINDOW_UPDATE => {
            if (header.length != 4) {
                session.fatal_error = error.HTTP2FrameSizeError;
                return;
            }
            const inc: i32 = @intCast(wire.UInt31WithReserved.fromBytes(payload[0..4]).uint31);
            if (header.streamIdentifier == 0) {
                if (inc == 0) {
                    session.fatal_error = error.HTTP2ProtocolError;
                    return;
                }
                const next = @as(i64, session.conn_send_window) + inc;
                if (next > wire.MAX_WINDOW_SIZE) {
                    session.fatal_error = error.HTTP2FlowControlError;
                    return;
                }
                session.conn_send_window = @intCast(next);
                session.stream_progressed = true;
            } else if (stream := session.streams.get(@truncate(header.streamIdentifier & 0x7fffffff))) {
                if (inc == 0) {
                    stream.rst(.PROTOCOL_ERROR);
                    stream.fatal_error = error.HTTP2ProtocolError;
                    return;
                }
                const next = @as(i64, stream.send_window) + inc;
                if (next > wire.MAX_WINDOW_SIZE) {
                    stream.rst(.FLOW_CONTROL_ERROR);
                    stream.fatal_error = error.HTTP2FlowControlError;
                    return;
                }
                stream.send_window = @intCast(next);
                session.stream_progressed = true;
            } else {
                const sid: u31 = @intCast(header.streamIdentifier);
                if ((sid & 1) == 0 || sid >= session.next_stream_id) {
                    session.fatal_error = error.HTTP2ProtocolError;
                    return;
                }
            }
        },
        .HTTP_FRAME_PING => {
            if (header.length != 8) {
                session.fatal_error = error.HTTP2FrameSizeError;
                return;
            }
            if (header.streamIdentifier != 0) {
                session.fatal_error = error.HTTP2ProtocolError;
                return;
            }
            if ((header.flags & @intFromEnum(wire.PingFrameFlags.ACK)) == 0) {
                if (session.write_buffer.size() >= write_buffer_control_limit) {
                    session.fatal_error = error.HTTP2EnhanceYourCalm;
                    return;
                }
                session.writeFrame(.HTTP_FRAME_PING, @intFromEnum(wire.PingFrameFlags.ACK), 0, payload[0..8]);
            }
        },
        .HTTP_FRAME_PRIORITY => {
            if (header.streamIdentifier == 0) {
                session.fatal_error = error.HTTP2ProtocolError;
                return;
            }
            if (header.length != wire.StreamPriority.byteSize) {
                session.fatal_error = error.HTTP2FrameSizeError;
                return;
            }
        },
        .HTTP_FRAME_HEADERS => {
            let mut fragment = payload;
            const stream_id: u31 = @intCast(header.streamIdentifier);
            if (stream := session.streams.get(stream_id)) {
                session.stream_progressed = true;
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.PADDED)) != 0) {
                    if (stripped := stripPadding(fragment)) {
                        fragment = stripped;
                    } else {
                        session.fatal_error = error.HTTP2ProtocolError;
                        return;
                    }
                }
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.PRIORITY)) != 0) {
                    if (fragment.len < wire.StreamPriority.byteSize) {
                        session.fatal_error = error.HTTP2ProtocolError;
                        return;
                    }
                    fragment = fragment[wire.StreamPriority.byteSize..];
                }
                if (fragment.len > local_max_header_list_size) {
                    session.fatal_error = error.HTTP2HeaderListTooLarge;
                    return;
                }
                stream.header_block.clearRetainingCapacity();
                bun.handleOom(stream.header_block.appendSlice(bun.default_allocator, fragment));
                stream.headers_end_stream = (header.flags & @intFromEnum(wire.HeadersFrameFlags.END_STREAM)) != 0;
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.END_HEADERS)) != 0) {
                    if (stream.headers_end_stream) {
                        stream.recvEndStream();
                    }
                    decodeHeaderBlock(session, stream);
                } else {
                    session.expecting_continuation = stream.id;
                }
            } else {
                if (stream_id == 0 || (stream_id & 1) == 0 || stream_id >= session.next_stream_id) {
                    session.fatal_error = error.HTTP2ProtocolError;
                    return;
                }
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.PADDED)) != 0) {
                    if (stripped := stripPadding(fragment)) {
                        fragment = stripped;
                    } else {
                        session.fatal_error = error.HTTP2ProtocolError;
                        return;
                    }
                }
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.PRIORITY)) != 0) {
                    if (fragment.len < wire.StreamPriority.byteSize) {
                        session.fatal_error = error.HTTP2ProtocolError;
                        return;
                    }
                    fragment = fragment[wire.StreamPriority.byteSize..];
                }
                if (fragment.len > local_max_header_list_size) {
                    session.fatal_error = error.HTTP2HeaderListTooLarge;
                    return;
                }
                session.orphan_header_block.clearRetainingCapacity();
                bun.handleOom(session.orphan_header_block.appendSlice(bun.default_allocator, fragment));
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.END_HEADERS)) != 0) {
                    decodeDiscardOrphan(session);
                } else {
                    session.expecting_continuation = stream_id;
                }
            }
        },
        .HTTP_FRAME_CONTINUATION => {
            if (session.expecting_continuation == 0 || header.streamIdentifier != session.expecting_continuation) {
                session.fatal_error = error.HTTP2ProtocolError;
                return;
            }
            if (stream := session.streams.get(session.expecting_continuation)) {
                if (stream.header_block.items.len + payload.len > local_max_header_list_size) {
                    session.fatal_error = error.HTTP2HeaderListTooLarge;
                    return;
                }
                bun.handleOom(stream.header_block.appendSlice(bun.default_allocator, payload));
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.END_HEADERS)) != 0) {
                    session.expecting_continuation = 0;
                    if (stream.headers_end_stream) {
                        stream.recvEndStream();
                    }
                    decodeHeaderBlock(session, stream);
                }
            } else {
                if (session.orphan_header_block.items.len + payload.len > local_max_header_list_size) {
                    session.fatal_error = error.HTTP2HeaderListTooLarge;
                    return;
                }
                bun.handleOom(session.orphan_header_block.appendSlice(bun.default_allocator, payload));
                if ((header.flags & @intFromEnum(wire.HeadersFrameFlags.END_HEADERS)) != 0) {
                    session.expecting_continuation = 0;
                    decodeDiscardOrphan(session);
                }
            }
        },
        .HTTP_FRAME_DATA => {
            zig { session.conn_unacked_bytes +|= header.length; }
            const stream_id: u31 = @intCast(header.streamIdentifier);
            if (stream := session.streams.get(stream_id)) {
                session.stream_progressed = true;
                if (stream.status_code == 0) {
                    stream.rst(.PROTOCOL_ERROR);
                    stream.fatal_error = error.HTTP2ProtocolError;
                    return;
                }
                if (stream.remoteClosed()) {
                    stream.fatal_error = stream.fatal_error ?? error.HTTP2ProtocolError;
                    return;
                }
                zig { stream.unacked_bytes +|= header.length; }
                let mut fragment = payload;
                if ((header.flags & @intFromEnum(wire.DataFrameFlags.PADDED)) != 0) {
                    if (stripped := stripPadding(fragment)) {
                        fragment = stripped;
                    } else {
                        session.fatal_error = error.HTTP2ProtocolError;
                        return;
                    }
                }
                if ((header.flags & @intFromEnum(wire.DataFrameFlags.END_STREAM)) != 0) {
                    stream.recvEndStream();
                }
                stream.data_bytes_received += fragment.len;
                if (fragment.len > 0) {
                    bun.handleOom(stream.body_buffer.appendSlice(bun.default_allocator, fragment));
                }
            } else {
                if (stream_id == 0 || (stream_id & 1) == 0 || stream_id >= session.next_stream_id) {
                    session.fatal_error = error.HTTP2ProtocolError;
                }
                return;
            }
        },
        .HTTP_FRAME_RST_STREAM => {
            if (header.length != 4) {
                session.fatal_error = error.HTTP2FrameSizeError;
                return;
            }
            const stream_id: u31 = @intCast(header.streamIdentifier);
            if (stream_id == 0 || (stream_id & 1) == 0 || stream_id >= session.next_stream_id) {
                session.fatal_error = error.HTTP2ProtocolError;
                return;
            }
            if (stream := session.streams.get(stream_id)) {
                const had_response = stream.remoteClosed();
                stream.rst_done = true;
                stream.state = .closed;
                const code: u32 = wire.u32FromBytes(payload[0..4]);
                if (code == @intFromEnum(wire.ErrorCode.NO_ERROR)) {
                    if (had_response) {
                        stream.fatal_error = null;
                    } else {
                        stream.fatal_error = error.HTTP2StreamReset;
                    }
                } else if (code == @intFromEnum(wire.ErrorCode.REFUSED_STREAM)) {
                    stream.fatal_error = error.HTTP2RefusedStream;
                } else {
                    stream.fatal_error = error.HTTP2StreamReset;
                }
            }
        },
        .HTTP_FRAME_GOAWAY => {
            if (header.streamIdentifier != 0) {
                session.fatal_error = error.HTTP2ProtocolError;
                return;
            }
            if (header.length < 8) {
                session.fatal_error = error.HTTP2FrameSizeError;
                return;
            }
            session.goaway_received = true;
            session.goaway_last_stream_id = wire.UInt31WithReserved.fromBytes(payload[0..4]).uint31;
            const code: u32 = wire.u32FromBytes(payload[4..8]);
            const graceful = code == @intFromEnum(wire.ErrorCode.NO_ERROR);
            let mut it = session.streams.iterator();
            while (e := it.next()) {
                const s = e.value_ptr.*;
                if (s.id > session.goaway_last_stream_id) {
                    if (graceful) {
                        s.fatal_error = error.HTTP2RefusedStream;
                    } else {
                        s.fatal_error = error.HTTP2GoAway;
                    }
                } else if (!graceful && !s.remoteClosed()) {
                    s.fatal_error = error.HTTP2GoAway;
                }
            }
        },
        .HTTP_FRAME_PUSH_PROMISE => {
            session.fatal_error = error.HTTP2ProtocolError;
        },
        else => {}
    }
}

pub fn decodeDiscardOrphan(session: *ClientSession): void {
    defer session.orphan_header_block.clearRetainingCapacity();
    let mut offset: usize = 0;
    while (offset < session.orphan_header_block.items.len) {
        const result = session.hpack.decode(session.orphan_header_block.items[offset..]) catch {
            session.fatal_error = error.HTTP2CompressionError;
            return;
        };
        offset += result.next;
    }
}

pub fn decodeHeaderBlock(session: *ClientSession, stream: *Stream): void {
    defer stream.header_block.clearRetainingCapacity();

    let mut status: u32 = 0;
    let mut bounds: std.ArrayListUnmanaged([3]u32) = {};
    defer bounds.deinit(bun.default_allocator);
    const start_len = stream.decoded_bytes.items.len;
    let mut seen_regular = false;
    let mut seen_status = false;
    let mut malformed = false;

    let mut offset: usize = 0;
    while (offset < stream.header_block.items.len) {
        const result = session.hpack.decode(stream.header_block.items[offset..]) catch {
            session.fatal_error = error.HTTP2CompressionError;
            return;
        };
        offset += result.next;
        if (result.name.len > 0 && result.name[0] == ':') {
            if (stream.status_code != 0 || seen_regular || seen_status || !strings.eqlComptime(result.name, ":status")) {
                malformed = true;
                continue;
            }
            seen_status = true;
            if (result.value.len == 3) {
                status = std.fmt.parseInt(u32, result.value, 10) catch 0;
            } else {
                status = 0;
            }
            if (status < 100 || status > 999) {
                malformed = true;
            }
            continue;
        }
        seen_regular = true;
        if (stream.status_code != 0 || malformed) {
            continue;
        }
        if (isMalformedResponseField(result.name)) {
            malformed = true;
            continue;
        }
        if (stream.decoded_bytes.items.len + result.name.len + result.value.len > local_max_header_list_size) {
            session.fatal_error = error.HTTP2HeaderListTooLarge;
            return;
        }
        const name_start: u32 = @intCast(stream.decoded_bytes.items.len);
        bun.handleOom(stream.decoded_bytes.appendSlice(bun.default_allocator, result.name));
        const value_start: u32 = @intCast(stream.decoded_bytes.items.len);
        bun.handleOom(stream.decoded_bytes.appendSlice(bun.default_allocator, result.value));
        bun.handleOom(bounds.append(bun.default_allocator, [name_start, value_start, @intCast(stream.decoded_bytes.items.len)]));
    }

    if (malformed) {
        stream.decoded_bytes.items.len = start_len;
        stream.rst(.PROTOCOL_ERROR);
        stream.fatal_error = error.HTTP2ProtocolError;
        return;
    }

    if (stream.status_code != 0) {
        if (!stream.headers_end_stream) {
            stream.fatal_error = error.HTTP2ProtocolError;
        }
        return;
    }

    if (status == 0) {
        stream.decoded_bytes.items.len = start_len;
        stream.fatal_error = error.HTTP2ProtocolError;
        return;
    }
    if (status >= 100 && status < 200) {
        stream.decoded_bytes.items.len = start_len;
        if (status == 100) {
            stream.awaiting_continue = false;
        }
        if (stream.remoteClosed()) {
            stream.fatal_error = error.HTTP2ProtocolError;
        }
        return;
    }

    stream.status_code = status;
    stream.headers_ready = true;
    if (stream.awaiting_continue) {
        stream.awaiting_continue = false;
        session.writeFrame(.HTTP_FRAME_DATA, @intFromEnum(wire.DataFrameFlags.END_STREAM), stream.id, "");
        stream.sentEndStream();
    }
    const bytes = stream.decoded_bytes.items;
    bun.handleOom(stream.decoded_headers.ensureTotalCapacityPrecise(bun.default_allocator, bounds.items.len));
    for (b of bounds.items) {
        stream.decoded_headers.appendAssumeCapacity({ name: bytes[b[0]..b[1]], value: bytes[b[1]..b[2]] });
    }
}

pub fn stripPadding(payload: string): string? {
    if (payload.len < 1) {
        return null;
    }
    const pad: usize = payload[0];
    if (pad >= payload.len) {
        return null;
    }
    return payload[1..(payload.len - pad)];
}

pub fn isMalformedResponseField(name: string): bool {
    for (c of name) {
        if (c >= 'A' && c <= 'Z') {
            return true;
        }
    }
    return forbidden_response_fields.has(name);
}

pub fn errorCodeFor(err: anyerror): wire.ErrorCode {
    if (err == error.HTTP2ProtocolError) {
        return .PROTOCOL_ERROR;
    }
    if (err == error.HTTP2FrameSizeError) {
        return .FRAME_SIZE_ERROR;
    }
    if (err == error.HTTP2FlowControlError) {
        return .FLOW_CONTROL_ERROR;
    }
    if (err == error.HTTP2CompressionError) {
        return .COMPRESSION_ERROR;
    }
    if (err == error.HTTP2HeaderListTooLarge || err == error.HTTP2EnhanceYourCalm) {
        return .ENHANCE_YOUR_CALM;
    }
    return .INTERNAL_ERROR;
}
