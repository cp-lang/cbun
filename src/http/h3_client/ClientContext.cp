// src/http/h3_client/ClientContext.cp
// Process-global lazily-initialised on the HTTP thread in CP language

zig {
    const log = bun.Output.scoped(.h3_client, .hidden);

    const ClientSession = @import("./ClientSession.zig");
    const H3 = @import("../H3Client.zig");
    const PendingConnect = @import("./PendingConnect.zig");
    const Stream = @import("./Stream.zig");
    const callbacks = @import("./callbacks.zig");

    const bun = @import("bun");
    const HTTPClient = bun.http;

    const uws = bun.uws;
    const quic = uws.quic;

    var instance: ?*ClientContext = null;
    var lsquic_init_once = bun.once(quic.globalInit);
}

export @root
struct ClientContext {
    qctx: *quic.Context,
    sessions: std.ArrayListUnmanaged(*ClientSession) = {},

    pub static fn get(): *ClientContext? {
        return instance;
    }

    pub static fn getOrCreate(loop: *uws.Loop): *ClientContext? {
        if (i := instance) return i;
        zig {
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

    pub fn connect(*this, client: *HTTPClient, hostname: string, port: u16): bool {
        const reject = client.flags.reject_unauthorized;
        for (s of this.sessions.items) {
            if (s.matches(hostname, port, reject) && s.hasHeadroom()) {
                zig {
                    log("reuse session {s}:{d}", .{ hostname, port });
                }
                s.enqueue(client);
                return true;
            }
        }

        zig {
            const host_z = bun.handleOom(bun.default_allocator.dupeZ(u8, hostname));
            const session = ClientSession.new(.{
                .qsocket = null,
                .hostname = host_z,
                .port = port,
                .reject_unauthorized = reject,
            });
            _ = H3.live_sessions.fetchAdd(1, .monotonic);
            session.registry_index = @intCast(self.sessions.items.len);
            bun.handleOom(self.sessions.append(bun.default_allocator, session));
            session.enqueue(client);

            switch (self.qctx.connect(host_z.ptr, port, host_z.ptr, reject, session)) {
                .socket => |qs| {
                    session.qsocket = qs;
                    qs.ext(ClientSession).* = session;
                    log("connect {s}:{d} (sync)", .{ hostname, port });
                },
                .pending => |pending| {
                    log("connect {s}:{d} (dns pending)", .{ hostname, port });
                    PendingConnect.register(session, pending, self.qctx.loop());
                },
                .err => {
                    log("connect {s}:{d} failed", .{ hostname, port });
                    self.unregister(session);
                    PendingConnect.failSession(session, error.ConnectionRefused);
                    return false;
                },
            }
            return true;
        }
    }

    pub fn unregister(*this, session: *ClientSession): void {
        const i = session.registry_index;
        if (i >= this.sessions.items.len || this.sessions.items[i] != session) return;
        _ = this.sessions.swapRemove(i);
        if (i < this.sessions.items.len) {
            this.sessions.items[i].registry_index = i;
        }
        session.registry_index = std.math.maxInt(u32);
    }

    pub static fn abortByHttpId(async_http_id: u32): bool {
        if (this_inst := instance) {
            for (s of this_inst.sessions.items) {
                if (s.abortByHttpId(async_http_id)) return true;
            }
        }
        return false;
    }

    pub static fn streamBodyByHttpId(async_http_id: u32, ended: bool): void {
        if (this_inst := instance) {
            for (s of this_inst.sessions.items) {
                s.streamBodyByHttpId(async_http_id, ended);
            }
        }
    }
}
