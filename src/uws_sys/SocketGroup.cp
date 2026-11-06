// src/uws_sys/SocketGroup.cp
// CP port of struct us_socket_group_t

import bun from "bun";
import { ConnectingSocket } from "./ConnectingSocket.cp";
import { ListenSocket } from "./ListenSocket.cp";
import { Loop } from "./Loop.cp";

zig {
    const uws = bun.uws;
    const SocketKind = uws.SocketKind;
    const SslCtx = uws.SslCtx;
    const us_socket_t = uws.us_socket_t;
}

export @extern class SocketGroup {
    loop: *Loop? = null,
    vtable: *const VTable? = null,
    #ext: *anyopaque? = null,
    head_sockets: *us_socket_t? = null,
    head_connecting_sockets: *ConnectingSocket? = null,
    head_listen_sockets: *uws.ListenSocket? = null,
    iterator: *us_socket_t? = null,
    prev: *SocketGroup? = null,
    next: *SocketGroup? = null,
    global_tick: u32 = 0,
    low_prio_count: u16 = 0,
    timestamp: u8 = 0,
    long_timestamp: u8 = 0,
    linked: u8 = 0,

    zig {
        pub const VTable = extern struct {
            on_open: ?*const fn (*us_socket_t, c_int, [*c]u8, c_int) callconv(.c) ?*us_socket_t = null,
            on_data: ?*const fn (*us_socket_t, [*c]u8, c_int) callconv(.c) ?*us_socket_t = null,
            on_fd: ?*const fn (*us_socket_t, c_int) callconv(.c) ?*us_socket_t = null,
            on_writable: ?*const fn (*us_socket_t) callconv(.c) ?*us_socket_t = null,
            on_close: ?*const fn (*us_socket_t, c_int, ?*anyopaque) callconv(.c) ?*us_socket_t = null,
            on_timeout: ?*const fn (*us_socket_t) callconv(.c) ?*us_socket_t = null,
            on_long_timeout: ?*const fn (*us_socket_t) callconv(.c) ?*us_socket_t = null,
            on_end: ?*const fn (*us_socket_t) callconv(.c) ?*us_socket_t = null,
            on_connect_error: ?*const fn (*us_socket_t, c_int) callconv(.c) ?*us_socket_t = null,
            on_connecting_error: ?*const fn (*ConnectingSocket, c_int) callconv(.c) ?*ConnectingSocket = null,
            on_handshake: ?*const fn (*us_socket_t, c_int, uws.us_bun_verify_error_t, ?*anyopaque) callconv(.c) void = null,
        };

        comptime {
            if (@sizeOf(SocketGroup) != 9 * @sizeOf(*anyopaque) + 16)
                @compileError("SocketGroup layout drifted from us_socket_group_t");
            if (@sizeOf(VTable) != 11 * @sizeOf(*anyopaque))
                @compileError("VTable layout drifted from us_socket_vtable_t");
        }

        pub const ConnectResult = union(enum) {
            socket: *us_socket_t,
            connecting: *ConnectingSocket,
            failed,
        };
    }

    pub fn init(*this, loop_: *Loop, vt: *const VTable?, owner_ptr: anytype): void {
        zig {
            const P = @TypeOf(owner_ptr);
            const erased: ?*anyopaque = if (P == @TypeOf(null)) null else switch (@typeInfo(P)) {
                .pointer => |p| if (p.size == .one) @ptrCast(@constCast(owner_ptr)) else @compileError("SocketGroup.init owner must be a single-item pointer"),
                .optional => if (owner_ptr) |o| @ptrCast(@constCast(o)) else null,
                else => @compileError("SocketGroup.init owner must be a pointer or null"),
            };
            c.us_socket_group_init(self, loop_, vt, erased);
        }
    }

    pub fn release(*this): void {
        this.deinit();
    }

    pub fn deinit(*this): void {
        zig {
            c.us_socket_group_deinit(self);
        }
    }

    pub fn closeAll(*this): void {
        zig {
            c.us_socket_group_close_all(self);
        }
    }

    pub fn getLoop(this): *Loop {
        zig {
            return self.loop.?;
        }
    }

    pub fn owner(this, @comptime T: type): *T {
        zig {
            return @ptrCast(@alignCast(self.#ext.?));
        }
    }

    pub fn isEmpty(this): bool {
        zig {
            return self.head_sockets == null and
                self.head_connecting_sockets == null and
                self.head_listen_sockets == null and
                self.low_prio_count == 0;
        }
    }

    pub fn listen(
        *this,
        kind: SocketKind,
        ssl_ctx: *SslCtx?,
        host: anytype,
        port: c_int,
        options: c_int,
        socket_ext_size: c_int,
        err: *c_int,
    ): *uws.ListenSocket? {
        zig {
            return c.us_socket_group_listen(self, @intFromEnum(kind), ssl_ctx, host, port, options, socket_ext_size, err);
        }
    }

    pub fn listenUnix(
        *this,
        kind: SocketKind,
        ssl_ctx: *SslCtx?,
        path: anytype,
        pathlen: usize,
        options: c_int,
        socket_ext_size: c_int,
        err: *c_int,
    ): *uws.ListenSocket? {
        zig {
            return c.us_socket_group_listen_unix(self, @intFromEnum(kind), ssl_ctx, path, pathlen, options, socket_ext_size, err);
        }
    }

    pub fn connect(
        *this,
        kind: SocketKind,
        ssl_ctx: *SslCtx?,
        host: anytype,
        port: c_int,
        options: c_int,
        socket_ext_size: c_int,
    ): ConnectResult {
        zig {
            var has_dns_resolved: c_int = 0;
            const ptr = c.us_socket_group_connect(self, @intFromEnum(kind), ssl_ctx, host, port, options, socket_ext_size, &has_dns_resolved) orelse return .failed;
            return if (has_dns_resolved != 0)
                .{ .socket = @ptrCast(@alignCast(ptr)) }
            else
                .{ .connecting = @ptrCast(@alignCast(ptr)) };
        }
    }

    pub fn connectUnix(
        *this,
        kind: SocketKind,
        ssl_ctx: *SslCtx?,
        path: anytype,
        pathlen: usize,
        options: c_int,
        socket_ext_size: c_int,
    ): *us_socket_t? {
        zig {
            return c.us_socket_group_connect_unix(self, @intFromEnum(kind), ssl_ctx, path, pathlen, options, socket_ext_size);
        }
    }

    pub fn fromFd(
        *this,
        kind: SocketKind,
        ssl_ctx: *SslCtx?,
        socket_ext_size: c_int,
        fd: uws.LIBUS_SOCKET_DESCRIPTOR,
        ipc: bool,
    ): *us_socket_t? {
        zig {
            return c.us_socket_from_fd(self, @intFromEnum(kind), ssl_ctx, socket_ext_size, fd, @intFromBool(ipc));
        }
    }

    pub fn pair(*this, kind: SocketKind, ext_size: c_int, fds: anytype): *us_socket_t? {
        zig {
            return c.us_socket_pair(self, @intFromEnum(kind), ext_size, fds);
        }
    }

    pub fn nextInLoop(*this): *SocketGroup? {
        zig {
            return c.us_socket_group_next(self);
        }
    }
}

zig {
    const c = struct {
        extern fn us_socket_group_init(*SocketGroup, *Loop, ?*const SocketGroup.VTable, ?*anyopaque) void;
        extern fn us_socket_group_deinit(*SocketGroup) void;
        extern fn us_socket_group_close_all(*SocketGroup) void;
        extern fn us_socket_group_timestamp(*SocketGroup) c_ushort;
        extern fn us_socket_group_loop(*SocketGroup) *Loop;
        extern fn us_socket_group_next(*SocketGroup) ?*SocketGroup;
        extern fn us_socket_group_listen(*SocketGroup, u8, ?*SslCtx, ?[*:0]const u8, c_int, c_int, c_int, *c_int) ?*uws.ListenSocket;
        extern fn us_socket_group_listen_unix(*SocketGroup, u8, ?*SslCtx, [*]const u8, usize, c_int, c_int, *c_int) ?*uws.ListenSocket;
        extern fn us_socket_group_connect(*SocketGroup, u8, ?*SslCtx, [*:0]const u8, c_int, c_int, c_int, *c_int) ?*anyopaque;
        extern fn us_socket_group_connect_unix(*SocketGroup, u8, ?*SslCtx, [*]const u8, usize, c_int, c_int) ?*us_socket_t;
        extern fn us_socket_from_fd(*SocketGroup, u8, ?*SslCtx, c_int, uws.LIBUS_SOCKET_DESCRIPTOR, c_int) ?*us_socket_t;
        extern fn us_socket_pair(*SocketGroup, u8, c_int, *[2]uws.LIBUS_SOCKET_DESCRIPTOR) ?*us_socket_t;
    };
}
