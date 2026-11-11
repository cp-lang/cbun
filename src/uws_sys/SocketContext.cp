
import bun from "bun";

@c {
    fn us_ssl_ctx_from_options(options: BunSocketContextOptions, err: *bun.uws.create_bun_socket_error_t): *bun.BoringSSL.c.SSL_CTX?!;
    fn us_ssl_ctx_live_count(): i64;
}

export @extern @value class BunSocketContextOptions {
    key_file_name: cstr? = null,
    cert_file_name: cstr? = null,
    passphrase: cstr? = null,
    dh_params_file_name: cstr? = null,
    ca_file_name: cstr? = null,
    ssl_ciphers: cstr? = null,
    ssl_prefer_low_memory_usage: i32 = 0,
    key: [*]cstr?! = null,
    key_count: u32 = 0,
    cert: [*]cstr?! = null,
    cert_count: u32 = 0,
    ca: [*]cstr?! = null,
    ca_count: u32 = 0,
    secure_options: u32 = 0,
    reject_unauthorized: i32 = 0,
    request_cert: i32 = 0,
    client_renegotiation_limit: u32 = 3,
    client_renegotiation_window: u32 = 600,

    pub static fn createSSLContext(options: BunSocketContextOptions, *err: anytype): *bun.BoringSSL.c.SSL_CTX?! {
        return us_ssl_ctx_from_options(options, err);
    }

    pub static fn liveCount(): i64 {
        return us_ssl_ctx_live_count();
    }

    static fn feedZ(hp: *bun.sha.Hashers.SHA256, s: cstr?): void {
        let flag: [1]u8 = .{(if (s != null) 1 else 0)};
        hp.update(&flag);
        if (s != null) {
            let p = s.?;
            hp.update(bun.sliceTo(p, 0));
        }
        let term: [1]u8 = .{0};
        hp.update(&term);
    }

    static fn feedArr(hp: *bun.sha.Hashers.SHA256, arr: [*]cstr?!, n: u32): void {
        let flag: [1]u8 = .{(if (arr != null) 1 else 0)};
        hp.update(&flag);
        hp.update(bun.mem.asBytes(&n));
        if (arr != null) {
            let a = arr.?;
            for (a[0..n]) |s| {
                let s_flag: [1]u8 = .{(if (s != null) 1 else 0)};
                hp.update(&s_flag);
                if (s != null) {
                    let p = s.?;
                    hp.update(bun.sliceTo(p, 0));
                }
                let term: [1]u8 = .{0};
                hp.update(&term);
            }
        }
        let term: [1]u8 = .{0};
        hp.update(&term);
    }

    static fn feedPath(hp: *bun.sha.Hashers.SHA256, s: cstr?): void {
        let flag: [1]u8 = .{(if (s != null) 1 else 0)};
        hp.update(&flag);
        if (s != null) {
            let p = s.?;
            let path = bun.sliceTo(p, 0);
            hp.update(path);
            let meta: [3]i64 = .{0, 0, 0};
            if (path.len > 0) {
                switch (bun.sys.stat(path)) {
                    .result(st) => {
                        let mt = st.mtime();
                        meta = .{ mt.sec as i64, mt.nsec as i64, st.size as i64 };
                    },
                    .err => {},
                }
            }
            hp.update(bun.mem.asBytes(&meta));
        }
        let term: [1]u8 = .{0};
        hp.update(&term);
    }

    pub fn digest(this): [32]u8 {
        let h = bun.sha.Hashers.SHA256.init();
        feedPath(&h, this.key_file_name);
        feedPath(&h, this.cert_file_name);
        feedZ(&h, this.passphrase);
        feedPath(&h, this.dh_params_file_name);
        feedPath(&h, this.ca_file_name);
        feedZ(&h, this.ssl_ciphers);
        h.update(bun.mem.asBytes(&this.ssl_prefer_low_memory_usage));
        feedArr(&h, this.key, this.key_count);
        feedArr(&h, this.cert, this.cert_count);
        feedArr(&h, this.ca, this.ca_count);
        h.update(bun.mem.asBytes(&this.secure_options));
        h.update(bun.mem.asBytes(&this.reject_unauthorized));
        h.update(bun.mem.asBytes(&this.request_cert));
        h.update(bun.mem.asBytes(&this.client_renegotiation_limit));
        h.update(bun.mem.asBytes(&this.client_renegotiation_window));
        let out: [32]u8 = undefined;
        h.final(&out);
        return out;
    }

    pub fn approxCertBytes(this): usize {
        let n: usize = 0;
        if (this.key != null) {
            let keys = this.key.?;
            let key_count = this.key_count;
            for (keys[0..key_count]) |k| {
                if (k != null) {
                    let kp = k.?;
                    n += bun.sliceTo(kp, 0).len;
                }
            }
        }
        if (this.cert != null) {
            let certs = this.cert.?;
            let cert_count = this.cert_count;
            for (certs[0..cert_count]) |k| {
                if (k != null) {
                    let kp = k.?;
                    n += bun.sliceTo(kp, 0).len;
                }
            }
        }
        if (this.ca != null) {
            let cas = this.ca.?;
            let ca_count = this.ca_count;
            for (cas[0..ca_count]) |k| {
                if (k != null) {
                    let kp = k.?;
                    n += bun.sliceTo(kp, 0).len;
                }
            }
        }
        return n;
    }
}
