// src/uws_sys/App.cp
// Zig bindings for µWebSockets App class in CP language

import bun from "bun";
import { ListenSocket } from "./ListenSocket.cp";
import { Request } from "./Request.cp";
import { Response, uws_res } from "./Response.cp";
import { BunSocketContextOptions } from "./SocketContext.cp";
import { us_socket_t } from "./us_socket_t.cp";
import { SocketHandler } from "./socket.cp";

export @opaque class uws_app_s {}
export type uws_app_t = uws_app_s;

export @extern @value class uws_app_listen_config_t {
    port: i32,
    host: cstr? = null,
    options: i32 = 0,
}

@c {
    fn uws_create_app(ssl: i32, opts: BunSocketContextOptions): *uws_app_t?!;
    fn uws_app_destroy(ssl: i32, app: *uws_app_s): void;
    fn uws_app_close(ssl: i32, app: *uws_app_s): void;
    fn uws_app_close_idle(ssl: i32, app: *uws_app_s): void;
    fn uws_app_set_on_clienterror(ssl: i32, app: *uws_app_s, handler: @c ((data: *any!, code: i32, socket: *us_socket_t, err_code: u8, raw_pkt: [*]u8?!, len: i32): void), user_data: *any!): void;
    fn uws_app_get(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_post(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_put(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_delete(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_patch(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_head(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_options(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_connect(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_trace(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_any(ssl: i32, app: *uws_app_t, pattern: [*]const u8!, pattern_length: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_app_run(ssl: i32, app: *uws_app_t): void;
    fn uws_app_domain(ssl: i32, app: *uws_app_t, domain: cstr): void;
    fn uws_app_set_flags(ssl: i32, app: *uws_app_t, require_host: bool, strict_method: bool): void;
    fn uws_app_set_max_http_header_size(ssl: i32, app: *uws_app_t, max_sz: u64): void;
    fn uws_app_listen(ssl: i32, app: *uws_app_t, host: cstr?, port: i32, options: i32, handler: @c ((socket: *ListenSocket?!, conf: uws_app_listen_config_t, data: *any?!): void), user_data: *any?!): void;
    fn uws_app_listen_with_config(ssl: i32, app: *uws_app_t, host: cstr?, port: u16, options: i32, handler: @c ((socket: *ListenSocket?!, data: *any?!): void), user_data: *any?!): void;
    fn uws_constructor_failed(ssl: i32, app: *uws_app_t): bool;
    fn uws_num_subscribers(ssl: i32, app: *uws_app_t, topic: [*]const u8!, len: usize): u32;
    fn uws_publish(ssl: i32, app: *uws_app_t, topic: [*]const u8!, tl: usize, msg: [*]const u8!, ml: usize, op: bun.uws.Opcode, compress: bool): bool;
    fn uws_get_native_handle(ssl: i32, app: *any!): *any?!;
    fn uws_remove_server_name(ssl: i32, app: *uws_app_t, pattern: cstr): void;
    fn uws_add_server_name(ssl: i32, app: *uws_app_t, pattern: cstr): void;
    fn uws_add_server_name_with_options(ssl: i32, app: *uws_app_t, pattern: cstr, opts: BunSocketContextOptions): i32;
    fn uws_app_clear_routes(ssl: i32, app: *uws_app_t): void;
    fn uws_app_listen_domain_with_options(ssl: i32, app: *uws_app_t, domain: cstr, pathlen: usize, flags: i32, cb: @c ((ls: *ListenSocket, domain: cstr, flags: i32, data: *any!): void), ud: *any?!): void;
}

export class App<const ssl: bool> {
    pub const is_ssl: bool = ssl;
    pub const ssl_flag: i32 = if (ssl) 1 else 0;

    pub type ResponseType = Response<if (ssl) 1 else 0>;

    pub fn close(*this): void {
        uws_app_close(ssl_flag, this as *uws_app_s);
    }

    pub fn closeIdleConnections(*this): void {
        uws_app_close_idle(ssl_flag, this as *uws_app_s);
    }

    pub static fn create(opts: BunSocketContextOptions): App<ssl>? {
        let app = uws_create_app(ssl_flag, opts);
        if (app == null) return null;
        return app! as App<ssl>;
    }

    pub fn destroy(*this): void {
        uws_app_destroy(ssl_flag, this as *uws_app_s);
    }

    pub fn setFlags(*this, require_host_header: bool, use_strict_method_validation: bool): void {
        uws_app_set_flags(ssl_flag, this as *uws_app_t, require_host_header, use_strict_method_validation);
    }

    pub fn setMaxHTTPHeaderSize(*this, max_header_size: u64): void {
        uws_app_set_max_http_header_size(ssl_flag, this as *uws_app_t, max_header_size);
    }

    pub fn clearRoutes(*this): void {
        uws_app_clear_routes(ssl_flag, this as *uws_app_t);
    }

    pub fn publish(*this, topic: string, message: string, opcode: bun.uws.Opcode, compress: bool): bool {
        return uws_publish(
            ssl_flag,
            this as *uws_app_t,
            topic.ptr,
            topic.len,
            message.ptr,
            message.len,
            opcode,
            compress,
        );
    }

    fn route<UD>(
        *this,
        f: @c ((ssl: i32, app: *uws_app_t, pattern: [*]const u8!, len: usize, handler: @c ((res: *uws_res, req: *Request, ud: *any?!): void)?, ud: *any?!): void),
        pattern: string,
        ud: *UD,
        handler: fn(*UD, *Request, *ResponseType): void,
    ): void {
        let Handler = struct {
            @c pub fn handle(res: *uws_res, req: *Request, user_data: *any?!): void {
                handler(user_data as *UD, req, res as *ResponseType);
            }
        };
        f(ssl_flag, this as *uws_app_t, pattern.ptr, pattern.len, Handler.handle, ud as *any?!);
    }

    pub fn get<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_get, pattern, ud, handler);
    }

    pub fn post<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_post, pattern, ud, handler);
    }

    pub fn put<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_put, pattern, ud, handler);
    }

    pub fn delete<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_delete, pattern, ud, handler);
    }

    pub fn patch<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_patch, pattern, ud, handler);
    }

    pub fn head<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_head, pattern, ud, handler);
    }

    pub fn options<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_options, pattern, ud, handler);
    }

    pub fn any<UD>(*this, pattern: string, ud: *UD, handler: fn(*UD, *Request, *ResponseType): void): void {
        this.route(uws_app_any, pattern, ud, handler);
    }

    pub fn method<UD>(
        *this,
        m: bun.http.Method,
        pattern: string,
        ud: *UD,
        handler: fn(*UD, *Request, *ResponseType): void,
    ): void {
        switch (m) {
            .GET => this.get(pattern, ud, handler),
            .POST => this.post(pattern, ud, handler),
            .PUT => this.put(pattern, ud, handler),
            .DELETE => this.delete(pattern, ud, handler),
            .PATCH => this.patch(pattern, ud, handler),
            .OPTIONS => this.options(pattern, ud, handler),
            .HEAD => this.head(pattern, ud, handler),
            .CONNECT => this.route(uws_app_connect, pattern, ud, handler),
            .TRACE => this.route(uws_app_trace, pattern, ud, handler),
            else => {},
        }
    }

    pub fn domain(*this, pattern: cstr): void {
        uws_app_domain(ssl_flag, this as *uws_app_t, pattern);
    }

    pub fn run(*this): void {
        uws_app_run(ssl_flag, this as *uws_app_t);
    }

    pub fn constructorFailed(*this): bool {
        return uws_constructor_failed(ssl_flag, this as *uws_app_t);
    }

    pub fn numSubscribers(*this, topic: string): u32 {
        return uws_num_subscribers(ssl_flag, this as *uws_app_t, topic.ptr, topic.len);
    }

    pub fn getNativeHandle(*this): *any?! {
        return uws_get_native_handle(ssl_flag, this as *any!);
    }

    pub fn removeServerName(*this, hostname_pattern: cstr): void {
        uws_remove_server_name(ssl_flag, this as *uws_app_t, hostname_pattern);
    }

    pub fn addServerName(*this, hostname_pattern: cstr): void {
        uws_add_server_name(ssl_flag, this as *uws_app_t, hostname_pattern);
    }

    pub fn addServerNameWithOptions(*this, hostname_pattern: cstr, opts: BunSocketContextOptions): ?void {
        if (uws_add_server_name_with_options(ssl_flag, this as *uws_app_t, hostname_pattern, opts) != 0) {
            return error.FailedToAddServerName;
        }
    }

    pub fn listen<UserData>(
        *this,
        port: i32,
        user_data: *UserData,
        handler: fn(*UserData, *ListenSocket?, uws_app_listen_config_t): void,
    ): void {
        let Wrapper = struct {
            @c pub fn handle(socket: *ListenSocket?!, conf: uws_app_listen_config_t, data: *any?!): void {
                handler(data as *UserData, socket, conf);
            }
        };
        uws_app_listen(ssl_flag, this as *uws_app_t, null, port, 0, Wrapper.handle, user_data as *any?!);
    }

    pub fn onClientError<UserData>(
        *this,
        user_data: *UserData,
        handler: fn(*UserData, *us_socket_t, u8, string): void,
    ): void {
        let Wrapper = struct {
            @c pub fn handle(data: *any!, code: i32, socket: *us_socket_t, error_code: u8, raw_packet: [*]u8?!, raw_packet_length: i32): void {
                let bytes = if (raw_packet != null && raw_packet_length > 0) raw_packet![0..(raw_packet_length as usize)] else "";
                handler(data as *UserData, socket, error_code, bytes);
            }
        };
        uws_app_set_on_clienterror(ssl_flag, this as *uws_app_s, Wrapper.handle, user_data as *any!);
    }

    pub fn listenWithConfig<UserData>(
        *this,
        user_data: *UserData,
        handler: fn(*UserData, *ListenSocket?): void,
        config: uws_app_listen_config_t,
    ): void {
        let Wrapper = struct {
            @c pub fn handle(socket: *ListenSocket?!, data: *any?!): void {
                handler(data as *UserData, socket);
            }
        };
        uws_app_listen_with_config(ssl_flag, this as *uws_app_t, config.host, config.port as u16, config.options, Wrapper.handle, user_data as *any?!);
    }

    pub fn listenOnUnixSocket<UserData>(
        *this,
        user_data: *UserData,
        handler: fn(*UserData, *ListenSocket?): void,
        domain_name: string,
        flags: i32,
    ): void {
        let Wrapper = struct {
            @c pub fn handle(socket: *ListenSocket, domain: cstr, fl: i32, data: *any!): void {
                handler(data as *UserData, socket);
            }
        };
        let domainZ: cstr = bun.handleOom(bun.default_allocator.dupeZ(u8, domain_name));
        defer bun.default_allocator.free(domainZ);
        uws_app_listen_domain_with_options(
            ssl_flag,
            this as *uws_app_t,
            domainZ,
            domain_name.len,
            flags,
            Wrapper.handle,
            user_data as *any?!,
        );
    }
}

pub type AppTCP = App<false>;
pub type AppTLS = App<true>;
