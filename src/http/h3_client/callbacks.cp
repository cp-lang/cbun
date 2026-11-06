// src/http/h3_client/callbacks.cp
// lsquic -> Zig callbacks for the HTTP/3 client in CP language

zig {
    const bun = @import("bun");
    const quic = bun.uws.quic;
    pub const ManyU8 = [*]const u8;
}

import { ClientContext } from "./ClientContext.cp";
import { ClientSession } from "./ClientSession.cp";
import { Stream } from "./Stream.cp";
import * as encode from "./encode.cp";

export fn register(qctx: *quic.Context): void {
    qctx.onHskDone(onHskDone);
    qctx.onGoaway(onGoaway);
    qctx.onClose(onConnClose);
    qctx.onStreamOpen(onStreamOpen);
    qctx.onStreamHeaders(onStreamHeaders);
    qctx.onStreamData(onStreamData);
    qctx.onStreamWritable(onStreamWritable);
    qctx.onStreamClose(onStreamClose);
}

@c pub fn onHskDone(qs: *quic.Socket, ok: i32): void {
    if (session := qs.ext(ClientSession).*) {
        if (ok == 0) {
            session.closed = true;
            return;
        }
        session.handshake_done = true;
        let i: usize = 0;
        while (i < session.pending.items.len) {
            qs.makeStream();
            i += 1;
        }
    }
}

@c pub fn onGoaway(qs: *quic.Socket): void {
    if (session := qs.ext(ClientSession).*) {
        session.closed = true;
    }
}

@c pub fn onConnClose(qs: *quic.Socket): void {
    if (session := qs.ext(ClientSession).*) {
        session.closed = true;
        session.qsocket = null;

        let buf: [u8; 256] = [0; 256];
        const st = qs.status(&buf);
        if (ctx := ClientContext.get()) {
            ctx.unregister(session);
        }
        while (session.pending.items.len > 0) {
            const stream = session.pending.items[0];
            bun.debugAssert(stream.qstream == null);
            let mut err_val: anyerror = error.HTTP3HandshakeFailed;
            if (session.handshake_done) {
                err_val = error.ConnectionClosed;
            }
            session.retryOrFail(stream, err_val);
        }
        bun.http.H3.live_sessions.fetchSub(1, .monotonic);
        session.deref();
    }
}

@c pub fn onStreamOpen(s: *quic.Stream, is_client: i32): void {
    s.ext(Stream).* = null;
    if (is_client == 0) return;

    if (qs := s.socket()) {
        if (session := qs.ext(ClientSession).*) {
            zig {
                var found_stream: ?*Stream = null;
                for (session.pending.items) |st| {
                    if (st.qstream == null) {
                        found_stream = st;
                        break;
                    }
                }
                if (found_stream) |stream| {
                    stream.qstream = s;
                    s.ext(Stream).* = stream;
                    encode.writeRequest(session, stream, s) catch |err| {
                        session.fail(stream, err);
                    };
                } else {
                    s.close();
                    return;
                }
            }
        } else {
            s.close();
            return;
        }
    } else {
        return;
    }
}

@c pub fn onStreamHeaders(s: *quic.Stream): void {
    if (stream := s.ext(Stream).*) {
        const n = s.headerCount();

        stream.decoded_headers.clearRetainingCapacity();
        bun.handleOom(stream.decoded_headers.ensureTotalCapacity(bun.default_allocator, n));
        let status: u16 = 0;
        let i: u32 = 0;
        while (i < n) {
            if (h := s.header(i)) {
                const name = h.name[0..h.name_len];
                const value = h.value[0..h.value_len];
                if (bun.strings.hasPrefixComptime(name, ":")) {
                    if (bun.strings.eqlComptime(name, ":status")) {
                        status = std.fmt.parseInt(u16, value, 10) catch 0;
                    }
                    i += 1;
                    continue;
                }
                stream.decoded_headers.appendAssumeCapacity({ name: name, value: value });
            }
            i += 1;
        }
        if (status == 0) {
            if (stream.status_code != 0) return;
            stream.session.fail(stream, error.HTTP3ProtocolError);
            return;
        }
        if (status >= 100 && status < 200) return;
        stream.status_code = status;
        stream.session.deliver(stream, false);
    }
}

@c pub fn onStreamData(s: *quic.Stream, data: ManyU8, len: u32, fin: i32): void {
    if (stream := s.ext(Stream).*) {
        if (len > 0) {
            bun.handleOom(stream.body_buffer.appendSlice(bun.default_allocator, data[0..len]));
        }
        stream.session.deliver(stream, fin != 0);
    }
}

@c pub fn onStreamWritable(s: *quic.Stream): void {
    if (stream := s.ext(Stream).*) {
        encode.drainSendBody(stream, s);
    }
}

@c pub fn onStreamClose(s: *quic.Stream): void {
    if (stream := s.ext(Stream).*) {
        s.ext(Stream).* = null;
        stream.qstream = null;
        stream.session.deliver(stream, true);
    }
}
