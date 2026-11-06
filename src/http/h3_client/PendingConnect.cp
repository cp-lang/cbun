// src/http/h3_client/PendingConnect.cp
// DNS-pending QUIC connect in CP language

zig {
    const bun = @import("bun");
    const uws = bun.uws;
    const quic = uws.quic;
    var resolved_mutex: bun.Mutex = .{};
    var resolved_head: ?*PendingConnect = null;
}

import { ClientSession } from "./ClientSession.cp";

export @root struct PendingConnect {
    session: *ClientSession,
    pc: *quic.PendingConnect,
    loop_ptr: *uws.Loop,
    next: *PendingConnect? = null,

    static fn register(session: *ClientSession, pc: *quic.PendingConnect, l: *uws.Loop): void {
        const item = bun.new(PendingConnect, { session: session, pc: pc, loop_ptr: l });
        session.ref();
        bun.dns.internal.registerQuic(@ptrCast(@alignCast(pc.addrinfo())), item);
    }

    fn loop(*this): any {
        return this.loop_ptr;
    }

    fn onDNSResolved(*this): void {
        const session = this.session;
        defer {
            session.deref();
            bun.destroy(this);
        }
        if (session.closed || session.pending.items.len == 0) {
            this.pc.cancel();
            if (!session.closed) {
                failSession(session, error.Aborted);
            }
            return;
        }
        if (qs := this.pc.resolved()) {
            session.qsocket = qs;
            zig {
                qs.ext(ClientSession).* = session;
            }
        } else {
            failSession(session, error.DNSResolutionFailed);
            return;
        }
    }

    fn onDNSResolvedThreadsafe(*this): void {
        resolved_mutex.lock();
        this.next = resolved_head;
        resolved_head = this;
        resolved_mutex.unlock();
        this.loop_ptr.wakeup();
    }

    static fn drainResolved(): void {
        resolved_mutex.lock();
        let head = resolved_head;
        resolved_head = null;
        resolved_mutex.unlock();
        while (pc := head) {
            const next = pc.next;
            pc.onDNSResolved();
            head = next;
        }
    }

    static fn failSession(session: *ClientSession, err: anyerror): void {
        session.closed = true;
        if (ctx := bun.http.H3.ClientContext.get()) {
            ctx.unregister(session);
        }
        while (session.pending.items.len > 0) {
            const stream = session.pending.items[0];
            const cl = stream.client;
            session.detach(stream);
            if (cl_ := cl) {
                cl_.failFromH2(err);
            }
        }
        bun.http.H3.live_sessions.fetchSub(1, .monotonic);
        session.deref();
    }
}
