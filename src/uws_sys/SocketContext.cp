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

    pub static fn createSSLContext(options: BunSocketContextOptions, err: anytype): *bun.BoringSSL.c.SSL_CTX?! {
        return us_ssl_ctx_from_options(options, err);
    }

    pub static fn liveCount(): i64 {
        return us_ssl_ctx_live_count();
    }

    pub fn digest(this): u8[32] {
        let h = bun.sha.Hashers.SHA256.init();
        let feedZ = fn(hp: *bun.sha.Hashers.SHA256, s: cstr?): void {
            hp.update([if (s != null) 1 else 0]);
            if (s != null) hp.update(bun.sliceTo(s!, 0));
            hp.update([0]);
        };
        let feedArr = fn(hp: *bun.sha.Hashers.SHA256, arr: [*]cstr?!, n: u32): void {
            hp.update([if (arr != null) 1 else 0]);
            hp.update(bun.mem.asBytes(&n));
            if (arr != null) {
                for (s in arr![0..n]) {
                    hp.update([if (s != null) 1 else 0]);
                    if (s != null) hp.update(bun.sliceTo(s!, 0));
                    hp.update([0]);
                }
            }
            hp.update([0]);
        };
        let feedPath = fn(hp: *bun.sha.Hashers.SHA256, s: cstr?): void {
            hp.update([if (s != null) 1 else 0]);
            if (s != null) {
                let path = bun.sliceTo(s!, 0);
                hp.update(path);
                let meta: i64[3] = [0, 0, 0];
                if (path.len > 0) {
                    switch (bun.sys.stat(path)) {
                        .result(st) => {
                            let mt = st.mtime();
                            meta = [mt.sec as i64, mt.nsec as i64, st.size as i64];
                        },
                        .err => {},
                    }
                }
                hp.update(bun.mem.asBytes(&meta));
            }
            hp.update([0]);
        };

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
        let out: u8[32] = undefined;
        h.final(&out);
        return out;
    }

    pub fn approxCertBytes(this): usize {
        let n: usize = 0;
        if (this.key != null) {
            for (k in this.key![0..this.key_count]) {
                if (k != null) n += bun.sliceTo(k!, 0).len;
            }
        }
        if (this.cert != null) {
            for (k in this.cert![0..this.cert_count]) {
                if (k != null) n += bun.sliceTo(k!, 0).len;
            }
        }
        if (this.ca != null) {
            for (k in this.ca![0..this.ca_count]) {
                if (k != null) n += bun.sliceTo(k!, 0).len;
            }
        }
        return n;
    }
}
