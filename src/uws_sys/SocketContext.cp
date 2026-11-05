zig {
    const std = @import("std");
    const bun = @import("bun");
    const uws = bun.uws;
    const BoringSSL = bun.BoringSSL.c;

    pub const c = struct {
        pub extern fn us_ssl_ctx_from_options(BunSocketContextOptions, *uws.create_bun_socket_error_t) ?*BoringSSL.SSL_CTX;
        pub extern fn us_ssl_ctx_live_count() c_long;
    };
}

export @extern class BunSocketContextOptions {
    zig {
        key_file_name: [*c]const u8 = null,
        cert_file_name: [*c]const u8 = null,
        passphrase: [*c]const u8 = null,
        dh_params_file_name: [*c]const u8 = null,
        ca_file_name: [*c]const u8 = null,
        ssl_ciphers: [*c]const u8 = null,
        ssl_prefer_low_memory_usage: i32 = 0,
        key: ?[*]const ?[*:0]const u8 = null,
        key_count: u32 = 0,
        cert: ?[*]const ?[*:0]const u8 = null,
        cert_count: u32 = 0,
        ca: ?[*]const ?[*:0]const u8 = null,
        ca_count: u32 = 0,
        secure_options: u32 = 0,
        reject_unauthorized: i32 = 0,
        request_cert: i32 = 0,
        client_renegotiation_limit: u32 = 3,
        client_renegotiation_window: u32 = 600,

        pub fn createSSLContext(options: BunSocketContextOptions, err: *uws.create_bun_socket_error_t) ?*BoringSSL.SSL_CTX {
            return c.us_ssl_ctx_from_options(options, err);
        }

        pub fn digest(self: BunSocketContextOptions) [32]u8 {
            var h = bun.sha.Hashers.SHA256.init();
            const feedZ = struct {
                fn f(hp: *bun.sha.Hashers.SHA256, s: [*c]const u8) void {
                    hp.update(&.{@intFromBool(s != null)});
                    if (s) |p| hp.update(bun.sliceTo(p, 0));
                    hp.update(&.{0});
                }
            }.f;
            const feedArr = struct {
                fn f(hp: *bun.sha.Hashers.SHA256, arr: ?[*]const ?[*:0]const u8, n: u32) void {
                    hp.update(&.{@intFromBool(arr != null)});
                    hp.update(std.mem.asBytes(&n));
                    if (arr) |a| for (a[0..n]) |s| {
                        hp.update(&.{@intFromBool(s != null)});
                        if (s) |p| hp.update(bun.sliceTo(p, 0));
                        hp.update(&.{0});
                    };
                    hp.update(&.{0});
                }
            }.f;
            const feedPath = struct {
                fn f(hp: *bun.sha.Hashers.SHA256, s: [*c]const u8) void {
                    hp.update(&.{@intFromBool(s != null)});
                    if (s) |p| {
                        const path = std.mem.span(@as([*:0]const u8, @ptrCast(p)));
                        hp.update(path);
                        var meta: [3]i64 = @splat(0);
                        if (path.len > 0) switch (bun.sys.stat(path)) {
                            .result => |st| {
                                const mt = st.mtime();
                                meta = .{ @intCast(mt.sec), @intCast(mt.nsec), @intCast(st.size) };
                            },
                            .err => {},
                        };
                        hp.update(std.mem.asBytes(&meta));
                    }
                    hp.update(&.{0});
                }
            }.f;
            feedPath(&h, self.key_file_name);
            feedPath(&h, self.cert_file_name);
            feedZ(&h, self.passphrase);
            feedPath(&h, self.dh_params_file_name);
            feedPath(&h, self.ca_file_name);
            feedZ(&h, self.ssl_ciphers);
            h.update(std.mem.asBytes(&self.ssl_prefer_low_memory_usage));
            feedArr(&h, self.key, self.key_count);
            feedArr(&h, self.cert, self.cert_count);
            feedArr(&h, self.ca, self.ca_count);
            h.update(std.mem.asBytes(&self.secure_options));
            h.update(std.mem.asBytes(&self.reject_unauthorized));
            h.update(std.mem.asBytes(&self.request_cert));
            h.update(std.mem.asBytes(&self.client_renegotiation_limit));
            h.update(std.mem.asBytes(&self.client_renegotiation_window));
            var out: [32]u8 = undefined;
            h.final(&out);
            return out;
        }

        pub fn approxCertBytes(self: BunSocketContextOptions) usize {
            var n: usize = 0;
            if (self.key) |arr| for (arr[0..self.key_count]) |k| {
                if (k) |s| n += bun.sliceTo(s, 0).len;
            };
            if (self.cert) |arr| for (arr[0..self.cert_count]) |k| {
                if (k) |s| n += bun.sliceTo(s, 0).len;
            };
            if (self.ca) |arr| for (arr[0..self.ca_count]) |k| {
                if (k) |s| n += bun.sliceTo(s, 0).len;
            };
            return n;
        }
    }
}
