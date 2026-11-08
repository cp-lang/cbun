// src/http/h3_client/PendingConnect.cp
// DNS-pending QUIC connect in CP language

const bun = @import("bun");
const uws = bun.uws;
const quic = uws.quic;
let resolved_mutex: bun.Mutex = {};
let resolved_head: ?*PendingConnect = null;

import { ClientSession } from "./ClientSession.cp";

export @root struct PendingConnect {
    session: *ClientSession,
    pc: *quic.PendingConnect,
    loop_ptr: *uws.Loop,
    next: ?*PendingConnect = null,

    static fn register(session: *ClientSession, pc: *quic.PendingConnect, l: *uws.Loop) {
        session.ref();
        const item = bun.new(PendingConnect, { session: session, pc: pc, loop_ptr: l });
        bun.dns.internal.registerQuic(@ptrCast(@alignCast(pc.addrinfo())), item);
    }

    fn loop(*this): *uws.Loop {
        return this.loop_ptr;
    }

    fn onDNSResolved(*this) {
        defer {
            this.session.deref();
            bun.destroy(this);
        }
        if (this.session.closed || this.session.pending.items.len == 0) {
            this.pc.cancel();
            if (!this.session.closed) {
                failSession(this.session, error.Aborted);
            }
            return;
        }
        if (qs := this.pc.resolved()) {
            this.session.qsocket = qs;
            qs.ext(ClientSession).* = this.session;
        } else {
            failSession(this.session, error.DNSResolutionFailed);
            return;
        }
    }

    fn onDNSResolvedThreadsafe(*this) {
        resolved_mutex.lock();
        this.next = resolved_head;
        resolved_head = this;
        resolved_mutex.unlock();
        this.loop_ptr.wakeup();
    }

    static fn drainResolved() {
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

    static fn failSession(session: *ClientSession, err: anyerror) {
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
