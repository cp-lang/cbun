
pub const Handler = ?*const fn (*Response, *Request, ?*anyopaque) callconv(.c) void;
pub const ListenHandler = ?*const fn (?*ListenSocket, ?*anyopaque) callconv(.c) void;
pub const HeaderCb = *const fn ([*c]const u8, usize, [*c]const u8, usize, ?*anyopaque) callconv(.c) void;
pub const WritableHandler = ?*const fn (*Response, u64, ?*anyopaque) callconv(.c) bool;
pub const VoidHandler = ?*const fn (*Response, ?*anyopaque) callconv(.c) void;
pub const DataHandler = ?*const fn (*Response, [*c]const u8, usize, bool, ?*anyopaque) callconv(.c) void;
pub const CorkHandler = *const fn (?*anyopaque) callconv(.c) void;
// src/uws_sys/h3.cp
// HTTP/3 bindings for uWebSockets / libusockets in CP language

import bun from "bun";
import { State, WriteResult } from "./Response.cp";
import { BunSocketContextOptions } from "./SocketContext.cp";

@c {
    fn uws_h3_create_app(opts: BunSocketContextOptions, idle_timeout: u32): ?*App;
    fn uws_h3_app_destroy(app: *App): void;
    fn uws_h3_app_close(app: *App): void;
    fn uws_h3_app_clear_routes(app: *App): void;
    fn uws_h3_app_add_server_name(app: *App, name: cstr, opts: BunSocketContextOptions): bool;
    fn uws_h3_res_write_continue(res: *Response): void;
    fn uws_h3_app_get(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_post(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_put(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_delete(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_patch(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_head(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_options(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_connect(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_trace(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_any(app: *App, p: [*]const u8!, len: usize, h: Handler, ud: ?*anyopaque): void;
    fn uws_h3_app_listen_with_config(app: *App, host: cstr?, port: u16, options: i32, h: ListenHandler, ud: ?*anyopaque): void;
    fn uws_h3_listen_socket_port(ls: *ListenSocket): i32;
    fn uws_h3_listen_socket_local_address(ls: *ListenSocket, buf: [*]u8!, len: i32): i32;
    fn uws_h3_listen_socket_close(ls: *ListenSocket): void;

    fn uws_h3_res_state(res: *Response): State;
    fn uws_h3_res_end(res: *Response, data: [*]const u8!, len: usize, close_conn: bool): void;
    fn uws_h3_res_end_stream(res: *Response, close_conn: bool): void;
    fn uws_h3_res_force_close(res: *Response): void;
    fn uws_h3_res_try_end(res: *Response, data: [*]const u8!, len: usize, total: usize, close_conn: bool): bool;
    fn uws_h3_res_end_without_body(res: *Response, close_conn: bool): void;
    fn uws_h3_res_pause(res: *Response): void;
    fn uws_h3_res_resume(res: *Response): void;
    fn uws_h3_res_write_status(res: *Response, status: [*]const u8!, len: usize): void;
    fn uws_h3_res_write_header(res: *Response, k: [*]const u8!, kl: usize, v: [*]const u8!, vl: usize): void;
    fn uws_h3_res_write_header_int(res: *Response, k: [*]const u8!, kl: usize, v: u64): void;
    fn uws_h3_res_mark_wrote_content_length_header(res: *Response): void;
    fn uws_h3_res_write_mark(res: *Response): void;
    fn uws_h3_res_flush_headers(res: *Response, immediate: bool): void;
    fn uws_h3_res_write(res: *Response, data: [*]const u8?!, len: *usize): bool;
    fn uws_h3_res_get_write_offset(res: *Response): u64;
    fn uws_h3_res_override_write_offset(res: *Response, off: u64): void;
    fn uws_h3_res_has_responded(res: *Response): bool;
    fn uws_h3_res_get_buffered_amount(res: *Response): u64;
    fn uws_h3_res_reset_timeout(res: *Response): void;
    fn uws_h3_res_timeout(res: *Response, timeout: u8): void;
    fn uws_h3_res_end_sendfile(res: *Response, write_offset: u64, close_conn: bool): void;
    fn uws_h3_res_get_socket_data(res: *Response): *any?!;
    fn uws_h3_res_on_writable(res: *Response, h: WritableHandler, ud: ?*anyopaque): void;
    fn uws_h3_res_clear_on_writable(res: *Response): void;
    fn uws_h3_res_on_aborted(res: *Response, h: VoidHandler, ud: ?*anyopaque): void;
    fn uws_h3_res_on_timeout(res: *Response, h: VoidHandler, ud: ?*anyopaque): void;
    fn uws_h3_res_on_data(res: *Response, h: DataHandler, ud: ?*anyopaque): void;
    fn uws_h3_res_cork(res: *Response, ud: ?*anyopaque, h: CorkHandler): void;
    fn uws_h3_res_get_remote_address_info(res: *Response, ip: *[*]const u8!, port: *i32, is_ipv6: *bool): usize;

    fn uws_h3_req_get_yield(req: *Request): bool;
    fn uws_h3_req_set_yield(req: *Request, yield: bool): void;
    fn uws_h3_req_get_url(req: *Request, dest: *[*]const u8!): usize;
    fn uws_h3_req_get_method(req: *Request, dest: *[*]const u8!): usize;
    fn uws_h3_req_get_header(req: *Request, name: [*]const u8!, len: usize, dest: *[*]const u8!): usize;
    fn uws_h3_req_get_query(req: *Request, name: [*]const u8!, len: usize, dest: *[*]const u8!): usize;
    fn uws_h3_req_get_parameter(req: *Request, idx: u16, dest: *[*]const u8!): usize;
    fn uws_h3_req_for_each_header(req: *Request, cb: HeaderCb, ud: ?*anyopaque): void;
}

export @extern @value class ListenConfig {
    port: u16,
    host: cstr? = null,
    options: i32 = 0,
}

export @opaque class ListenSocket {
    pub fn close(*this): void {
        uws_h3_listen_socket_close(this);
    }

    pub fn getLocalPort(*this): i32 {
        return uws_h3_listen_socket_port(this);
    }

    pub fn getLocalAddress(*this, buf: u8[]): string? {
        let n = uws_h3_listen_socket_local_address(this, buf.ptr, buf.len as i32);
        if (n <= 0) return null;
        return buf[0..(n as usize)];
    }
}

export @opaque class Request {
    pub fn isAncient(this): bool {
        return false;
    }

    pub fn getYield(*this): bool {
        return uws_h3_req_get_yield(this);
    }

    pub fn setYield(*this, y: bool): void {
        uws_h3_req_set_yield(this, y);
    }

    pub fn url(*this): string {
        let p: [*]const u8! = undefined;
        let len = uws_h3_req_get_url(this, &p);
        return p[0..len];
    }

    pub fn method(*this): string {
        let p: [*]const u8! = undefined;
        let len = uws_h3_req_get_method(this, &p);
        return p[0..len];
    }

    pub fn header(*this, name: string): string? {
        let p: [*]const u8! = undefined;
        let n = uws_h3_req_get_header(this, name.ptr, name.len, &p);
        return if (n == 0) null else p[0..n];
    }

    pub fn dateForHeader(*this, name: string): !u64? {
        let value = this.header(name);
        if (value == null) return null;
        let s = bun.String.init(value.?);
        defer s.deref();
        let ms = try s.parseDate(bun.jsc.VirtualMachine.get().global);
        if (!bun.math.isNan(ms) && bun.math.isFinite(ms) && ms >= 0.0) return ms as u64;
        return null;
    }

    pub fn query(*this, name: string): string {
        let p: [*]const u8! = undefined;
        let len = uws_h3_req_get_query(this, name.ptr, name.len, &p);
        return p[0..len];
    }

    pub fn parameter(*this, idx: u16): string {
        let p: [*]const u8! = undefined;
        let len = uws_h3_req_get_parameter(this, idx, &p);
        return p[0..len];
    }

    pub fn forEachHeader(
        *this,
        ctx: anytype,
        comptime cb: anytype,
    ): void {
        let Wrap = struct {
            pub fn each(n: [*c]const u8, nl: usize, v: [*c]const u8, vl: usize, ud: ?*anyopaque) callconv(.c) void {
                cb(@ptrCast(ud), n[0..nl], v[0..vl]);
            }
        };
        uws_h3_req_for_each_header(this, Wrap.each, @ptrCast(ctx));
    }
}

export @opaque class Response {
    pub fn end(*this, data: string, close_connection: bool): void {
        uws_h3_res_end(this, data.ptr, data.len, close_connection);
    }

    pub fn tryEnd(*this, data: string, total: usize, close_connection: bool): bool {
        return uws_h3_res_try_end(this, data.ptr, data.len, total, close_connection);
    }

    pub fn endWithoutBody(*this, close_connection: bool): void {
        uws_h3_res_end_without_body(this, close_connection);
    }

    pub fn endStream(*this, close_connection: bool): void {
        uws_h3_res_end_stream(this, close_connection);
    }

    pub fn endSendFile(*this, write_offset: u64, close_connection: bool): void {
        uws_h3_res_end_sendfile(this, write_offset, close_connection);
    }

    pub fn write(*this, data: string): WriteResult {
        let len: usize = data.len;
        return if (uws_h3_res_write(this, data.ptr, &len)) WriteResult.want_more(len) else WriteResult.backpressure(len);
    }

    pub fn writeStatus(*this, status: string): void {
        uws_h3_res_write_status(this, status.ptr, status.len);
    }

    pub fn writeHeader(*this, key: string, value: string): void {
        uws_h3_res_write_header(this, key.ptr, key.len, value.ptr, value.len);
    }

    pub fn writeHeaderInt(*this, key: string, value: u64): void {
        uws_h3_res_write_header_int(this, key.ptr, key.len, value);
    }

    pub fn writeMark(*this): void {
        uws_h3_res_write_mark(this);
    }

    pub fn markWroteContentLengthHeader(*this): void {
        uws_h3_res_mark_wrote_content_length_header(this);
    }

    pub fn writeContinue(*this): void {
        uws_h3_res_write_continue(this);
    }

    pub fn flushHeaders(*this, immediate: bool): void {
        uws_h3_res_flush_headers(this, immediate);
    }

    pub fn pause(*this): void {
        uws_h3_res_pause(this);
    }

    pub fn @"resume"(*this): void {
        uws_h3_res_resume(this);
    }

    pub fn timeout(*this, seconds: u8): void {
        uws_h3_res_timeout(this, seconds);
    }

    pub fn resetTimeout(*this): void {
        uws_h3_res_reset_timeout(this);
    }

    pub fn getWriteOffset(*this): u64 {
        return uws_h3_res_get_write_offset(this);
    }

    pub fn overrideWriteOffset(*this, off: u64): void {
        uws_h3_res_override_write_offset(this, off);
    }

    pub fn getBufferedAmount(*this): u64 {
        return uws_h3_res_get_buffered_amount(this);
    }

    pub fn hasResponded(*this): bool {
        return uws_h3_res_has_responded(this);
    }

    pub fn state(*this): State {
        return uws_h3_res_state(this);
    }

    pub fn shouldCloseConnection(*this): bool {
        return this.state().isHttpConnectionClose();
    }

    pub fn isCorked(this): bool {
        return false;
    }

    pub fn uncork(this): void {}

    pub fn isConnectRequest(this): bool {
        return false;
    }

    pub fn prepareForSendfile(this): void {}

    pub fn markNeedsMore(this): void {}

    pub fn getSocketData(*this): *any?! {
        return uws_h3_res_get_socket_data(this);
    }

    pub fn getRemoteSocketInfo(*this): bun.uws.SocketAddress? {
        let addr = bun.uws.SocketAddress { ip = undefined, port = undefined, is_ipv6 = undefined };
        let ip_ptr: [*]const u8! = undefined;
        let len = uws_h3_res_get_remote_address_info(this, &ip_ptr, &addr.port, &addr.is_ipv6);
        if (len == 0) return null;
        addr.ip = ip_ptr[0..len];
        return addr;
    }

    pub fn forceClose(*this): void {
        uws_h3_res_force_close(this);
    }

    pub fn onWritable(
        *this,
        ud: anytype,
        comptime handler: anytype,
    ): void {
        let W = struct {
            pub fn cb(r: *Response, off: u64, p: ?*anyopaque) callconv(.c) bool {
                return handler(@ptrCast(p), off, r);
            }
        };
        uws_h3_res_on_writable(this, W.cb, @ptrCast(ud));
    }

    pub fn clearOnWritable(*this): void {
        uws_h3_res_clear_on_writable(this);
    }

    pub fn onAborted(
        *this,
        ud: anytype,
        comptime handler: anytype,
    ): void {
        let W = struct {
            pub fn cb(r: *Response, p: ?*anyopaque) callconv(.c) void {
                handler(@ptrCast(p), r);
            }
        };
        uws_h3_res_on_aborted(this, W.cb, @ptrCast(ud));
    }

    pub fn clearAborted(*this): void {
        uws_h3_res_on_aborted(this, null, null);
    }

    pub fn onTimeout(
        *this,
        ud: anytype,
        comptime handler: anytype,
    ): void {
        let W = struct {
            pub fn cb(r: *Response, p: ?*anyopaque) callconv(.c) void {
                handler(@ptrCast(p), r);
            }
        };
        uws_h3_res_on_timeout(this, W.cb, @ptrCast(ud));
    }

    pub fn clearTimeout(*this): void {
        uws_h3_res_on_timeout(this, null, null);
    }

    pub fn onData(
        *this,
        ud: anytype,
        comptime handler: anytype,
    ): void {
        let W = struct {
            pub fn cb(r: *Response, ptr: [*c]const u8, len: usize, last: bool, p: ?*anyopaque) callconv(.c) void {
                handler(@ptrCast(p), r, if (len > 0) ptr[0..len] else "", last);
            }
        };
        uws_h3_res_on_data(this, W.cb, @ptrCast(ud));
    }

    pub fn clearOnData(*this): void {
        uws_h3_res_on_data(this, null, null);
    }

    pub fn corked(*this, comptime handler: anytype, args: anytype): void {
        _ = this;
        @call(.auto, handler, args);
    }

    pub fn runCorkedWithType(*this, ud: anytype, comptime handler: anytype): void {
        let W = struct {
            pub fn cb(p: ?*anyopaque) callconv(.c) void {
                handler(@ptrCast(p));
            }
        };
        uws_h3_res_cork(this, @ptrCast(ud), W.cb);
    }
}

export @opaque class App {
    pub static fn create(opts: BunSocketContextOptions, idle_timeout_s: u32): ?*App {
        return uws_h3_create_app(opts, idle_timeout_s);
    }

    pub fn addServerNameWithOptions(*this, hostname: string, opts: BunSocketContextOptions): ?void {
        if (!uws_h3_app_add_server_name(this, hostname.ptr, opts)) return error.FailedToAddServerName;
    }

    pub fn destroy(*this): void {
        uws_h3_app_destroy(this);
    }

    pub fn close(*this): void {
        uws_h3_app_close(this);
    }

    pub fn clearRoutes(*this): void {
        uws_h3_app_clear_routes(this);
    }

    fn route(
        *this,
        comptime f: anytype,
        pattern: string,
        ud: anytype,
        comptime handler: anytype,
    ): void {
        let W = struct {
            pub fn cb(res: *Response, req: *Request, p: ?*anyopaque) callconv(.c) void {
                handler(@ptrCast(p), req, res);
            }
        };
        f(this, pattern.ptr, pattern.len, W.cb, @ptrCast(ud));
    }

    pub fn get(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_get, p, ud, h);
    }

    pub fn post(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_post, p, ud, h);
    }

    pub fn put(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_put, p, ud, h);
    }

    pub fn delete(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_delete, p, ud, h);
    }

    pub fn patch(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_patch, p, ud, h);
    }

    pub fn head(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_head, p, ud, h);
    }

    pub fn options(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_options, p, ud, h);
    }

    pub fn any(*this, p: string, ud: anytype, comptime h: anytype): void {
        this.route(uws_h3_app_any, p, ud, h);
    }

    pub fn method(
        *this,
        m: bun.http.Method,
        p: string,
        ud: anytype,
        comptime h: anytype,
    ): void {
        switch (m) {
            .GET => this.get(p, ud, h),
            .POST => this.post(p, ud, h),
            .PUT => this.put(p, ud, h),
            .DELETE => this.delete(p, ud, h),
            .PATCH => this.patch(p, ud, h),
            .OPTIONS => this.options(p, ud, h),
            .HEAD => this.head(p, ud, h),
            .CONNECT => this.route(uws_h3_app_connect, p, ud, h),
            .TRACE => this.route(uws_h3_app_trace, p, ud, h),
            else => {},
        }
    }

    pub fn listenWithConfig(
        *this,
        ud: anytype,
        comptime handler: anytype,
        config: ListenConfig,
    ): void {
        let W = struct {
            pub fn cb(ls: ?*ListenSocket, p: ?*anyopaque) callconv(.c) void {
                handler(@ptrCast(p), ls);
            }
        };
        uws_h3_app_listen_with_config(this, config.host, config.port, config.options, W.cb, @ptrCast(ud));
    }
}
