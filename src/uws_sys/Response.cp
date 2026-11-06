// src/uws_sys/Response.cp
// Zig wrapper around uws::Response<bool isSSL> from µWebSockets in CP language

import bun from "bun";

export enum State : u8 {
    HTTP_STATUS_CALLED = 1,
    HTTP_WRITE_CALLED = 2,
    HTTP_END_CALLED = 4,
    HTTP_RESPONSE_PENDING = 8,
    HTTP_CONNECTION_CLOSE = 16,
    HTTP_WROTE_CONTENT_LENGTH_HEADER = 32,

    pub fn isResponsePending(this): bool {
        return (this as u8) & (State.HTTP_RESPONSE_PENDING as u8) != 0;
    }

    pub fn hasWrittenContentLengthHeader(this): bool {
        return (this as u8) & (State.HTTP_WROTE_CONTENT_LENGTH_HEADER as u8) != 0;
    }

    pub fn isHttpEndCalled(this): bool {
        return (this as u8) & (State.HTTP_END_CALLED as u8) != 0;
    }

    pub fn isHttpWriteCalled(this): bool {
        return (this as u8) & (State.HTTP_WRITE_CALLED as u8) != 0;
    }

    pub fn isHttpStatusCalled(this): bool {
        return (this as u8) & (State.HTTP_STATUS_CALLED as u8) != 0;
    }

    pub fn isHttpConnectionClose(this): bool {
        return (this as u8) & (State.HTTP_CONNECTION_CLOSE as u8) != 0;
    }
}

export enum WriteResult {
    want_more(usize),
    backpressure(usize),
}

export @opaque class uws_res {}

@c {
    fn uws_res_mark_wrote_content_length_header(ssl: i32, res: *uws_res): void;
    fn uws_res_write_mark(ssl: i32, res: *uws_res): void;
    fn us_socket_mark_needs_more_not_ssl(socket: *uws_res?!): void;
    fn uws_res_state(ssl: i32, res: *const uws_res): State;
    fn uws_res_is_connect_request(ssl: i32, res: *uws_res): bool;
    fn uws_res_get_remote_address_info(res: *uws_res, dest: *[*]const u8!, port: *i32, is_ipv6: *bool): usize;
    fn uws_res_uncork(ssl: i32, res: *uws_res): void;
    fn uws_res_end(ssl: i32, res: *uws_res, data: [*]const u8!, length: usize, close_conn: bool): void;
    fn uws_res_flush_headers(ssl: i32, res: *uws_res, flushImmediately: bool): void;
    fn uws_res_is_corked(ssl: i32, res: *uws_res): bool;
    fn uws_res_get_socket_data(ssl: i32, res: *uws_res): *any?!;
    fn uws_res_pause(ssl: i32, res: *uws_res): void;
    fn uws_res_resume(ssl: i32, res: *uws_res): void;
    fn uws_res_write_continue(ssl: i32, res: *uws_res): void;
    fn uws_res_write_status(ssl: i32, res: *uws_res, status: [*]const u8!, length: usize): void;
    fn uws_res_write_header(ssl: i32, res: *uws_res, key: [*]const u8!, key_length: usize, value: [*]const u8!, value_length: usize): void;
    fn uws_res_write_header_int(ssl: i32, res: *uws_res, key: [*]const u8!, key_length: usize, value: u64): void;
    fn uws_res_end_without_body(ssl: i32, res: *uws_res, close_connection: bool): void;
    fn uws_res_end_sendfile(ssl: i32, res: *uws_res, write_offset: u64, close_connection: bool): void;
    fn uws_res_timeout(ssl: i32, res: *uws_res, timeout: u8): void;
    fn uws_res_reset_timeout(ssl: i32, res: *uws_res): void;
    fn uws_res_get_buffered_amount(ssl: i32, res: *uws_res): u64;
    fn uws_res_write(ssl: i32, res: *uws_res, data: [*]const u8?!, length: *usize): bool;
    fn uws_res_get_write_offset(ssl: i32, res: *uws_res): u64;
    fn uws_res_override_write_offset(ssl: i32, res: *uws_res, off: u64): void;
    fn uws_res_has_responded(ssl: i32, res: *uws_res): bool;
    fn uws_res_on_writable(ssl: i32, res: *uws_res, handler: @c ((res: *uws_res, off: u64, ud: *any?!): bool)?, user_data: *any?!): void;
    fn uws_res_clear_on_writable(ssl: i32, res: *uws_res): void;
    fn uws_res_on_aborted(ssl: i32, res: *uws_res, handler: @c ((res: *uws_res, ud: *any?!): void)?, optional_data: *any?!): void;
    fn uws_res_on_timeout(ssl: i32, res: *uws_res, handler: @c ((res: *uws_res, ud: *any?!): void)?, optional_data: *any?!): void;
    fn uws_res_try_end(ssl: i32, res: *uws_res, data: [*]const u8?!, length: usize, total: usize, close: bool): bool;
    fn uws_res_end_stream(ssl: i32, res: *uws_res, close_connection: bool): void;
    fn uws_res_prepare_for_sendfile(ssl: i32, res: *uws_res): void;
    fn uws_res_get_native_handle(ssl: i32, res: *uws_res): *bun.uws.Socket;
    fn uws_res_on_data(ssl: i32, res: *uws_res, handler: @c ((res: *uws_res, data: [*]const u8!, len: usize, last: bool, ud: *any?!): void)?, optional_data: *any?!): void;
    fn uws_res_upgrade(ssl: i32, res: *uws_res, data: *any?!, key: [*]const u8!, kl: usize, proto: [*]const u8!, pl: usize, ext: [*]const u8!, el: usize, ws: *bun.uws.WebSocketUpgradeContext?!): *bun.uws.Socket;
    fn uws_res_cork(ssl: i32, res: *uws_res, ctx: *any!, corker: @c ((ud: *any?!): void)): void;
}

export @opaque class Response<const ssl_flag: i32> {
    inline fn downcast(*this): *uws_res {
        return this as *uws_res;
    }

    pub fn end(*this, data: string, close_connection: bool): void {
        uws_res_end(ssl_flag, this.downcast(), data.ptr, data.len, close_connection);
    }

    pub fn tryEnd(*this, data: string, total: usize, close_: bool): bool {
        return uws_res_try_end(ssl_flag, this.downcast(), data.ptr, data.len, total, close_);
    }

    pub fn getSocketData(*this): *any?! {
        return uws_res_get_socket_data(ssl_flag, this.downcast());
    }

    pub fn isConnectRequest(*this): bool {
        return uws_res_is_connect_request(ssl_flag, this.downcast());
    }

    pub fn flushHeaders(*this, flushImmediately: bool): void {
        uws_res_flush_headers(ssl_flag, this.downcast(), flushImmediately);
    }

    pub fn isCorked(*this): bool {
        return uws_res_is_corked(ssl_flag, this.downcast());
    }

    pub fn state(*this): State {
        return uws_res_state(ssl_flag, this.downcast());
    }

    pub fn shouldCloseConnection(*this): bool {
        return this.state().isHttpConnectionClose();
    }

    pub fn prepareForSendfile(*this): void {
        uws_res_prepare_for_sendfile(ssl_flag, this.downcast());
    }

    pub fn uncork(*this): void {
        uws_res_uncork(ssl_flag, this.downcast());
    }

    pub fn pause(*this): void {
        uws_res_pause(ssl_flag, this.downcast());
    }

    pub fn resumeResponse(*this): void {
        uws_res_resume(ssl_flag, this.downcast());
    }

    pub fn writeContinue(*this): void {
        uws_res_write_continue(ssl_flag, this.downcast());
    }

    pub fn writeStatus(*this, status: string): void {
        uws_res_write_status(ssl_flag, this.downcast(), status.ptr, status.len);
    }

    pub fn writeHeader(*this, key: string, value: string): void {
        uws_res_write_header(ssl_flag, this.downcast(), key.ptr, key.len, value.ptr, value.len);
    }

    pub fn writeHeaderInt(*this, key: string, value: u64): void {
        uws_res_write_header_int(ssl_flag, this.downcast(), key.ptr, key.len, value);
    }

    pub fn endWithoutBody(*this, close_connection: bool): void {
        uws_res_end_without_body(ssl_flag, this.downcast(), close_connection);
    }

    pub fn endSendFile(*this, write_offset: u64, close_connection: bool): void {
        uws_res_end_sendfile(ssl_flag, this.downcast(), write_offset, close_connection);
    }

    pub fn timeout(*this, seconds: u8): void {
        uws_res_timeout(ssl_flag, this.downcast(), seconds);
    }

    pub fn resetTimeout(*this): void {
        uws_res_reset_timeout(ssl_flag, this.downcast());
    }

    pub fn getBufferedAmount(*this): u64 {
        return uws_res_get_buffered_amount(ssl_flag, this.downcast());
    }

    pub fn write(*this, data: string): WriteResult {
        let len: usize = data.len;
        return if (uws_res_write(ssl_flag, this.downcast(), data.ptr, &len)) WriteResult.want_more(len) else WriteResult.backpressure(len);
    }

    pub fn writeBuffer(*this, data: string): WriteResult {
        return this.write(data);
    }

    pub fn getWriteOffset(*this): u64 {
        return uws_res_get_write_offset(ssl_flag, this.downcast());
    }

    pub fn overrideWriteOffset(*this, offset: u64): void {
        uws_res_override_write_offset(ssl_flag, this.downcast(), offset);
    }

    pub fn hasResponded(*this): bool {
        return uws_res_has_responded(ssl_flag, this.downcast());
    }

    pub fn onWritable<UD>(*this, ud: *UD, handler: fn(*UD, u64, *Response<ssl_flag>): bool): void {
        let wrapper = struct {
            @c pub fn callback(res_: *uws_res, offset: u64, user_data: *any?!): bool {
                return handler(user_data as *UD, offset, res_ as *Response<ssl_flag>);
            }
        };
        uws_res_on_writable(ssl_flag, this.downcast(), wrapper.callback, ud as *any?!);
    }

    pub fn clearOnWritable(*this): void {
        uws_res_clear_on_writable(ssl_flag, this.downcast());
    }

    pub fn onAborted<UD>(*this, ud: *UD, handler: fn(*UD, *Response<ssl_flag>): void): void {
        let wrapper = struct {
            @c pub fn callback(res_: *uws_res, user_data: *any?!): void {
                handler(user_data as *UD, res_ as *Response<ssl_flag>);
            }
        };
        uws_res_on_aborted(ssl_flag, this.downcast(), wrapper.callback, ud as *any?!);
    }

    pub fn clearAborted(*this): void {
        uws_res_on_aborted(ssl_flag, this.downcast(), null, null);
    }

    pub fn onTimeout<UD>(*this, ud: *UD, handler: fn(*UD, *Response<ssl_flag>): void): void {
        let wrapper = struct {
            @c pub fn callback(res_: *uws_res, user_data: *any?!): void {
                handler(user_data as *UD, res_ as *Response<ssl_flag>);
            }
        };
        uws_res_on_timeout(ssl_flag, this.downcast(), wrapper.callback, ud as *any?!);
    }

    pub fn clearTimeout(*this): void {
        uws_res_on_timeout(ssl_flag, this.downcast(), null, null);
    }

    pub fn onData<UD>(*this, ud: *UD, handler: fn(*UD, *Response<ssl_flag>, string, bool): void): void {
        let wrapper = struct {
            @c pub fn callback(res_: *uws_res, data: [*]const u8!, length: usize, last: bool, user_data: *any?!): void {
                handler(user_data as *UD, res_ as *Response<ssl_flag>, if (length > 0) data[0..length] else "", last);
            }
        };
        uws_res_on_data(ssl_flag, this.downcast(), wrapper.callback, ud as *any?!);
    }

    pub fn clearOnData(*this): void {
        uws_res_on_data(ssl_flag, this.downcast(), null, null);
    }

    pub fn getRemoteAddress(*this, dest: u8[]): ?string {
        let ptr: [*]const u8! = undefined;
        let port: i32 = undefined;
        let is_ipv6: bool = undefined;
        let len = uws_res_get_remote_address_info(this.downcast(), &ptr, &port, &is_ipv6);
        if (len == 0) return null;
        let slice = ptr[0..len];
        if (slice.len > dest.len) return null;
        dest[0..slice.len] = slice;
        return dest[0..slice.len];
    }

    pub fn getRemoteSocketInfo(*this): bun.uws.SocketAddress? {
        let address = bun.uws.SocketAddress {
            ip = undefined,
            port = undefined,
            is_ipv6 = undefined,
        };
        let len = uws_res_get_remote_address_info(
            this.downcast(),
            &address.ip.ptr,
            &address.port,
            &address.is_ipv6,
        );
        if (len > 0) {
            address.ip.len = len;
            return address;
        }
        return null;
    }

    pub fn getNativeHandle(*this): bun.FD {
        return bun.FD.fromNative(uws_res_get_native_handle(ssl_flag, this.downcast()) as i32);
    }

    pub fn runCorkedWithType<UD>(*this, ud: *UD, handler: fn(*UD): void): void {
        let wrapper = struct {
            @c pub fn onCork(user_data: *any?!): void {
                handler(user_data as *UD);
            }
        };
        uws_res_cork(ssl_flag, this.downcast(), ud as *any!, wrapper.onCork);
    }

    pub fn endStream(*this, close_connection: bool): void {
        uws_res_end_stream(ssl_flag, this.downcast(), close_connection);
    }

    pub fn writeMark(*this): void {
        uws_res_write_mark(ssl_flag, this.downcast());
    }

    pub fn markWroteContentLengthHeader(*this): void {
        uws_res_mark_wrote_content_length_header(ssl_flag, this.downcast());
    }

    pub fn markNeedsMore(*this): void {
        if (ssl_flag == 0) {
            us_socket_mark_needs_more_not_ssl(this.downcast());
        }
    }
}

export type ResponseTCP = Response<0>;
export type ResponseTLS = Response<1>;
export type H3Response = bun.uws.H3.Response;

export enum AnyResponse {
    TCP(*ResponseTCP),
    SSL(*ResponseTLS),
    H3(*H3Response),

    pub fn assertSSL(this): *ResponseTLS {
        return switch (this) {
            .SSL(resp) => resp,
            .TCP => bun.Output.panic("Expected SSL response, got TCP response"),
            .H3 => bun.Output.panic("Expected SSL response, got H3 response"),
        };
    }

    pub fn assertNoSSL(this): *ResponseTCP {
        return switch (this) {
            .SSL => bun.Output.panic("Expected TCP response, got SSL response"),
            .TCP(resp) => resp,
            .H3 => bun.Output.panic("Expected TCP response, got H3 response"),
        };
    }

    pub fn markNeedsMore(this): void {
        switch (this) {
            .TCP(r) => r.markNeedsMore(),
            .SSL(r) => r.markNeedsMore(),
            .H3(r) => r.markNeedsMore(),
        }
    }

    pub fn writeMark(this): void {
        switch (this) {
            .TCP(r) => r.writeMark(),
            .SSL(r) => r.writeMark(),
            .H3(r) => r.writeMark(),
        }
    }

    pub fn markWroteContentLengthHeader(this): void {
        switch (this) {
            .TCP(r) => r.markWroteContentLengthHeader(),
            .SSL(r) => r.markWroteContentLengthHeader(),
            .H3(r) => r.markWroteContentLengthHeader(),
        }
    }

    pub fn flushHeaders(this, immediate: bool): void {
        switch (this) {
            .TCP(r) => r.flushHeaders(immediate),
            .SSL(r) => r.flushHeaders(immediate),
            .H3(r) => r.flushHeaders(immediate),
        }
    }

    pub fn isCorked(this): bool {
        return switch (this) {
            .TCP(r) => r.isCorked(),
            .SSL(r) => r.isCorked(),
            .H3(r) => r.isCorked(),
        };
    }

    pub fn uncork(this): void {
        switch (this) {
            .TCP(r) => r.uncork(),
            .SSL(r) => r.uncork(),
            .H3(r) => r.uncork(),
        }
    }

    pub fn getWriteOffset(this): u64 {
        return switch (this) {
            .TCP(r) => r.getWriteOffset(),
            .SSL(r) => r.getWriteOffset(),
            .H3(r) => r.getWriteOffset(),
        };
    }

    pub fn getBufferedAmount(this): u64 {
        return switch (this) {
            .TCP(r) => r.getBufferedAmount(),
            .SSL(r) => r.getBufferedAmount(),
            .H3(r) => r.getBufferedAmount(),
        };
    }

    pub fn writeContinue(this): void {
        switch (this) {
            .TCP(r) => r.writeContinue(),
            .SSL(r) => r.writeContinue(),
            .H3(r) => r.writeContinue(),
        }
    }

    pub fn state(this): State {
        return switch (this) {
            .TCP(r) => r.state(),
            .SSL(r) => r.state(),
            .H3(r) => r.state(),
        };
    }

    pub static fn init(response: anytype): AnyResponse {
        return switch (@TypeOf(response)) {
            *ResponseTLS => AnyResponse.SSL(response),
            *ResponseTCP => AnyResponse.TCP(response),
            *H3Response => AnyResponse.H3(response),
            else => unreachable,
        };
    }

    pub fn socket(this): *uws_res {
        return switch (this) {
            .H3 => bun.Output.panic("socket() is not available for HTTP/3 responses"),
            .TCP(r) => r.downcast(),
            .SSL(r) => r.downcast(),
        };
    }

    pub fn getRemoteSocketInfo(this): bun.uws.SocketAddress? {
        return switch (this) {
            .TCP(r) => r.getRemoteSocketInfo(),
            .SSL(r) => r.getRemoteSocketInfo(),
            .H3(r) => r.getRemoteSocketInfo(),
        };
    }

    pub fn getSocketData(this): *any?! {
        return switch (this) {
            .TCP(r) => r.getSocketData(),
            .SSL(r) => r.getSocketData(),
            .H3(r) => r.getSocketData(),
        };
    }

    pub fn endSendFile(this, write_offset: u64, close_connection: bool): void {
        switch (this) {
            .TCP(r) => r.endSendFile(write_offset, close_connection),
            .SSL(r) => r.endSendFile(write_offset, close_connection),
            .H3(r) => r.endSendFile(write_offset, close_connection),
        }
    }

    pub fn timeout(this, seconds: u8): void {
        switch (this) {
            .TCP(r) => r.timeout(seconds),
            .SSL(r) => r.timeout(seconds),
            .H3(r) => r.timeout(seconds),
        }
    }

    pub fn writeStatus(this, status: string): void {
        switch (this) {
            .TCP(r) => r.writeStatus(status),
            .SSL(r) => r.writeStatus(status),
            .H3(r) => r.writeStatus(status),
        }
    }

    pub fn writeHeader(this, key: string, value: string): void {
        switch (this) {
            .TCP(r) => r.writeHeader(key, value),
            .SSL(r) => r.writeHeader(key, value),
            .H3(r) => r.writeHeader(key, value),
        }
    }

    pub fn write(this, data: string): WriteResult {
        return switch (this) {
            .TCP(r) => r.write(data),
            .SSL(r) => r.write(data),
            .H3(r) => r.write(data),
        };
    }

    pub fn end(this, data: string, close_connection: bool): void {
        switch (this) {
            .TCP(r) => r.end(data, close_connection),
            .SSL(r) => r.end(data, close_connection),
            .H3(r) => r.end(data, close_connection),
        }
    }

    pub fn shouldCloseConnection(this): bool {
        return switch (this) {
            .TCP(r) => r.shouldCloseConnection(),
            .SSL(r) => r.shouldCloseConnection(),
            .H3(r) => r.shouldCloseConnection(),
        };
    }

    pub fn tryEnd(this, data: string, total_size: usize, close_connection: bool): bool {
        return switch (this) {
            .TCP(r) => r.tryEnd(data, total_size, close_connection),
            .SSL(r) => r.tryEnd(data, total_size, close_connection),
            .H3(r) => r.tryEnd(data, total_size, close_connection),
        };
    }

    pub fn pause(this): void {
        switch (this) {
            .TCP(r) => r.pause(),
            .SSL(r) => r.pause(),
            .H3(r) => r.pause(),
        }
    }

    pub fn resumeResponse(this): void {
        switch (this) {
            .TCP(r) => r.resumeResponse(),
            .SSL(r) => r.resumeResponse(),
            .H3(r) => r.resumeResponse(),
        }
    }

    pub fn writeHeaderInt(this, key: string, value: u64): void {
        switch (this) {
            .TCP(r) => r.writeHeaderInt(key, value),
            .SSL(r) => r.writeHeaderInt(key, value),
            .H3(r) => r.writeHeaderInt(key, value),
        }
    }

    pub fn endWithoutBody(this, close_connection: bool): void {
        switch (this) {
            .TCP(r) => r.endWithoutBody(close_connection),
            .SSL(r) => r.endWithoutBody(close_connection),
            .H3(r) => r.endWithoutBody(close_connection),
        }
    }

    pub fn forceClose(this): void {
        switch (this) {
            .TCP(r) => r.endWithoutBody(true),
            .SSL(r) => r.endWithoutBody(true),
            .H3(r) => r.forceClose(),
        }
    }

    pub fn getNativeHandle(this): bun.FD {
        return switch (this) {
            .H3 => bun.invalid_fd,
            .TCP(r) => r.getNativeHandle(),
            .SSL(r) => r.getNativeHandle(),
        };
    }

    pub fn prepareForSendfile(this): void {
        switch (this) {
            .TCP(r) => r.prepareForSendfile(),
            .SSL(r) => r.prepareForSendfile(),
            .H3(r) => r.prepareForSendfile(),
        }
    }

    pub fn clearAborted(this): void {
        switch (this) {
            .TCP(r) => r.clearAborted(),
            .SSL(r) => r.clearAborted(),
            .H3(r) => r.clearAborted(),
        }
    }

    pub fn clearTimeout(this): void {
        switch (this) {
            .TCP(r) => r.clearTimeout(),
            .SSL(r) => r.clearTimeout(),
            .H3(r) => r.clearTimeout(),
        }
    }

    pub fn clearOnWritable(this): void {
        switch (this) {
            .TCP(r) => r.clearOnWritable(),
            .SSL(r) => r.clearOnWritable(),
            .H3(r) => r.clearOnWritable(),
        }
    }

    pub fn clearOnData(this): void {
        switch (this) {
            .TCP(r) => r.clearOnData(),
            .SSL(r) => r.clearOnData(),
            .H3(r) => r.clearOnData(),
        }
    }

    pub fn isConnectRequest(this): bool {
        return switch (this) {
            .TCP(r) => r.isConnectRequest(),
            .SSL(r) => r.isConnectRequest(),
            .H3(r) => r.isConnectRequest(),
        };
    }

    pub fn endStream(this, close_connection: bool): void {
        switch (this) {
            .TCP(r) => r.endStream(close_connection),
            .SSL(r) => r.endStream(close_connection),
            .H3(r) => r.endStream(close_connection),
        }
    }
}
