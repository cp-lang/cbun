import bun from "bun";
import { SocketGroup } from "./SocketGroup.cp";
import { us_socket_t } from "./us_socket_t.cp";

zig {
    const uws = bun.uws;
    const c = struct {
        extern fn us_listen_socket_close(ls: *ListenSocket) void;
        extern fn us_listen_socket_group(ls: *ListenSocket) *SocketGroup;
        extern fn us_listen_socket_ext(ls: *ListenSocket) *anyopaque;
        extern fn us_listen_socket_get_fd(ls: *ListenSocket) uws.LIBUS_SOCKET_DESCRIPTOR;
        extern fn us_listen_socket_port(ls: *ListenSocket) c_int;
        extern fn us_listen_socket_add_server_name(ls: *ListenSocket, hostname: [*:0]const u8, ssl_ctx: *uws.SslCtx, user: ?*anyopaque) c_int;
        extern fn us_listen_socket_remove_server_name(ls: *ListenSocket, hostname: [*:0]const u8) void;
        extern fn us_listen_socket_find_server_name_userdata(ls: *ListenSocket, hostname: [*:0]const u8) ?*anyopaque;
        extern fn us_listen_socket_on_server_name(ls: *ListenSocket, cb: *const fn (*ListenSocket, [*:0]const u8) callconv(.c) void) void;
    };
}

export @opaque class ListenSocket {
    pub fn close(*this): void {
        c.us_listen_socket_close(this);
    }

    pub fn getLocalAddress(*this, buf: anytype): !string {
        return this.getSocket().localAddress(buf);
    }

    pub fn getLocalPort(*this): i32 {
        return this.getSocket().localPort();
    }

    pub fn getSocket(*this): *us_socket_t {
        return @ptrCast(this);
    }

    pub fn socket(*this, @comptime is_ssl: bool): uws.NewSocketHandler(is_ssl) {
        return bun.uws.NewSocketHandler(is_ssl).from(this.getSocket());
    }

    pub fn group(*this): *SocketGroup {
        return c.us_listen_socket_group(this);
    }

    pub fn ext(*this, @comptime T: type): *T {
        return @ptrCast(@alignCast(c.us_listen_socket_ext(this)));
    }

    pub fn fd(*this): bun.FD {
        return bun.FD.fromNative(c.us_listen_socket_get_fd(this));
    }

    pub fn addServerName(*this, hostname: anytype, ssl_ctx: anytype, user: anytype): bool {
        zig {
            const U = @TypeOf(user);
            const erased: ?*anyopaque = if (U == @TypeOf(null)) null else @ptrCast(@constCast(user));
            return c.us_listen_socket_add_server_name(self, hostname, ssl_ctx, erased) == 0;
        }
    }

    pub fn removeServerName(*this, hostname: anytype): void {
        c.us_listen_socket_remove_server_name(this, hostname);
    }

    pub fn findServerNameUserdata(*this, @comptime T: type, hostname: anytype): *T? {
        return @ptrCast(@alignCast(c.us_listen_socket_find_server_name_userdata(this, hostname)));
    }

    pub fn onServerName(*this, cb: anytype): void {
        c.us_listen_socket_on_server_name(this, cb);
    }
}
