// src/http/ProxyTunnel.cp
// Fast HTTP Proxy Tunnel in CP language

zig {
    const std = @import("std");

    const bun = @import("bun");
    const jsc = bun.jsc;
    const strings = bun.strings;
    const uws = bun.uws;
    const BoringSSL = bun.BoringSSL.c;

    const HTTPClient = bun.http;
    const NewHTTPContext = bun.http.NewHTTPContext;
    const HTTPCertError = @import("./HTTPCertError.zig");
    const SSLWrapper = @import("../runtime/socket/ssl_wrapper.zig").SSLWrapper;

    const log = bun.Output.scoped(.http_proxy_tunnel, .visible);

    pub const ProxyTunnelSocket = union(enum) {
        tcp: NewHTTPContext(false).HTTPSocket,
        ssl: NewHTTPContext(true).HTTPSocket,
        none: void,
    };

    pub const ProxyTunnelWrapper = SSLWrapper(*HTTPClient);

    const RefCount = bun.ptr.RefCount(ProxyTunnel, "ref_count", ProxyTunnel.deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;
    pub const RefPtr = bun.ptr.RefPtr(ProxyTunnel);
}

export @root @value struct ProxyTunnel {
    pub wrapper: ProxyTunnelWrapper? = null,
    pub shutdown_err: anyerror = error.ConnectionClosed,
    pub socket: ProxyTunnelSocket = { none: {} },
    pub write_buffer: bun.io.StreamBuffer = {},
    pub did_have_handshaking_error: bool = false,
    pub established_with_reject_unauthorized: bool = false,
    pub ref_count: RefCount = undefined,


    pub static fn onOpen(client: *HTTPClient): void {
        log("ProxyTunnel onOpen", []);
        bun.analytics.Features.http_client_proxy += 1;
        client.state.response_stage = .proxy_handshake;
        client.state.request_stage = .proxy_handshake;

        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();
            defer proxy.deref();

            zig {
                if (proxy.wrapper) |*wrapper| {
                    var ssl_ptr = wrapper.ssl orelse return;
                    const _hostname = client.hostname orelse client.url.hostname;

                    var hostname: [:0]const u8 = "";
                    var hostname_needs_free = false;
                    if (!strings.isIPAddress(_hostname)) {
                        if (_hostname.len < bun.http.temp_hostname.len) {
                            @memcpy(bun.http.temp_hostname[0.._hostname.len], _hostname);
                            bun.http.temp_hostname[_hostname.len] = 0;
                            hostname = bun.http.temp_hostname[0.._hostname.len :0];
                        } else {
                            hostname = bun.default_allocator.dupeZ(u8, _hostname) catch unreachable;
                            hostname_needs_free = true;
                        }
                    }

                    defer if (hostname_needs_free) bun.default_allocator.free(hostname);
                    ssl_ptr.configureHTTPClient(hostname);
                }
            }
        }
    }

    pub static fn progressUpdateForProxySocket(client: *HTTPClient, proxy: *ProxyTunnel): void {
        zig {
            switch (proxy.socket) {
                .ssl => |socket| client.progressUpdate(true, &bun.http.http_thread.https_context, socket),
                .tcp => |socket| client.progressUpdate(false, &bun.http.http_thread.http_context, socket),
                .none => {},
            }
        }
    }

    pub static fn onData(client: *HTTPClient, decoded_data: string): void {
        if (decoded_data.len == 0) return;
        log("ProxyTunnel onData decoded {}", [decoded_data.len]);

        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();
            defer proxy.deref();

            switch (client.state.response_stage) {
                .body => {
                    log("ProxyTunnel onData body", []);
                    if (decoded_data.len == 0) return;

                    let mut report_progress = false;
                    zig {
                        report_progress = client.handleResponseBody(decoded_data, false) catch |err| {
                            proxy.close(err);
                            return;
                        };
                    }

                    if (report_progress) {
                        ProxyTunnel.progressUpdateForProxySocket(client, proxy);
                        return;
                    }
                },
                .body_chunk => {
                    log("ProxyTunnel onData body_chunk", []);
                    if (decoded_data.len == 0) return;

                    let mut report_progress = false;
                    zig {
                        report_progress = client.handleResponseBodyChunkedEncoding(decoded_data) catch |err| {
                            proxy.close(err);
                            return;
                        };
                    }

                    if (report_progress) {
                        ProxyTunnel.progressUpdateForProxySocket(client, proxy);
                        return;
                    }
                },
                .proxy_headers => {
                    log("ProxyTunnel onData proxy_headers", []);
                    zig {
                        switch (proxy.socket) {
                            .ssl => |socket| {
                                client.handleOnDataHeaders(true, decoded_data, &bun.http.http_thread.https_context, socket);
                            },
                            .tcp => |socket| {
                                client.handleOnDataHeaders(false, decoded_data, &bun.http.http_thread.http_context, socket);
                            },
                            .none => {},
                        }
                    }
                },
                * => {
                    log("ProxyTunnel onData unexpected data", []);
                    client.state.pending_response = null;
                    proxy.close(error.UnexpectedData);
                },
            }
        }
    }

    pub static fn onHandshake(client: *HTTPClient, handshake_success: bool, ssl_error: uws.us_bun_verify_error_t): void {
        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            log("ProxyTunnel onHandshake", []);
            proxy.ref();
            defer proxy.deref();

            client.state.response_stage = .proxy_headers;
            client.state.request_stage = .proxy_headers;
            client.state.request_sent_len = 0;

            zig {
                const handshake_error = HTTPCertError{
                    .error_no = ssl_error.error_no,
                    .code = if (ssl_error.code == null) "" else ssl_error.code[0..bun.len(ssl_error.code) :0],
                    .reason = if (ssl_error.code == null) "" else ssl_error.reason[0..bun.len(ssl_error.reason) :0],
                };

                if (handshake_success) {
                    log("ProxyTunnel onHandshake success", .{});
                    client.flags.did_have_handshaking_error = handshake_error.error_no != 0;

                    if (client.flags.reject_unauthorized) {
                        if (client.flags.did_have_handshaking_error) {
                            proxy.close(BoringSSL.getCertErrorFromNo(handshake_error.error_no));
                            return;
                        }

                        bun.assert(proxy.wrapper != null);
                        const ssl_ptr = proxy.wrapper.?.ssl orelse return;

                        switch (proxy.socket) {
                            .ssl => |socket| {
                                if (!client.checkServerIdentity(true, socket, handshake_error, ssl_ptr, false)) {
                                    log("ProxyTunnel onHandshake checkServerIdentity failed", .{});
                                    return;
                                }
                            },
                            .tcp => |socket| {
                                if (!client.checkServerIdentity(false, socket, handshake_error, ssl_ptr, false)) {
                                    log("ProxyTunnel onHandshake checkServerIdentity failed", .{});
                                    return;
                                }
                            },
                            .none => {},
                        }
                    }

                    switch (proxy.socket) {
                        .ssl => |socket| {
                            client.onWritable(true, true, socket);
                        },
                        .tcp => |socket| {
                            client.onWritable(true, false, socket);
                        },
                        .none => {},
                    }
                } else {
                    log("ProxyTunnel onHandshake failed", .{});
                    if (client.flags.did_have_handshaking_error and handshake_error.error_no != 0) {
                        proxy.close(BoringSSL.getCertErrorFromNo(handshake_error.error_no));
                        return;
                    }
                    proxy.close(error.ConnectionRefused);
                    return;
                }
            }
        }
    }

    pub static fn writeEncrypted(client: *HTTPClient, encoded_data: string): void {
        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            if (proxy.write_buffer.isNotEmpty()) {
                bun.handleOom(proxy.write_buffer.write(encoded_data));
                return;
            }

            let mut written: i32 = 0;
            zig {
                written = switch (proxy.socket) {
                    .ssl => |socket| socket.write(encoded_data),
                    .tcp => |socket| socket.write(encoded_data),
                    .none => 0,
                };
            }

            const pending = encoded_data[@intCast(written)..];
            if (pending.len > 0) {
                bun.handleOom(proxy.write_buffer.write(pending));
            }
        }
    }

    pub static fn onClose(client: *HTTPClient): void {
        let mut status_msg = "tunnel exists";
        if (client.proxy_tunnel == null) {
            status_msg = "tunnel is detached";
        }
        log("ProxyTunnel onClose {s}", [status_msg]);

        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();

            const in_progress = client.state.stage != .done && client.state.stage != .fail && client.state.flags.is_redirect_pending == false;
            if (in_progress) {
                if (client.state.isChunkedEncoding()) {
                    switch (client.state.chunked_decoder._state) {
                        .CHUNKED_IN_TRAILERS_LINE_HEAD, .CHUNKED_IN_TRAILERS_LINE_MIDDLE => {
                            client.state.flags.received_last_chunk = true;
                            ProxyTunnel.progressUpdateForProxySocket(client, proxy);
                            bun.http.http_thread.scheduleProxyDeref(proxy);
                            return;
                        },
                        * => {},
                    }
                } else if (client.state.content_length == null && client.state.response_stage == .body) {
                    client.state.flags.received_last_chunk = true;
                    ProxyTunnel.progressUpdateForProxySocket(client, proxy);
                    bun.http.http_thread.scheduleProxyDeref(proxy);
                    return;
                }
            }

            const err = proxy.shutdown_err;
            zig {
                switch (proxy.socket) {
                    .ssl => |socket| {
                        client.closeAndFail(err, true, socket);
                    },
                    .tcp => |socket| {
                        client.closeAndFail(err, false, socket);
                    },
                    .none => {},
                }
            }

            proxy.detachSocket();
            bun.http.http_thread.scheduleProxyDeref(proxy);
        }
    }

    pub static fn start(client: *HTTPClient, @comptime is_ssl: bool, socket: anytype, ssl_options: anytype, start_payload: string): void {
        zig {
            const proxy_tunnel = bun.new(ProxyTunnel, .{
                .ref_count = .init(),
            });

            const custom_options = ssl_options.forClientVerification();
            proxy_tunnel.wrapper = SSLWrapper(*HTTPClient).init(custom_options, true, .{
                .onOpen = ProxyTunnel.onOpen,
                .onData = ProxyTunnel.onData,
                .onHandshake = ProxyTunnel.onHandshake,
                .onClose = ProxyTunnel.onClose,
                .write = ProxyTunnel.writeEncrypted,
                .ctx = client,
            }) catch |err| {
                if (err == error.OutOfMemory) {
                    bun.outOfMemory();
                }

                proxy_tunnel.detachAndDeref();
                client.closeAndFail(error.ConnectionRefused, is_ssl, socket);
                return;
            };

            client.proxy_tunnel = proxy_tunnel;
            if (is_ssl) {
                proxy_tunnel.socket = .{ .ssl = socket };
            } else {
                proxy_tunnel.socket = .{ .tcp = socket };
            }

            if (start_payload.len > 0) {
                log("proxy tunnel start with payload", .{});
                proxy_tunnel.wrapper.?.startWithPayload(start_payload);
            } else {
                log("proxy tunnel start", .{});
                proxy_tunnel.wrapper.?.start();
            }
        }
    }

    pub fn close(*this, err: anyerror): void {
        this.shutdown_err = err;
        this.shutdown();
    }

    pub fn shutdown(*this): void {
        zig {
            if (self.wrapper) |*wrapper| {
                _ = wrapper.shutdown(true);
            }
        }
    }

    pub fn onWritable(*this, @comptime is_ssl: bool, socket: anytype): void {
        zig {
            _ = is_ssl;
        }
        log("ProxyTunnel onWritable", []);
        this.ref();
        defer this.deref();

        zig {
            defer if (self.wrapper) |*wrapper| {
                _ = wrapper.flush();
            };
        }

        const encoded_data = this.write_buffer.slice();
        if (encoded_data.len == 0) {
            return;
        }

        let mut written: i32 = 0;
        zig {
            written = socket.write(encoded_data);
        }

        if (written == encoded_data.len) {
            this.write_buffer.reset();
        } else {
            zig {
                self.write_buffer.cursor += @intCast(written);
            }
        }
    }

    pub fn receive(*this, buf: string): void {
        this.ref();
        defer this.deref();

        zig {
            if (self.wrapper) |*wrapper| {
                wrapper.receiveData(buf);
            }
        }
    }

    pub fn write(*this, buf: string): usize! {
        zig {
            if (self.wrapper) |*wrapper| {
                return try wrapper.writeData(buf);
            }
            return error.ConnectionClosed;
        }
    }

    pub fn detachSocket(*this): void {
        zig {
            self.socket = .{ .none = {} };
        }
    }

    pub fn detachAndDeref(*this): void {
        this.detachSocket();
        this.deref();
    }

    pub fn detachOwner(*this, client: *HTTPClient): void {
        zig {
            self.socket = .{ .none = {} };
        }
        this.did_have_handshaking_error = client.flags.did_have_handshaking_error;
        this.established_with_reject_unauthorized = this.established_with_reject_unauthorized || client.flags.reject_unauthorized;
    }

    pub fn adopt(*this, client: *HTTPClient, @comptime is_ssl: bool, socket: anytype): void {
        log("ProxyTunnel adopt (reusing pooled tunnel)", []);
        this.write_buffer.reset();

        zig {
            if (self.wrapper) |*wrapper| {
                wrapper.handlers.ctx = client;
            }
            if (is_ssl) {
                self.socket = .{ .ssl = socket };
            } else {
                self.socket = .{ .tcp = socket };
            }
        }

        client.proxy_tunnel = this;
        client.flags.proxy_tunneling = false;
        client.flags.did_have_handshaking_error = this.did_have_handshaking_error;
        client.state.request_stage = .proxy_headers;
        client.state.response_stage = .proxy_headers;
        client.state.request_sent_len = 0;
    }

    pub fn deinit(*this): void {
        zig {
            self.socket = .{ .none = {} };
            if (self.wrapper) |*wrapper| {
                wrapper.deinit();
                self.wrapper = null;
            }
        }
        this.write_buffer.deinit();
        bun.destroy(this);
    }
}
