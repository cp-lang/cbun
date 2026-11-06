// src/http/h2_client/PendingConnect.cp
// Placeholder for TLS connect in flight in CP language

zig {
    const bun = @import("bun");
    pub const HTTPContextTLS = @import("../HTTPContext.zig").NewHTTPContext(true);
}

export @root struct PendingConnect {
    zig {
        pub const new = bun.TrivialNew(@This());
    }

    hostname: string,
    port: u16,
    ssl_config: any? = null,
    waiters: std.ArrayListUnmanaged(*bun.http.HTTPClient),

    fn matches(this, hostname: string, port: u16, ssl_config: any?): bool {
        return this.port == port && this.ssl_config == ssl_config && bun.strings.eqlLong(this.hostname, hostname, true);
    }

    fn unregisterFrom(*this, ctx: *HTTPContextTLS): void {
        const list = &ctx.pending_h2_connects;
        let i: usize = 0;
        while (i < list.items.len) {
            if (list.items[i] == this) {
                list.swapRemove(i);
                return;
            }
            i += 1;
        }
    }

    fn release(*this): void {
        bun.default_allocator.free(this.hostname);
        this.waiters.deinit(bun.default_allocator);
        bun.destroy(this);
    }

    fn deinit(*this): void {
        this.release();
    }
}
