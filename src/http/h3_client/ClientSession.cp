// src/http/h3_client/ClientSession.cp
// One QUIC connection to an origin in CP language

zig {
    const ClientContext = @import("./ClientContext.zig");
    const H3 = @import("../H3Client.zig");
    const Stream = @import("./Stream.zig");
    const encode = @import("./encode.zig");

    const bun = @import("bun");
    const HTTPClient = bun.http;
    const picohttp = bun.picohttp;
    const strings = bun.strings;
    const quic = bun.uws.quic;

    const log = bun.Output.scoped(.h3_client, .hidden);
    const HeaderResult = enum { has_body, finished };
    const PendingList = std.ArrayListUnmanaged(*Stream);
}

export @root struct ClientSession {
    ref_count: RefCount,
    qsocket: *quic.Socket?,
    hostname: string,
    port: u16,
    reject_unauthorized: bool,
    handshake_done: bool = false,
    closed: bool = false,
    registry_index: u32 = 4294967295,
    pending: PendingList,

    zig {
        pub const new = bun.TrivialNew(@This());

        const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
        pub const ref = RefCount.ref;
        pub const deref = RefCount.deref;

        fn deinit(this: *ClientSession) void {
            bun.debugAssert(this.pending.items.len == 0);
            this.pending.deinit(bun.default_allocator);
            bun.default_allocator.free(this.hostname);
            bun.destroy(this);
        }
    }

    pub fn matches(this, hostname: string, port: u16, reject_unauthorized: bool): bool {
        return !this.closed && this.port == port &&
            this.reject_unauthorized == reject_unauthorized &&
            strings.eqlLong(this.hostname, hostname, true);
    }

    pub fn hasHeadroom(this): bool {
        if (this.closed) return false;
        zig {
            const qs = self.qsocket orelse return self.pending.items.len < 64;
            if (!self.handshake_done) return self.pending.items.len < 64;
            return qs.streamsAvail() > 0;
        }
    }

    pub fn enqueue(*this, client: *HTTPClient): void {
        zig {
            bun.debugAssert(!self.closed);
            client.h3 = null;
            client.flags.protocol = .http3;
            client.allow_retry = false;

            const stream = Stream.new(.{ .session = self, .client = client });
            _ = H3.live_streams.fetchAdd(1, .monotonic);
            client.h3 = stream;
            bun.handleOom(self.pending.append(bun.default_allocator, stream));
            self.ref();

            if (self.handshake_done) {
                self.qsocket.?.makeStream();
            }
        }
    }

    pub fn streamBodyByHttpId(*this, async_http_id: u32, ended: bool): void {
        zig {
            for (self.pending.items) |stream| {
                const client = stream.client orelse continue;
                if (client.async_http_id != async_http_id) continue;
                if (client.state.original_request_body != .stream) return;
                client.state.original_request_body.stream.ended = ended;
                if (stream.qstream) |qs| encode.drainSendBody(stream, qs);
                return;
            }
        }
    }

    pub fn detach(*this, stream: *Stream): void {
        zig {
            if (stream.client) |cl| cl.h3 = null;
            stream.client = null;
            if (stream.qstream) |qs| {
                qs.ext(Stream).* = null;
                if (!stream.request_body_done) qs.reset();
            }
            stream.qstream = null;
            if (std.mem.indexOfScalar(*Stream, self.pending.items, stream)) |i| {
                _ = self.pending.orderedRemove(i);
            }
            stream.deinit();
            self.deref();
        }
    }

    pub fn fail(*this, stream: *Stream, err: anyerror): void {
        zig {
            const client = stream.client;
            stream.abort();
            self.detach(stream);
            if (client) |cl| cl.failFromH2(err);
        }
    }

    pub fn retryOrFail(*this, stream: *Stream, err: anyerror): void {
        zig {
            const client = stream.client orelse return self.fail(stream, err);
            if (client.flags.h3_retried or stream.is_streaming_body) {
                return self.fail(stream, err);
            }
            const ctx = ClientContext.get() orelse return self.fail(stream, err);
            client.flags.h3_retried = true;
            self.closed = true;
            const port = self.port;
            const host = bun.handleOom(bun.default_allocator.dupe(u8, self.hostname));
            defer bun.default_allocator.free(host);
            log("retry {s}:{d} after {s}", .{ host, port, @errorName(err) });
            stream.abort();
            self.detach(stream);
            if (!ctx.connect(client, host, port)) {
                client.failFromH2(err);
            }
        }
    }

    pub fn abortByHttpId(*this, async_http_id: u32): bool {
        zig {
            for (self.pending.items) |stream| {
                const cl = stream.client orelse continue;
                if (cl.async_http_id == async_http_id) {
                    self.fail(stream, error.Aborted);
                    return true;
                }
            }
            return false;
        }
    }

    pub fn deliver(*this, stream: *Stream, done: bool): void {
        zig {
            const client = stream.client orelse {
                if (done) self.detach(stream);
                return;
            };

            if (client.signals.get(.aborted)) {
                return self.fail(stream, error.Aborted);
            }

            if (stream.status_code != 0 and !stream.headers_delivered) {
                stream.headers_delivered = true;
                const result = self.applyHeaders(stream, client) catch |err| {
                    return self.fail(stream, err);
                };
                if (result == .finished or (done and stream.body_buffer.items.len == 0)) {
                    if (client.state.flags.is_redirect_pending) {
                        self.detach(stream);
                        return client.doRedirectH3();
                    }
                    client.cloneMetadata();
                    client.state.flags.received_last_chunk = true;
                    if (result == .finished) client.state.content_length = 0;
                    self.detach(stream);
                    return finish(client);
                }
                client.cloneMetadata();
                if (client.signals.get(.header_progress)) client.progressUpdateH3();
            }

            if (client.state.response_stage != .body) {
                if (done) {
                    return self.retryOrFail(stream, if (stream.status_code == 0)
                        error.HTTP3StreamReset
                    else
                        error.ConnectionClosed);
                }
                return;
            }

            if (stream.body_buffer.items.len > 0) {
                if (done) {
                    client.state.flags.received_last_chunk = true;
                }
                const report = client.handleResponseBody(stream.body_buffer.items, false) catch |err| {
                    stream.body_buffer.clearRetainingCapacity();
                    return self.fail(stream, err);
                };
                stream.body_buffer.clearRetainingCapacity();
                if (done) {
                    self.detach(stream);
                    return finish(client);
                }
                if (report) {
                    if (client.state.isDone()) {
                        self.detach(stream);
                        return client.progressUpdateH3();
                    }
                    client.progressUpdateH3();
                }
                return;
            }

            if (done) {
                self.detach(stream);
                client.state.flags.received_last_chunk = true;
                return finish(client);
            }
        }
    }

    fn applyHeaders(_: *ClientSession, stream: *Stream, client: *HTTPClient): HeaderResult! {
        zig {
            var response = picohttp.Response{
                .minor_version = 0,
                .status_code = stream.status_code,
                .status = "",
                .headers = .{ .list = stream.decoded_headers.items },
                .bytes_read = 0,
            };
            client.state.pending_response = response;
            const should_continue = try client.handleResponseMetadata(&response);
            client.state.pending_response = response;
            client.state.transfer_encoding = .identity;
            if (client.state.response_stage == .body_chunk) client.state.response_stage = .body;
            client.state.flags.allow_keepalive = true;
            return if (should_continue == .finished) .finished else .has_body;
        }
    }

    static fn finish(client: *HTTPClient): void {
        zig {
            if (client.state.content_length) |cl| {
                if (client.state.total_body_received != cl) {
                    return client.failFromH2(error.HTTP3ContentLengthMismatch);
                }
            }
            client.progressUpdateH3();
        }
    }
}
