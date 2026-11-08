import { SocketGroup } from "./SocketGroup.cp";
import { SocketKind } from "./SocketKind.cp";
import { Loop } from "./Loop.cp";

@c {
    fn us_connecting_socket_close(s: *ConnectingSocket): void;
    fn us_connecting_socket_group(s: *ConnectingSocket): *SocketGroup;
    fn us_connecting_socket_kind(s: *ConnectingSocket): u8;
    fn us_connecting_socket_ext(s: *ConnectingSocket): *!;
    fn us_connecting_socket_get_error(s: *ConnectingSocket): i32;
    fn us_connecting_socket_get_native_handle(s: *ConnectingSocket): *!?;
    fn us_connecting_socket_is_closed(s: *ConnectingSocket): i32;
    fn us_connecting_socket_is_shut_down(s: *ConnectingSocket): i32;
    fn us_connecting_socket_long_timeout(s: *ConnectingSocket, seconds: u32): void;
    fn us_connecting_socket_shutdown(s: *ConnectingSocket): void;
    fn us_connecting_socket_shutdown_read(s: *ConnectingSocket): void;
    fn us_connecting_socket_timeout(s: *ConnectingSocket, seconds: u32): void;
    fn us_connecting_socket_get_loop(s: *ConnectingSocket): *Loop;
}

export @opaque class ConnectingSocket {
    pub fn close(*this): void {
        us_connecting_socket_close(this);
    }

    pub fn group(*this): *SocketGroup {
        return us_connecting_socket_group(this);
    }

    pub fn rawGroup(*this): *SocketGroup {
        return this.group();
    }

    pub fn kind(*this): SocketKind {
        return us_connecting_socket_kind(this) as SocketKind;
    }

    pub fn loop(*this): *Loop {
        return us_connecting_socket_get_loop(this);
    }

    pub fn ext(*this, @comptime T: type): *T {
        unsafe {
            return us_connecting_socket_ext(this) as *T;
        }
    }

    pub fn getError(*this): i32 {
        return us_connecting_socket_get_error(this);
    }

    pub fn getNativeHandle(*this): *!? {
        return us_connecting_socket_get_native_handle(this);
    }

    pub fn isClosed(*this): bool {
        return us_connecting_socket_is_closed(this) == 1;
    }

    pub fn isShutdown(*this): bool {
        return us_connecting_socket_is_shut_down(this) == 1;
    }

    pub fn longTimeout(*this, seconds: u32): void {
        us_connecting_socket_long_timeout(this, seconds);
    }

    pub fn shutdown(*this): void {
        us_connecting_socket_shutdown(this);
    }

    pub fn shutdownRead(*this): void {
        us_connecting_socket_shutdown_read(this);
    }

    pub fn timeout(*this, seconds: u32): void {
        us_connecting_socket_timeout(this, seconds);
    }
}
