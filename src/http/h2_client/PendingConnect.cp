// src/http/h2_client/PendingConnect.cp
// Placeholder for TLS connect in flight in CP language

zig {
    const bun = @import("bun");
    const strings = bun.strings;
    const SSLConfig = bun.api.server.ServerConfig.SSLConfig;

    const HTTPClient = bun.http;
    const NewHTTPContext = HTTPClient.NewHTTPContext;
    const WaiterList = std.ArrayListUnmanaged(*HTTPClient);
}

export @root struct PendingConnect {
    hostname: string,
    port: u16,
    ssl_config: *SSLConfig?,
    waiters: WaiterList,

    zig {
        pub const new = bun.TrivialNew(@This());
    }

    pub fn matches(this, hostname: string, port: u16, ssl_config: *SSLConfig?): bool {
        return this.port == port && this.ssl_config == ssl_config && strings.eqlLong(this.hostname, hostname, true);
    }

    pub fn unregisterFrom(*this, ctx: *NewHTTPContext(true)): void {
        zig {
            const list = &ctx.pending_h2_connects;
            for (list.items, 0..) |p, i| {
                if (p == self) {
                    _ = list.swapRemove(i);
                    return;
                }
            }
        }
    }

    pub fn deinit(*this): void {
        zig {
            bun.default_allocator.free(self.hostname);
            self.waiters.deinit(bun.default_allocator);
            bun.destroy(self);
        }
    }
}
