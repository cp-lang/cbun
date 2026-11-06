// src/http/h2_client/ClientSession.cp
// One TCP+TLS connection running the HTTP/2 protocol for `fetch()`.

zig {
    const bun = @import("bun");
    pub const HTTPContextTLS = @import("../HTTPContext.zig").NewHTTPContext(true);
    pub const Socket = HTTPContextTLS.HTTPSocket;
    pub const HTTPClient = bun.http.HTTPClient;
    pub const SSLConfig = bun.api.server.ServerConfig.SSLConfig;
}

import { Stream } from "./Stream.cp";
import * as dispatch from "./dispatch.cp";
import * as encode from "./encode.cp";
import { HPACK } from "../lshpack.cp";
import * as H2FrameParser from "../H2FrameParser.cp";
import { HTTPContext, NewHTTPContext } from "../HTTPContext.cp";

enum HeaderResult {
    has_body,
    finished,
}

export @root struct ClientSession {
    zig {
        pub const new = bun.TrivialNew(@This());
        pub const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
        pub const ref = RefCount.ref;
        pub const deref = RefCount.deref;
    }

    ref_count: RefCount,
    hpack: *HPACK,
    socket: Socket,
    ctx: *HTTPContextTLS,

    hostname: string,
    port: u16,
    ssl_config: bun.api.server.ServerConfig.SSLConfig.SharedPtr? = null,
    did_have_handshaking_error: bool,

    write_buffer: bun.io.StreamBuffer = {},
    read_buffer: std.ArrayListUnmanaged(u8) = {},
    streams: std.AutoArrayHashMapUnmanaged(u31, *Stream) = {},
    next_stream_id: u31 = 1,
    expecting_continuation: u31 = 0,
    pending_attach: std.ArrayListUnmanaged(*HTTPClient) = {},

    preface_sent: bool = false,
    settings_received: bool = false,
    goaway_received: bool = false,
    encoder_poisoned: bool = false,
    delivering: bool = false,
    stream_progressed: bool = false,
    goaway_last_stream_id: u31 = 0,
    fatal_error: anyerror? = null,
    orphan_header_block: std.ArrayListUnmanaged(u8) = {},
    encode_scratch: std.ArrayListUnmanaged(u8) = {},

    remote_max_frame_size: u24 = H2FrameParser.DEFAULT_MAX_FRAME_SIZE,
    remote_max_concurrent_streams: u32 = 100,
    remote_initial_window_size: u32 = H2FrameParser.DEFAULT_WINDOW_SIZE,
    pending_hpack_enc_capacity: u32? = null,
    conn_send_window: i32 = H2FrameParser.DEFAULT_WINDOW_SIZE,
    conn_unacked_bytes: u32 = 0,
    registry_index: u32 = 4294967295,

    fn release(*this): void {
        bun.http.H2.live_sessions.fetchSub(1, .monotonic);
        bun.debugAssert(this.registry_index == std.math.maxInt(u32));
        this.hpack.deinit();
        this.write_buffer.deinit();
        this.read_buffer.deinit(bun.default_allocator);
        let mut it = this.streams.iterator();
        while (e := it.next()) e.value_ptr.*.deinit();
        this.streams.deinit(bun.default_allocator);
        this.pending_attach.deinit(bun.default_allocator);
        this.orphan_header_block.deinit(bun.default_allocator);
        this.encode_scratch.deinit(bun.default_allocator);
        bun.default_allocator.free(this.hostname);
        zig {
            if (self.ssl_config) |*s| s.deinit();
        }
        bun.destroy(this);
    }

    fn deinit(*this): void {
        this.release();
    }

    static fn create(ctx: *HTTPContextTLS, socket: Socket, client: *HTTPClient): *ClientSession {
        let mut ssl_config: bun.api.server.ServerConfig.SSLConfig.SharedPtr? = null;
        if (p := client.tls_props) {
            ssl_config = p.clone();
        }
        const session = ClientSession.new({
            ref_count: RefCount.init(),
            hpack: HPACK.init(4096),
            socket: socket,
            ctx: ctx,
            hostname: bun.handleOom(bun.default_allocator.dupe(u8, client.connected_url.hostname)),
            port: client.connected_url.getPortAuto(),
            ssl_config: ssl_config,
            did_have_handshaking_error: client.flags.did_have_handshaking_error,
        });
        bun.http.H2.live_sessions.fetchAdd(1, .monotonic);
        ctx.registerH2(session);
        return session;
    }

    fn hasHeadroom(this): bool {
        return !this.goaway_received &&
            !this.encoder_poisoned &&
            this.fatal_error == null &&
            this.streams.count() < this.remote_max_concurrent_streams &&
            this.next_stream_id < H2FrameParser.MAX_STREAM_ID;
    }

    fn matches(this, hostname: string, port: u16, ssl_config: *SSLConfig?): bool {
        return this.port == port && bun.api.server.ServerConfig.SSLConfig.rawPtr(this.ssl_config) == ssl_config && bun.strings.eqlLong(this.hostname, hostname, true);
    }

    fn adopt(*this, client: *HTTPClient): void {
        client.registerAbortTracker(true, this.socket);
        if (this.delivering || !this.settings_received) {
            bun.handleOom(this.pending_attach.append(bun.default_allocator, client));
            this.rearmTimeout();
            return;
        }
        if (!this.hasHeadroom()) {
            client.retryAfterH2Coalesce();
            this.maybeRelease();
            return;
        }
        this.attach(client);
        if (this.encoder_poisoned) {
            this.maybeRelease();
        }
    }

    fn enqueue(*this, client: *HTTPClient): void {
        client.registerAbortTracker(true, this.socket);
        bun.handleOom(this.pending_attach.append(bun.default_allocator, client));
        this.rearmTimeout();
    }

    fn drainPending(*this): void {
        if (!this.settings_received || this.pending_attach.items.len == 0) {
            return;
        }
        let mut waiters = this.pending_attach;
        this.pending_attach = {};
        defer waiters.deinit(bun.default_allocator);
        for (client of waiters.items) {
            if (err := this.fatal_error) {
                client.failFromH2(err);
            } else if (client.signals.get(.aborted)) {
                client.failFromH2(error.Aborted);
            } else if (this.hasHeadroom()) {
                this.attach(client);
            } else {
                client.retryAfterH2Coalesce();
            }
        }
    }

    fn canPool(this): bool {
        return this.streams.count() == 0 &&
            !this.goaway_received &&
            !this.encoder_poisoned &&
            this.fatal_error == null &&
            this.expecting_continuation == 0 &&
            this.read_buffer.items.len == 0 &&
            this.write_buffer.isEmpty() &&
            this.remote_max_concurrent_streams > 0 &&
            this.next_stream_id < H2FrameParser.MAX_STREAM_ID;
    }

    fn queue(*this, bytes: string): void {
        bun.handleOom(this.write_buffer.write(bytes));
    }

    fn writeFrame(*this, frame_type: H2FrameParser.FrameType, flags: u8, stream_id: u32, payload: string): void {
        let header: H2FrameParser.FrameHeader = {
            "type": @intFromEnum(frame_type),
            flags: flags,
            streamIdentifier: stream_id,
            length: @intCast(payload.len),
        };
        std.mem.byteSwapAllFields(H2FrameParser.FrameHeader, &header);
        this.queue(std.mem.asBytes(&header)[0..H2FrameParser.FrameHeader.byteSize]);
        this.queue(payload);
    }

    fn attach(*this, client: *HTTPClient): void {
        bun.debugAssert(this.hasHeadroom());

        const stream = Stream.new({
            id: this.next_stream_id,
            session: this,
            client: client,
            send_window: @intCast(@min(this.remote_initial_window_size, @as(u32, H2FrameParser.MAX_WINDOW_SIZE))),
        });
        bun.http.H2.live_streams.fetchAdd(1, .monotonic);
        this.next_stream_id += 2;
        bun.handleOom(this.streams.put(bun.default_allocator, stream.id, stream));
        client.h2 = stream;
        client.flags.protocol = .http2;
        client.allow_retry = false;

        if (!this.preface_sent) encode.writePreface(this);

        this.rearmTimeout();
        const request = client.buildRequest(client.state.original_request_body.len());
        encode.writeRequest(this, client, stream, request) catch (err) {
            this.encoder_poisoned = true;
            this.streams.swapRemove(stream.id);
            stream.deinit();
            client.h2 = null;
            client.failFromH2(err);
            for (c of this.pending_attach.items) c.retryAfterH2Coalesce();
            this.pending_attach.clearRetainingCapacity();
            this.flush() catch {};
            return;
        };
        if (client.verbose != .none) {
            bun.http.printRequest(.http2, request, client.url.href, !client.flags.reject_unauthorized, client.state.request_body, client.verbose == .curl);
        }
        if (stream.localClosed()) {
            client.state.request_stage = .done;
        } else {
            client.state.request_stage = .body;
        }
        client.state.response_stage = .headers;

        this.flush() catch (err) {
            this.failAll(err);
            return;
        };

        if (client.flags.is_streaming_request_body) {
            client.progressUpdate(true, this.ctx, this.socket);
        }
    }

    fn removeStream(*this, stream: *Stream): void {
        if (this.expecting_continuation == stream.id) {
            this.orphan_header_block.deinit(bun.default_allocator);
            this.orphan_header_block = stream.header_block;
            stream.header_block = {};
        }
        this.streams.swapRemove(stream.id);
        stream.deinit();
    }

    fn detachWithFailure(*this, stream: *Stream, err: anyerror): void {
        stream.rst(.CANCEL);
        this.flush() catch {};
        const client = stream.client;
        stream.client = null;
        if (c := client) {
            c.h2 = null;
        }
        this.removeStream(stream);
        if (c := client) {
            c.failFromH2(err);
        }
    }

    fn rearmTimeout(*this): void {
        let mut want = false;
        for (s of this.streams.values()) {
            if (c := s.client) {
                if (!c.flags.disable_timeout) {
                    want = true;
                    break;
                }
            }
        }
        if (!want) {
            for (c of this.pending_attach.items) {
                if (!c.flags.disable_timeout) {
                    want = true;
                    break;
                }
            }
        }
        let mut timeout: u32 = 0;
        if (want) {
            timeout = bun.http.idle_timeout_seconds;
        }
        this.socket.setTimeout(timeout);
    }

    fn drainResponseBodyByHttpId(*this, async_http_id: u32): void {
        this.ref();
        defer this.deref();
        for (stream of this.streams.values()) {
            if (client := stream.client) {
                if (client.async_http_id != async_http_id) continue;
                client.drainResponseBody(true, this.socket);
                return;
            }
        }
    }

    fn streamBodyByHttpId(*this, async_http_id: u32, ended: bool): void {
        this.ref();
        defer this.deref();
        for (stream of this.streams.values()) {
            if (client := stream.client) {
                if (client.async_http_id != async_http_id) continue;
                if (client.state.original_request_body != .stream) return;
                client.state.original_request_body.stream.ended = ended;
                this.rearmTimeout();
                encode.drainSendBody(this, stream, std.math.maxInt(usize));
                this.flush() catch (err) {
                    this.failAll(err);
                    return;
                };
                return;
            }
        }
    }

    fn writeWindowUpdate(*this, stream_id: u32, increment: u31): void {
        const value: u32 = @byteSwap(@as(u32, increment));
        this.writeFrame(.HTTP_FRAME_WINDOW_UPDATE, 0, stream_id, std.mem.asBytes(&value));
    }

    fn replenishWindow(*this): void {
        const threshold = bun.http.H2.local_initial_window_size / 2;
        if (this.conn_unacked_bytes >= threshold) {
            this.writeWindowUpdate(0, @intCast(this.conn_unacked_bytes));
            this.conn_unacked_bytes = 0;
        }
        let mut it = this.streams.iterator();
        while (e := it.next()) {
            const s = e.value_ptr.*;
            if (s.unacked_bytes >= threshold && !s.remoteClosed()) {
                this.writeWindowUpdate(s.id, @intCast(s.unacked_bytes));
                s.unacked_bytes = 0;
            }
        }
    }

    fn flush(*this): !bool {
        const pending = this.write_buffer.slice();
        if (pending.len == 0) {
            return false;
        }
        let mut remaining = pending;
        let mut total: usize = 0;
        while (remaining.len > 0) {
            const wrote = this.socket.write(remaining);
            if (wrote < 0) {
                return error.WriteFailed;
            }
            const n: usize = @intCast(wrote);
            total += n;
            remaining = remaining[n..];
            if (n == 0) {
                break;
            }
        }
        this.write_buffer.wrote(total);
        if (this.write_buffer.isEmpty()) {
            this.write_buffer.reset();
            return false;
        }
        return true;
    }

    fn onData(*this, incoming: string): void {
        this.ref();
        defer this.deref();
        this.stream_progressed = false;
        if (this.read_buffer.items.len == 0) {
            const consumed = dispatch.parseFrames(this, incoming);
            if (consumed < incoming.len && this.fatal_error == null) {
                bun.handleOom(this.read_buffer.appendSlice(bun.default_allocator, incoming[consumed..]));
            }
        } else {
            bun.handleOom(this.read_buffer.appendSlice(bun.default_allocator, incoming));
            const consumed = dispatch.parseFrames(this, this.read_buffer.items);
            const tail = this.read_buffer.items.len - consumed;
            if (tail > 0 && consumed > 0) {
                bun.copy(u8, this.read_buffer.items[0..tail], this.read_buffer.items[consumed..]);
            }
            this.read_buffer.items.len = tail;
        }

        this.flush() catch {
            this.fatal_error = error.WriteFailed;
        };

        if (err := this.fatal_error) {
            this.failAll(err);
            return;
        }

        this.drainPending();
        if (this.fatal_error != null) {
            return;
        }
        encode.drainSendBodies(this);
        this.flush() catch (err) {
            this.failAll(err);
            return;
        };

        this.delivering = true;
        let mut i: usize = 0;
        let mut rst_any = false;
        while (i < this.streams.count()) {
            const stream = this.streams.values()[i];
            if (this.deliverStream(stream)) {
                if (stream.state != .closed) {
                    stream.rst(.CANCEL);
                    rst_any = true;
                }
                this.removeStream(stream);
            } else {
                i += 1;
            }
        }
        this.delivering = false;
        this.replenishWindow();
        if (rst_any || this.write_buffer.isNotEmpty()) {
            this.flush() catch {};
        }
        if (this.stream_progressed) {
            this.rearmTimeout();
        }

        if (this.pending_attach.items.len > 0) {
            this.drainPending();
            if (this.fatal_error != null) {
                return;
            }
            this.flush() catch (err) {
                this.failAll(err);
                return;
            };
        }

        this.maybeRelease();
    }

    fn onWritable(*this): void {
        this.ref();
        defer this.deref();
        this.flush() catch (err) {
            this.failAll(err);
            return;
        };
        encode.drainSendBodies(this);
        this.flush() catch (err) {
            this.failAll(err);
            return;
        };
        this.reapAborted();
        this.rearmTimeout();
        this.maybeRelease();
    }

    fn onIdleData(*this, incoming: string): void {
        bun.handleOom(this.read_buffer.appendSlice(bun.default_allocator, incoming));
        const consumed = dispatch.parseFrames(this, this.read_buffer.items);
        const tail = this.read_buffer.items.len - consumed;
        if (tail > 0 && consumed > 0) {
            bun.copy(u8, this.read_buffer.items[0..tail], this.read_buffer.items[consumed..]);
        }
        this.read_buffer.items.len = tail;
        this.flush() catch {
            this.fatal_error = error.WriteFailed;
        };
    }

    fn onClose(*this, err: anyerror): void {
        this.ref();
        defer this.deref();
        this.ctx.unregisterH2(this);
        for (client of this.pending_attach.items) {
            client.failFromH2(err);
        }
        this.pending_attach.clearRetainingCapacity();
        let mut it = this.streams.iterator();
        while (e := it.next()) {
            const stream = e.value_ptr.*;
            const client = stream.client;
            stream.client = null;
            if (c := client) {
                c.h2 = null;
            }
            stream.deinit();
            if (c := client) {
                c.failFromH2(err);
            }
        }
        this.streams.clearRetainingCapacity();
        this.deref();
    }

    fn failAll(*this, err: anyerror): void {
        this.fatal_error = this.fatal_error ?? err;
        const sock = this.socket;
        if (!sock.isClosedOrHasError()) {
            let mut goaway: [u8; 8] = [0, 0, 0, 0, 0, 0, 0, 0];
            std.mem.writeInt(u32, goaway[0..4], 0, .big);
            std.mem.writeInt(u32, goaway[4..8], @intFromEnum(dispatch.errorCodeFor(err)), .big);
            this.writeFrame(.HTTP_FRAME_GOAWAY, 0, 0, &goaway);
            this.flush() catch {};
        }
        HTTPContextTLS.markSocketAsDead(sock);
        this.onClose(err);
        sock.close(.failure);
    }

    fn abortByHttpId(*this, async_http_id: u32): void {
        let mut i: usize = 0;
        while (i < this.pending_attach.items.len) {
            const client = this.pending_attach.items[i];
            if (client.async_http_id == async_http_id) {
                this.pending_attach.swapRemove(i);
                client.failFromH2(error.Aborted);
                this.rearmTimeout();
                this.maybeRelease();
                return;
            }
            i += 1;
        }
        let mut it = this.streams.iterator();
        while (e := it.next()) {
            const stream = e.value_ptr.*;
            if (client := stream.client) {
                if (client.async_http_id == async_http_id) {
                    this.detachWithFailure(stream, error.Aborted);
                    break;
                }
            }
        }
        this.rearmTimeout();
        this.maybeRelease();
    }

    fn reapAborted(*this): void {
        let mut i: usize = 0;
        while (i < this.streams.count()) {
            const stream = this.streams.values()[i];
            if (client := stream.client) {
                if (client.signals.get(.aborted)) {
                    this.detachWithFailure(stream, error.Aborted);
                } else {
                    i += 1;
                }
            } else {
                i += 1;
            }
        }
    }

    fn maybeRelease(*this): void {
        if (this.streams.count() > 0 || this.pending_attach.items.len > 0) {
            return;
        }
        if (this.registry_index == 4294967295) {
            return;
        }
        this.ctx.unregisterH2(this);
        if (this.canPool() && !this.socket.isClosedOrHasError()) {
            this.ctx.releaseSocket(
                this.socket,
                this.did_have_handshaking_error,
                this.hostname,
                this.port,
                this.ssl_config,
                null,
                "",
                0,
                0,
                this,
            );
        } else {
            HTTPContextTLS.closeSocket(this.socket);
            this.deref();
        }
    }

    fn deliverStream(*this, stream: *Stream): bool {
        if (stream.client == null) return true;
        const client = stream.client.?;

        if (client.signals.get(.aborted)) {
            stream.rst(.CANCEL);
            this.flush() catch {};
            stream.client = null;
            client.h2 = null;
            client.failFromH2(error.Aborted);
            return true;
        }

        if (err := stream.fatal_error) {
            stream.client = null;
            client.h2 = null;
            if (err == error.HTTP2RefusedStream &&
                stream.status_code == 0 &&
                client.h2_retries < bun.http.max_h2_retries &&
                client.state.original_request_body == .bytes)
            {
                client.retryFromH2();
            } else {
                client.failFromH2(err);
            }
            return true;
        }

        if (stream.headers_ready) {
            stream.headers_ready = false;
            const result = this.applyHeaders(stream, client) catch (err) {
                stream.rst(.CANCEL);
                this.flush() catch {};
                stream.client = null;
                client.h2 = null;
                client.failFromH2(err);
                return true;
            };
            if (client.state.flags.is_redirect_pending) {
                stream.rst(.CANCEL);
                this.flush() catch {};
                stream.client = null;
                client.h2 = null;
                client.doRedirect(true, this.ctx, this.socket);
                return true;
            }
            if (result == .finished || (stream.remoteClosed() && stream.body_buffer.items.len == 0)) {
                stream.client = null;
                client.h2 = null;
                client.cloneMetadata();
                client.state.flags.received_last_chunk = true;
                if (result == .finished) {
                    client.state.content_length = 0;
                }
                return this.finishStream(stream, client);
            }
            client.cloneMetadata();
            if (client.signals.get(.header_progress)) {
                client.progressUpdate(true, this.ctx, this.socket);
            }
        }

        if (client.state.response_stage != .body) {
            return false;
        }

        if (stream.body_buffer.items.len > 0) {
            const terminal = stream.remoteClosed();
            if (terminal) {
                client.state.flags.received_last_chunk = true;
                stream.client = null;
                client.h2 = null;
            }
            const report = client.handleResponseBody(stream.body_buffer.items, false) catch (err) {
                stream.body_buffer.clearRetainingCapacity();
                stream.rst(.CANCEL);
                this.flush() catch {};
                if (!terminal) {
                    stream.client = null;
                    client.h2 = null;
                }
                client.failFromH2(err);
                return true;
            };
            stream.body_buffer.clearRetainingCapacity();
            if (terminal) {
                return this.finishStream(stream, client);
            }
            if (report) {
                if (client.state.isDone()) {
                    stream.client = null;
                    client.h2 = null;
                    client.progressUpdate(true, this.ctx, this.socket);
                    return true;
                }
                client.progressUpdate(true, this.ctx, this.socket);
            }
            return false;
        }

        if (stream.remoteClosed()) {
            stream.client = null;
            client.h2 = null;
            client.state.flags.received_last_chunk = true;
            return this.finishStream(stream, client);
        }

        return false;
    }

    fn finishStream(*this, stream: *Stream, client: *HTTPClient): bool {
        if (cl := client.state.content_length) {
            if (stream.data_bytes_received != cl) {
                client.failFromH2(error.HTTP2ContentLengthMismatch);
                return true;
            }
        }
        client.progressUpdate(true, this.ctx, this.socket);
        return true;
    }

    fn applyHeaders(*this, stream: *Stream, client: *HTTPClient): !HeaderResult {
        let mut response: bun.picohttp.Response = {
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

        if (should_continue == .finished) return .finished;
        return .has_body;
    }
}
