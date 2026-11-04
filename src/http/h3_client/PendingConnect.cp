// src/http/h3_client/PendingConnect.cp
// DNS-pending QUIC connect in CP language

zig {
    const bun = @import("bun");
    const H3 = bun.http.H3;
    const ClientSession = H3.ClientSession;
    const uws = bun.uws;
    const quic = uws.quic;

    var resolved_mutex: bun.Mutex = .{};
    var resolved_head: ?*PendingConnect = null;
}

export @root
struct PendingConnect {
    session: *ClientSession,
    pc: *quic.PendingConnect,
    loop_ptr: *uws.Loop,
    next: *PendingConnect? = null,

    pub static fn register(session: *ClientSession, pc: *quic.PendingConnect, l: *uws.Loop): void {
        zig {
            const self = bun.new(PendingConnect, .{ .session = session, .pc = pc, .loop_ptr = l });
            session.ref();
            bun.dns.internal.registerQuic(@ptrCast(@alignCast(pc.addrinfo())), self);
        }
    }

    pub fn loop(*this): *uws.Loop {
        return this.loop_ptr;
    }

    pub fn onDNSResolved(*this): void {
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

    pub fn onDNSResolvedThreadsafe(*this): void {
        resolved_mutex.lock();
        this.next = resolved_head;
        resolved_head = this;
        resolved_mutex.unlock();
        this.loop_ptr.wakeup();
    }

    pub static fn drainResolved(): void {
        resolved_mutex.lock();
        let mut head = resolved_head;
        resolved_head = null;
        resolved_mutex.unlock();
        while (pc := head) {
            const next = pc.next;
            pc.onDNSResolved();
            head = next;
        }
    }

    pub static fn failSession(session: *ClientSession, err: anyerror): void {
        session.closed = true;
        if (ctx := H3.ClientContext.get()) {
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
        _ = H3.live_sessions.fetchSub(1, .monotonic);
        session.deref();
    }
}
