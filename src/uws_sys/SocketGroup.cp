// src/uws_sys/SocketGroup.cp
// CP port of struct us_socket_group_t

import bun from "bun";
import { ConnectingSocket } from "./ConnectingSocket.cp";
import { ListenSocket } from "./ListenSocket.cp";
import { Loop } from "./Loop.cp";
import { SocketKind } from "./SocketKind.cp";
import { us_socket_t } from "./us_socket_t.cp";

export class VTable {
    on_open: @c ((s: *us_socket_t, is_client: i32, ip: [*]u8!, ip_len: i32): *us_socket_t?!)? = null,
    on_data: @c ((s: *us_socket_t, data: [*]u8!, len: i32): *us_socket_t?!)? = null,
    on_fd: @c ((s: *us_socket_t, fd: i32): *us_socket_t?!)? = null,
    on_writable: @c ((s: *us_socket_t): *us_socket_t?!)? = null,
    on_close: @c ((s: *us_socket_t, code: i32, ext: *any?!): *us_socket_t?!)? = null,
    on_timeout: @c ((s: *us_socket_t): *us_socket_t?!)? = null,
    on_long_timeout: @c ((s: *us_socket_t): *us_socket_t?!)? = null,
    on_end: @c ((s: *us_socket_t): *us_socket_t?!)? = null,
    on_connect_error: @c ((s: *us_socket_t, code: i32): *us_socket_t?!)? = null,
    on_connecting_error: @c ((s: *ConnectingSocket, code: i32): *ConnectingSocket?!)? = null,
    on_handshake: @c ((s: *us_socket_t, success: i32, verify_error: bun.uws.us_bun_verify_error_t, custom: *any?!): void)? = null,
}

export enum ConnectResult {
    socket(*us_socket_t),
    connecting(*ConnectingSocket),
    failed,
}

@c {
    fn us_socket_group_init(g: *SocketGroup, loop: *Loop, vt: *const VTable?!, owner: *any?!): void;
    fn us_socket_group_deinit(g: *SocketGroup): void;
    fn us_socket_group_close_all(g: *SocketGroup): void;
    fn us_socket_group_timestamp(g: *SocketGroup): u16;
    fn us_socket_group_loop(g: *SocketGroup): *Loop;
    fn us_socket_group_next(g: *SocketGroup): *SocketGroup?!;
    fn us_socket_group_listen(g: *SocketGroup, kind: u8, ssl_ctx: *bun.uws.SslCtx?!, host: cstr?, port: i32, options: i32, ext_size: i32, err: *i32): *ListenSocket?!;
    fn us_socket_group_listen_unix(g: *SocketGroup, kind: u8, ssl_ctx: *bun.uws.SslCtx?!, path: [*]const u8!, pathlen: usize, options: i32, ext_size: i32, err: *i32): *ListenSocket?!;
    fn us_socket_group_connect(g: *SocketGroup, kind: u8, ssl_ctx: *bun.uws.SslCtx?!, host: cstr, port: i32, options: i32, ext_size: i32, dns_resolved: *i32): *any?!;
    fn us_socket_group_connect_unix(g: *SocketGroup, kind: u8, ssl_ctx: *bun.uws.SslCtx?!, path: [*]const u8!, pathlen: usize, options: i32, ext_size: i32): *us_socket_t?!;
    fn us_socket_from_fd(g: *SocketGroup, kind: u8, ssl_ctx: *bun.uws.SslCtx?!, ext_size: i32, fd: i32, ipc: i32): *us_socket_t?!;
    fn us_socket_pair(g: *SocketGroup, kind: u8, ext_size: i32, fds: *i32[2]): *us_socket_t?!;
}

export @extern class SocketGroup {
    loop: *Loop? = null,
    vtable: *const VTable? = null,
    #ext: *any?! = null,
    head_sockets: *us_socket_t? = null,
    head_connecting_sockets: *ConnectingSocket? = null,
    head_listen_sockets: *ListenSocket? = null,
    iterator: *us_socket_t? = null,
    prev: *SocketGroup? = null,
    next: *SocketGroup? = null,
    global_tick: u32 = 0,
    low_prio_count: u16 = 0,
    timestamp: u8 = 0,
    long_timestamp: u8 = 0,
    linked: u8 = 0,

    pub fn init(*this, loop_: *Loop, vt: *const VTable?, owner_ptr: *any?!): void {
        us_socket_group_init(this, loop_, vt, owner_ptr);
    }

    pub fn release(*this): void {
        this.deinit();
    }

    pub fn deinit(*this): void {
        us_socket_group_deinit(this);
    }

    pub fn closeAll(*this): void {
        us_socket_group_close_all(this);
    }

    pub fn getLoop(this): *Loop {
        return this.loop!;
    }

    pub fn owner<T>(this): *T {
        return this.#ext! as *T;
    }

    pub fn isEmpty(this): bool {
        return this.head_sockets == null &&
            this.head_connecting_sockets == null &&
            this.head_listen_sockets == null &&
            this.low_prio_count == 0;
    }

    pub fn listen(
        *this,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        host: cstr?,
        port: i32,
        options: i32,
        socket_ext_size: i32,
        err: *i32,
    ): *ListenSocket? {
        return us_socket_group_listen(this, kind as u8, ssl_ctx, host, port, options, socket_ext_size, err);
    }

    pub fn listenUnix(
        *this,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        path: string,
        options: i32,
        socket_ext_size: i32,
        err: *i32,
    ): *ListenSocket? {
        return us_socket_group_listen_unix(this, kind as u8, ssl_ctx, path.ptr, path.len, options, socket_ext_size, err);
    }

    pub fn connect(
        *this,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        host: cstr,
        port: i32,
        options: i32,
        socket_ext_size: i32,
    ): ConnectResult {
        let has_dns_resolved: i32 = 0;
        let ptr = us_socket_group_connect(this, kind as u8, ssl_ctx, host, port, options, socket_ext_size, &has_dns_resolved);
        if (ptr == null) return ConnectResult.failed;
        if (has_dns_resolved != 0) {
            return ConnectResult.socket(ptr! as *us_socket_t);
        } else {
            return ConnectResult.connecting(ptr! as *ConnectingSocket);
        }
    }

    pub fn connectUnix(
        *this,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        path: string,
        options: i32,
        socket_ext_size: i32,
    ): *us_socket_t? {
        return us_socket_group_connect_unix(this, kind as u8, ssl_ctx, path.ptr, path.len, options, socket_ext_size);
    }

    pub fn fromFd(
        *this,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        socket_ext_size: i32,
        fd: bun.FD,
        ipc: bool,
    ): *us_socket_t? {
        return us_socket_from_fd(this, kind as u8, ssl_ctx, socket_ext_size, fd.native(), if (ipc) 1 else 0);
    }

    pub fn pair(*this, kind: SocketKind, ext_size: i32, fds: *i32[2]): *us_socket_t? {
        return us_socket_pair(this, kind as u8, ext_size, fds);
    }

    pub fn nextInLoop(*this): *SocketGroup? {
        return us_socket_group_next(this);
    }
}
