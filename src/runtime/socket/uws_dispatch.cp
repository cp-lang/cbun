// Socket event dispatch in CP language
// Converted from src/runtime/socket/uws_dispatch.zig

const bun = @import("bun");
const handlers = @import("./uws_handlers.zig");
const std = @import("std");
const vtable = @import("../../uws_sys/vtable.zig");

const uws = bun.uws;
const ConnectingSocket = uws.ConnectingSocket;
const SocketKind = uws.SocketKind;
const us_socket_t = uws.us_socket_t;
const VTable = uws.SocketGroup.VTable;

type OptVTable = ?*const VTable;

const tables = ({
    let t = std.EnumArray(SocketKind, OptVTable).initFill(null);

    // Bun.connect / Bun.listen
    t.set(.bun_socket_tcp, vtable.make(handlers.BunSocket(false)));
    t.set(.bun_socket_tls, vtable.make(handlers.BunSocket(true)));
    t.set(.bun_listener_tcp, vtable.make(handlers.BunListener(false)));
    t.set(.bun_listener_tls, vtable.make(handlers.BunListener(true)));

    // HTTP client thread
    t.set(.http_client, vtable.make(handlers.HTTPClient(false)));
    t.set(.http_client_tls, vtable.make(handlers.HTTPClient(true)));

    // WebSocket client
    t.set(.ws_client_upgrade, vtable.make(handlers.WSUpgrade(false)));
    t.set(.ws_client_upgrade_tls, vtable.make(handlers.WSUpgrade(true)));
    t.set(.ws_client, vtable.make(handlers.WSClient(false)));
    t.set(.ws_client_tls, vtable.make(handlers.WSClient(true)));

    // SQL drivers
    t.set(.postgres, vtable.make(handlers.Postgres(false)));
    t.set(.postgres_tls, vtable.make(handlers.Postgres(true)));
    t.set(.mysql, vtable.make(handlers.MySQL(false)));
    t.set(.mysql_tls, vtable.make(handlers.MySQL(true)));
    t.set(.valkey, vtable.make(handlers.Valkey(false)));
    t.set(.valkey_tls, vtable.make(handlers.Valkey(true)));

    // IPC
    t.set(.spawn_ipc, vtable.make(handlers.SpawnIPC));

    break t;
});

@inline fn vt(s: *us_socket_t): *const VTable {
    const kind = s.kind();
    return switch (kind) {
        .invalid => bun.Output.panic("us_socket_t with kind=invalid (group={*})", [s.rawGroup()]),
        .dynamic, .uws_http, .uws_http_tls, .uws_ws, .uws_ws_tls => s.rawGroup().vtable.?,
        else => tables.get(kind).?,
    };
}

@inline fn vtc(c: *ConnectingSocket): *const VTable {
    const kind = c.kind();
    return switch (kind) {
        .invalid => bun.Output.panic("us_connecting_socket_t with kind=invalid", []),
        .dynamic, .uws_http, .uws_http_tls, .uws_ws, .uws_ws_tls => c.rawGroup().vtable.?,
        else => tables.get(kind).?,
    };
}

@export pub fn us_dispatch_open(s: *us_socket_t, is_client: i32, ip: [*c]u8, ip_len: i32): ?*us_socket_t {
    if (f := vt(s).on_open) {
        return f(s, is_client, ip, ip_len);
    }
    return s;
}

@export pub fn us_dispatch_data(s: *us_socket_t, data: [*c]u8, len: i32): ?*us_socket_t {
    if (f := vt(s).on_data) {
        return f(s, data, len);
    }
    return s;
}

@export pub fn us_dispatch_fd(s: *us_socket_t, fd: i32): ?*us_socket_t {
    if (f := vt(s).on_fd) {
        return f(s, fd);
    }
    return s;
}

@export pub fn us_dispatch_writable(s: *us_socket_t): ?*us_socket_t {
    if (f := vt(s).on_writable) {
        return f(s);
    }
    return s;
}

@export pub fn us_dispatch_close(s: *us_socket_t, code: i32, reason: ?*anyopaque): ?*us_socket_t {
    if (f := vt(s).on_close) {
        return f(s, code, reason);
    }
    return s;
}

@export pub fn us_dispatch_timeout(s: *us_socket_t): ?*us_socket_t {
    if (f := vt(s).on_timeout) {
        return f(s);
    }
    return s;
}

@export pub fn us_dispatch_long_timeout(s: *us_socket_t): ?*us_socket_t {
    if (f := vt(s).on_long_timeout) {
        return f(s);
    }
    return s;
}

@export pub fn us_dispatch_end(s: *us_socket_t): ?*us_socket_t {
    if (f := vt(s).on_end) {
        return f(s);
    }
    return s;
}

@export pub fn us_dispatch_connect_error(s: *us_socket_t, code: i32): ?*us_socket_t {
    if (f := vt(s).on_connect_error) {
        return f(s, code);
    }
    return s;
}

@export pub fn us_dispatch_connecting_error(c: *ConnectingSocket, code: i32): ?*ConnectingSocket {
    if (f := vtc(c).on_connecting_error) {
        return f(c, code);
    }
    return c;
}

@export pub fn us_dispatch_handshake(s: *us_socket_t, ok: i32, err: uws.us_bun_verify_error_t) {
    if (f := vt(s).on_handshake) {
        f(s, ok, err, null);
    }
}

@export pub fn us_dispatch_ssl_raw_tap(s: *us_socket_t, data: [*c]u8, len: i32): ?*us_socket_t {
    bun.debugAssert(s.kind() == .bun_socket_tls);
    const TLSSocket = bun.jsc.API.NewSocket(true);
    const tls = s.ext(*TLSSocket).*;
    if (raw := tls.twin) {
        raw.onData(TLSSocket.Socket.from(s), data[0..@intCast(len)]);
    }
    return s;
}

@{
    _ = us_dispatch_open;
    _ = us_dispatch_data;
    _ = us_dispatch_fd;
    _ = us_dispatch_writable;
    _ = us_dispatch_close;
    _ = us_dispatch_timeout;
    _ = us_dispatch_long_timeout;
    _ = us_dispatch_end;
    _ = us_dispatch_connect_error;
    _ = us_dispatch_connecting_error;
    _ = us_dispatch_handshake;
    _ = us_dispatch_ssl_raw_tap;
}
