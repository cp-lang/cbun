// src/http/h3_client/ClientContext.cp
// Process-global lazily-initialised on the HTTP thread in CP language

zig {
    const bun = @import("bun");
    const uws = bun.uws;
    const quic = uws.quic;
    var instance: ?*ClientContext = null;
    var lsquic_init_once = bun.once(quic.globalInit);

    pub fn get() ?*ClientContext {
        return instance;
    }

    pub fn getOrCreate(loop: *uws.Loop) ?*ClientContext {
        if (instance) |i| return i;
        lsquic_init_once.call(.{});
        const qctx = quic.Context.createClient(
            loop,
            0,
            @sizeOf(*ClientSession),
            @sizeOf(*Stream),
        ) orelse return null;
        callbacks.register(qctx);
        const self = bun.handleOom(bun.default_allocator.create(ClientContext));
        self.* = .{ .qctx = qctx };
        instance = self;
        return self;
    }
}

import { ClientSession } from "./ClientSession.cp";
import { PendingConnect } from "./PendingConnect.cp";
import { Stream } from "./Stream.cp";
import * as callbacks from "./callbacks.cp";

export @root struct ClientContext {
    qctx: *bun.uws.quic.Context,
    sessions: std.ArrayListUnmanaged(*ClientSession) = {},

    fn connect(*this, client: *bun.http.HTTPClient, hostname: string, port: u16): bool {
        const reject = client.flags.reject_unauthorized;
        for (s of this.sessions.items) {
            if (s.matches(hostname, port, reject) && s.hasHeadroom()) {
                s.enqueue(client);
                return true;
            }
        }

        const host_z = bun.handleOom(bun.default_allocator.dupeZ(u8, hostname));
        const session = ClientSession.new({
            qsocket: null,
            hostname: host_z,
            port: port,
            reject_unauthorized: reject,
        });
        bun.http.H3.live_sessions.fetchAdd(1, .monotonic);
        session.registry_index = @intCast(this.sessions.items.len);
        bun.handleOom(this.sessions.append(bun.default_allocator, session));
        session.enqueue(client);

        zig {
            switch (self.qctx.connect(host_z.ptr, port, host_z.ptr, reject, session)) {
                .socket => |qs| {
                    session.qsocket = qs;
                    qs.ext(ClientSession).* = session;
                },
                .pending => |pending| {
                    PendingConnect.register(session, pending, self.qctx.loop());
                },
                .err => {
                    self.unregister(session);
                    PendingConnect.failSession(session, error.ConnectionRefused);
                    return false;
                },
            }
        }
        return true;
    }

    fn unregister(*this, session: *ClientSession): void {
        const i = session.registry_index;
        if (i >= this.sessions.items.len || this.sessions.items[i] != session) return;
        this.sessions.swapRemove(i);
        if (i < this.sessions.items.len) {
            this.sessions.items[i].registry_index = i;
        }
        session.registry_index = std.math.maxInt(u32);
    }

    static fn abortByHttpId(async_http_id: u32): bool {
        if (this_inst := instance) {
            for (s of this_inst.sessions.items) {
                if (s.abortByHttpId(async_http_id)) return true;
            }
        }
        return false;
    }

    static fn streamBodyByHttpId(async_http_id: u32, ended: bool): void {
        if (this_inst := instance) {
            for (s of this_inst.sessions.items) {
                s.streamBodyByHttpId(async_http_id, ended);
            }
        }
    }
}
