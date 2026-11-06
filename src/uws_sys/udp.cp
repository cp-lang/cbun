import bun from "bun";
import { Loop } from "./Loop.cp";

@c {
    fn us_create_udp_socket(
        loop: *Loop?!,
        data_cb: @c ((s: *Socket, buf: *PacketBuffer, count: i32): void)?,
        drain_cb: @c ((s: *Socket): void)?,
        close_cb: @c ((s: *Socket): void)?,
        recv_error_cb: @c ((s: *Socket, err: i32): void)?,
        host: cstr?,
        port: u16,
        options: i32,
        err: *i32?!,
        user_data: *any?!,
    ): *Socket?!;
    fn us_udp_socket_connect(socket: *Socket, hostname: cstr, port: u32): i32;
    fn us_udp_socket_disconnect(socket: *Socket): i32;
    fn us_udp_socket_send(socket: *Socket, payloads: [*]const [*]const u8!, lengths: [*]const usize!, addresses: [*]const *any?!, count: i32): i32;
    fn us_udp_socket_user(socket: *Socket): *any?!;
    fn us_udp_socket_bind(socket: *Socket, hostname: cstr, port: u32): i32;
    fn us_udp_socket_bound_port(socket: *Socket): i32;
    fn us_udp_socket_bound_ip(socket: *Socket, buf: [*]u8!, length: *i32): void;
    fn us_udp_socket_remote_ip(socket: *Socket, buf: [*]u8!, length: *i32): void;
    fn us_udp_socket_close(socket: *Socket): void;
    fn us_udp_socket_set_broadcast(socket: *Socket, enabled: i32): i32;
    fn us_udp_socket_set_ttl_unicast(socket: *Socket, ttl: i32): i32;
    fn us_udp_socket_set_ttl_multicast(socket: *Socket, ttl: i32): i32;
    fn us_udp_socket_set_multicast_loopback(socket: *Socket, enabled: i32): i32;
    fn us_udp_socket_set_multicast_interface(socket: *Socket, iface: *any!): i32;
    fn us_udp_socket_set_membership(socket: *Socket, address: *any!, iface: *any?!, drop: i32): i32;
    fn us_udp_socket_set_source_specific_membership(socket: *Socket, source: *any!, group: *any!, iface: *any?!, drop: i32): i32;

    fn us_udp_packet_buffer_peer(buf: *PacketBuffer?!, index: i32): *any!;
    fn us_udp_packet_buffer_payload(buf: *PacketBuffer?!, index: i32): [*]u8!;
    fn us_udp_packet_buffer_payload_length(buf: *PacketBuffer?!, index: i32): i32;
    fn us_udp_packet_buffer_truncated(buf: *PacketBuffer?!, index: i32): i32;
}

export @opaque class Socket {
    pub static fn create(
        loop: *Loop,
        data_cb: @c ((s: *Socket, buf: *PacketBuffer, count: i32): void)?,
        drain_cb: @c ((s: *Socket): void)?,
        close_cb: @c ((s: *Socket): void)?,
        recv_error_cb: @c ((s: *Socket, err: i32): void)?,
        host: cstr?,
        port: u16,
        options: i32,
        err: *i32?,
        user_data: *any?!,
    ): *Socket? {
        return us_create_udp_socket(loop, data_cb, drain_cb, close_cb, recv_error_cb, host, port, options, err, user_data);
    }

    pub fn send(*this, payloads: [*]const u8![], lengths: usize[], addresses: *any?[], count: i32): i32 {
        return us_udp_socket_send(this, payloads.ptr, lengths.ptr, addresses.ptr, count);
    }

    pub fn user(*this): *any?! {
        return us_udp_socket_user(this);
    }

    pub fn bind(*this, hostname: cstr, port: u32): i32 {
        return us_udp_socket_bind(this, hostname, port);
    }

    pub fn boundIp(*this, buf: u8[], length: *i32): void {
        us_udp_socket_bound_ip(this, buf.ptr, length);
    }

    pub fn remoteIp(*this, buf: u8[], length: *i32): void {
        us_udp_socket_remote_ip(this, buf.ptr, length);
    }

    pub fn connect(*this, hostname: cstr, port: u32): i32 {
        return us_udp_socket_connect(this, hostname, port);
    }

    pub fn setBroadcast(*this, enabled: bool): i32 {
        return us_udp_socket_set_broadcast(this, if (enabled) 1 else 0);
    }

    pub fn setUnicastTTL(*this, ttl: i32): i32 {
        return us_udp_socket_set_ttl_unicast(this, ttl);
    }

    pub fn setMulticastTTL(*this, ttl: i32): i32 {
        return us_udp_socket_set_ttl_multicast(this, ttl);
    }

    pub fn setMulticastLoopback(*this, enabled: bool): i32 {
        return us_udp_socket_set_multicast_loopback(this, if (enabled) 1 else 0);
    }

    pub fn setMulticastInterface(*this, iface: *any!): i32 {
        return us_udp_socket_set_multicast_interface(this, iface);
    }

    pub fn setMembership(*this, address: *any!, iface: *any?!, drop: bool): i32 {
        return us_udp_socket_set_membership(this, address, iface, if (drop) 1 else 0);
    }

    pub fn setSourceSpecificMembership(*this, source: *any!, group: *any!, iface: *any?!, drop: bool): i32 {
        return us_udp_socket_set_source_specific_membership(this, source, group, iface, if (drop) 1 else 0);
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
    pub fn getPeer(*this, index: i32): *any! {
        return us_udp_packet_buffer_peer(this, index);
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
