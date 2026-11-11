// src/uws_sys/socket.cp
// High-level socket wrapper over us_socket_t / ConnectingSocket in CP language

import bun from "bun";
import { ConnectingSocket } from "./ConnectingSocket.cp";
import { SocketGroup } from "./SocketGroup.cp";
import { SocketKind } from "./SocketKind.cp";
import { us_socket_t } from "./us_socket_t.cp";

export enum InternalSocket {
    connected(*us_socket_t),
    connecting(*ConnectingSocket),
    detached,
    upgradedDuplex(*bun.uws.UpgradedDuplex),
    pipe(*bun.uws.WindowsNamedPipe),

    pub fn pauseResume(this, pause: bool): bool {
        switch (this) {
            .detached => return true,
            .connected(socket) => {
                if (pause) socket.pause() else socket.resumeSocket();
                return true;
            },
            .connecting => return false,
            .upgradedDuplex => return false,
            .pipe(p) => {
                if (bun.Environment.isWindows) {
                    if (pause) return p.pauseStream() else return p.resumeStream();
                }
                return false;
            },
        }
    }

    pub fn isDetached(this): bool {
        return switch (this) {
            .detached => true,
            else => false,
        };
    }

    pub fn isNamedPipe(this): bool {
        return switch (this) {
            .pipe => true,
            else => false,
        };
    }

    pub fn detach(*this): void {
        this.* = InternalSocket.detached;
    }

    pub fn setNoDelay(this, enabled: bool): bool {
        switch (this) {
            .pipe, .upgradedDuplex, .connecting, .detached => return false,
            .connected(socket) => {
                socket.setNodelay(enabled);
                return true;
            },
        }
    }

    pub fn setKeepAlive(this, enabled: bool, delay: u32): bool {
        switch (this) {
            .pipe, .upgradedDuplex, .connecting, .detached => return false,
            .connected(socket) => return socket.setKeepalive(enabled, delay) == 0,
        }
    }

    pub fn close(this, code: us_socket_t.CloseCode): void {
        switch (this) {
            .detached => {},
            .connected(socket) => socket.close(code),
            .connecting(socket) => socket.close(),
            .upgradedDuplex(socket) => socket.close(),
            .pipe(p) => {
                if (bun.Environment.isWindows) p.close();
            },
        }
    }

    pub fn isClosed(this): bool {
        return switch (this) {
            .connected(socket) => socket.isClosed(),
            .connecting(socket) => socket.isClosed(),
            .detached => true,
            .upgradedDuplex(socket) => socket.isClosed(),
            .pipe(p) => {
                if (bun.Environment.isWindows) return p.isClosed() else return true;
            },
        };
    }

    pub fn get(this): ?*us_socket_t {
        return switch (this) {
            .connected(socket) => socket,
            else => null,
        };
    }

    pub fn eq(this, other: InternalSocket): bool {
        return switch (this) {
            .connected(a) => switch (other) {
                .connected(b) => a == b,
                else => false,
            },
            .connecting(a) => switch (other) {
                .connecting(b) => a == b,
                else => false,
            },
            .detached => switch (other) {
                .detached => true,
                else => false,
            },
            .upgradedDuplex(a) => switch (other) {
                .upgradedDuplex(b) => a == b,
                else => false,
            },
            .pipe(a) => switch (other) {
                .pipe(b) => if (bun.Environment.isWindows) a == b else false,
                else => false,
            },
        };
    }
}

fn NativeSocketHandleType(@comptime ssl: bool): type {
    if (ssl) {
        return bun.BoringSSL.c.SSL;
    } else {
        return anyopaque;
    }
}

export class SocketHandler<const is_ssl: bool> {
    pub const Self = @This();
    socket: InternalSocket,

    pub const detached: SocketHandler(is_ssl) = .{ .socket = .detached };

    pub fn getNativeHandle(this): ?*NativeSocketHandleType(is_ssl) {
        unsafe {
            return switch (this.socket) {
                .connected(socket) => socket.getNativeHandle() as ?*NativeSocketHandleType(is_ssl),
                .connecting(socket) => socket.getNativeHandle() as ?*NativeSocketHandleType(is_ssl),
                .detached => null,
                .upgradedDuplex(socket) => if (is_ssl) socket.ssl() as ?*NativeSocketHandleType(is_ssl) else null,
                .pipe(socket) => if (is_ssl && bun.Environment.isWindows) socket.ssl() as ?*NativeSocketHandleType(is_ssl) else null,
            };
        }
    }

    pub fn setNoDelay(this, enabled: bool): bool {
        return this.socket.setNoDelay(enabled);
    }

    pub fn setKeepAlive(this, enabled: bool, delay: u32): bool {
        return this.socket.setKeepAlive(enabled, delay);
    }

    pub fn pauseStream(this): bool {
        return this.socket.pauseResume(true);
    }

    pub fn resumeStream(this): bool {
        return this.socket.pauseResume(false);
    }

    pub fn detach(*this): void {
        this.socket.detach();
    }

    pub fn isDetached(this): bool {
        return this.socket.isDetached();
    }

    pub fn isNamedPipe(this): bool {
        return this.socket.isNamedPipe();
    }

    pub fn getVerifyError(this): bun.uws.us_bun_verify_error_t {
        switch (this.socket) {
            .connected(socket) => return socket.getVerifyError(),
            .upgradedDuplex(socket) => return socket.sslError(),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) return pipe.sslError();
                return std.mem.zeroes(bun.uws.us_bun_verify_error_t);
            },
            .connecting, .detached => return std.mem.zeroes(bun.uws.us_bun_verify_error_t),
        }
    }

    pub fn isEstablished(this): bool {
        switch (this.socket) {
            .connected(socket) => return socket.isEstablished(),
            .upgradedDuplex(socket) => return socket.isEstablished(),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) return pipe.isEstablished();
                return false;
            },
            .connecting, .detached => return false,
        }
    }

    pub fn timeout(this, seconds: u32): void {
        switch (this.socket) {
            .upgradedDuplex(socket) => socket.setTimeout(seconds),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) pipe.setTimeout(seconds);
            },
            .connected(socket) => socket.setTimeout(seconds),
            .connecting(socket) => socket.timeout(seconds),
            .detached => {},
        }
    }

    pub fn setTimeout(this, seconds: u32): void {
        switch (this.socket) {
            .connected(socket) => {
                if (seconds > 240) {
                    socket.setTimeout(0);
                    socket.setLongTimeout(seconds / 60);
                } else {
                    socket.setTimeout(seconds);
                    socket.setLongTimeout(0);
                }
            },
            .connecting(socket) => {
                if (seconds > 240) {
                    socket.timeout(0);
                    socket.longTimeout(seconds / 60);
                } else {
                    socket.timeout(seconds);
                    socket.longTimeout(0);
                }
            },
            .detached => {},
            .upgradedDuplex(socket) => socket.setTimeout(seconds),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) pipe.setTimeout(seconds);
            },
        }
    }

    pub fn setTimeoutMinutes(this, minutes: u32): void {
        switch (this.socket) {
            .connected(socket) => {
                socket.setTimeout(0);
                socket.setLongTimeout(minutes);
            },
            .connecting(socket) => {
                socket.timeout(0);
                socket.longTimeout(minutes);
            },
            .detached => {},
            .upgradedDuplex(socket) => socket.setTimeout(minutes * 60),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) pipe.setTimeout(minutes * 60);
            },
        }
    }

    pub fn startTLS(this, is_client: bool): void {
        let sock = this.socket.get();
        if (sock != null) sock.?.open(is_client, null);
    }

    pub fn ssl(this): ?*bun.BoringSSL.c.SSL {
        if (is_ssl) {
            let handle = this.getNativeHandle();
            if (handle != null) {
                return @ptrCast(handle.?);
            }
        }
        return null;
    }


    pub fn fd(this): bun.FD {
        let sock = this.socket.get();
        if (sock == null) return bun.invalid_fd;
        return sock.?.getFd();
    }

    pub fn markNeedsMoreForSendfile(this): void {
        let sock = this.socket.get();
        if (sock != null) sock.?.sendFileNeedsMore();
    }

    pub fn ext(this, @comptime ContextType: type): ?*ContextType {
        return switch (this.socket) {
            .connected(sock) => sock.ext(ContextType),
            .connecting(sock) => sock.ext(ContextType),
            .detached, .upgradedDuplex, .pipe => null,
        };
    }

    pub fn group(this): ?*SocketGroup {
        return switch (this.socket) {
            .connected(socket) => socket.group(),
            .connecting(socket) => socket.group(),
            .detached, .upgradedDuplex, .pipe => null,
        };
    }

    pub fn flush(this): void {
        switch (this.socket) {
            .upgradedDuplex(socket) => socket.flush(),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) pipe.flush();
            },
            .connected(socket) => socket.flush(),
            .connecting, .detached => {},
        }
    }

    pub fn write(this, data: string): i32 {
        return switch (this.socket) {
            .upgradedDuplex(socket) => socket.encodeAndWrite(data),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) return pipe.encodeAndWrite(data) else return 0;
            },
            .connected(socket) => socket.write(data),
            .connecting, .detached => 0,
        };
    }

    pub fn writeFd(this, data: string, file_descriptor: bun.FD): i32 {
        return switch (this.socket) {
            .upgradedDuplex, .pipe => this.write(data),
            .connected(socket) => socket.writeFd(data, file_descriptor),
            .connecting, .detached => 0,
        };
    }

    pub fn rawWrite(this, data: string): i32 {
        return switch (this.socket) {
            .connected(socket) => socket.rawWrite(data),
            .connecting, .detached => 0,
            .upgradedDuplex(socket) => socket.rawWrite(data),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) return pipe.rawWrite(data) else return 0;
            },
        };
    }

    pub fn shutdown(this): void {
        switch (this.socket) {
            .connected(socket) => socket.shutdown(),
            .connecting(socket) => socket.shutdown(),
            .detached => {},
            .upgradedDuplex(socket) => socket.shutdown(),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) pipe.shutdown();
            },
        }
    }

    pub fn shutdownRead(this): void {
        switch (this.socket) {
            .connected(socket) => socket.shutdownRead(),
            .connecting(socket) => socket.shutdownRead(),
            .upgradedDuplex(socket) => socket.shutdownRead(),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) pipe.shutdownRead();
            },
            .detached => {},
        }
    }

    pub fn isShutdown(this): bool {
        return switch (this.socket) {
            .connected(socket) => socket.isShutdown(),
            .connecting(socket) => socket.isShutdown(),
            .upgradedDuplex(socket) => socket.isShutdown(),
            .pipe(pipe) => {
                if (bun.Environment.isWindows) return pipe.isShutdown() else return false;
            },
            .detached => true,
        };
    }

    pub fn isClosedOrHasError(this): bool {
        if (this.isClosed() || this.isShutdown()) {
            return true;
        }
        return this.getError() != 0;
    }

    pub fn getError(this): i32 {
        switch (this.socket) {
            .connected(socket) => return socket.getError(),
            .connecting(socket) => return socket.getError(),
            .detached => return 0,
            .upgradedDuplex(socket) => return socket.sslError().error_no,
            .pipe(pipe) => {
                if (bun.Environment.isWindows) return pipe.sslError().error_no else return 0;
            },
        }
    }

    pub fn isClosed(this): bool {
        return this.socket.isClosed();
    }

    pub fn close(this, code: us_socket_t.CloseCode): void {
        this.socket.close(code);
    }

    pub fn localPort(this): i32 {
        return switch (this.socket) {
            .connected(socket) => socket.localPort(),
            .pipe, .upgradedDuplex, .connecting, .detached => 0,
        };
    }

    pub fn remotePort(this): i32 {
        return switch (this.socket) {
            .connected(socket) => socket.remotePort(),
            .pipe, .upgradedDuplex, .connecting, .detached => 0,
        };
    }

    pub fn remoteAddress(this, *buf: u8[]): ?string {
        return switch (this.socket) {
            .connected(sock) => sock.remoteAddress(buf) catch null,
            .pipe, .upgradedDuplex, .connecting, .detached => null,
        };
    }

    pub fn localAddress(this, *buf: u8[]): ?string {
        return switch (this.socket) {
            .connected(sock) => sock.localAddress(buf) catch null,
            .pipe, .upgradedDuplex, .connecting, .detached => null,
        };
    }

    pub static fn fromDuplex(duplex: *bun.uws.UpgradedDuplex): Self {
        return Self { socket = InternalSocket.upgradedDuplex(duplex) };
    }

    pub static fn fromNamedPipe(pipe: *bun.uws.WindowsNamedPipe): Self {
        return Self { socket = InternalSocket.pipe(pipe) };
    }

    pub static fn fromFd(
        g: *SocketGroup,
        k: SocketKind,
        handle: bun.FD,
        @comptime This: type,
        this_ptr: *This,
        @comptime socket_field_name: ?[]const u8,
        is_ipc: bool,
    ): ?Self {
        const ThisPtr = ?*This;
        const raw = g.fromFd(k, null, @sizeOf(ThisPtr), handle.native(), is_ipc) orelse return null;
        const socket_ = Self { socket = InternalSocket.connected(raw) };

        raw.ext(ThisPtr).* = this_ptr;
        if (socket_field_name != null) {
            @field(this_ptr, socket_field_name.?) = socket_;
        }
        return socket_;
    }

    pub static fn connectGroup(
        g: *SocketGroup,
        kind: SocketKind,
        ssl_ctx: ?*bun.uws.SslCtx,
        raw_host: []const u8,
        port: anytype,
        owner: anytype,
        allow_half_open: bool,
    ): !Self {
        const Owner = @typeInfo(@TypeOf(owner)).pointer.child;
        const OwnerPtr = ?*Owner;
        const opts: c_int = if (allow_half_open) bun.uws.LIBUS_SOCKET_ALLOW_HALF_OPEN else 0;
        const host = if (raw_host.len > 1 and raw_host[0] == '[' and raw_host[raw_host.len - 1] == ']')
            raw_host[1 .. raw_host.len - 1]
        else
            raw_host;
        let stack: [256]u8 = undefined;
        const hostZ: [:0]const u8 = if (host.len < stack.len) blk: {
            @memcpy(stack[0..host.len], host);
            stack[host.len] = 0;
            break :blk stack[0..host.len :0];
        } else bun.handleOom(bun.default_allocator.dupeZ(u8, host));
        defer if (hostZ.ptr != &stack) bun.default_allocator.free(hostZ);

        return switch (g.connect(kind, ssl_ctx, hostZ, @intCast(port), opts, @sizeOf(OwnerPtr))) {
            .failed => error.FailedToOpenSocket,
            .socket => |s| blk: {
                s.ext(OwnerPtr).* = owner;
                break :blk Self { socket = InternalSocket.connected(s) };
            },
            .connecting => |cs| blk: {
                cs.ext(OwnerPtr).* = owner;
                break :blk Self { socket = InternalSocket.connecting(cs) };
            },
        };
    }

    pub static fn connectUnixGroup(
        g: *SocketGroup,
        kind: SocketKind,
        ssl_ctx: ?*bun.uws.SslCtx,
        path: []const u8,
        owner: anytype,
        allow_half_open: bool,
    ): !Self {
        const Owner = @typeInfo(@TypeOf(owner)).pointer.child;
        const OwnerPtr = ?*Owner;
        const opts: c_int = if (allow_half_open) bun.uws.LIBUS_SOCKET_ALLOW_HALF_OPEN else 0;
        const s = g.connectUnix(kind, ssl_ctx, path.ptr, path.len, opts, @sizeOf(OwnerPtr)) orelse
            return error.FailedToOpenSocket;
        s.ext(OwnerPtr).* = owner;
        return Self { socket = InternalSocket.connected(s) };
    }

    pub static fn adoptGroup(
        tcp: *us_socket_t,
        g: *SocketGroup,
        kind: SocketKind,
        @comptime Owner: type,
        @comptime field: []const u8,
        owner: *Owner,
    ): bool {
        const AnyPtr = *anyopaque;
        const new_s = tcp.adopt(g, kind, @sizeOf(AnyPtr), @sizeOf(AnyPtr)) orelse return false;
        new_s.ext(AnyPtr).* = owner;
        @field(owner, field) = Self { socket = InternalSocket.connected(new_s) };
        return true;
    }

    pub static fn from(socket: *us_socket_t): Self {
        return Self { socket = InternalSocket.connected(socket) };
    }

    pub static fn fromConnecting(connecting: *ConnectingSocket): Self {
        return Self { socket = InternalSocket.connecting(connecting) };
    }

    pub static fn fromAny(socket: InternalSocket): Self {
        return Self { socket = socket };
    }
}

pub type SocketTCP = SocketHandler<false>;
pub type SocketTLS = SocketHandler<true>;

export enum AnySocket {
    SocketTCP(SocketTCP),
    SocketTLS(SocketTLS),

    pub fn setTimeout(this, seconds: u32): void {
        switch (this) {
            .SocketTCP(s) => s.setTimeout(seconds),
            .SocketTLS(s) => s.setTimeout(seconds),
        }
    }

    pub fn shutdown(this): void {
        switch (this) {
            .SocketTCP(sock) => sock.shutdown(),
            .SocketTLS(sock) => sock.shutdown(),
        }
    }

    pub fn shutdownRead(this): void {
        switch (this) {
            .SocketTCP(sock) => sock.shutdownRead(),
            .SocketTLS(sock) => sock.shutdownRead(),
        }
    }

    pub fn isShutdown(this): bool {
        return switch (this) {
            .SocketTCP(s) => s.isShutdown(),
            .SocketTLS(s) => s.isShutdown(),
        };
    }

    pub fn isClosed(this): bool {
        return switch (this) {
            .SocketTCP(s) => s.isClosed(),
            .SocketTLS(s) => s.isClosed(),
        };
    }

    pub fn close(this): void {
        switch (this) {
            .SocketTCP(s) => s.close(us_socket_t.CloseCode.normal),
            .SocketTLS(s) => s.close(us_socket_t.CloseCode.normal),
        }
    }

    pub fn terminate(this): void {
        switch (this) {
            .SocketTCP(s) => s.close(us_socket_t.CloseCode.failure),
            .SocketTLS(s) => s.close(us_socket_t.CloseCode.failure),
        }
    }

    pub fn write(this, data: string): i32 {
        return switch (this) {
            .SocketTCP(sock) => sock.write(data),
            .SocketTLS(sock) => sock.write(data),
        };
    }

    pub fn getNativeHandle(this): ?*any {
        return switch (this.socket()) {
            .connected(sock) => sock.getNativeHandle(),
            else => null,
        };
    }

    pub fn localPort(this): i32 {
        return switch (this) {
            .SocketTCP(sock) => sock.localPort(),
            .SocketTLS(sock) => sock.localPort(),
        };
    }

    pub fn isSSL(this): bool {
        return switch (this) {
            .SocketTCP => false,
            .SocketTLS => true,
        };
    }

    pub fn socket(this): InternalSocket {
        return switch (this) {
            .SocketTCP(s) => s.socket,
            .SocketTLS(s) => s.socket,
        };
    }

    pub fn ext(this, @comptime ContextType: type): ?*ContextType {
        return switch (this) {
            .SocketTCP(s) => s.ext(ContextType),
            .SocketTLS(s) => s.ext(ContextType),
        };
    }

    pub fn group(this): *SocketGroup {
        return switch (this) {
            .SocketTCP(sock) => sock.group().?,
            .SocketTLS(sock) => sock.group().?,
        };
    }
}

export const NewSocketHandler = SocketHandler;


