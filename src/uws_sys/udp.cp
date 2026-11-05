zig {
    const bun = @import("bun");
    const std = @import("std");

    const uws = bun.uws;
    const Loop = uws.Loop;

    const udp = @This();
}

export @opaque class Socket {
    zig {
        pub fn create(loop: *Loop, data_cb: *const fn (*Socket, *PacketBuffer, c_int) callconv(.c) void, drain_cb: *const fn (*Socket) callconv(.c) void, close_cb: *const fn (*Socket) callconv(.c) void, recv_error_cb: *const fn (*Socket, c_int) callconv(.c) void, host: [*c]const u8, port: c_ushort, options: c_int, err: ?*c_int, user_data: ?*anyopaque) ?*Socket {
            return us_create_udp_socket(loop, data_cb, drain_cb, close_cb, recv_error_cb, host, port, options, err, user_data);
        }

        pub fn send(this: *Socket, payloads: []const [*]const u8, lengths: []const usize, addresses: []const ?*const anyopaque) c_int {
            bun.assert(payloads.len == lengths.len and payloads.len == addresses.len);
            return us_udp_socket_send(this, payloads.ptr, lengths.ptr, addresses.ptr, @intCast(payloads.len));
        }

        pub fn user(this: *Socket) ?*anyopaque {
            return us_udp_socket_user(this);
        }

        pub fn bind(this: *Socket, hostname: [*c]const u8, port: c_uint) c_int {
            return us_udp_socket_bind(this, hostname, port);
        }

        pub fn boundIp(this: *Socket, buf: [*c]u8, length: *i32) void {
            return us_udp_socket_bound_ip(this, buf, length);
        }

        pub fn remoteIp(this: *Socket, buf: [*c]u8, length: *i32) void {
            return us_udp_socket_remote_ip(this, buf, length);
        }

        pub fn connect(this: *Socket, hostname: [*c]const u8, port: c_uint) c_int {
            return us_udp_socket_connect(this, hostname, port);
        }

        pub fn setBroadcast(this: *Socket, enabled: bool) c_int {
            return us_udp_socket_set_broadcast(this, @intCast(@intFromBool(enabled)));
        }

        pub fn setUnicastTTL(this: *Socket, ttl: i32) c_int {
            return us_udp_socket_set_ttl_unicast(this, @intCast(ttl));
        }

        pub fn setMulticastTTL(this: *Socket, ttl: i32) c_int {
            return us_udp_socket_set_ttl_multicast(this, @intCast(ttl));
        }

        pub fn setMulticastLoopback(this: *Socket, enabled: bool) c_int {
            return us_udp_socket_set_multicast_loopback(this, @intCast(@intFromBool(enabled)));
        }

        pub fn setMulticastInterface(this: *Socket, iface: *const std.posix.sockaddr.storage) c_int {
            return us_udp_socket_set_multicast_interface(this, iface);
        }

        pub fn setMembership(this: *Socket, address: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: bool) c_int {
            return us_udp_socket_set_membership(this, address, iface, @intFromBool(drop));
        }

        pub fn setSourceSpecificMembership(this: *Socket, source: *const std.posix.sockaddr.storage, group: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: bool) c_int {
            return us_udp_socket_set_source_specific_membership(this, source, group, iface, @intFromBool(drop));
        }

        extern fn us_create_udp_socket(loop: ?*Loop, data_cb: *const fn (*Socket, *PacketBuffer, c_int) callconv(.c) void, drain_cb: *const fn (*Socket) callconv(.c) void, close_cb: *const fn (*Socket) callconv(.c) void, recv_error_cb: *const fn (*Socket, c_int) callconv(.c) void, host: [*c]const u8, port: c_ushort, options: c_int, err: ?*c_int, user_data: ?*anyopaque) ?*Socket;
        extern fn us_udp_socket_connect(socket: *Socket, hostname: [*c]const u8, port: c_uint) c_int;
        extern fn us_udp_socket_disconnect(socket: *Socket) c_int;
        extern fn us_udp_socket_send(socket: *Socket, [*c]const [*c]const u8, [*c]const usize, [*c]const ?*const anyopaque, c_int) c_int;
        extern fn us_udp_socket_user(socket: *Socket) ?*anyopaque;
        extern fn us_udp_socket_bind(socket: *Socket, hostname: [*c]const u8, port: c_uint) c_int;
        extern fn us_udp_socket_bound_port(socket: *Socket) c_int;
        extern fn us_udp_socket_bound_ip(socket: *Socket, buf: [*c]u8, length: [*c]i32) void;
        extern fn us_udp_socket_remote_ip(socket: *Socket, buf: [*c]u8, length: [*c]i32) void;
        extern fn us_udp_socket_close(socket: *Socket) void;
        extern fn us_udp_socket_set_broadcast(socket: *Socket, enabled: c_int) c_int;
        extern fn us_udp_socket_set_ttl_unicast(socket: *Socket, ttl: c_int) c_int;
        extern fn us_udp_socket_set_ttl_multicast(socket: *Socket, ttl: c_int) c_int;
        extern fn us_udp_socket_set_multicast_loopback(socket: *Socket, enabled: c_int) c_int;
        extern fn us_udp_socket_set_multicast_interface(socket: *Socket, iface: *const std.posix.sockaddr.storage) c_int;
        extern fn us_udp_socket_set_membership(socket: *Socket, address: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: c_int) c_int;
        extern fn us_udp_socket_set_source_specific_membership(socket: *Socket, source: *const std.posix.sockaddr.storage, group: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: c_int) c_int;
    }

    pub fn boundPort(*this): i32 {
        zig {
            return us_udp_socket_bound_port(self);
        }
    }

    pub fn close(*this): void {
        zig {
            us_udp_socket_close(self);
        }
    }

    pub fn disconnect(*this): i32 {
        zig {
            return us_udp_socket_disconnect(self);
        }
    }
}

export @opaque class PacketBuffer {
    zig {
        pub fn getPeer(this: *PacketBuffer, index: c_int) *std.posix.sockaddr.storage {
            return us_udp_packet_buffer_peer(this, index);
        }

        pub fn getPayload(this: *PacketBuffer, index: c_int) []u8 {
            const payload = us_udp_packet_buffer_payload(this, index);
            const len = us_udp_packet_buffer_payload_length(this, index);
            return payload[0..@as(usize, @intCast(len))];
        }

        extern fn us_udp_packet_buffer_peer(buf: ?*PacketBuffer, index: c_int) *std.posix.sockaddr.storage;
        extern fn us_udp_packet_buffer_payload(buf: ?*PacketBuffer, index: c_int) [*]u8;
        extern fn us_udp_packet_buffer_payload_length(buf: ?*PacketBuffer, index: c_int) c_int;
        extern fn us_udp_packet_buffer_truncated(buf: ?*PacketBuffer, index: c_int) c_int;
    }

    pub fn getTruncated(*this, index: i32): bool {
        zig {
            return us_udp_packet_buffer_truncated(self, index) != 0;
        }
    }
}
