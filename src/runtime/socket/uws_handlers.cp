// Per-SocketKind handler adapters in CP language
// Converted from src/runtime/socket/uws_handlers.zig

const IPC = @import("../../jsc/ipc.zig");
const js_valkey = @import("../valkey_jsc/js_valkey.zig");
const mysql = @import("../../sql_jsc/mysql.zig");
const websocket_client = @import("../../http_jsc/websocket_client.zig");
const websocket_upgrade_client = @import("../../http_jsc/websocket_client/WebSocketUpgradeClient.zig");

const bun = @import("bun");
const api = bun.jsc.API;

const uws = bun.uws;
const ConnectingSocket = uws.ConnectingSocket;
const us_socket_t = uws.us_socket_t;

@inline fn swallow(result: auto) {
    if (@typeInfo(@TypeOf(result)) == .error_union) {
        result catch {};
    }
}

pub fn PtrHandler(comptime T: type, comptime ssl: bool): type {
    const S = uws.NewSocketHandler(ssl);
    return struct {
        pub const Ext = *?*T;
        inline fn wrap(s: *us_socket_t) S {
            return S.from(s);
        }
        pub fn onOpen(ext: Ext, s: *us_socket_t, _: bool, _: string) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onOpen")) {
                swallow(this.onOpen(wrap(s)));
            }
        }
        pub fn onData(ext: Ext, s: *us_socket_t, data: string) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onData")) {
                swallow(this.onData(wrap(s), data));
            }
        }
        pub fn onWritable(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onWritable")) {
                swallow(this.onWritable(wrap(s)));
            }
        }
        pub fn onClose(ext: Ext, s: *us_socket_t, code: i32, reason: ?*anyopaque) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onClose")) {
                swallow(this.onClose(wrap(s), code, reason));
            }
        }
        pub fn onTimeout(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onTimeout")) {
                swallow(this.onTimeout(wrap(s)));
            }
        }
        pub fn onLongTimeout(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onLongTimeout")) {
                swallow(this.onLongTimeout(wrap(s)));
            }
        }
        pub fn onEnd(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onEnd")) {
                swallow(this.onEnd(wrap(s)));
            }
        }
        pub fn onConnectError(ext: Ext, s: *us_socket_t, code: i32) void {
            const this = ext.*;
            s.close(.failure);
            if (this) |t| {
                if (@hasDecl(T, "onConnectError")) {
                    swallow(t.onConnectError(wrap(s), code));
                }
            }
        }
        pub fn onConnectingError(c: *ConnectingSocket, code: i32) void {
            const this = c.ext(?*T).* orelse return;
            if (@hasDecl(T, "onConnectError")) {
                swallow(this.onConnectError(S.fromConnecting(c), code));
            }
        }
        pub fn onHandshake(ext: Ext, s: *us_socket_t, ok: bool, err: uws.us_bun_verify_error_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onHandshake")) {
                swallow(this.onHandshake(wrap(s), @intFromBool(ok), err));
            }
        }
        pub fn onFd(ext: Ext, s: *us_socket_t, fd: i32) void {
            const this = ext.* orelse return;
            if (@hasDecl(T, "onFd")) {
                swallow(this.onFd(wrap(s), fd));
            }
        }
    };
}

pub fn BunSocket(comptime ssl: bool): type {
    return PtrHandler(api.NewSocket(ssl), ssl);
}

pub fn BunListener(comptime ssl: bool): type {
    const S = uws.NewSocketHandler(ssl);
    const NS = api.NewSocket(ssl);
    return struct {
        pub fn onOpen(s: *us_socket_t, _: bool, _: string) void {
            const listener = s.group().owner(api.Listener);
            const ns = api.Listener.onCreate(ssl, listener, S.from(s));
            swallow(ns.onOpen(S.from(s)));
        }
        pub fn onClose(s: *us_socket_t, code: i32, reason: ?*anyopaque) void {
            if (s.ext(?*NS).*) |ns| {
                swallow(ns.onClose(S.from(s), code, reason));
            }
        }
        pub fn onData(s: *us_socket_t, data: string) void {
            if (s.ext(?*NS).*) |ns| {
                swallow(ns.onData(S.from(s), data));
            }
        }
        pub fn onWritable(s: *us_socket_t) void {
            if (s.ext(?*NS).*) |ns| {
                swallow(ns.onWritable(S.from(s)));
            }
        }
        pub fn onEnd(s: *us_socket_t) void {
            if (s.ext(?*NS).*) |ns| {
                swallow(ns.onEnd(S.from(s)));
            }
        }
        pub fn onTimeout(s: *us_socket_t) void {
            if (s.ext(?*NS).*) |ns| {
                swallow(ns.onTimeout(S.from(s)));
            }
        }
        pub fn onHandshake(s: *us_socket_t, ok: bool, err: uws.us_bun_verify_error_t) void {
            if (s.ext(?*NS).*) |ns| {
                swallow(ns.onHandshake(S.from(s), @intFromBool(ok), err));
            }
        }
    };
}

pub fn NsHandler(comptime Owner: type, comptime H: type, comptime ssl: bool): type {
    const S = uws.NewSocketHandler(ssl);
    return struct {
        pub const Ext = *?*Owner;
        inline fn wrap(s: *us_socket_t) S {
            return S.from(s);
        }
        pub fn onOpen(ext: Ext, s: *us_socket_t, _: bool, _: string) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onOpen")) {
                swallow(H.onOpen(this, wrap(s)));
            }
        }
        pub fn onData(ext: Ext, s: *us_socket_t, data: string) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onData")) {
                swallow(H.onData(this, wrap(s), data));
            }
        }
        pub fn onWritable(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onWritable")) {
                swallow(H.onWritable(this, wrap(s)));
            }
        }
        pub fn onClose(ext: Ext, s: *us_socket_t, code: i32, reason: ?*anyopaque) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onClose")) {
                swallow(H.onClose(this, wrap(s), code, reason));
            }
        }
        pub fn onTimeout(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onTimeout")) {
                swallow(H.onTimeout(this, wrap(s)));
            }
        }
        pub fn onLongTimeout(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onLongTimeout")) {
                swallow(H.onLongTimeout(this, wrap(s)));
            }
        }
        pub fn onEnd(ext: Ext, s: *us_socket_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onEnd")) {
                swallow(H.onEnd(this, wrap(s)));
            }
        }
        pub fn onConnectError(ext: Ext, s: *us_socket_t, code: i32) void {
            const this = ext.*;
            s.close(.failure);
            if (this) |t| {
                if (@hasDecl(H, "onConnectError")) {
                    swallow(H.onConnectError(t, wrap(s), code));
                }
            }
        }
        pub fn onConnectingError(c: *ConnectingSocket, code: i32) void {
            const this = c.ext(?*Owner).* orelse return;
            if (@hasDecl(H, "onConnectError")) {
                swallow(H.onConnectError(this, S.fromConnecting(c), code));
            }
        }
        pub fn onHandshake(ext: Ext, s: *us_socket_t, ok: bool, err: uws.us_bun_verify_error_t) void {
            const this = ext.* orelse return;
            if (@hasDecl(H, "onHandshake") and @TypeOf(H.onHandshake) != @TypeOf(null)) {
                swallow(H.onHandshake(this, wrap(s), @intFromBool(ok), err));
            }
        }
    };
}

pub fn HTTPClient(comptime ssl: bool): type {
    const H = bun.http.NewHTTPContext(ssl).Handler;
    const S = uws.NewSocketHandler(ssl);
    return struct {
        pub const Ext = *?*anyopaque;
        inline fn wrap(s: *us_socket_t) S {
            return S.from(s);
        }
        inline fn fwd(ext: Ext, comptime name: []const u8, args: anytype) void {
            if (@hasDecl(H, name) and @TypeOf(@field(H, name)) != @TypeOf(null)) {
                swallow(@call(.auto, @field(H, name), .{ext.* orelse return} ++ args));
            }
        }
        pub fn onOpen(ext: Ext, s: *us_socket_t, _: bool, _: string) void {
            fwd(ext, "onOpen", .{wrap(s)});
        }
        pub fn onData(ext: Ext, s: *us_socket_t, data: string) void {
            fwd(ext, "onData", .{ wrap(s), data });
        }
        pub fn onWritable(ext: Ext, s: *us_socket_t) void {
            fwd(ext, "onWritable", .{wrap(s)});
        }
        pub fn onClose(ext: Ext, s: *us_socket_t, code: i32, reason: ?*anyopaque) void {
            fwd(ext, "onClose", .{ wrap(s), code, reason });
        }
        pub fn onTimeout(ext: Ext, s: *us_socket_t) void {
            fwd(ext, "onTimeout", .{wrap(s)});
        }
        pub fn onLongTimeout(ext: Ext, s: *us_socket_t) void {
            fwd(ext, "onLongTimeout", .{wrap(s)});
        }
        pub fn onEnd(ext: Ext, s: *us_socket_t) void {
            fwd(ext, "onEnd", .{wrap(s)});
        }
        pub fn onConnectError(ext: Ext, s: *us_socket_t, code: i32) void {
            const owner = ext.*;
            s.close(.failure);
            if (@hasDecl(H, "onConnectError") and @TypeOf(H.onConnectError) != @TypeOf(null)) {
                swallow(H.onConnectError(owner orelse return, wrap(s), code));
            }
        }
        pub fn onConnectingError(cs: *ConnectingSocket, code: i32) void {
            if (@hasDecl(H, "onConnectError")) {
                swallow(H.onConnectError(cs.ext(?*anyopaque).* orelse return, S.fromConnecting(cs), code));
            }
        }
        pub fn onHandshake(ext: Ext, s: *us_socket_t, ok: bool, err: uws.us_bun_verify_error_t) void {
            fwd(ext, "onHandshake", .{ wrap(s), @as(i32, @intFromBool(ok)), err });
        }
    };
}

pub fn WSUpgrade(comptime ssl: bool): type {
    return PtrHandler(websocket_upgrade_client.NewHTTPUpgradeClient(ssl), ssl);
}

pub fn WSClient(comptime ssl: bool): type {
    return PtrHandler(websocket_client.NewWebSocketClient(ssl), ssl);
}

pub fn Postgres(comptime ssl: bool): type {
    const C = bun.api.Postgres.PostgresSQLConnection;
    return NsHandler(C, C.SocketHandler(ssl), ssl);
}

pub fn MySQL(comptime ssl: bool): type {
    return NsHandler(mysql.MySQLConnection, mysql.MySQLConnection.SocketHandler(ssl), ssl);
}

pub fn Valkey(comptime ssl: bool): type {
    return NsHandler(js_valkey.JSValkeyClient, js_valkey.SocketHandler(ssl), ssl);
}

pub struct SpawnIPC {
    const H = IPC.IPCHandlers.PosixSocket;
    const S = uws.NewSocketHandler(false);
    pub const Ext = *?*IPC.SendQueue;
    pub static fn onOpen(_: *?*IPC.SendQueue, _: *us_socket_t, _: bool, _: string) {}
    pub static fn onData(ext: *?*IPC.SendQueue, s: *us_socket_t, data: string) {
        const this = ext.* orelse return;
        H.onData(this, S.from(s), data);
    }
    pub static fn onFd(ext: *?*IPC.SendQueue, s: *us_socket_t, fd: i32) {
        const this = ext.* orelse return;
        H.onFd(this, S.from(s), fd);
    }
    pub static fn onWritable(ext: *?*IPC.SendQueue, s: *us_socket_t) {
        const this = ext.* orelse return;
        H.onWritable(this, S.from(s));
    }
    pub static fn onClose(ext: *?*IPC.SendQueue, s: *us_socket_t, code: i32, reason: ?*anyopaque) {
        const this = ext.* orelse return;
        H.onClose(this, S.from(s), code, reason);
    }
    pub static fn onTimeout(ext: *?*IPC.SendQueue, s: *us_socket_t) {
        const this = ext.* orelse return;
        H.onTimeout(this, S.from(s));
    }
    pub static fn onEnd(ext: *?*IPC.SendQueue, s: *us_socket_t) {
        const this = ext.* orelse return;
        H.onEnd(this, S.from(s));
    }
}
