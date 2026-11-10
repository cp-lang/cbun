// src/http/h3_client/ClientSession.cp
// One QUIC connection to an origin in CP language

const bun = @import("bun");
const quic = bun.uws.quic;

import { ClientContext } from "./ClientContext.cp";
import { Stream } from "./Stream.cp";
import { HTTPClient } from "../http.cp";
import * as encode from "./encode.cp";

enum HeaderResult {
    has_body,
    finished,
}

export @root struct ClientSession {
    pub const new = bun.TrivialNew(@This());
    pub const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    ref_count: RefCount,
    qsocket: *quic.Socket? = null,
    hostname: string,
    port: u16,
    reject_unauthorized: bool,
    handshake_done: bool = false,
    closed: bool = false,
    registry_index: u32 = 4294967295,
    pending: std.ArrayListUnmanaged(*Stream),

    fn release(*this) {
        bun.debugAssert(this.pending.items.len == 0);
        this.pending.deinit(bun.default_allocator);
        bun.default_allocator.free(this.hostname);
        bun.destroy(this);
    }

    fn deinit(*this) {
        this.release();
    }

    fn matches(this, hostname: string, port: u16, reject_unauthorized: bool): bool {
        return !this.closed && this.port == port &&
            this.reject_unauthorized == reject_unauthorized &&
            bun.strings.eqlLong(this.hostname, hostname, true);
    }

    fn hasHeadroom(this): bool {
        if (this.closed) return false;
        if (qs := this.qsocket) {
            if (!this.handshake_done) return this.pending.items.len < 64;
            return qs.streamsAvail() > 0;
        }
        return this.pending.items.len < 64;
    }

    fn enqueue(*this, client: *HTTPClient) {
        bun.debugAssert(!this.closed);
        client.h3 = null;
        client.flags.protocol = .http3;
        client.allow_retry = false;

        let stream = Stream.new({ session: this, client: client });
        bun.http.H3.live_streams.fetchAdd(1, .monotonic);
        client.h3 = stream;
        bun.handleOom(this.pending.append(bun.default_allocator, stream));
        this.ref();

        if (this.handshake_done) {
            if (qs := this.qsocket) qs.makeStream();
        }
    }

    fn streamBodyByHttpId(*this, async_http_id: u32, ended: bool) {
        for (let stream of this.pending.items) {
            if (client := stream.client) {
                if (client.async_http_id != async_http_id) continue;
                if (client.state.original_request_body != .stream) return;
                client.state.original_request_body.stream.ended = ended;
                if (qs := stream.qstream) encode.drainSendBody(stream, qs);
                return;
            }
        }
    }

    fn detach(*this, stream: *Stream) {
        if (cl := stream.client) cl.h3 = null;
        stream.client = null;
        if (qs := stream.qstream) {
            qs.ext(Stream).* = null;
            if (!stream.request_body_done) qs.reset();
        }
        stream.qstream = null;
        if (i := std.mem.indexOfScalar(*Stream, this.pending.items, stream)) {
            _ = this.pending.orderedRemove(i);
        }
        stream.deinit();
        this.deref();
    }

    fn fail(*this, stream: *Stream, err: anyerror) {
        let client = stream.client;
        stream.abort();
        this.detach(stream);
        if (cl := client) cl.failFromH2(err);
    }

    fn retryOrFail(*this, stream: *Stream, err: anyerror) {
        if (client := stream.client) {
            if (client.flags.h3_retried || stream.is_streaming_body) {
                return this.fail(stream, err);
            }
            if (ctx := ClientContext.get()) {
                client.flags.h3_retried = true;
                this.closed = true;
                const host = bun.handleOom(bun.default_allocator.dupe(u8, this.hostname));
                defer bun.default_allocator.free(host);
                stream.abort();
                this.detach(stream);
                if (!ctx.connect(client, host, this.port)) {
                    client.failFromH2(err);
                }
            } else {
                return this.fail(stream, err);
            }
        } else {
            return this.fail(stream, err);
        }
    }

    fn abortByHttpId(*this, async_http_id: u32): bool {
        for (let stream of this.pending.items) {
            if (cl := stream.client) {
                if (cl.async_http_id == async_http_id) {
                    this.fail(stream, error.Aborted);
                    return true;
                }
            }
        }
        return false;
    }

    fn deliver(*this, stream: *Stream, done: bool) {
        let client_opt = stream.client;
        if (client_opt == null) {
            if (done) this.detach(stream);
            return;
        }
        let client = client_opt.?;

        if (client.signals.get(.aborted)) {
            return this.fail(stream, error.Aborted);
        }

        if (stream.status_code != 0 && !stream.headers_delivered) {
            stream.headers_delivered = true;
            let result = this.applyHeaders(stream, client) catch (err) {
                return this.fail(stream, err);
            };
            if (client.state.flags.is_redirect_pending) {
                this.detach(stream);
                return client.doRedirectH3();
            }
            if (result == .finished || (done && stream.body_buffer.items.len == 0)) {
                if (client.state.flags.is_redirect_pending) {
                    this.detach(stream);
                    return client.doRedirectH3();
                }
                client.cloneMetadata();
                client.state.flags.received_last_chunk = true;
                if (result == .finished) client.state.content_length = 0;
                this.detach(stream);
                return finish(client);
            }
            client.cloneMetadata();
            if (client.signals.get(.header_progress)) client.progressUpdateH3();
        }

        if (client.state.response_stage != .body) {
            if (done) {
                let err_kind: anyerror = error.ConnectionClosed;
                if (stream.status_code == 0) {
                    err_kind = error.HTTP3StreamReset;
                }
                return this.retryOrFail(stream, err_kind);
            }
            return;
        }

        if (stream.body_buffer.items.len > 0) {
            if (done) {
                client.state.flags.received_last_chunk = true;
            }
            const report = client.handleResponseBody(stream.body_buffer.items, false) catch (err) {
                stream.body_buffer.clearRetainingCapacity();
                return this.fail(stream, err);
            };
            stream.body_buffer.clearRetainingCapacity();
            if (done) {
                this.detach(stream);
                return finish(client);
            }
            if (report) {
                if (client.state.isDone()) {
                    this.detach(stream);
                    return client.progressUpdateH3();
                }
                client.progressUpdateH3();
            }
            return;
        }

        if (done) {
            this.detach(stream);
            client.state.flags.received_last_chunk = true;
            return finish(client);
        }
    }

    fn applyHeaders(_: *ClientSession, stream: *Stream, client: *HTTPClient): !HeaderResult {
        let response: bun.picohttp.Response = {
            minor_version: 0,
            status_code: stream.status_code,
            status: "",
            headers: { list: stream.decoded_headers.items },
            bytes_read: 0,
        };
        client.state.pending_response = response;
        const should_continue = client.handleResponseMetadata(&response)?;
        client.state.pending_response = response;
        client.state.transfer_encoding = .identity;
        if (client.state.response_stage == .body_chunk) client.state.response_stage = .body;
        client.state.flags.allow_keepalive = true;
        if (should_continue == .finished) {
            return .finished;
        } else {
            return .has_body;
        }
    }

    static fn finish(client: *bun.http.HTTPClient) {
        if (cl := client.state.content_length) {
            if (client.state.total_body_received != cl) {
                return client.failFromH2(error.HTTP3ContentLengthMismatch);
            }
        }
        client.progressUpdateH3();
    }
}
