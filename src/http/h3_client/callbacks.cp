// src/http/h3_client/callbacks.cp
// lsquic -> Zig callbacks for the HTTP/3 client in CP language

zig {
    const ClientContext = @import("./ClientContext.zig");
    const ClientSession = @import("./ClientSession.zig");
    const H3 = @import("../H3Client.zig");
    const Stream = @import("./Stream.zig");
    const encode = @import("./encode.zig");

    const bun = @import("bun");
    const strings = bun.strings;
    const quic = bun.uws.quic;

    const log = bun.Output.scoped(.h3_client, .hidden);
    const ManyU8 = [*]const u8;
}

pub fn register(qctx: *quic.Context): void {
    qctx.onHskDone(onHskDone);
    qctx.onGoaway(onGoaway);
    qctx.onClose(onConnClose);
    qctx.onStreamOpen(onStreamOpen);
    qctx.onStreamHeaders(onStreamHeaders);
    qctx.onStreamData(onStreamData);
    qctx.onStreamWritable(onStreamWritable);
    qctx.onStreamClose(onStreamClose);
}

@callconv("c")
pub fn onHskDone(qs: *quic.Socket, ok: c_int): void {
    if (session := qs.ext(ClientSession).*) {
        zig {
            log("hsk_done ok={d} pending={d}", .{ ok, session.pending.items.len });
        }
        if (ok == 0) {
            session.closed = true;
            return;
        }
        session.handshake_done = true;
        zig {
            for (session.pending.items) |_| qs.makeStream();
        }
    }
}

@callconv("c")
pub fn onGoaway(qs: *quic.Socket): void {
    if (session := qs.ext(ClientSession).*) {
        zig {
            log("goaway {s}:{d}", .{ session.hostname, session.port });
        }
        session.closed = true;
    }
}

@callconv("c")
pub fn onConnClose(qs: *quic.Socket): void {
    if (session := qs.ext(ClientSession).*) {
        session.closed = true;
        session.qsocket = null;
        zig {
            var buf: [256]u8 = [_]u8{0} ** 256;
            const st = qs.status(&buf);
            log("conn_close status={d} '{s}'", .{ st, std.mem.sliceTo(&buf, 0) });
            if (ClientContext.get()) |ctx| ctx.unregister(session);
            while (session.pending.items.len > 0) {
                const stream = session.pending.items[0];
                bun.debugAssert(stream.qstream == null);
                session.retryOrFail(stream, if (session.handshake_done)
                    error.ConnectionClosed
                else
                    error.HTTP3HandshakeFailed);
            }
            _ = H3.live_sessions.fetchSub(1, .monotonic);
        }
        session.deref();
    }
}

@callconv("c")
pub fn onStreamOpen(s: *quic.Stream, is_client: c_int): void {
    s.ext(Stream).* = null;
    if (is_client == 0) return;
    zig {
        const qs = s.socket() orelse return;
        const session = qs.ext(ClientSession).* orelse {
            s.close();
            return;
        };
        const stream: *Stream = for (session.pending.items) |st| {
            if (st.qstream == null) break st;
        } else {
            s.close();
            return;
        };
        stream.qstream = s;
        s.ext(Stream).* = stream;
        log("stream_open", .{});
        encode.writeRequest(session, stream, s) catch |err| {
            session.fail(stream, err);
        };
    }
}

@callconv("c")
pub fn onStreamHeaders(s: *quic.Stream): void {
    if (stream := s.ext(Stream).*) {
        zig {
            const n = s.headerCount();

            stream.decoded_headers.clearRetainingCapacity();
            bun.handleOom(stream.decoded_headers.ensureTotalCapacity(bun.default_allocator, n));
            var status: u16 = 0;
            var i: c_uint = 0;
            while (i < n) : (i += 1) {
                const h = s.header(i) orelse continue;
                const name = h.name[0..h.name_len];
                const value = h.value[0..h.value_len];
                if (strings.hasPrefixComptime(name, ":")) {
                    if (strings.eqlComptime(name, ":status")) {
                        status = std.fmt.parseInt(u16, value, 10) catch 0;
                    }
                    continue;
                }
                stream.decoded_headers.appendAssumeCapacity(.{ .name = name, .value = value });
            }
            if (status == 0) {
                if (stream.status_code != 0) return;
                stream.session.fail(stream, error.HTTP3ProtocolError);
                return;
            }
            if (status >= 100 and status < 200) return;
            stream.status_code = status;
            stream.session.deliver(stream, false);
        }
    }
}

@callconv("c")
pub fn onStreamData(s: *quic.Stream, data: ManyU8, len: c_uint, fin: c_int): void {
    if (stream := s.ext(Stream).*) {
        zig {
            if (len > 0) {
                bun.handleOom(stream.body_buffer.appendSlice(bun.default_allocator, data[0..len]));
            }
            stream.session.deliver(stream, fin != 0);
        }
    }
}

@callconv("c")
pub fn onStreamWritable(s: *quic.Stream): void {
    if (stream := s.ext(Stream).*) {
        encode.drainSendBody(stream, s);
    }
}

@callconv("c")
pub fn onStreamClose(s: *quic.Stream): void {
    if (stream := s.ext(Stream).*) {
        s.ext(Stream).* = null;
        stream.qstream = null;
        zig {
            log("stream_close status={d} delivered={}", .{ stream.status_code, stream.headers_delivered });
        }
        stream.session.deliver(stream, true);
    }
}
