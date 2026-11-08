export enum SocketKind : u8 {
    invalid = 0,
    dynamic,

    bun_socket_tcp,
    bun_socket_tls,
    bun_listener_tcp,
    bun_listener_tls,

    http_client,
    http_client_tls,

    ws_client_upgrade,
    ws_client_upgrade_tls,
    ws_client,
    ws_client_tls,

    postgres,
    postgres_tls,
    mysql,
    mysql_tls,
    valkey,
    valkey_tls,

    spawn_ipc,

    uws_http,
    uws_http_tls,
    uws_ws,
    uws_ws_tls,

    pub fn isTLS(this): bool {
        return switch (this) {
            .bun_socket_tls,
            .bun_listener_tls,
            .http_client_tls,
            .ws_client_upgrade_tls,
            .ws_client_tls,
            .postgres_tls,
            .mysql_tls,
            .valkey_tls,
            .uws_http_tls,
            .uws_ws_tls => true,
            else => false,
        };
    }
}

zig {
    pub const exported_ordinals = struct {
        export const BUN_SOCKET_KIND_DYNAMIC: u8 = @intFromEnum(SocketKind.dynamic);
        export const BUN_SOCKET_KIND_UWS_HTTP: u8 = @intFromEnum(SocketKind.uws_http);
        export const BUN_SOCKET_KIND_UWS_HTTP_TLS: u8 = @intFromEnum(SocketKind.uws_http_tls);
        export const BUN_SOCKET_KIND_UWS_WS: u8 = @intFromEnum(SocketKind.uws_ws);
        export const BUN_SOCKET_KIND_UWS_WS_TLS: u8 = @intFromEnum(SocketKind.uws_ws_tls);
    };
    comptime {
        _ = exported_ordinals;
    }
}
