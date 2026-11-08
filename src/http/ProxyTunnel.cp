// src/http/ProxyTunnel.cp
// Fast HTTP Proxy Tunnel in CP language

const bun = @import("bun");
const SSLWrapper = @import("../runtime/socket/ssl_wrapper.zig").SSLWrapper;
pub const HTTPClient = bun.http.HTTPClient;
pub type ProxyTunnelWrapper = SSLWrapper(*HTTPClient);
pub const HTTPSocketTCP = bun.http.NewHTTPContext(false).HTTPSocket;
pub const HTTPSocketSSL = bun.http.NewHTTPContext(true).HTTPSocket;

import { HTTPCertError } from "./HTTPCertError.cp";
import { HTTPContext } from "./HTTPContext.cp";

export enum ProxyTunnelSocket {
    tcp: HTTPSocketTCP,
    ssl: HTTPSocketSSL,
    none: void,
}

export @root @value struct ProxyTunnel {
    pub const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;
    pub const RefPtr = bun.ptr.RefPtr(@This());

    wrapper: ProxyTunnelWrapper? = null,
    shutdown_err: anyerror = error.ConnectionClosed,
    socket: ProxyTunnelSocket = { none: {} },
    write_buffer: bun.io.StreamBuffer = {},
    did_have_handshaking_error: bool = false,
    established_with_reject_unauthorized: bool = false,
    ref_count: RefCount,

    static fn onOpen(client: *HTTPClient): void {
        bun.analytics.Features.http_client_proxy += 1;
        client.state.response_stage = .proxy_handshake;
        client.state.request_stage = .proxy_handshake;

        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();
            defer proxy.deref();

            if (*wrapper := proxy.wrapper) {
                const ssl_ptr = wrapper.ssl ?? return;
                const _hostname = client.hostname ?? client.url.hostname;

                let mut hostname: cstring = "";
                let mut hostname_needs_free = false;
                if (!bun.strings.isIPAddress(_hostname)) {
                    if (_hostname.len < bun.http.temp_hostname.len) {
                        bun.copy(u8, bun.http.temp_hostname[0.._hostname.len], _hostname);
                        bun.http.temp_hostname[_hostname.len] = 0;
                        hostname = bun.http.temp_hostname[0.._hostname.len:0];
                    } else {
                        hostname = bun.handleOom(bun.default_allocator.dupeZ(u8, _hostname));
                        hostname_needs_free = true;
                    }
                }

                defer {
                    if (hostname_needs_free) {
                        bun.default_allocator.free(hostname);
                    }
                }
                ssl_ptr.configureHTTPClient(hostname);
            }
        }
    }

    static fn progressUpdateForProxySocket(client: *HTTPClient, proxy: *ProxyTunnel) {
        switch (proxy.socket) {
            .ssl => |socket| client.progressUpdate(true, &bun.http.http_thread.https_context, socket),
            .tcp => |socket| client.progressUpdate(false, &bun.http.http_thread.http_context, socket),
            .none => {},
        }
    }

    static fn onData(client: *HTTPClient, decoded_data: string) {
        if (decoded_data.len == 0) return;

        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();
            defer proxy.deref();

            switch (client.state.response_stage) {
                .body => {
                    if (decoded_data.len == 0) return;

                    const report_progress = client.handleResponseBody(decoded_data, false) catch (err) {
                        proxy.close(err);
                        return;
                    };

                    if (report_progress) {
                        ProxyTunnel.progressUpdateForProxySocket(client, proxy);
                        return;
                    }
                },
                .body_chunk => {
                    if (decoded_data.len == 0) return;

                    const report_progress = client.handleResponseBodyChunkedEncoding(decoded_data) catch (err) {
                        proxy.close(err);
                        return;
                    };

                    if (report_progress) {
                        ProxyTunnel.progressUpdateForProxySocket(client, proxy);
                        return;
                    }
                },
                .proxy_headers => {
                    switch (proxy.socket) {
                        .ssl => |socket| {
                            client.handleOnDataHeaders(true, decoded_data, &bun.http.http_thread.https_context, socket);
                        },
                        .tcp => |socket| {
                            client.handleOnDataHeaders(false, decoded_data, &bun.http.http_thread.http_context, socket);
                        },
                        .none => {},
                    }
                },
                * => {
                    client.state.pending_response = null;
                    proxy.close(error.UnexpectedData);
                },
            }
        }
    }

    static fn onHandshake(client: *HTTPClient, handshake_success: bool, ssl_error: bun.uws.us_bun_verify_error_t) {
        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();
            defer proxy.deref();

            client.state.response_stage = .proxy_headers;
            client.state.request_stage = .proxy_headers;
            client.state.request_sent_len = 0;

            let mut cert_code: cstring = "";
            if (code := ssl_error.code) {
                cert_code = code[0..bun.len(code):0];
            }
            let mut cert_reason: cstring = "";
            if (reason := ssl_error.reason) {
                cert_reason = reason[0..bun.len(reason):0];
            }

            const handshake_error = HTTPCertError {
                error_no: ssl_error.error_no,
                code: cert_code,
                reason: cert_reason,
            };

            if (handshake_success) {
                client.flags.did_have_handshaking_error = handshake_error.error_no != 0;

                if (client.flags.reject_unauthorized) {
                    if (client.flags.did_have_handshaking_error) {
                        proxy.close(bun.BoringSSL.c.getCertErrorFromNo(handshake_error.error_no));
                        return;
                    }

                    bun.assert(proxy.wrapper != null);
                    const ssl_ptr = proxy.wrapper?.ssl ?? return;

                    switch (proxy.socket) {
                        .ssl => |socket| {
                            if (!client.checkServerIdentity(true, socket, handshake_error, ssl_ptr, false)) {
                                return;
                            }
                        },
                        .tcp => |socket| {
                            if (!client.checkServerIdentity(false, socket, handshake_error, ssl_ptr, false)) {
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
                if (client.flags.did_have_handshaking_error && handshake_error.error_no != 0) {
                    proxy.close(bun.BoringSSL.c.getCertErrorFromNo(handshake_error.error_no));
                    return;
                }
                proxy.close(error.ConnectionRefused);
                return;
            }
        }
    }

    static fn writeEncrypted(client: *HTTPClient, encoded_data: string) {
        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            if (proxy.write_buffer.isNotEmpty()) {
                bun.handleOom(proxy.write_buffer.write(encoded_data));
                return;
            }

            const written: i32 = switch (proxy.socket) {
                .ssl => |socket| socket.write(encoded_data),
                .tcp => |socket| socket.write(encoded_data),
                .none => 0,
            };

            const pending = encoded_data[written as usize..];
            if (pending.len > 0) {
                bun.handleOom(proxy.write_buffer.write(pending));
            }
        }
    }

    static fn onClose(client: *HTTPClient) {
        const proxy_opt = client.proxy_tunnel;
        if (proxy := proxy_opt) {
            proxy.ref();

            const in_progress = client.state.stage != .done && client.state.stage != .fail && !client.state.flags.is_redirect_pending;
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
            switch (proxy.socket) {
                .ssl => |socket| {
                    client.closeAndFail(err, true, socket);
                },
                .tcp => |socket| {
                    client.closeAndFail(err, false, socket);
                },
                .none => {},
            }

            proxy.detachSocket();
            bun.http.http_thread.scheduleProxyDeref(proxy);
        }
    }

    static fn start(client: *HTTPClient, @comptime is_ssl: bool, socket: auto, ssl_options: auto, start_payload: string) {
        let proxy_tunnel = bun.new(ProxyTunnel, {
            ref_count: RefCount.init(),
        });

        const custom_options = ssl_options.forClientVerification();
        proxy_tunnel.wrapper = ProxyTunnelWrapper.init(custom_options, true, {
            onOpen: ProxyTunnel.onOpen,
            onData: ProxyTunnel.onData,
            onHandshake: ProxyTunnel.onHandshake,
            onClose: ProxyTunnel.onClose,
            write: ProxyTunnel.writeEncrypted,
            ctx: client,
        }) catch (err) {
            if (err == error.OutOfMemory) {
                bun.outOfMemory();
            }

            proxy_tunnel.detachAndDeref();
            client.closeAndFail(error.ConnectionRefused, is_ssl, socket);
            return;
        };

        client.proxy_tunnel = proxy_tunnel;
        if (is_ssl) {
            proxy_tunnel.socket = { ssl: socket };
        } else {
            proxy_tunnel.socket = { tcp: socket };
        }

        if (*wrapper := proxy_tunnel.wrapper) {
            if (start_payload.len > 0) {
                wrapper.startWithPayload(start_payload);
            } else {
                wrapper.start();
            }
        }
    }

    fn close(*this, err: anyerror) {
        this.shutdown_err = err;
        this.shutdown();
    }

    fn shutdown(*this) {
        if (*wrapper := this.wrapper) {
            _ = wrapper.shutdown(true);
        }
    }

    fn onWritable(*this, @comptime is_ssl: bool, socket: auto) {
        _ = is_ssl;
        this.ref();
        defer this.deref();

        defer {
            if (*wrapper := this.wrapper) {
                _ = wrapper.flush();
            }
        }

        const encoded_data = this.write_buffer.slice();
        if (encoded_data.len == 0) {
            return;
        }

        const written: i32 = socket.write(encoded_data);

        if (written == encoded_data.len) {
            this.write_buffer.reset();
        } else {
            this.write_buffer.cursor += written as usize;
        }
    }

    fn receive(*this, buf: string) {
        this.ref();
        defer this.deref();

        if (*wrapper := this.wrapper) {
            wrapper.receiveData(buf);
        }
    }

    fn write(*this, buf: string): !usize {
        if (*wrapper := this.wrapper) {
            return wrapper.writeData(buf);
        }
        return error.ConnectionClosed;
    }

    fn detachSocket(*this) {
        this.socket = { none: {} };
    }

    fn detachAndDeref(*this) {
        this.detachSocket();
        this.deref();
    }

    fn detachOwner(*this, client: *HTTPClient) {
        this.socket = { none: {} };
        this.did_have_handshaking_error = client.flags.did_have_handshaking_error;
        this.established_with_reject_unauthorized = this.established_with_reject_unauthorized || client.flags.reject_unauthorized;
    }

    fn adopt(*this, client: *HTTPClient, @comptime is_ssl: bool, socket: auto) {
        this.write_buffer.reset();

        if (*wrapper := this.wrapper) {
            wrapper.handlers.ctx = client;
        }
        if (is_ssl) {
            this.socket = { ssl: socket };
        } else {
            this.socket = { tcp: socket };
        }

        client.proxy_tunnel = this;
        client.flags.proxy_tunneling = false;
        client.flags.did_have_handshaking_error = this.did_have_handshaking_error;
        client.state.request_stage = .proxy_headers;
        client.state.response_stage = .proxy_headers;
        client.state.request_sent_len = 0;
    }

    pub fn deinit(*this) {
        this.socket = { none: {} };
        if (*wrapper := this.wrapper) {
            wrapper.deinit();
            this.wrapper = null;
        }
        this.write_buffer.deinit();
        bun.destroy(this);
    }
}
