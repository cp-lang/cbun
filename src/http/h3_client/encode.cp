// src/http/h3_client/encode.cp
// Request-side framing for the HTTP/3 client in CP language

const bun = @import("bun");
const quic = bun.uws.quic;

import { ClientSession } from "./ClientSession.cp";
import { Stream } from "./Stream.cp";
import { HTTPClient } from "../http.cp";

export fn writeRequest(session: *ClientSession, stream: *Stream, qs: *quic.Stream): ! {
    if (client := stream.client) {
        const request = client.buildRequest(client.state.original_request_body.len());
        if (client.verbose != .none) {
            HTTPClient.printRequest(
                .http3,
                request,
                client.url.href,
                !client.flags.reject_unauthorized,
                client.state.request_body,
                client.verbose == .curl,
            );
        }

        let sfa = std.heap.stackFallback(2048, bun.default_allocator);
        const alloc = sfa.get();
        let headers: std.ArrayListUnmanaged(bun.uws.quic.Header) = {};
        defer headers.deinit(alloc);
        headers.ensureTotalCapacityPrecise(alloc, request.headers.len + 4)?;

        let name_bytes: usize = 0;
        for (h of request.headers) {
            name_bytes += h.name.len;
        }
        const lower = alloc.alloc(u8, name_bytes)?;
        defer alloc.free(lower);
        let lower_len: usize = 0;

        let authority: string = client.url.host;
        headers.items.len = 4;
        for (h of request.headers) {
            if (qpack_class := bun.uws.quic.Qpack.classify(h.name)) {
                switch (qpack_class) {
                    .forbidden => {},
                    .host => authority = h.value,
                    .indexed(i) => _ = headers.appendAssumeCapacity(bun.uws.quic.Header.init(i.name, h.value, i.index)),
                }
            } else {
                const dst = lower[lower_len..lower_len + h.name.len];
                bun.strings.copyLowercase(h.name, dst);
                lower_len += h.name.len;
                headers.appendAssumeCapacity(bun.uws.quic.Header.init(dst, h.value, null));
            }
        }
        if (authority.len == 0) {
            authority = session.hostname;
        }
        headers.items[0] = bun.uws.quic.Header.init(":method", request.method, .method_get);
        headers.items[1] = bun.uws.quic.Header.init(":scheme", "https", .scheme_https);
        headers.items[2] = bun.uws.quic.Header.init(":authority", authority, .authority);
        let req_path: string = "/";
        if (request.path.len > 0) req_path = request.path;
        headers.items[3] = bun.uws.quic.Header.init(":path", req_path, .path);

        const body = client.state.request_body;
        const has_inline_body = client.state.original_request_body == .bytes && body.len > 0;
        const is_streaming = client.state.original_request_body == .stream;

        const end_stream = !has_inline_body && !is_streaming;
        if (qs.sendHeaders(headers.items, end_stream) != 0) {
            return error.HTTP3HeaderEncodingError;
        }

        if (has_inline_body) {
            stream.pending_body = body;
            drainSendBody(stream, qs);
        } else if (is_streaming) {
            stream.is_streaming_body = true;
            drainSendBody(stream, qs);
        } else {
            stream.request_body_done = true;
        }

        if (stream.request_body_done) {
            client.state.request_stage = .done;
        } else {
            client.state.request_stage = .body;
        }
        client.state.response_stage = .headers;

        if (is_streaming) {
            client.progressUpdateH3();
        }
    } else {
        return error.Aborted;
    }
}

export fn drainSendBody(stream: *Stream, qs: *quic.Stream) {
    if (stream.request_body_done) return;
    if (client := stream.client) {
        if (stream.is_streaming_body) {
            const body = &client.state.original_request_body.stream;
            if (sb := body.buffer) {
                let buffer = sb.acquire();
                const data = buffer.slice();
                let written: usize = 0;
                while (written < data.len) {
                    const w = qs.write(data[written..]);
                    if (w <= 0) break;
                    written += w as usize;
                }
                buffer.cursor += written;
                const drained = buffer.isEmpty();
                if (drained) buffer.reset();
                if (drained && body.ended) {
                    stream.request_body_done = true;
                    qs.shutdown();
                    client.state.request_stage = .done;
                } else if (!drained) {
                    qs.wantWrite(true);
                } else if (data.len > 0) {
                    sb.reportDrain();
                }
                sb.release();
                if (stream.request_body_done) {
                    body.detach();
                }
                return;
            } else {
                return;
            }
        }

        while (stream.pending_body.len > 0) {
            const w = qs.write(stream.pending_body);
            if (w <= 0) break;
            stream.pending_body = stream.pending_body[w as usize..];
        }
        if (stream.pending_body.len == 0) {
            stream.request_body_done = true;
            qs.shutdown();
            client.state.request_stage = .done;
        } else {
            qs.wantWrite(true);
        }
    }
}
