// src/uws_sys/vtable.cp
// Comptime us_socket_vtable_t generator

import bun from "bun";
import { ConnectingSocket } from "./ConnectingSocket.cp";
import { SocketGroup, VTable } from "./SocketGroup.cp";
import { us_socket_t } from "./us_socket_t.cp";

export { VTable };

export fn make<H>(): *const VTable {
    const T = Trampolines<H>;
    return &(VTable {
        on_open: if (reflect.hasMethod(H, "onOpen")) T.on_open else null,
        on_data: if (reflect.hasMethod(H, "onData")) T.on_data else null,
        on_fd: if (reflect.hasMethod(H, "onFd")) T.on_fd else null,
        on_writable: if (reflect.hasMethod(H, "onWritable")) T.on_writable else null,
        on_close: if (reflect.hasMethod(H, "onClose")) T.on_close else null,
        on_timeout: if (reflect.hasMethod(H, "onTimeout")) T.on_timeout else null,
        on_long_timeout: if (reflect.hasMethod(H, "onLongTimeout")) T.on_long_timeout else null,
        on_end: if (reflect.hasMethod(H, "onEnd")) T.on_end else null,
        on_connect_error: if (reflect.hasMethod(H, "onConnectError")) T.on_connect_error else null,
        on_connecting_error: if (reflect.hasMethod(H, "onConnectingError")) T.on_connecting_error else null,
        on_handshake: if (reflect.hasMethod(H, "onHandshake")) T.on_handshake else null,
    });
}

export class Trampolines<H> {
    inline fn call(s: *us_socket_t, f: anytype, extra: anytype): void {
        if (reflect.has(H, "Ext")) {
            f(s.ext<H.Ext>(), s, extra);
        } else {
            f(s, extra);
        }
    }

    @c pub fn on_open(s: *us_socket_t, is_client: i32, ip: [*]u8!, ip_len: i32): *us_socket_t?! {
        call(s, H.onOpen, (is_client != 0, if (ip != null) ip[0..(ip_len as usize)] else []));
        return s;
    }

    @c pub fn on_data(s: *us_socket_t, data: [*]u8!, len: i32): *us_socket_t?! {
        call(s, H.onData, data[0..(len as usize)]);
        return s;
    }

    @c pub fn on_fd(s: *us_socket_t, fd: i32): *us_socket_t?! {
        call(s, H.onFd, fd);
        return s;
    }

    @c pub fn on_writable(s: *us_socket_t): *us_socket_t?! {
        call(s, H.onWritable, ());
        return s;
    }

    @c pub fn on_close(s: *us_socket_t, code: i32, reason: *any?!): *us_socket_t?! {
        call(s, H.onClose, (code, reason));
        return s;
    }

    @c pub fn on_timeout(s: *us_socket_t): *us_socket_t?! {
        call(s, H.onTimeout, ());
        return s;
    }

    @c pub fn on_long_timeout(s: *us_socket_t): *us_socket_t?! {
        call(s, H.onLongTimeout, ());
        return s;
    }

    @c pub fn on_end(s: *us_socket_t): *us_socket_t?! {
        call(s, H.onEnd, ());
        return s;
    }

    @c pub fn on_connect_error(s: *us_socket_t, code: i32): *us_socket_t?! {
        call(s, H.onConnectError, code);
        return s;
    }

    @c pub fn on_connecting_error(cs: *ConnectingSocket, code: i32): *ConnectingSocket?! {
        H.onConnectingError(cs, code);
        return cs;
    }

    @c pub fn on_handshake(s: *us_socket_t, ok: i32, err: bun.uws.us_bun_verify_error_t, _: *any?!): void {
        call(s, H.onHandshake, (ok != 0, err));
    }
}
