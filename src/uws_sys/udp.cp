import bun from "bun";
import { Loop } from "./Loop.cp";

@c {
    fn us_create_udp_socket(
        loop: ?*Loop,
        data_cb: ?@c fn (*Socket, *PacketBuffer, i32): void,
        drain_cb: ?@c fn (*Socket): void,
        close_cb: ?@c fn (*Socket): void,
        recv_error_cb: ?@c fn (*Socket, i32): void,
        host: cstr?,
        port: u16,
        options: i32,
        err: ?*i32,
        user_data: *!?,
    ): ?*Socket;
    fn us_udp_socket_connect(socket: *Socket, hostname: cstr, port: u32): i32;
    fn us_udp_socket_disconnect(socket: *Socket): i32;
    fn us_udp_socket_send(socket: *Socket, payloads: [*c]const [*c]const u8, lengths: [*c]const usize, addresses: [*c]const ?*const anyopaque, count: i32): i32;
    fn us_udp_socket_user(socket: *Socket): *!?;
    fn us_udp_socket_bind(socket: *Socket, hostname: cstr, port: u32): i32;
    fn us_udp_socket_bound_port(socket: *Socket): i32;
    fn us_udp_socket_bound_ip(socket: *Socket, buf: [*c]u8, length: *i32): void;
    fn us_udp_socket_remote_ip(socket: *Socket, buf: [*c]u8, length: *i32): void;
    fn us_udp_socket_close(socket: *Socket): void;
    fn us_udp_socket_set_broadcast(socket: *Socket, enabled: i32): i32;
    fn us_udp_socket_set_ttl_unicast(socket: *Socket, ttl: i32): i32;
    fn us_udp_socket_set_ttl_multicast(socket: *Socket, ttl: i32): i32;
    fn us_udp_socket_set_multicast_loopback(socket: *Socket, enabled: i32): i32;
    fn us_udp_socket_set_multicast_interface(socket: *Socket, iface: *const std.posix.sockaddr.storage): i32;
    fn us_udp_socket_set_membership(socket: *Socket, address: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: i32): i32;
    fn us_udp_socket_set_source_specific_membership(socket: *Socket, source: *const std.posix.sockaddr.storage, group: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: i32): i32;

    fn us_udp_packet_buffer_peer(buf: ?*PacketBuffer, index: i32): *std.posix.sockaddr.storage;
    fn us_udp_packet_buffer_payload(buf: ?*PacketBuffer, index: i32): [*]u8!;
    fn us_udp_packet_buffer_payload_length(buf: ?*PacketBuffer, index: i32): i32;
    fn us_udp_packet_buffer_truncated(buf: ?*PacketBuffer, index: i32): i32;
}

export @opaque class Socket {
    pub static fn create(
        loop: *Loop,
        data_cb: ?@c fn (*Socket, *PacketBuffer, i32): void,
        drain_cb: ?@c fn (*Socket): void,
        close_cb: ?@c fn (*Socket): void,
        recv_error_cb: ?@c fn (*Socket, i32): void,
        host: cstr?,
        port: u16,
        options: i32,
        err: *i32?,
        user_data: *!?,
    ): ?*Socket {
        return us_create_udp_socket(loop, data_cb, drain_cb, close_cb, recv_error_cb, host, port, options, err, user_data);
    }

    zig {
        pub fn send(this: *Socket, payloads: []const [*]const u8, lengths: []const usize, addresses: []const ?*const anyopaque) c_int {
            bun.assert(payloads.len == lengths.len and payloads.len == addresses.len);
            return us_udp_socket_send(this, payloads.ptr, lengths.ptr, addresses.ptr, @intCast(payloads.len));
        }
    }

    pub fn user(*this): *!? {
        return us_udp_socket_user(this);
    }

    pub fn bind(*this, hostname: cstr, port: u32): i32 {
        return us_udp_socket_bind(this, hostname, port);
    }

    pub fn boundIp(*this, buf: [*c]u8, length: *i32): void {
        us_udp_socket_bound_ip(this, buf, length);
    }

    pub fn remoteIp(*this, buf: [*c]u8, length: *i32): void {
        us_udp_socket_remote_ip(this, buf, length);
    }

    pub fn connect(*this, hostname: cstr, port: u32): i32 {
        return us_udp_socket_connect(this, hostname, port);
    }

    pub fn setBroadcast(*this, enabled: bool): i32 {
        let mut en: i32 = 0;
        if (enabled) {
            en = 1;
        }
        return us_udp_socket_set_broadcast(this, en);
    }

    pub fn setUnicastTTL(*this, ttl: i32): i32 {
        return us_udp_socket_set_ttl_unicast(this, ttl);
    }

    pub fn setMulticastTTL(*this, ttl: i32): i32 {
        return us_udp_socket_set_ttl_multicast(this, ttl);
    }

    pub fn setMulticastLoopback(*this, enabled: bool): i32 {
        let mut en: i32 = 0;
        if (enabled) {
            en = 1;
        }
        return us_udp_socket_set_multicast_loopback(this, en);
    }

    zig {
        pub fn setMulticastInterface(this: *Socket, iface: *const std.posix.sockaddr.storage) c_int {
            return us_udp_socket_set_multicast_interface(this, iface);
        }

        pub fn setMembership(this: *Socket, address: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: bool) c_int {
            return us_udp_socket_set_membership(this, address, iface, @intFromBool(drop));
        }

        pub fn setSourceSpecificMembership(this: *Socket, source: *const std.posix.sockaddr.storage, group: *const std.posix.sockaddr.storage, iface: ?*const std.posix.sockaddr.storage, drop: bool) c_int {
            return us_udp_socket_set_source_specific_membership(this, source, group, iface, @intFromBool(drop));
        }
    }

    pub fn boundPort(*this): i32 {
        return us_udp_socket_bound_port(this);
    }

    pub fn close(*this): void {
        us_udp_socket_close(this);
    }

    pub fn disconnect(*this): i32 {
        return us_udp_socket_disconnect(this);
    }
}

export @opaque class PacketBuffer {
    pub fn getPeer(*this, index: i32): *std.posix.sockaddr.storage {
        zig {
            return us_udp_packet_buffer_peer(self, index);
        }
    }

    pub fn getPayload(*this, index: i32): u8[] {
        let payload = us_udp_packet_buffer_payload(this, index);
        let len = us_udp_packet_buffer_payload_length(this, index);
        return payload[0..(len as usize)];
    }

    pub fn getTruncated(*this, index: i32): bool {
        return us_udp_packet_buffer_truncated(this, index) != 0;
    }
}
