// src/uws_sys/socket.cp
// High-level socket wrapper over us_socket_t / ConnectingSocket in CP language

import bun from "bun";
import { ConnectingSocket } from "./ConnectingSocket.cp";
import { SocketGroup } from "./SocketGroup.cp";
import { SocketKind } from "./SocketKind.cp";
import { us_socket_t, CloseCode } from "./us_socket_t.cp";

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
            .pipe(p) => return if (pause) p.pauseStream() else p.resumeStream(),
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

    pub fn close(this, code: CloseCode): void {
        switch (this) {
            .detached => {},
            .connected(socket) => socket.close(code),
            .connecting(socket) => socket.close(),
            .upgradedDuplex(socket) => socket.close(),
            .pipe(p) => p.close(),
        }
    }

    pub fn isClosed(this): bool {
        return switch (this) {
            .connected(socket) => socket.isClosed(),
            .connecting(socket) => socket.isClosed(),
            .detached => true,
            .upgradedDuplex(socket) => socket.isClosed(),
            .pipe(p) => p.isClosed(),
        };
    }

    pub fn get(this): *us_socket_t?! {
        return switch (this) {
            .connected(socket) => socket,
            else => null,
        };
    }
}

export class SocketHandler<const is_ssl: bool> {
    socket: InternalSocket,

    pub static const detached: SocketHandler<is_ssl> = SocketHandler<is_ssl> { socket = InternalSocket.detached };

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
            .pipe(pipe) => return pipe.sslError(),
            .connecting, .detached => return bun.mem.zeroes(bun.uws.us_bun_verify_error_t),
        }
    }

    pub fn isEstablished(this): bool {
        switch (this.socket) {
            .connected(socket) => return socket.isEstablished(),
            .upgradedDuplex(socket) => return socket.isEstablished(),
            .pipe(pipe) => return pipe.isEstablished(),
            .connecting, .detached => return false,
        }
    }

    pub fn timeout(this, seconds: u32): void {
        switch (this.socket) {
            .upgradedDuplex(socket) => socket.setTimeout(seconds),
            .pipe(pipe) => pipe.setTimeout(seconds),
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
            .pipe(pipe) => pipe.setTimeout(seconds),
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
            .pipe(pipe) => pipe.setTimeout(minutes * 60),
        }
    }

    pub fn startTLS(this, is_client: bool): void {
        let sock = this.socket.get();
        if (sock != null) sock!.open(is_client, null);
    }

    pub fn ssl(this): *bun.BoringSSL.c.SSL?! {
        if (is_ssl) {
            let handle = this.getNativeHandle();
            if (handle != null) {
                return handle! as *bun.BoringSSL.c.SSL?!;
            }
        }
        return null;
    }

    pub fn getNativeHandle(this): *any?! {
        return switch (this.socket) {
            .connected(socket) => socket.getNativeHandle(),
            .connecting(socket) => socket.getNativeHandle(),
            .detached => null,
            .upgradedDuplex(socket) => if (is_ssl) (socket.ssl() as *any?!) else null,
            .pipe(socket) => if (is_ssl) (socket.ssl() as *any?!) else null,
        };
    }

    pub fn fd(this): bun.FD {
        let sock = this.socket.get();
        if (sock == null) return bun.invalid_fd;
        return sock!.getFd();
    }

    pub fn markNeedsMoreForSendfile(this): void {
        let sock = this.socket.get();
        if (sock != null) sock!.sendFileNeedsMore();
    }

    pub fn ext<ContextType>(this): *ContextType?! {
        return switch (this.socket) {
            .connected(sock) => sock.ext<ContextType>(),
            .connecting(sock) => sock.ext<ContextType>(),
            .detached, .upgradedDuplex, .pipe => null,
        };
    }

    pub fn group(this): *SocketGroup? {
        return switch (this.socket) {
            .connected(socket) => socket.group(),
            .connecting(socket) => socket.group(),
            .detached, .upgradedDuplex, .pipe => null,
        };
    }

    pub fn flush(this): void {
        switch (this.socket) {
            .upgradedDuplex(socket) => socket.flush(),
            .pipe(pipe) => pipe.flush(),
            .connected(socket) => socket.flush(),
            .connecting, .detached => {},
        }
    }

    pub fn write(this, data: string): i32 {
        return switch (this.socket) {
            .upgradedDuplex(socket) => socket.encodeAndWrite(data),
            .pipe(pipe) => pipe.encodeAndWrite(data),
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
            .pipe(pipe) => pipe.rawWrite(data),
        };
    }

    pub fn shutdown(this): void {
        switch (this.socket) {
            .connected(socket) => socket.shutdown(),
            .connecting(socket) => socket.shutdown(),
            .detached => {},
            .upgradedDuplex(socket) => socket.shutdown(),
            .pipe(pipe) => pipe.shutdown(),
        }
    }

    pub fn shutdownRead(this): void {
        switch (this.socket) {
            .connected(socket) => socket.shutdownRead(),
            .connecting(socket) => socket.shutdownRead(),
            .upgradedDuplex(socket) => socket.shutdownRead(),
            .pipe(pipe) => pipe.shutdownRead(),
            .detached => {},
        }
    }

    pub fn isShutdown(this): bool {
        return switch (this.socket) {
            .connected(socket) => socket.isShutdown(),
            .connecting(socket) => socket.isShutdown(),
            .upgradedDuplex(socket) => socket.isShutdown(),
            .pipe(pipe) => pipe.isShutdown(),
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
            .pipe(pipe) => return pipe.sslError().error_no,
        }
    }

    pub fn isClosed(this): bool {
        return this.socket.isClosed();
    }

    pub fn close(this, code: CloseCode): void {
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

    pub fn remoteAddress(this, buf: u8[]): ?string {
        return switch (this.socket) {
            .connected(sock) => sock.remoteAddress(buf)?,
            .pipe, .upgradedDuplex, .connecting, .detached => null,
        };
    }

    pub fn localAddress(this, buf: u8[]): ?string {
        return switch (this.socket) {
            .connected(sock) => sock.localAddress(buf)?,
            .pipe, .upgradedDuplex, .connecting, .detached => null,
        };
    }

    pub static fn fromDuplex(duplex: *bun.uws.UpgradedDuplex): SocketHandler<is_ssl> {
        return SocketHandler<is_ssl> { socket = InternalSocket.upgradedDuplex(duplex) };
    }

    pub static fn fromNamedPipe(pipe: *bun.uws.WindowsNamedPipe): SocketHandler<is_ssl> {
        return SocketHandler<is_ssl> { socket = InternalSocket.pipe(pipe) };
    }

    pub static fn fromFd<This>(
        g: *SocketGroup,
        k: SocketKind,
        handle: bun.FD,
        this_ptr: *This,
        is_ipc: bool,
    ): SocketHandler<is_ssl>? {
        let raw = g.fromFd(k, null, @sizeOf(*This) as i32, handle, is_ipc);
        if (raw == null) return null;
        let sock = raw!;
        let s_handler = SocketHandler<is_ssl> { socket = InternalSocket.connected(sock) };
        sock.ext<*This>().* = this_ptr;
        return s_handler;
    }

    pub static fn connectGroup<Owner>(
        g: *SocketGroup,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        raw_host: string,
        port: i32,
        owner: *Owner,
        allow_half_open: bool,
    ): ?SocketHandler<is_ssl> {
        let opts: i32 = if (allow_half_open) bun.uws.LIBUS_SOCKET_ALLOW_HALF_OPEN else 0;
        let host = if (raw_host.len > 1 && raw_host[0] == '[' && raw_host[raw_host.len - 1] == ']')
            raw_host[1..(raw_host.len - 1)]
        else
            raw_host;
        let hostZ: cstr = bun.handleOom(bun.default_allocator.dupeZ(u8, host));
        defer bun.default_allocator.free(hostZ);

        switch (g.connect(kind, ssl_ctx, hostZ, port, opts, @sizeOf(*Owner) as i32)) {
            .failed => return error.FailedToOpenSocket,
            .socket(s) => {
                s.ext<*Owner>().* = owner;
                return SocketHandler<is_ssl> { socket = InternalSocket.connected(s) };
            },
            .connecting(cs) => {
                cs.ext<*Owner>().* = owner;
                return SocketHandler<is_ssl> { socket = InternalSocket.connecting(cs) };
            },
        }
    }

    pub static fn connectUnixGroup<Owner>(
        g: *SocketGroup,
        kind: SocketKind,
        ssl_ctx: *bun.uws.SslCtx?,
        path: string,
        owner: *Owner,
        allow_half_open: bool,
    ): ?SocketHandler<is_ssl> {
        let opts: i32 = if (allow_half_open) bun.uws.LIBUS_SOCKET_ALLOW_HALF_OPEN else 0;
        let s = g.connectUnix(kind, ssl_ctx, path, opts, @sizeOf(*Owner) as i32);
        if (s == null) return error.FailedToOpenSocket;
        s!.ext<*Owner>().* = owner;
        return SocketHandler<is_ssl> { socket = InternalSocket.connected(s!) };
    }

    pub static fn adoptGroup<Owner>(
        tcp: *us_socket_t,
        g: *SocketGroup,
        kind: SocketKind,
        owner: *Owner,
    ): SocketHandler<is_ssl>? {
        let new_s = tcp.adopt(g, kind, @sizeOf(*any!) as i32, @sizeOf(*any!) as i32);
        if (new_s == null) return null;
        new_s!.ext<*Owner>().* = owner;
        return SocketHandler<is_ssl> { socket = InternalSocket.connected(new_s!) };
    }

    pub static fn from(socket: *us_socket_t): SocketHandler<is_ssl> {
        return SocketHandler<is_ssl> { socket = InternalSocket.connected(socket) };
    }

    pub static fn fromConnecting(connecting: *ConnectingSocket): SocketHandler<is_ssl> {
        return SocketHandler<is_ssl> { socket = InternalSocket.connecting(connecting) };
    }

    pub static fn fromAny(socket: InternalSocket): SocketHandler<is_ssl> {
        return SocketHandler<is_ssl> { socket = socket };
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
            .SocketTCP(s) => s.close(CloseCode.normal),
            .SocketTLS(s) => s.close(CloseCode.normal),
        }
    }

    pub fn terminate(this): void {
        switch (this) {
            .SocketTCP(s) => s.close(CloseCode.failure),
            .SocketTLS(s) => s.close(CloseCode.failure),
        }
    }

    pub fn write(this, data: string): i32 {
        return switch (this) {
            .SocketTCP(sock) => sock.write(data),
            .SocketTLS(sock) => sock.write(data),
        };
    }

    pub fn getNativeHandle(this): *any?! {
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

    pub fn ext<ContextType>(this): *ContextType?! {
        return switch (this) {
            .SocketTCP(s) => s.ext<ContextType>(),
            .SocketTLS(s) => s.ext<ContextType>(),
        };
    }

    pub fn group(this): *SocketGroup {
        return switch (this) {
            .SocketTCP(sock) => sock.group()!,
            .SocketTLS(sock) => sock.group()!,
        };
    }
}
