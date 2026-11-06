// src/uws_sys/h3.cp
// HTTP/3 bindings for uWebSockets / libusockets in CP language

import bun from "bun";
import { State, WriteResult } from "./Response.cp";
import { BunSocketContextOptions } from "./SocketContext.cp";

zig {
    const std = @import("std");
    const uws = bun.uws;
    const SocketAddress = uws.SocketAddress;

    const c = struct {
        const Handler = ?*const fn (*Response, *Request, ?*anyopaque) callconv(.c) void;
        const ListenHandler = ?*const fn (?*ListenSocket, ?*anyopaque) callconv(.c) void;
        const HeaderCb = *const fn ([*]const u8, usize, [*]const u8, usize, ?*anyopaque) callconv(.c) void;

        extern fn uws_h3_create_app(BunSocketContextOptions, u32) ?*App;
        extern fn uws_h3_app_destroy(*App) void;
        extern fn uws_h3_app_close(*App) void;
        extern fn uws_h3_app_clear_routes(*App) void;
        extern fn uws_h3_app_add_server_name(*App, [*:0]const u8, BunSocketContextOptions) bool;
        extern fn uws_h3_res_write_continue(*Response) void;
        extern fn uws_h3_app_get(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_post(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_put(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_delete(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_patch(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_head(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_options(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_connect(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_trace(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_any(*App, [*]const u8, usize, Handler, ?*anyopaque) void;
        extern fn uws_h3_app_listen_with_config(*App, ?[*:0]const u8, u16, i32, ListenHandler, ?*anyopaque) void;
        extern fn uws_h3_listen_socket_port(*ListenSocket) i32;
        extern fn uws_h3_listen_socket_local_address(*ListenSocket, [*]u8, c_int) c_int;
        extern fn uws_h3_listen_socket_close(*ListenSocket) void;

        extern fn uws_h3_res_state(*Response) State;
        extern fn uws_h3_res_end(*Response, [*c]const u8, usize, bool) void;
        extern fn uws_h3_res_end_stream(*Response, bool) void;
        extern fn uws_h3_res_force_close(*Response) void;
        extern fn uws_h3_res_try_end(*Response, [*c]const u8, usize, usize, bool) bool;
        extern fn uws_h3_res_end_without_body(*Response, bool) void;
        extern fn uws_h3_res_pause(*Response) void;
        extern fn uws_h3_res_resume(*Response) void;
        extern fn uws_h3_res_write_status(*Response, [*c]const u8, usize) void;
        extern fn uws_h3_res_write_header(*Response, [*c]const u8, usize, [*c]const u8, usize) void;
        extern fn uws_h3_res_write_header_int(*Response, [*c]const u8, usize, u64) void;
        extern fn uws_h3_res_mark_wrote_content_length_header(*Response) void;
        extern fn uws_h3_res_write_mark(*Response) void;
        extern fn uws_h3_res_flush_headers(*Response, bool) void;
        extern fn uws_h3_res_write(*Response, ?[*]const u8, *usize) bool;
        extern fn uws_h3_res_get_write_offset(*Response) u64;
        extern fn uws_h3_res_override_write_offset(*Response, u64) void;
        extern fn uws_h3_res_has_responded(*Response) bool;
        extern fn uws_h3_res_get_buffered_amount(*Response) u64;
        extern fn uws_h3_res_reset_timeout(*Response) void;
        extern fn uws_h3_res_timeout(*Response, u8) void;
        extern fn uws_h3_res_end_sendfile(*Response, u64, bool) void;
        extern fn uws_h3_res_get_socket_data(*Response) ?*anyopaque;
        extern fn uws_h3_res_on_writable(*Response, ?*const fn (*Response, u64, ?*anyopaque) callconv(.c) bool, ?*anyopaque) void;
        extern fn uws_h3_res_clear_on_writable(*Response) void;
        extern fn uws_h3_res_on_aborted(*Response, ?*const fn (*Response, ?*anyopaque) callconv(.c) void, ?*anyopaque) void;
        extern fn uws_h3_res_on_timeout(*Response, ?*const fn (*Response, ?*anyopaque) callconv(.c) void, ?*anyopaque) void;
        extern fn uws_h3_res_on_data(*Response, ?*const fn (*Response, [*c]const u8, usize, bool, ?*anyopaque) callconv(.c) void, ?*anyopaque) void;
        extern fn uws_h3_res_cork(*Response, ?*anyopaque, *const fn (?*anyopaque) callconv(.c) void) void;
        extern fn uws_h3_res_get_remote_address_info(*Response, *[*]const u8, *i32, *bool) usize;

        extern fn uws_h3_req_get_yield(*Request) bool;
        extern fn uws_h3_req_set_yield(*Request, bool) void;
        extern fn uws_h3_req_get_url(*Request, *[*]const u8) usize;
        extern fn uws_h3_req_get_method(*Request, *[*]const u8) usize;
        extern fn uws_h3_req_get_header(*Request, [*c]const u8, usize, *[*]const u8) usize;
        extern fn uws_h3_req_get_query(*Request, [*c]const u8, usize, *[*]const u8) usize;
        extern fn uws_h3_req_get_parameter(*Request, u16, *[*]const u8) usize;
        extern fn uws_h3_req_for_each_header(*Request, HeaderCb, ?*anyopaque) void;
    };
}

export @opaque class ListenSocket {
    pub fn close(*this): void {
        zig {
            c.uws_h3_listen_socket_close(self);
        }
    }

    pub fn getLocalPort(*this): i32 {
        zig {
            return c.uws_h3_listen_socket_port(self);
        }
    }

    pub fn getLocalAddress(*this, buf: &mut u8[]): string? {
        zig {
            const n = c.uws_h3_listen_socket_local_address(self, buf.ptr, @intCast(buf.len));
            if (n <= 0) return null;
            return buf[0..@intCast(n)];
        }
    }
}

export @opaque class Request {
    pub fn isAncient(this): bool {
        return false;
    }

    pub fn getYield(*this): bool {
        zig {
            return c.uws_h3_req_get_yield(self);
        }
    }

    pub fn setYield(*this, y: bool): void {
        zig {
            c.uws_h3_req_set_yield(self, y);
        }
    }

    pub fn url(*this): string {
        zig {
            var p: [*]const u8 = undefined;
            return p[0..c.uws_h3_req_get_url(self, &p)];
        }
    }

    pub fn method(*this): string {
        zig {
            var p: [*]const u8 = undefined;
            return p[0..c.uws_h3_req_get_method(self, &p)];
        }
    }

    pub fn header(*this, name: string): string? {
        zig {
            var p: [*]const u8 = undefined;
            const n = c.uws_h3_req_get_header(self, name.ptr, name.len, &p);
            return if (n == 0) null else p[0..n];
        }
    }

    pub fn dateForHeader(*this, name: string): !u64? {
        zig {
            const value = self.header(name) orelse return null;
            var s = bun.String.init(value);
            defer s.deref();
            const ms = try s.parseDate(bun.jsc.VirtualMachine.get().global);
            if (!std.math.isNan(ms) and std.math.isFinite(ms) and ms >= 0) return @intFromFloat(ms);
            return null;
        }
    }

    pub fn query(*this, name: string): string {
        zig {
            var p: [*]const u8 = undefined;
            return p[0..c.uws_h3_req_get_query(self, name.ptr, name.len, &p)];
        }
    }

    pub fn parameter(*this, idx: u16): string {
        zig {
            var p: [*]const u8 = undefined;
            return p[0..c.uws_h3_req_get_parameter(self, idx, &p)];
        }
    }

    pub fn forEachHeader(
        *this,
        @comptime Ctx: type,
        @comptime cb: anytype,
        ctx: Ctx,
    ): void {
        zig {
            const Wrap = struct {
                fn each(n: [*]const u8, nl: usize, v: [*]const u8, vl: usize, ud: ?*anyopaque) callconv(.c) void {
                    cb(@ptrCast(@alignCast(ud.?)), n[0..nl], v[0..vl]);
                }
            };
            c.uws_h3_req_for_each_header(self, Wrap.each, ctx);
        }
    }
}

export @opaque class Response {
    pub fn end(*this, data: string, close_connection: bool): void {
        zig {
            c.uws_h3_res_end(self, data.ptr, data.len, close_connection);
        }
    }

    pub fn tryEnd(*this, data: string, total: usize, close_connection: bool): bool {
        zig {
            return c.uws_h3_res_try_end(self, data.ptr, data.len, total, close_connection);
        }
    }

    pub fn endWithoutBody(*this, close_connection: bool): void {
        zig {
            c.uws_h3_res_end_without_body(self, close_connection);
        }
    }

    pub fn endStream(*this, close_connection: bool): void {
        zig {
            c.uws_h3_res_end_stream(self, close_connection);
        }
    }

    pub fn endSendFile(*this, write_offset: u64, close_connection: bool): void {
        zig {
            c.uws_h3_res_end_sendfile(self, write_offset, close_connection);
        }
    }

    pub fn write(*this, data: string): WriteResult {
        zig {
            var len: usize = data.len;
            return if (c.uws_h3_res_write(self, data.ptr, &len)) .{ .want_more = len } else .{ .backpressure = len };
        }
    }

    pub fn writeStatus(*this, status: string): void {
        zig {
            c.uws_h3_res_write_status(self, status.ptr, status.len);
        }
    }

    pub fn writeHeader(*this, key: string, value: string): void {
        zig {
            c.uws_h3_res_write_header(self, key.ptr, key.len, value.ptr, value.len);
        }
    }

    pub fn writeHeaderInt(*this, key: string, value: u64): void {
        zig {
            c.uws_h3_res_write_header_int(self, key.ptr, key.len, value);
        }
    }

    pub fn writeMark(*this): void {
        zig {
            c.uws_h3_res_write_mark(self);
        }
    }

    pub fn markWroteContentLengthHeader(*this): void {
        zig {
            c.uws_h3_res_mark_wrote_content_length_header(self);
        }
    }

    pub fn writeContinue(*this): void {
        zig {
            c.uws_h3_res_write_continue(self);
        }
    }

    pub fn flushHeaders(*this, immediate: bool): void {
        zig {
            c.uws_h3_res_flush_headers(self, immediate);
        }
    }

    pub fn pause(*this): void {
        zig {
            c.uws_h3_res_pause(self);
        }
    }

    pub fn @"resume"(*this): void {
        zig {
            c.uws_h3_res_resume(self);
        }
    }

    pub fn timeout(*this, seconds: u8): void {
        zig {
            c.uws_h3_res_timeout(self, seconds);
        }
    }

    pub fn resetTimeout(*this): void {
        zig {
            c.uws_h3_res_reset_timeout(self);
        }
    }

    pub fn getWriteOffset(*this): u64 {
        zig {
            return c.uws_h3_res_get_write_offset(self);
        }
    }

    pub fn overrideWriteOffset(*this, off: u64): void {
        zig {
            c.uws_h3_res_override_write_offset(self, off);
        }
    }

    pub fn getBufferedAmount(*this): u64 {
        zig {
            return c.uws_h3_res_get_buffered_amount(self);
        }
    }

    pub fn hasResponded(*this): bool {
        zig {
            return c.uws_h3_res_has_responded(self);
        }
    }

    pub fn state(*this): State {
        zig {
            return c.uws_h3_res_state(self);
        }
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

    pub fn getSocketData(*this): *anyopaque? {
        zig {
            return c.uws_h3_res_get_socket_data(self);
        }
    }

    pub fn getRemoteSocketInfo(*this): SocketAddress? {
        zig {
            var addr: SocketAddress = .{ .ip = undefined, .port = undefined, .is_ipv6 = undefined };
            var ip_ptr: [*]const u8 = undefined;
            const len = c.uws_h3_res_get_remote_address_info(self, &ip_ptr, &addr.port, &addr.is_ipv6);
            if (len == 0) return null;
            addr.ip = ip_ptr[0..len];
            return addr;
        }
    }

    pub fn forceClose(*this): void {
        zig {
            c.uws_h3_res_force_close(self);
        }
    }

    pub fn onWritable(
        *this,
        @comptime UD: type,
        @comptime handler: anytype,
        ud: UD,
    ): void {
        zig {
            const W = struct {
                fn cb(r: *Response, off: u64, p: ?*anyopaque) callconv(.c) bool {
                    return handler(@ptrCast(@alignCast(p.?)), off, r);
                }
            };
            c.uws_h3_res_on_writable(self, W.cb, ud);
        }
    }

    pub fn clearOnWritable(*this): void {
        zig {
            c.uws_h3_res_clear_on_writable(self);
        }
    }

    pub fn onAborted(
        *this,
        @comptime UD: type,
        @comptime handler: anytype,
        ud: UD,
    ): void {
        zig {
            const W = struct {
                fn cb(r: *Response, p: ?*anyopaque) callconv(.c) void {
                    handler(@ptrCast(@alignCast(p.?)), r);
                }
            };
            c.uws_h3_res_on_aborted(self, W.cb, ud);
        }
    }

    pub fn clearAborted(*this): void {
        zig {
            c.uws_h3_res_on_aborted(self, null, null);
        }
    }

    pub fn onTimeout(
        *this,
        @comptime UD: type,
        @comptime handler: anytype,
        ud: UD,
    ): void {
        zig {
            const W = struct {
                fn cb(r: *Response, p: ?*anyopaque) callconv(.c) void {
                    handler(@ptrCast(@alignCast(p.?)), r);
                }
            };
            c.uws_h3_res_on_timeout(self, W.cb, ud);
        }
    }

    pub fn clearTimeout(*this): void {
        zig {
            c.uws_h3_res_on_timeout(self, null, null);
        }
    }

    pub fn onData(
        *this,
        @comptime UD: type,
        @comptime handler: anytype,
        ud: UD,
    ): void {
        zig {
            const W = struct {
                fn cb(r: *Response, ptr: [*c]const u8, len: usize, last: bool, p: ?*anyopaque) callconv(.c) void {
                    handler(@ptrCast(@alignCast(p.?)), r, if (len > 0) ptr[0..len] else "", last);
                }
            };
            c.uws_h3_res_on_data(self, W.cb, ud);
        }
    }

    pub fn clearOnData(*this): void {
        zig {
            c.uws_h3_res_on_data(self, null, null);
        }
    }

    pub fn corked(*this, @comptime handler: anytype, args: anytype): void {
        zig {
            _ = self;
            @call(.auto, handler, args);
        }
    }

    pub fn runCorkedWithType(*this, @comptime UD: type, @comptime handler: anytype, ud: UD): void {
        zig {
            const W = struct {
                fn cb(p: ?*anyopaque) callconv(.c) void {
                    handler(@ptrCast(@alignCast(p.?)));
                }
            };
            c.uws_h3_res_cork(self, ud, W.cb);
        }
    }
}

export @opaque class App {
    zig {
        fn route(
            comptime which: @TypeOf(.x),
            this: *App,
            pattern: []const u8,
            comptime UD: type,
            ud: UD,
            comptime handler: fn (UD, *Request, *Response) void,
        ) void {
            const W = struct {
                fn cb(res: *Response, req: *Request, p: ?*anyopaque) callconv(.c) void {
                    handler(@ptrCast(@alignCast(p.?)), req, res);
                }
            };
            const f = switch (which) {
                .get => c.uws_h3_app_get,
                .post => c.uws_h3_app_post,
                .put => c.uws_h3_app_put,
                .delete => c.uws_h3_app_delete,
                .patch => c.uws_h3_app_patch,
                .head => c.uws_h3_app_head,
                .options => c.uws_h3_app_options,
                .connect => c.uws_h3_app_connect,
                .trace => c.uws_h3_app_trace,
                .any => c.uws_h3_app_any,
                else => unreachable,
            };
            f(this, pattern.ptr, pattern.len, W.cb, ud);
        }
    }

    pub static fn create(opts: BunSocketContextOptions, idle_timeout_s: u32): App? {
        zig {
            return c.uws_h3_create_app(opts, idle_timeout_s);
        }
    }

    pub fn addServerNameWithOptions(*this, hostname: anytype, opts: BunSocketContextOptions): !void {
        zig {
            if (!c.uws_h3_app_add_server_name(self, hostname.ptr, opts)) return error.FailedToAddServerName;
        }
    }

    pub fn destroy(*this): void {
        zig {
            c.uws_h3_app_destroy(self);
        }
    }

    pub fn close(*this): void {
        zig {
            c.uws_h3_app_close(self);
        }
    }

    pub fn clearRoutes(*this): void {
        zig {
            c.uws_h3_app_clear_routes(self);
        }
    }

    pub fn get(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.get, self, p, UD, ud, h);
        }
    }

    pub fn post(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.post, self, p, UD, ud, h);
        }
    }

    pub fn put(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.put, self, p, UD, ud, h);
        }
    }

    pub fn delete(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.delete, self, p, UD, ud, h);
        }
    }

    pub fn patch(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.patch, self, p, UD, ud, h);
        }
    }

    pub fn head(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.head, self, p, UD, ud, h);
        }
    }

    pub fn options(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.options, self, p, UD, ud, h);
        }
    }

    pub fn any(*this, p: string, @comptime UD: type, ud: UD, @comptime h: anytype): void {
        zig {
            route(.any, self, p, UD, ud, h);
        }
    }

    pub fn method(
        *this,
        m: bun.http.Method,
        p: string,
        @comptime UD: type,
        ud: UD,
        @comptime h: anytype,
    ): void {
        zig {
            switch (m) {
                .GET => self.get(p, UD, ud, h),
                .POST => self.post(p, UD, ud, h),
                .PUT => self.put(p, UD, ud, h),
                .DELETE => self.delete(p, UD, ud, h),
                .PATCH => self.patch(p, UD, ud, h),
                .OPTIONS => self.options(p, UD, ud, h),
                .HEAD => self.head(p, UD, ud, h),
                .CONNECT => route(.connect, self, p, UD, ud, h),
                .TRACE => route(.trace, self, p, UD, ud, h),
                else => {},
            }
        }
    }

    pub fn listenWithConfig(
        *this,
        @comptime UD: type,
        ud: UD,
        @comptime handler: anytype,
        config: ListenConfig,
    ): void {
        zig {
            const W = struct {
                fn cb(ls: ?*ListenSocket, p: ?*anyopaque) callconv(.c) void {
                    handler(@ptrCast(@alignCast(p.?)), ls);
                }
            };
            c.uws_h3_app_listen_with_config(self, config.host, config.port, config.options, W.cb, ud);
        }
    }
}

export @extern @value class ListenConfig {
    zig {
        port: u16,
        host: ?[*:0]const u8 = null,
        options: i32 = 0,
    }
}
