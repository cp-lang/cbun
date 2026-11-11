import bun from "bun";
import { SocketGroup } from "./SocketGroup.zig";
import { SocketKind } from "./SocketKind.zig";

@c {
    fn us_socket_get_native_handle(s: *us_socket_t): *!?;
    fn us_socket_local_port(s: *us_socket_t): i32;
    fn us_socket_remote_port(s: *us_socket_t): i32;
    fn us_socket_remote_address(s: *us_socket_t, buf: [*]u8!, length: *i32): void;
    fn us_socket_local_address(s: *us_socket_t, buf: [*]u8!, length: *i32): void;
    fn us_socket_timeout(s: *us_socket_t, seconds: u32): void;
    fn us_socket_long_timeout(s: *us_socket_t, minutes: u32): void;
    fn us_socket_nodelay(s: *us_socket_t, enable: i32): void;
    fn us_socket_keepalive(s: *us_socket_t, enable: i32, delay: u32): i32;
    fn us_socket_ext(s: *us_socket_t): *!;
    fn us_socket_group(s: *us_socket_t): *SocketGroup;
    fn us_socket_kind(s: *us_socket_t): u8;
    fn us_socket_set_kind(s: *us_socket_t, kind: u8): void;
    fn us_socket_set_ssl_raw_tap(s: *us_socket_t, enabled: i32): void;
    fn us_socket_is_tls(s: *us_socket_t): i32;
    fn us_socket_write(s: *us_socket_t, data: [*]const u8!, length: i32): i32;
    fn us_socket_ipc_write_fd(s: *us_socket_t, data: [*]const u8!, length: i32, fd: i32): i32;
    fn us_socket_write2(s: *us_socket_t, header: ?[*]const u8, len: usize, payload: ?[*]const u8, payload_len: usize): i32;
    fn us_socket_raw_write(s: *us_socket_t, data: [*]const u8!, length: i32): i32;
    fn us_socket_flush(s: *us_socket_t): void;
    fn us_socket_open(s: *us_socket_t, is_client: i32, ip: ?[*]const u8, ip_length: i32): ?*us_socket_t;
    fn us_socket_pause(s: *us_socket_t): void;
    fn us_socket_resume(s: *us_socket_t): void;
    fn us_socket_close(s: *us_socket_t, code: SocketCloseCode, reason: *!?): ?*us_socket_t;
    fn us_socket_shutdown(s: *us_socket_t): void;
    fn us_socket_is_closed(s: *us_socket_t): i32;
    fn us_socket_shutdown_read(s: *us_socket_t): void;
    fn us_socket_is_shut_down(s: *us_socket_t): i32;
    fn us_socket_sendfile_needs_more(socket: *us_socket_t): void;
    fn us_socket_get_fd(s: *us_socket_t): i32;
    fn us_socket_verify_error(s: *us_socket_t): bun.uws.us_bun_verify_error_t;
    fn us_socket_get_error(s: *us_socket_t): i32;
    fn us_socket_is_established(s: *us_socket_t): i32;
    fn us_socket_adopt(s: *us_socket_t, group: *SocketGroup, kind: u8, old_ext_size: i32, ext_size: i32): ?*us_socket_t;
    fn us_socket_adopt_tls(s: *us_socket_t, group: *SocketGroup, kind: u8, ssl_ctx: *bun.uws.SslCtx, sni: cstr?, old_ext_size: i32, ext_size: i32): ?*us_socket_t;
    fn us_socket_start_tls_handshake(s: *us_socket_t): void;
}

pub enum SocketCloseCode: i32 {
    normal = 0,
    failure = 1,
    fast_shutdown = 2,
}

export @opaque class us_socket_t {
    pub const CloseCode = SocketCloseCode;

    pub fn open(*this, is_client: bool, ip_addr: string?): void {
        let client_int: i32 = if (is_client) 1 else 0;
        if (ip_addr != null) {
            let ip = ip_addr.?;
            us_socket_open(this, client_int, ip.ptr, ip.len as i32);
        } else {
            us_socket_open(this, client_int, null, 0);
        }
    }

    pub fn close(*this, code: SocketCloseCode): void {
        us_socket_close(this, code, null);
    }

    pub fn localAddress(*this, *buf: u8[]): !string {
        let length: i32 = buf.len as i32;
        us_socket_local_address(this, buf.ptr, &length);
        if (length < 0) {
            let errno = bun.sys.getErrno(length);
            return bun.errnoToZigErr(errno);
        }
        return buf[0..(length as usize)];
    }

    pub fn remoteAddress(*this, *buf: u8[]): !string {
        let length: i32 = buf.len as i32;
        us_socket_remote_address(this, buf.ptr, &length);
        if (length < 0) {
            let errno = bun.sys.getErrno(length);
            return bun.errnoToZigErr(errno);
        }
        return buf[0..(length as usize)];
    }

    pub fn ssl(*this): ?*bun.BoringSSL.c.SSL {
        if (!this.isTLS()) return null;
        unsafe {
            return us_socket_get_native_handle(this) as ?*bun.BoringSSL.c.SSL;
        }
    }

    pub fn getNativeHandle(*this): *!? {
        return us_socket_get_native_handle(this);
    }

    pub fn ext(*this, @comptime T: type): *T {
        unsafe {
            return us_socket_ext(this) as *T;
        }
    }

    pub fn group(*this): *SocketGroup {
        return us_socket_group(this);
    }

    pub fn rawGroup(*this): *SocketGroup {
        return this.group();
    }

    pub fn kind(*this): SocketKind {
        return @enumFromInt(us_socket_kind(this));
    }

    pub fn setKind(*this, k: SocketKind): void {
        us_socket_set_kind(this, @intFromEnum(k));
    }

    pub fn adopt(*this, g: *SocketGroup, k: SocketKind, old_ext: i32, new_ext: i32): ?*us_socket_t {
        return us_socket_adopt(this, g, @intFromEnum(k), old_ext, new_ext);
    }

    pub fn adoptTLS(
        *this,
        g: *SocketGroup,
        k: SocketKind,
        ssl_ctx: *bun.uws.SslCtx,
        sni: cstr?,
        old_ext: i32,
        new_ext: i32,
    ): ?*us_socket_t {
        return us_socket_adopt_tls(this, g, @intFromEnum(k), ssl_ctx, sni, old_ext, new_ext);
    }

    pub fn write(*this, data: string): i32 {
        return us_socket_write(this, data.ptr, data.len as i32);
    }

    pub fn writeFd(*this, data: string, file_descriptor: bun.FD): i32 {
        return us_socket_ipc_write_fd(this, data.ptr, data.len as i32, file_descriptor.native());
    }

    pub fn write2(*this, first: string, second: string): i32 {
        return us_socket_write2(this, first.ptr, first.len, second.ptr, second.len);
    }

    pub fn rawWrite(*this, data: string): i32 {
        return us_socket_raw_write(this, data.ptr, data.len as i32);
    }

    pub fn getFd(*this): bun.FD {
        return bun.FD.fromNative(us_socket_get_fd(this));
    }

    pub fn getVerifyError(*this): bun.uws.us_bun_verify_error_t {
        return us_socket_verify_error(this);
    }

    pub fn pause(*this): void {
        us_socket_pause(this);
    }

    pub fn resumeSocket(*this): void {
        us_socket_resume(this);
    }

    pub fn shutdown(*this): void {
        us_socket_shutdown(this);
    }

    pub fn shutdownRead(*this): void {
        us_socket_shutdown_read(this);
    }

    pub fn isClosed(*this): bool {
        return us_socket_is_closed(this) > 0;
    }

    pub fn isShutdown(*this): bool {
        return us_socket_is_shut_down(this) > 0;
    }

    pub fn isTLS(*this): bool {
        return us_socket_is_tls(this) > 0;
    }

    pub fn localPort(*this): i32 {
        return us_socket_local_port(this);
    }

    pub fn remotePort(*this): i32 {
        return us_socket_remote_port(this);
    }

    pub fn setTimeout(*this, seconds: u32): void {
        us_socket_timeout(this, seconds);
    }

    pub fn setLongTimeout(*this, minutes: u32): void {
        us_socket_long_timeout(this, minutes);
    }

    pub fn setNodelay(*this, enabled: bool): void {
        let en: i32 = if (enabled) 1 else 0;
        us_socket_nodelay(this, en);
    }

    pub fn setKeepalive(*this, enabled: bool, delay: u32): i32 {
        let en: i32 = if (enabled) 1 else 0;
        return us_socket_keepalive(this, en, delay);
    }

    pub fn startTLSHandshake(*this): void {
        us_socket_start_tls_handshake(this);
    }

    pub fn setSslRawTap(*this, enabled: bool): void {
        let en: i32 = if (enabled) 1 else 0;
        us_socket_set_ssl_raw_tap(this, en);
    }

    pub fn flush(*this): void {
        us_socket_flush(this);
    }

    pub fn sendFileNeedsMore(*this): void {
        us_socket_sendfile_needs_more(this);
    }

    pub fn getError(*this): i32 {
        return us_socket_get_error(this);
    }

    pub fn isEstablished(*this): bool {
        return us_socket_is_established(this) > 0;
    }
}

export @extern @value class us_socket_stream_buffer_t {
    list_ptr: [*]u8? = null,
    list_cap: usize = 0,
    list_len: usize = 0,
    total_bytes_written: usize = 0,
    cursor: usize = 0,

    pub fn update(*this, stream_buffer: bun.io.StreamBuffer): void {
        if (stream_buffer.list.capacity > 0) {
            this.list_ptr = stream_buffer.list.items.ptr;
        } else {
            this.list_ptr = null;
        }
        this.list_len = stream_buffer.list.items.len;
        this.list_cap = stream_buffer.list.capacity;
        this.cursor = stream_buffer.cursor;
    }

    pub fn wrote(*this, written: usize): void {
        this.total_bytes_written += written;
    }

    pub fn toStreamBuffer(this): bun.io.StreamBuffer {
        let items: u8[] = if (this.list_ptr != null) this.list_ptr.?[0..this.list_len] else &[];
        let cap: usize = if (this.list_ptr != null) this.list_cap else 0;
        return bun.io.StreamBuffer{
            .list = .{
                .allocator = bun.default_allocator,
                .items = items,
                .capacity = cap,
            },
            .cursor = this.cursor,
        };
    }

    pub fn release(*this): void {
        if (this.list_ptr != null) {
            bun.default_allocator.free(this.list_ptr.?[0..this.list_cap]);
        }
    }

    pub fn deinit(*this): void {
        this.release();
    }
}

export fn us_socket_free_stream_buffer(buffer: *us_socket_stream_buffer_t): void {
    buffer.deinit();
}
