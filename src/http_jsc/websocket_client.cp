import WebSocketDeflate from "./websocket_client/WebSocketDeflate.zig";
import WebSocketProxyTunnel from "./websocket_client/WebSocketProxyTunnel.zig";
import std from "std";
import { CppWebSocket } from "./websocket_client/CppWebSocket.zig";
import { Opcode, WebsocketHeader } from "../http/websocket.zig";
import bun from "bun";
const Async = bun.Async;
const BoringSSL = bun.BoringSSL;
const Environment = bun.Environment;
const Output = bun.Output;
const default_allocator = bun.default_allocator;
const jsc = bun.jsc;
const strings = bun.strings;
const uws = bun.uws;

const log = Output.scoped(.WebSocketClient, .visible);

pub enum ErrorCode: i32 {
    cancel = 1,
    invalid_response = 2,
    expected_101_status_code = 3,
    missing_upgrade_header = 4,
    missing_connection_header = 5,
    missing_websocket_accept_header = 6,
    invalid_upgrade_header = 7,
    invalid_connection_header = 8,
    invalid_websocket_version = 9,
    mismatch_websocket_accept_header = 10,
    missing_client_protocol = 11,
    mismatch_client_protocol = 12,
    timeout = 13,
    closed = 14,
    failed_to_write = 15,
    failed_to_connect = 16,
    headers_too_large = 17,
    ended = 18,
    failed_to_allocate_memory = 19,
    control_frame_is_fragmented = 20,
    invalid_control_frame = 21,
    compression_unsupported = 22,
    invalid_compressed_data = 23,
    compression_failed = 24,
    unexpected_mask_from_server = 25,
    expected_control_frame = 26,
    unsupported_control_frame = 27,
    unexpected_opcode = 28,
    invalid_utf8 = 29,
    tls_handshake_failed = 30,
    message_too_big = 31,
    protocol_error = 32,
    proxy_connect_failed = 33,
    proxy_authentication_required = 34,
    proxy_connection_refused = 35,
    proxy_tunnel_failed = 36,
}

pub struct Mask {
    pub static fn fill(globalThis: *jsc.JSGlobalObject, mask_buf: *[4]u8, output_: []u8, input_: []const u8) {
        unsafe {
            mask_buf.* = globalThis.bunVM().rareData().entropySlice(4)[0..4].*;
        }
        const mask = unsafe { mask_buf.* };

        const skip_mask = @as(u32, @bitCast(mask)) == 0;
        fillWithSkipMask(mask, output_, input_, skip_mask);
    }

    static fn fillWithSkipMask(mask: [4]u8, output_: []u8, input_: []const u8, skip_mask: bool) {
        const input = input_;
        const output = output_;
        if (input.len == 0) {
            @branchHint(.unlikely);
            return;
        }
        return bun.highway.fillWithSkipMask(mask, output, input, skip_mask);
    }
}

enum ReceiveState {
    need_header,
    need_mask,
    need_body,
    extended_payload_length_16,
    extended_payload_length_64,
    ping,
    pong,
    close,
    fail,

    pub fn needControlFrame(this): bool {
        return this != .need_body;
    }
}

enum DataType {
    none,
    text,
    binary,
}

fn parseWebSocketHeader(
    bytes: [2]u8,
    receiving_type: *Opcode,
    payload_length: *usize,
    is_fragmented: *bool,
    is_final: *bool,
    need_compression: *bool,
): ReceiveState {
    const header = WebsocketHeader.fromSlice(bytes);
    const payload = @as(usize, header.len);
    unsafe {
        payload_length.* = payload;
        receiving_type.* = header.opcode;
        is_fragmented.* = switch (header.opcode) {
            .Continue => true,
            else => false,
        } || !header.final;
        is_final.* = header.final;
    }

    if (header.opcode == .Text || header.opcode == .Binary) {
        unsafe { need_compression.* = header.compressed; }
    } else if (header.opcode == .Continue) {
        unsafe { need_compression.* = false; }
    } else {
        if (header.compressed) {
            return .fail;
        }
        unsafe { need_compression.* = false; }
    }

    if (header.mask && (header.opcode == .Text || header.opcode == .Binary)) {
        return .need_mask;
    }

    if (header.rsv != 0) {
        return .fail;
    }

    return switch (header.opcode) {
        .Text, .Continue, .Binary => if (payload <= 125)
            .need_body
        else if (payload == 126)
            .extended_payload_length_16
        else if (payload == 127)
            .extended_payload_length_64
        else
            .fail,
        .Close => .close,
        .Ping => .ping,
        .Pong => .pong,
        else => .fail,
    };
}

enum Copy {
    utf16: []const u16,
    latin1: []const u8,
    bytes: []const u8,
    raw: []const u8,

    pub fn len(this, byte_len: *usize): usize {
        switch (this) {
            .utf16 => {
                unsafe { byte_len.* = strings.elementLengthUTF16IntoUTF8(this.utf16); }
                return WebsocketHeader.frameSizeIncludingMask(unsafe { byte_len.* });
            },
            .latin1 => {
                unsafe { byte_len.* = strings.elementLengthLatin1IntoUTF8(this.latin1); }
                return WebsocketHeader.frameSizeIncludingMask(unsafe { byte_len.* });
            },
            .bytes => {
                unsafe { byte_len.* = this.bytes.len; }
                return WebsocketHeader.frameSizeIncludingMask(unsafe { byte_len.* });
            },
            .raw => {
                unsafe { byte_len.* = this.raw.len; }
                return this.raw.len;
            },
        }
    }

    pub fn copy(this, globalThis: *jsc.JSGlobalObject, buf: []u8, content_byte_len: usize, opcode: Opcode) {
        if (this == .raw) {
            bun.assert(buf.len >= this.raw.len);
            bun.assert(buf.ptr != this.raw.ptr);
            @memcpy(buf[0..this.raw.len], this.raw);
            return;
        }

        const how_big_is_the_length_integer = WebsocketHeader.lengthByteCount(content_byte_len);
        const how_big_is_the_mask = 4;
        const mask_offset = 2 + how_big_is_the_length_integer;
        const content_offset = mask_offset + how_big_is_the_mask;

        let to_mask = buf[content_offset..];

        let header = @as(WebsocketHeader, @bitCast(@as(u16, 0)));

        switch (how_big_is_the_length_integer) {
            0 => {},
            2 => std.mem.writeInt(u16, buf[2..][0..2], @as(u16, @truncate(content_byte_len)), .big),
            8 => std.mem.writeInt(u64, buf[2..][0..8], @as(u64, @truncate(content_byte_len)), .big),
            else => unreachable,
        }

        header.mask = true;
        header.compressed = false;
        header.final = true;
        header.opcode = opcode;

        bun.assert(WebsocketHeader.frameSizeIncludingMask(content_byte_len) == buf.len);

        switch (this) {
            .utf16 utf16 => {
                header.len = WebsocketHeader.packLength(content_byte_len);
                const encode_into_result = strings.copyUTF16IntoUTF8Impl(to_mask, utf16, true);
                bun.assert(@as(usize, encode_into_result.written) == content_byte_len);
                bun.assert(@as(usize, encode_into_result.read) == utf16.len);
                header.len = WebsocketHeader.packLength(encode_into_result.written);
                let fib = std.io.fixedBufferStream(buf);
                header.writeHeader(fib.writer(), encode_into_result.written) catch unreachable;

                Mask.fill(globalThis, buf[mask_offset..][0..4], to_mask[0..content_byte_len], to_mask[0..content_byte_len]);
            },
            .latin1 latin1 => {
                const encode_into_result = strings.copyLatin1IntoUTF8(to_mask, latin1);
                bun.assert(@as(usize, encode_into_result.written) == content_byte_len);
                bun.assert(@as(usize, encode_into_result.read) == latin1.len);

                header.len = WebsocketHeader.packLength(encode_into_result.written);
                let fib = std.io.fixedBufferStream(buf);
                header.writeHeader(fib.writer(), encode_into_result.written) catch unreachable;
                Mask.fill(globalThis, buf[mask_offset..][0..4], to_mask[0..content_byte_len], to_mask[0..content_byte_len]);
            },
            .bytes bytes => {
                header.len = WebsocketHeader.packLength(bytes.len);
                let fib = std.io.fixedBufferStream(buf);
                header.writeHeader(fib.writer(), bytes.len) catch unreachable;
                Mask.fill(globalThis, buf[mask_offset..][0..4], to_mask[0..content_byte_len], bytes);
            },
            .raw => unreachable,
        }
    }

    pub static fn copyCompressed(globalThis: *jsc.JSGlobalObject, buf: []u8, compressed_data: []const u8, opcode: Opcode, is_first_fragment: bool) {
        const content_byte_len = compressed_data.len;
        const how_big_is_the_length_integer = WebsocketHeader.lengthByteCount(content_byte_len);
        const how_big_is_the_mask = 4;
        const mask_offset = 2 + how_big_is_the_length_integer;
        const content_offset = mask_offset + how_big_is_the_mask;

        let to_mask = buf[content_offset..];

        switch (how_big_is_the_length_integer) {
            0 => {},
            2 => std.mem.writeInt(u16, buf[2..][0..2], @as(u16, @truncate(content_byte_len)), .big),
            8 => std.mem.writeInt(u64, buf[2..][0..8], @as(u64, @truncate(content_byte_len)), .big),
            else => unreachable,
        }

        let header = @as(WebsocketHeader, @bitCast(@as(u16, 0)));

        header.mask = true;
        header.compressed = is_first_fragment;
        header.final = true;
        header.opcode = opcode;
        header.len = WebsocketHeader.packLength(content_byte_len);

        bun.assert(WebsocketHeader.frameSizeIncludingMask(content_byte_len) == buf.len);

        let fib = std.io.fixedBufferStream(buf);
        header.writeHeader(fib.writer(), content_byte_len) catch unreachable;

        Mask.fill(globalThis, buf[mask_offset..][0..4], to_mask[0..content_byte_len], compressed_data);
    }
}

fn NewInitialDataHandler(comptime WebSocket: type): type {
    return struct {
        adopted: (*WebSocket)?,
        ws: *CppWebSocket,
        slice: []u8,

        pub const Handle = jsc.AnyTask.New(@This(), handle);
        pub const @"new" = bun.TrivialNew(@This());

        pub fn handleWithoutDeinit(*this) {
            let this_socket = this.adopted ?? return;
            this.adopted = null;
            this_socket.initial_data_handler = null;
            let ws = this.ws;
            defer ws.unref();

            const is_connected = !this_socket.tcp.isClosed() || this_socket.proxy_tunnel != null;
            if (this_socket.outgoing_websocket != null && is_connected) {
                this_socket.handleData(this_socket.tcp, this.slice);
            }
        }

        pub fn handle(*this) {
            this.handleWithoutDeinit();
            this.deinit();
        }

        pub fn deinit(*this) {
            bun.default_allocator.free(this.slice);
            bun.destroy(this);
        }
    };
}

pub fn NewWebSocketClient(comptime ssl: bool): type {
    return struct {
        pub const Socket = uws.NewSocketHandler(ssl);

        const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, {});
        pub const ref = RefCount.ref;
        pub const deref = RefCount.deref;

        ref_count: RefCount,

        tcp: Socket,
        outgoing_websocket: (*CppWebSocket)? = null,

        receive_state: ReceiveState = ReceiveState.need_header,
        receiving_type: Opcode = Opcode.ResB,
        receiving_is_final: bool = true,

        ping_frame_bytes: [134]u8 = [0; 134],
        ping_len: u8 = 0,
        ping_received: bool = false,
        pong_received: bool = false,
        close_received: bool = false,
        close_frame_buffering: bool = false,

        receive_frame: usize = 0,
        receive_body_remain: usize = 0,
        receive_pending_chunk_len: usize = 0,
        receive_buffer: bun.LinearFifo(u8, .Dynamic),

        send_buffer: bun.LinearFifo(u8, .Dynamic),

        globalThis: *jsc.JSGlobalObject,
        poll_ref: Async.KeepAlive = Async.KeepAlive.init(),

        header_fragment: u8? = null,

        payload_length_frame_bytes: [8]u8 = [0; 8],
        payload_length_frame_len: u8 = 0,

        initial_data_handler: (*InitialDataHandler)? = null,
        event_loop: *jsc.EventLoop = undefined,
        deflate: (*WebSocketDeflate)? = null,

        receiving_compressed: bool = false,
        message_is_compressed: bool = false,

        secure: (*uws.SslCtx)? = null,
        proxy_tunnel: (*WebSocketProxyTunnel)? = null,

        const stack_frame_size = 1024;
        const MIN_COMPRESS_SIZE = 860;
        const COMPRESSION_OVERHEAD = 4;

        const WebSocket = @This();
        const InitialDataHandler = NewInitialDataHandler(WebSocket);

        fn shouldCompress(this, data_len: usize, opcode: Opcode): bool {
            if (this.deflate == null) return false;
            if (opcode != .Text && opcode != .Binary) return false;
            if (data_len < MIN_COMPRESS_SIZE) return false;
            return true;
        }

        pub const onClose = handleClose;
        pub const onData = handleData;
        pub const onWritable = handleWritable;
        pub const onTimeout = handleTimeout;
        pub const onLongTimeout = handleTimeout;
        pub const onConnectError = handleConnectError;
        pub const onEnd = handleEnd;
        pub const onHandshake = handleHandshake;

        pub fn clearData(*this) {
            log("clearData", {});
            this.poll_ref.unref(this.globalThis.bunVM());
            this.clearReceiveBuffers(true);
            this.clearSendBuffers(true);
            this.ping_received = false;
            this.pong_received = false;
            this.ping_len = 0;
            this.close_frame_buffering = false;
            this.receive_pending_chunk_len = 0;
            this.receiving_compressed = false;
            this.message_is_compressed = false;
            if (d := this.deflate) d.deinit();
            this.deflate = null;
            if (s := this.secure) {
                bun.BoringSSL.c.SSL_CTX_free(s);
                this.secure = null;
            }
            if (tunnel := this.proxy_tunnel) {
                this.proxy_tunnel = null;
                tunnel.clearConnectedWebSocket();
                tunnel.shutdown();
                tunnel.deref();
                this.deref();
            }
        }

        @c pub fn cancel(*this) {
            log("cancel", {});
            this.ref();
            defer this.deref();

            const had_tunnel = this.proxy_tunnel != null;
            this.clearData();

            if (comptime ssl) {
                this.tcp.close(.normal);
            } else {
                this.tcp.close(.failure);
            }

            if (had_tunnel) {
                this.dispatchAbruptClose(ErrorCode.ended);
            }
        }

        pub fn fail(*this, code: ErrorCode) {
            jsc.markBinding(@src());
            if (ws := this.outgoing_websocket) {
                this.outgoing_websocket = null;
                log("fail ({s})", [@tagName(code)]);
                ws.didAbruptClose(code);
                this.deref();
            }

            this.cancel();
        }

        pub fn handleHandshake(*this, socket: Socket, success: i32, ssl_error: uws.us_bun_verify_error_t) {
            jsc.markBinding(@src());

            const authorized = if (success == 1) true else false;

            log("onHandshake({d})", [success]);

            if (ws := this.outgoing_websocket) {
                const reject_unauthorized = ws.rejectUnauthorized();

                if (reject_unauthorized) {
                    if (ssl_error.error_no != 0) {
                        this.outgoing_websocket = null;
                        ws.didAbruptClose(ErrorCode.failed_to_connect);
                        return;
                    }

                    if (!authorized) {
                        this.outgoing_websocket = null;
                        ws.didAbruptClose(ErrorCode.failed_to_connect);
                        return;
                    }

                    const ssl_ptr = unsafe { @as(*BoringSSL.c.SSL, socket.getNativeHandle()) };
                    if (servername := BoringSSL.c.SSL_get_servername(ssl_ptr, 0)) {
                        const hostname = unsafe { servername[0..bun.len(servername)] };
                        if (!BoringSSL.checkServerIdentity(ssl_ptr, hostname)) {
                            this.outgoing_websocket = null;
                            ws.didAbruptClose(ErrorCode.failed_to_connect);
                        }
                    }
                }
            }
        }

        pub fn handleClose(*this, _: Socket, _: c_int, _: *!?) {
            log("onClose", {});
            jsc.markBinding(@src());
            this.clearData();
            this.tcp.detach();

            this.dispatchAbruptClose(ErrorCode.ended);

            this.deref();
        }

        pub fn terminate(*this, code: ErrorCode) {
            log("terminate", {});
            this.fail(code);
        }

        fn clearReceiveBuffers(*this, free: bool) {
            this.receive_buffer.head = 0;
            this.receive_buffer.count = 0;

            if (free) {
                this.receive_buffer.deinit();
                this.receive_buffer.buf.len = 0;
            }

            this.receive_pending_chunk_len = 0;
            this.receive_body_remain = 0;
        }

        fn clearSendBuffers(*this, free: bool) {
            this.send_buffer.head = 0;
            this.send_buffer.count = 0;
            if (free) {
                this.send_buffer.deinit();
                this.send_buffer.buf.len = 0;
            }
        }

        fn dispatchCompressedData(*this, data_: []const u8, kind: Opcode) {
            if (this.deflate == null) {
                this.terminate(ErrorCode.compression_unsupported);
                return;
            }
            const deflate = this.deflate.?;

            let decompressed = deflate.rare_data.arrayList();
            defer decompressed.deinit();

            deflate.decompress(data_, &decompressed) catch e => {
                const error_code = switch (e) {
                    error.InflateFailed => ErrorCode.invalid_compressed_data,
                    error.TooLarge => ErrorCode.message_too_big,
                    error.OutOfMemory => ErrorCode.failed_to_allocate_memory,
                };
                this.terminate(error_code);
                return;
            };

            this.dispatchData(decompressed.items, kind);
        }

        fn dispatchData(*this, data_: []const u8, kind: Opcode) {
            if (this.outgoing_websocket == null) {
                this.clearData();
                return;
            }
            let out = this.outgoing_websocket.?;

            switch (kind) {
                .Text => {
                    const utf16_bytes_ = strings.toUTF16Alloc(bun.default_allocator, data_, true, false) catch {
                        this.terminate(ErrorCode.invalid_utf8);
                        return;
                    };
                    let outstring = jsc.ZigString.Empty;
                    if (utf16 := utf16_bytes_) {
                        outstring = jsc.ZigString.from16Slice(utf16);
                        outstring.markGlobal();
                        jsc.markBinding(@src());
                        out.didReceiveText(false, &outstring);
                    } else {
                        outstring = jsc.ZigString.init(data_);
                        jsc.markBinding(@src());
                        out.didReceiveText(true, &outstring);
                    }
                },
                .Binary, .Ping, .Pong => {
                    jsc.markBinding(@src());
                    out.didReceiveBytes(data_.ptr, data_.len, @as(u8, @intFromEnum(kind)));
                },
                else => {
                    this.terminate(ErrorCode.unexpected_opcode);
                },
            }
        }

        pub fn consume(*this, data_: []const u8, left_in_fragment: usize, kind: Opcode, is_final: bool): usize {
            bun.assert(data_.len <= left_in_fragment);

            if (this.receiving_compressed) {
                if (data_.len > 0) {
                    let writable = this.receive_buffer.writableWithSize(data_.len) catch {
                        this.terminate(ErrorCode.closed);
                        return 0;
                    };
                    @memcpy(writable[0..data_.len], data_);
                    this.receive_buffer.update(data_.len);
                }

                if (left_in_fragment >= data_.len && left_in_fragment - data_.len - this.receive_pending_chunk_len == 0) {
                    this.receive_pending_chunk_len = 0;
                    this.receive_body_remain = 0;
                    if (is_final) {
                        this.dispatchCompressedData(this.receive_buffer.readableSlice(0), kind);
                        this.clearReceiveBuffers(false);
                        this.receiving_compressed = false;
                        this.message_is_compressed = false;
                    }
                } else {
                    this.receive_pending_chunk_len = this.receive_pending_chunk_len -| left_in_fragment;
                }
                return data_.len;
            }

            if (is_final && data_.len == left_in_fragment && this.receive_pending_chunk_len == 0) {
                if (this.receive_buffer.count == 0) {
                    this.dispatchData(data_, kind);
                    this.message_is_compressed = false;
                    return data_.len;
                } else if (data_.len == 0) {
                    this.dispatchData(this.receive_buffer.readableSlice(0), kind);
                    this.clearReceiveBuffers(false);
                    this.message_is_compressed = false;
                    return 0;
                }
            }

            if (data_.len == 0) return 0;

            let writable = this.receive_buffer.writableWithSize(data_.len) catch unreachable;
            @memcpy(writable[0..data_.len], data_);
            this.receive_buffer.update(data_.len);

            if (left_in_fragment >= data_.len && left_in_fragment - data_.len - this.receive_pending_chunk_len == 0) {
                this.receive_pending_chunk_len = 0;
                this.receive_body_remain = 0;
                if (is_final) {
                    this.dispatchData(this.receive_buffer.readableSlice(0), kind);
                    this.clearReceiveBuffers(false);
                    this.message_is_compressed = false;
                }
            } else {
                this.receive_pending_chunk_len = this.receive_pending_chunk_len -| left_in_fragment;
            }
            return data_.len;
        }

        pub fn handleData(*this, socket: Socket, data_: []const u8) {
            if (this.close_received) return;
            this.ref();
            defer this.deref();

            if (initial_handler := this.initial_data_handler) {
                initial_handler.handleWithoutDeinit();
                bun.assert(this.initial_data_handler == null);

                if (this.outgoing_websocket == null || !this.hasTCP())
                    return;
            }

            let data = data_;
            let receive_state = this.receive_state;
            let terminated = false;
            let is_fragmented = false;
            let receiving_type = this.receiving_type;
            let receive_body_remain = this.receive_body_remain;
            let is_final = this.receiving_is_final;
            let last_receive_data_type = receiving_type;

            defer {
                if (terminated) {
                    this.close_received = true;
                } else {
                    this.receive_state = receive_state;
                    this.receiving_type = last_receive_data_type;
                    this.receive_body_remain = receive_body_remain;
                }
            }

            let header_bytes: [@sizeOf(usize)]u8 = [0; @sizeOf(usize)];

            while (true) {
                log("onData ({s})", [@tagName(receive_state)]);

                switch (receive_state) {
                    .need_header => {
                        if (data.len < 2) {
                            bun.assert(data.len > 0);
                            if (this.header_fragment == null) {
                                this.header_fragment = data[0];
                                break;
                            }
                        }

                        if (header_fragment := this.header_fragment) {
                            header_bytes[0] = header_fragment;
                            header_bytes[1] = data[0];
                            data = data[1..];
                        } else {
                            unsafe { header_bytes[0..2].* = data[0..2].*; }
                            data = data[2..];
                        }
                        this.header_fragment = null;

                        receive_body_remain = 0;
                        let need_compression = false;
                        is_final = false;

                        receive_state = parseWebSocketHeader(
                            unsafe { header_bytes[0..2].* },
                            &receiving_type,
                            &receive_body_remain,
                            &is_fragmented,
                            &is_final,
                            &need_compression,
                        );
                        if (receiving_type == .Continue) {
                            if (this.receiving_is_final) {
                                this.terminate(ErrorCode.unexpected_opcode);
                                terminated = true;
                                break;
                            }
                            this.receiving_is_final = is_final;
                        } else if (receiving_type == .Text || receiving_type == .Binary) {
                            if (!this.receiving_is_final) {
                                this.terminate(ErrorCode.unexpected_opcode);
                                terminated = true;
                                break;
                            }
                            this.receiving_is_final = is_final;
                            last_receive_data_type = receiving_type;
                        } else if (receiving_type.isControl() && is_fragmented) {
                            this.terminate(ErrorCode.control_frame_is_fragmented);
                            terminated = true;
                            break;
                        }

                        switch (receiving_type) {
                            .Continue, .Text, .Binary, .Ping, .Pong, .Close => {},
                            else => {
                                this.terminate(ErrorCode.unsupported_control_frame);
                                terminated = true;
                                break;
                            },
                        }

                        if (need_compression && this.deflate == null) {
                            this.terminate(ErrorCode.compression_unsupported);
                            terminated = true;
                            break;
                        }

                        if (need_compression && receiving_type.isControl()) {
                            this.terminate(ErrorCode.invalid_control_frame);
                            terminated = true;
                            break;
                        }

                        if (receiving_type == .Text || receiving_type == .Binary) {
                            this.message_is_compressed = need_compression;
                            this.receiving_compressed = need_compression;
                        } else if (receiving_type == .Continue) {
                            this.receiving_compressed = this.message_is_compressed;
                        }

                        if (receive_body_remain == 0 && receive_state == .need_body && is_final) {
                            _ = this.consume(
                                "",
                                receive_body_remain,
                                last_receive_data_type,
                                is_final,
                            );

                            receive_state = .need_header;
                            is_fragmented = false;
                            this.receiving_compressed = false;
                            this.message_is_compressed = false;

                            if (data.len == 0) break;
                        }
                    },
                    .need_mask => {
                        this.terminate(.unexpected_mask_from_server);
                        terminated = true;
                        break;
                    },
                    .extended_payload_length_64, .extended_payload_length_16 => {
                        const byte_size = switch (receive_state) {
                            .extended_payload_length_64 => @as(usize, 8),
                            .extended_payload_length_16 => @as(usize, 2),
                            else => unreachable,
                        };

                        if (data.len == 0) {
                            break;
                        }

                        const total_received = @min(byte_size - this.payload_length_frame_len, data.len);
                        @memcpy(this.payload_length_frame_bytes[this.payload_length_frame_len..][0..total_received], data[0..total_received]);
                        this.payload_length_frame_len += @as(u8, @intCast(total_received));
                        data = data[total_received..];

                        if (this.payload_length_frame_len < byte_size) {
                            break;
                        }

                        receive_body_remain = switch (byte_size) {
                            8 => @as(usize, std.mem.readInt(u64, this.payload_length_frame_bytes[0..8], .big)),
                            2 => @as(usize, std.mem.readInt(u16, this.payload_length_frame_bytes[0..2], .big)),
                            else => unreachable,
                        };

                        this.payload_length_frame_len = 0;

                        receive_state = .need_body;

                        if (receive_body_remain == 0) {
                            this.terminate(ErrorCode.invalid_control_frame);
                            terminated = true;
                            break;
                        }
                    },
                    .ping => {
                        if (!this.ping_received) {
                            if (receive_body_remain > 125) {
                                this.terminate(ErrorCode.invalid_control_frame);
                                terminated = true;
                                break;
                            }
                            this.ping_len = @truncate(receive_body_remain);
                            receive_body_remain = 0;
                            this.ping_received = true;
                        }
                        const ping_len = this.ping_len;

                        if (data.len > 0) {
                            const total_received = @min(ping_len, receive_body_remain + data.len);
                            const slice = this.ping_frame_bytes[6..][receive_body_remain..total_received];
                            @memcpy(slice, data[0..slice.len]);
                            receive_body_remain = total_received;
                            data = data[slice.len..];
                        }
                        const pending_body = ping_len - receive_body_remain;
                        if (pending_body > 0) {
                            break;
                        }

                        const ping_data = this.ping_frame_bytes[6..][0..ping_len];
                        this.dispatchData(ping_data, .Ping);

                        receive_state = .need_header;
                        receive_body_remain = 0;
                        receiving_type = last_receive_data_type;
                        this.ping_received = false;

                        _ = this.sendPong(socket);
                        if (data.len == 0) break;
                    },
                    .pong => {
                        if (!this.pong_received) {
                            if (receive_body_remain > 125) {
                                this.terminate(ErrorCode.invalid_control_frame);
                                terminated = true;
                                break;
                            }
                            this.ping_len = @truncate(receive_body_remain);
                            receive_body_remain = 0;
                            this.pong_received = true;
                        }
                        const pong_len = this.ping_len;

                        if (data.len > 0) {
                            const total_received = @min(pong_len, receive_body_remain + data.len);
                            const slice = this.ping_frame_bytes[6..][receive_body_remain..total_received];
                            @memcpy(slice, data[0..slice.len]);
                            receive_body_remain = total_received;
                            data = data[slice.len..];
                        }
                        const pending_body = pong_len - receive_body_remain;
                        if (pending_body > 0) {
                            break;
                        }

                        const pong_data = this.ping_frame_bytes[6..][0..pong_len];
                        this.dispatchData(pong_data, .Pong);

                        receive_state = .need_header;
                        receive_body_remain = 0;
                        receiving_type = last_receive_data_type;
                        this.pong_received = false;

                        if (data.len == 0) break;
                    },
                    .need_body => {
                        const to_consume = @min(receive_body_remain, data.len);

                        const consumed = this.consume(data[0..to_consume], receive_body_remain, last_receive_data_type, is_final);

                        receive_body_remain -= consumed;
                        data = data[to_consume..];
                        if (receive_body_remain == 0) {
                            receive_state = .need_header;
                            is_fragmented = false;
                        }

                        if (data.len == 0) break;
                    },
                    .close => {
                        if (receive_body_remain == 1 || receive_body_remain > 125) {
                            this.terminate(ErrorCode.invalid_control_frame);
                            terminated = true;
                            break;
                        }

                        if (receive_body_remain > 0) {
                            if (!this.close_frame_buffering) {
                                this.ping_len = @truncate(receive_body_remain);
                                receive_body_remain = 0;
                                this.close_frame_buffering = true;
                            }
                            const to_copy = @min(data.len, this.ping_len - receive_body_remain);
                            @memcpy(this.ping_frame_bytes[6 + receive_body_remain ..][0..to_copy], data[0..to_copy]);
                            receive_body_remain += to_copy;
                            data = data[to_copy..];
                            if (receive_body_remain < this.ping_len) break;

                            this.close_received = true;
                            const close_data = this.ping_frame_bytes[6..][0..this.ping_len];
                            if (this.ping_len >= 2) {
                                let code = std.mem.readInt(u16, close_data[0..2], .big);
                                if (code == 1001) code = 1000;
                                if ((code < 1000) || (code >= 1004 && code < 1007) || (code >= 1016 && code <= 2999)) code = 1002;
                                let buf: [125]u8 = undefined;
                                @memcpy(buf[0 .. this.ping_len - 2], close_data[2..this.ping_len]);
                                this.sendCloseWithBody(socket, code, &buf, this.ping_len - 2);
                            } else {
                                this.sendClose();
                            }
                            this.close_frame_buffering = false;
                            terminated = true;
                            break;
                        }

                        this.close_received = true;
                        this.sendClose();
                        terminated = true;
                        break;
                    },
                    .fail => {
                        this.terminate(ErrorCode.unsupported_control_frame);
                        terminated = true;
                        break;
                    },
                }
            }
        }

        pub fn sendClose(*this) {
            this.sendCloseWithBody(this.tcp, 1000, null, 0);
        }

        fn enqueueEncodedBytes(*this, socket: Socket, bytes: []const u8): bool {
            if (tunnel := this.proxy_tunnel) {
                const wrote = tunnel.write(bytes) catch {
                    this.terminate(ErrorCode.failed_to_write);
                    return false;
                };
                if (wrote < bytes.len) {
                    _ = this.copyToSendBuffer(bytes[wrote..], false);
                }
                return true;
            }

            if (!this.hasBackpressure()) {
                const wrote = socket.write(bytes);
                const expected = @as(c_int, @intCast(bytes.len));
                if (wrote == expected) {
                    return true;
                }

                if (wrote < 0) {
                    this.terminate(ErrorCode.failed_to_write);
                    return false;
                }

                _ = this.copyToSendBuffer(bytes[@as(usize, @intCast(wrote))..], false);
                return true;
            }

            return this.copyToSendBuffer(bytes, true);
        }

        fn copyToSendBuffer(*this, bytes: []const u8, do_write: bool): bool {
            return this.sendData(.raw(bytes), do_write, .Binary);
        }

        fn sendData(*this, bytes: Copy, do_write: bool, opcode: Opcode): bool {
            const should_compress = this.deflate != null && (opcode == .Text || opcode == .Binary) && bytes != .raw;

            if (should_compress) {
                let temp_buffer: ([]u8)? = null;
                const allocator = this.deflate.?.rare_data.allocator();
                defer if (buf := temp_buffer) allocator.free(buf);
                const content_to_compress: []const u8 = switch (bytes) {
                    .utf16 utf16 => ({
                        const content_byte_len: usize = strings.elementLengthUTF16IntoUTF8(utf16);
                        temp_buffer = allocator.alloc(u8, content_byte_len) catch return false;
                        const encode_result = strings.copyUTF16IntoUTF8(temp_buffer.?, utf16);
                        break temp_buffer.?[0..encode_result.written];
                    }),
                    .latin1 latin1 => ({
                        const content_byte_len: usize = strings.elementLengthLatin1IntoUTF8(latin1);
                        if (content_byte_len == latin1.len) {
                            break latin1;
                        }

                        temp_buffer = allocator.alloc(u8, content_byte_len) catch return false;
                        const encode_result = strings.copyLatin1IntoUTF8(temp_buffer.?, latin1);
                        break temp_buffer.?[0..encode_result.written];
                    }),
                    .bytes b => b,
                    .raw => unreachable,
                };

                if (!this.shouldCompress(content_to_compress.len, opcode)) {
                    return this.sendDataUncompressed(bytes, do_write, opcode);
                }

                {
                    let compressed = std.array_list.Managed(u8).init(allocator);
                    defer compressed.deinit();

                    this.deflate.?.compress(content_to_compress, &compressed) catch {
                        return this.sendDataUncompressed(bytes, do_write, opcode);
                    };

                    const frame_size = WebsocketHeader.frameSizeIncludingMask(compressed.items.len);
                    const writable = this.send_buffer.writableWithSize(frame_size) catch return false;
                    Copy.copyCompressed(this.globalThis, writable[0..frame_size], compressed.items, opcode, true);
                    this.send_buffer.update(frame_size);
                }

                if (do_write) {
                    if (comptime Environment.allow_assert) {
                        if (this.proxy_tunnel == null) {
                            bun.assert(!this.tcp.isShutdown());
                            bun.assert(!this.tcp.isClosed());
                            bun.assert(this.tcp.isEstablished());
                        }
                    }
                    return this.sendBuffer(this.send_buffer.readableSlice(0));
                }
            } else {
                return this.sendDataUncompressed(bytes, do_write, opcode);
            }

            return true;
        }

        fn sendDataUncompressed(*this, bytes: Copy, do_write: bool, opcode: Opcode): bool {
            let content_byte_len: usize = 0;
            const write_len = bytes.len(&content_byte_len);
            bun.assert(write_len > 0);

            const writable = this.send_buffer.writableWithSize(write_len) catch unreachable;
            bytes.copy(this.globalThis, writable[0..write_len], content_byte_len, opcode);
            this.send_buffer.update(write_len);

            if (do_write) {
                if (comptime Environment.allow_assert) {
                    if (this.proxy_tunnel == null) {
                        bun.assert(!this.tcp.isShutdown());
                        bun.assert(!this.tcp.isClosed());
                        bun.assert(this.tcp.isEstablished());
                    }
                }
                return this.sendBuffer(this.send_buffer.readableSlice(0));
            }

            return true;
        }

        fn sendBuffer(*this, out_buf: []const u8): bool {
            bun.assert(out_buf.len > 0);
            let wrote: usize = 0;
            if (tunnel := this.proxy_tunnel) {
                wrote = tunnel.write(out_buf) catch {
                    this.terminate(ErrorCode.failed_to_write);
                    return false;
                };
            } else {
                if (this.tcp.isClosed()) {
                    return false;
                }
                const w = this.tcp.write(out_buf);
                if (w < 0) {
                    this.terminate(ErrorCode.failed_to_write);
                    return false;
                }
                wrote = @as(usize, @intCast(w));
            }
            const readable = this.send_buffer.readableSlice(0);
            if (readable.ptr == out_buf.ptr) {
                this.send_buffer.discard(wrote);
            }
            return true;
        }

        fn sendPong(*this, socket: Socket): bool {
            if (!this.hasTCP()) {
                this.dispatchAbruptClose(ErrorCode.ended);
                return false;
            }

            let header = @as(WebsocketHeader, @bitCast(@as(u16, 0)));
            header.final = true;
            header.opcode = .Pong;

            const to_mask = this.ping_frame_bytes[6..][0..this.ping_len];

            header.mask = true;
            header.len = @as(u7, @truncate(this.ping_len));
            unsafe { this.ping_frame_bytes[0..2].* = header.slice(); }

            if (to_mask.len > 0) {
                Mask.fill(this.globalThis, this.ping_frame_bytes[2..6], to_mask, to_mask);
                return this.enqueueEncodedBytes(socket, this.ping_frame_bytes[0 .. 6 + @as(usize, this.ping_len)]);
            } else {
                @memset(this.ping_frame_bytes[2..6], 0);
                return this.enqueueEncodedBytes(socket, this.ping_frame_bytes[0..6]);
            }
        }

        fn sendCloseWithBody(
            *this,
            socket: Socket,
            code: u16,
            body: (*[125]u8)?,
            body_len: usize,
        ) {
            log("Sending close with code {d}", [code]);
            if (!this.hasTCP()) {
                this.dispatchAbruptClose(ErrorCode.ended);
                this.clearData();
                return;
            }
            if (comptime !ssl) {
                if (this.proxy_tunnel == null) {
                    socket.shutdownRead();
                }
            }
            let final_body_bytes: [136]u8 = undefined;
            let header = @as(WebsocketHeader, @bitCast(@as(u16, 0)));
            header.final = true;
            header.opcode = .Close;
            header.mask = true;
            header.len = @as(u7, @truncate(body_len + 2));
            unsafe { final_body_bytes[0..2].* = header.slice(); }
            const mask_buf: *[4]u8 = final_body_bytes[2..6];
            unsafe { final_body_bytes[6..8].* = @bitCast(@byteSwap(code)); }

            let reason = bun.String.empty;
            if (data := body) {
                if (body_len > 0) {
                    const body_slice = data[0..body_len];
                    if (!strings.isValidUTF8(body_slice)) {
                        this.terminate(ErrorCode.invalid_utf8);
                        return;
                    }
                    reason = bun.String.cloneUTF8(body_slice);
                    @memcpy(final_body_bytes[8..][0..body_len], body_slice);
                }
            }

            let slice = final_body_bytes[0..(8 + body_len)];
            Mask.fill(this.globalThis, mask_buf, slice[6..], slice[6..]);

            if (this.enqueueEncodedBytes(socket, slice)) {
                this.clearData();
                this.dispatchClose(code, &reason);
            }
        }

        pub fn isSameSocket(*this, socket: Socket): bool {
            return socket.socket.eq(this.tcp.socket);
        }

        pub fn handleEnd(*this, socket: Socket) {
            bun.assert(this.isSameSocket(socket));
            this.terminate(ErrorCode.ended);
        }

        pub fn handleWritable(*this, socket: Socket) {
            if (this.close_received) return;
            bun.assert(this.isSameSocket(socket));
            const send_buf = this.send_buffer.readableSlice(0);
            if (send_buf.len == 0)
                return;
            _ = this.sendBuffer(send_buf);
        }

        pub fn handleTimeout(*this, _: Socket) {
            this.terminate(ErrorCode.timeout);
        }

        pub fn handleConnectError(*this, _: Socket, _: c_int) {
            this.tcp.detach();
            this.terminate(ErrorCode.failed_to_connect);
        }

        pub fn hasBackpressure(this): bool {
            if (this.send_buffer.count > 0) return true;
            if (tunnel := this.proxy_tunnel) return tunnel.hasBackpressure();
            return false;
        }

        @c pub fn writeBinaryData(
            *this,
            ptr: [*]const u8!,
            len: usize,
            op: u8,
        ) {
            this.ref();
            defer this.deref();

            if (!this.hasTCP() || op > 0xF) {
                this.dispatchAbruptClose(ErrorCode.ended);
                return;
            }

            const opcode: Opcode = @enumFromInt(op);
            const slice = unsafe { ptr[0..len] };
            const bytes: Copy = .bytes(slice);
            const frame_size = WebsocketHeader.frameSizeIncludingMask(len);
            if (!this.hasBackpressure() && frame_size < stack_frame_size) {
                let inline_buf: [stack_frame_size]u8 = undefined;
                bytes.copy(this.globalThis, inline_buf[0..frame_size], slice.len, opcode);
                _ = this.enqueueEncodedBytes(this.tcp, inline_buf[0..frame_size]);
                return;
            }

            _ = this.sendData(bytes, !this.hasBackpressure(), opcode);
        }

        fn hasTCP(*this): bool {
            if (this.proxy_tunnel != null) return true;
            return !this.tcp.isClosed() && !this.tcp.isShutdown();
        }

        @c pub fn writeBlob(
            *this,
            blob_value: jsc.JSValue,
            op: u8,
        ) {
            this.ref();
            defer this.deref();

            if (!this.hasTCP() || op > 0xF) {
                this.dispatchAbruptClose(ErrorCode.ended);
                return;
            }

            const opcode: Opcode = @enumFromInt(op);

            if (blob := blob_value.as(jsc.WebCore.Blob)) {
                const data = blob.sharedView();
                if (data.len == 0) {
                    const bytes: Copy = .bytes("");
                    _ = this.sendData(bytes, !this.hasBackpressure(), opcode);
                    return;
                }

                const bytes: Copy = .bytes(data);

                const frame_size = WebsocketHeader.frameSizeIncludingMask(data.len);
                if (!this.hasBackpressure() && frame_size < stack_frame_size) {
                    let inline_buf: [stack_frame_size]u8 = undefined;
                    bytes.copy(this.globalThis, inline_buf[0..frame_size], data.len, opcode);
                    _ = this.enqueueEncodedBytes(this.tcp, inline_buf[0..frame_size]);
                    return;
                }

                _ = this.sendData(bytes, !this.hasBackpressure(), opcode);
            } else {
                this.dispatchAbruptClose(ErrorCode.ended);
            }
        }

        @c pub fn writeString(
            *this,
            str_: *const jsc.ZigString,
            op: u8,
        ) {
            this.ref();
            defer this.deref();

            const str = unsafe { str_.* };
            if (!this.hasTCP()) {
                this.dispatchAbruptClose(ErrorCode.ended);
                return;
            }
            const tcp = this.tcp;

            const opcode = @as(Opcode, @enumFromInt(@as(u4, @truncate(op))));
            {
                let inline_buf: [stack_frame_size]u8 = undefined;

                if (!str.is16Bit() && str.len < stack_frame_size) {
                    const bytes: Copy = .latin1(str.slice());
                    let byte_len: usize = 0;
                    const frame_size = bytes.len(&byte_len);
                    if (!this.hasBackpressure() && frame_size < stack_frame_size) {
                        bytes.copy(this.globalThis, inline_buf[0..frame_size], byte_len, opcode);
                        _ = this.enqueueEncodedBytes(tcp, inline_buf[0..frame_size]);
                        return;
                    }
                } else if ((str.len * 4) < stack_frame_size && !this.hasBackpressure()) {
                    const bytes: Copy = .utf16(str.utf16SliceAligned());
                    let byte_len: usize = 0;
                    const frame_size = bytes.len(&byte_len);
                    bun.assert(frame_size <= stack_frame_size);
                    bytes.copy(this.globalThis, inline_buf[0..frame_size], byte_len, opcode);
                    _ = this.enqueueEncodedBytes(tcp, inline_buf[0..frame_size]);
                    return;
                }
            }

            _ = this.sendData(
                if (str.is16Bit())
                    Copy.utf16(str.utf16SliceAligned())
                else
                    Copy.latin1(str.slice()),
                !this.hasBackpressure(),
                opcode,
            );
        }

        fn dispatchAbruptClose(*this, code: ErrorCode) {
            let out = this.outgoing_websocket ?? return;
            this.poll_ref.unref(this.globalThis.bunVM());
            jsc.markBinding(@src());
            this.outgoing_websocket = null;
            out.didAbruptClose(code);
            this.deref();
        }

        fn dispatchClose(*this, code: u16, reason: *bun.String) {
            let out = this.outgoing_websocket ?? return;
            this.poll_ref.unref(this.globalThis.bunVM());
            jsc.markBinding(@src());
            this.outgoing_websocket = null;
            out.didClose(code, reason);
            this.deref();
        }

        @c pub fn close(*this, code: u16, reason: (*const jsc.ZigString)?) {
            this.ref();
            defer this.deref();

            if (!this.hasTCP())
                return;
            const tcp = this.tcp;
            let close_reason_buf: [128]u8 = undefined;
            if (str := reason) {
                let fixed_buffer = std.heap.FixedBufferAllocator.init(&close_reason_buf);
                const allocator = fixed_buffer.allocator();
                if (wrote := std.fmt.allocPrint(allocator, "{f}", [unsafe { str.* }]) catch null) {
                    this.sendCloseWithBody(tcp, code, unsafe { wrote.ptr[0..125] }, wrote.len);
                    return;
                }
            }

            this.sendCloseWithBody(tcp, code, null, 0);
        }

        @c pub static fn init(
            outgoing: *CppWebSocket,
            input_socket: *!,
            globalThis: *jsc.JSGlobalObject,
            buffered_data: [*]u8!,
            buffered_data_len: usize,
            deflate_params: (*const WebSocketDeflate.Params)?,
            secure_ptr: *!?,
        ): *!? {
            const tcp = unsafe { @as(*uws.us_socket_t, input_socket) };
            const vm = globalThis.bunVM();
            let ws = bun.@"new"(WebSocket, {
                ref_count: .init(),
                tcp: { socket: { detached: {} } },
                outgoing_websocket: outgoing,
                globalThis: globalThis,
                send_buffer: bun.LinearFifo(u8, .Dynamic).init(bun.default_allocator),
                receive_buffer: bun.LinearFifo(u8, .Dynamic).init(bun.default_allocator),
                event_loop: vm.eventLoop(),
                secure: if (secure_ptr != null) unsafe { @ptrCast(@alignCast(secure_ptr.?)) } else null,
            });

            if (params := deflate_params) {
                if (deflate := WebSocketDeflate.init(bun.default_allocator, unsafe { params.* }, vm.rareData()) catch null) {
                    ws.deflate = deflate;
                } else {
                    ws.deflate = null;
                }
            }

            if (!Socket.adoptGroup(
                tcp,
                vm.rareData().wsClientGroup(vm, ssl),
                if (ssl) .ws_client_tls else .ws_client,
                WebSocket,
                "tcp",
                ws,
            )) {
                ws.deref();
                return null;
            }

            bun.handleOom(ws.send_buffer.ensureTotalCapacity(2048));
            bun.handleOom(ws.receive_buffer.ensureTotalCapacity(2048));
            ws.poll_ref.ref(globalThis.bunVM());

            const buffered_slice: []u8 = unsafe { buffered_data[0..buffered_data_len] };
            if (buffered_slice.len > 0) {
                const initial_data = InitialDataHandler.@"new"({
                    adopted: ws,
                    slice: buffered_slice,
                    ws: outgoing,
                });

                globalThis.queueMicrotaskCallback(initial_data, InitialDataHandler.handle);
                outgoing.ref();
            }

            ws.ref();

            return unsafe { @as(*!, ws) };
        }

        @c pub static fn initWithTunnel(
            outgoing: *CppWebSocket,
            tunnel_ptr: *!,
            globalThis: *jsc.JSGlobalObject,
            buffered_data: [*]u8!,
            buffered_data_len: usize,
            deflate_params: (*const WebSocketDeflate.Params)?,
        ): *!? {
            const tunnel: *WebSocketProxyTunnel = unsafe { @ptrCast(@alignCast(tunnel_ptr)) };

            let ws = bun.@"new"(WebSocket, {
                ref_count: .init(),
                tcp: { socket: { detached: {} } },
                outgoing_websocket: outgoing,
                globalThis: globalThis,
                send_buffer: bun.LinearFifo(u8, .Dynamic).init(bun.default_allocator),
                receive_buffer: bun.LinearFifo(u8, .Dynamic).init(bun.default_allocator),
                event_loop: globalThis.bunVM().eventLoop(),
                proxy_tunnel: tunnel,
            });

            tunnel.ref();

            if (params := deflate_params) {
                if (deflate := WebSocketDeflate.init(bun.default_allocator, unsafe { params.* }, globalThis.bunVM().rareData()) catch null) {
                    ws.deflate = deflate;
                } else {
                    ws.deflate = null;
                }
            }

            bun.handleOom(ws.send_buffer.ensureTotalCapacity(2048));
            bun.handleOom(ws.receive_buffer.ensureTotalCapacity(2048));
            ws.poll_ref.ref(globalThis.bunVM());

            const buffered_slice: []u8 = unsafe { buffered_data[0..buffered_data_len] };
            if (buffered_slice.len > 0) {
                const initial_data = InitialDataHandler.@"new"({
                    adopted: ws,
                    slice: buffered_slice,
                    ws: outgoing,
                });
                globalThis.queueMicrotaskCallback(initial_data, InitialDataHandler.handle);
                outgoing.ref();
            }

            ws.ref();

            return unsafe { @as(*!, ws) };
        }

        pub fn handleTunnelData(*this, data: []const u8) {
            this.handleData(this.tcp, data);
        }

        pub fn handleTunnelWritable(*this) {
            if (this.close_received) return;
            this.ref();
            defer this.deref();

            const send_buf = this.send_buffer.readableSlice(0);
            if (send_buf.len == 0) return;
            _ = this.sendBuffer(send_buf);
        }

        @c pub fn finalize(*this) {
            log("finalize", {});
            this.ref();
            defer this.deref();

            this.clearData();

            if (this.outgoing_websocket != null) {
                this.outgoing_websocket = null;
                this.deref();
            }

            if (!this.tcp.isClosed()) {
                if (comptime ssl) {
                    this.tcp.close(.normal);
                } else {
                    this.tcp.close(.failure);
                }
            }
        }

        pub fn deinit(*this) {
            this.clearData();
            if (d := this.deflate) d.deinit();
            this.deflate = null;
            bun.destroy(this);
        }

        @c pub fn memoryCost(*this): usize {
            let cost: usize = @sizeOf(WebSocket);
            cost += this.send_buffer.buf.len;
            cost += this.receive_buffer.buf.len;
            return cost;
        }

        pub static fn exportAll() {
            const name = comptime if (ssl) "WebSocketClientTLS" else "WebSocketClient";
            @export(&cancel, { name: "Bun__" + name + "__cancel" });
            @export(&close, { name: "Bun__" + name + "__close" });
            @export(&finalize, { name: "Bun__" + name + "__finalize" });
            @export(&init, { name: "Bun__" + name + "__init" });
            @export(&initWithTunnel, { name: "Bun__" + name + "__initWithTunnel" });
            @export(&memoryCost, { name: "Bun__" + name + "__memoryCost" });
            @export(&writeBinaryData, { name: "Bun__" + name + "__writeBinaryData" });
            @export(&writeBlob, { name: "Bun__" + name + "__writeBlob" });
            @export(&writeString, { name: "Bun__" + name + "__writeString" });
        }
    };
}
