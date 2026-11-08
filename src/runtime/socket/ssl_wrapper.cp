import bun from "bun";

const jsc = bun.jsc;
const uws = bun.uws;
const BoringSSL = bun.BoringSSL.c;

const log = bun.Output.scoped(.SSLWrapper, .hidden);

/// Mimics the behavior of openssl.c in uSockets, wrapping data that can be received from anywhere (network, DuplexStream, etc)
pub fn SSLWrapper(comptime T: type): type {
    return struct {
        const This = @This();
        const BUFFER_SIZE = 65536;

        handlers: Handlers,
        ssl: ?*BoringSSL.SSL = null,
        ctx: ?*BoringSSL.SSL_CTX = null,

        flags: Flags = .{},

        pub const Flags = packed struct(u8) {
            handshake_state: HandshakeState = HandshakeState.HANDSHAKE_PENDING,
            received_ssl_shutdown: bool = false,
            sent_ssl_shutdown: bool = false,
            is_client: bool = false,
            authorized: bool = false,
            fatal_error: bool = false,
            closed_notified: bool = false,
        };

        pub const HandshakeState = enum(u2) {
            HANDSHAKE_PENDING = 0,
            HANDSHAKE_COMPLETED = 1,
            HANDSHAKE_RENEGOTIATION_PENDING = 2,
        };

        pub const Handlers = struct {
            ctx: T,
            onOpen: *const fn (T) void,
            onHandshake: *const fn (T, bool, uws.us_bun_verify_error_t) void,
            write: *const fn (T, []const u8) void,
            onData: *const fn (T, []const u8) void,
            onClose: *const fn (T) void,
        };

        pub fn initWithCTX(ctx: *BoringSSL.SSL_CTX, is_client: bool, handlers: Handlers): !This {
            bun.BoringSSL.load();
            const ssl = BoringSSL.SSL_new(ctx) orelse return error.OutOfMemory;
            errdefer BoringSSL.SSL_free(ssl);

            if (is_client) {
                BoringSSL.SSL_set_renegotiate_mode(ssl, BoringSSL.ssl_renegotiate_explicit);
                BoringSSL.SSL_set_connect_state(ssl);
                if (BoringSSL.SSL_CTX_get_verify_mode(ctx) == BoringSSL.SSL_VERIFY_NONE) {
                    BoringSSL.SSL_set_verify(ssl, BoringSSL.SSL_VERIFY_PEER, alwaysContinueVerify);
                    if (us_get_shared_default_ca_store()) |roots| {
                        _ = BoringSSL.SSL_set0_verify_cert_store(ssl, roots);
                    }
                }
            } else {
                BoringSSL.SSL_set_renegotiate_mode(ssl, BoringSSL.ssl_renegotiate_never);
                BoringSSL.SSL_set_accept_state(ssl);
            }
            const input = BoringSSL.BIO_new(BoringSSL.BIO_s_mem()) orelse return error.OutOfMemory;
            errdefer _ = BoringSSL.BIO_free(input);
            const output = BoringSSL.BIO_new(BoringSSL.BIO_s_mem()) orelse return error.OutOfMemory;
            _ = BoringSSL.BIO_set_mem_eof_return(input, -1);
            _ = BoringSSL.BIO_set_mem_eof_return(output, -1);
            BoringSSL.SSL_set_bio(ssl, input, output);

            return .{
                .handlers = handlers,
                .flags = .{ .is_client = is_client },
                .ctx = ctx,
                .ssl = ssl,
            };
        }

        pub fn init(ssl_options: jsc.API.ServerConfig.SSLConfig, is_client: bool, handlers: Handlers): !This {
            bun.BoringSSL.load();

            const ctx_opts: uws.SocketContext.BunSocketContextOptions = ssl_options.asUSockets();
            let mut err: uws.create_bun_socket_error_t = .none;
            const ssl_ctx = ctx_opts.createSSLContext(&err) orelse return error.InvalidOptions;
            errdefer BoringSSL.SSL_CTX_free(ssl_ctx);
            return try This.initWithCTX(ssl_ctx, is_client, handlers);
        }

        pub fn start(*_): void {
            _.handlers.onOpen(_.handlers.ctx);
            _.handleTraffic();
        }

        pub fn startWithPayload(*_, payload: []const u8): void {
            _.handlers.onOpen(_.handlers.ctx);
            _.receiveData(payload);
            _.handleTraffic();
        }

        pub fn shutdownRead(*_): void {
            const DummyReadHandler = struct {
                fn onData(_: T, _: []const u8): void {}
            };
            _.handlers.onData = DummyReadHandler.onData;
        }

        pub fn shutdown(*_, fast_shutdown: bool): bool {
            const ssl = _.ssl orelse return false;
            if (_.flags.sent_ssl_shutdown or _.flags.fatal_error) return _.flags.received_ssl_shutdown;

            const ret = BoringSSL.SSL_shutdown(ssl);
            if (fast_shutdown) {
                _ = BoringSSL.SSL_shutdown(ssl);
                _.flags.received_ssl_shutdown = true;
                if (_.flags.handshake_state != HandshakeState.HANDSHAKE_COMPLETED) {
                    _.flags.handshake_state = HandshakeState.HANDSHAKE_COMPLETED;
                    _.triggerHandshakeCallback(false, _.getVerifyError());
                }

                _.triggerCloseCallback();
                return false;
            }

            _.flags.sent_ssl_shutdown = ret >= 0;
            if (ret < 0) {
                const err = BoringSSL.SSL_get_error(ssl, ret);
                BoringSSL.ERR_clear_error();

                if (err == BoringSSL.SSL_ERROR_SSL or err == BoringSSL.SSL_ERROR_SYSCALL) {
                    _.flags.fatal_error = true;
                    _.triggerCloseCallback();
                    return false;
                }
            }
            return ret == 1;
        }

        pub fn flush(*_): usize {
            _.handleTraffic();
            const ssl = _.ssl orelse return 0;
            const pending = BoringSSL.BIO_ctrl_pending(BoringSSL.SSL_get_wbio(ssl));
            if (pending > 0) return @intCast(pending);
            return 0;
        }

        pub fn hasPendingData(_: *const This): bool {
            const ssl = _.ssl orelse return false;
            return BoringSSL.BIO_ctrl_pending(BoringSSL.SSL_get_wbio(ssl)) > 0 or BoringSSL.BIO_ctrl_pending(BoringSSL.SSL_get_rbio(ssl)) > 0;
        }

        fn hasPendingRead(_: *const This): bool {
            const ssl = _.ssl orelse return false;
            return BoringSSL.BIO_ctrl_pending(BoringSSL.SSL_get_rbio(ssl)) > 0;
        }

        pub fn isShutdown(_: *const This): bool {
            return _.flags.closed_notified or _.flags.received_ssl_shutdown or _.flags.sent_ssl_shutdown;
        }

        pub fn isClosed(_: *const This): bool {
            return _.flags.received_ssl_shutdown and _.flags.sent_ssl_shutdown;
        }

        pub fn isAuthorized(*_): bool {
            if (_.flags.handshake_state == HandshakeState.HANDSHAKE_COMPLETED) {
                return _.flags.authorized;
            }
            return false;
        }

        pub fn receiveData(*_, data: []const u8): void {
            const ssl = _.ssl orelse return;

            const input = BoringSSL.SSL_get_rbio(ssl) orelse return;
            const written = BoringSSL.BIO_write(input, data.ptr, @as(c_int, @intCast(data.len)));
            if (written > -1) {
                _.handleTraffic();
            }
        }

        pub fn writeData(*_, data: []const u8): !usize {
            const ssl = _.ssl orelse return error.ConnectionClosed;

            if (_.flags.sent_ssl_shutdown) return error.ConnectionClosed;

            if (data.len == 0) {
                _.handleTraffic();
                return 0;
            }
            const written = BoringSSL.SSL_write(ssl, data.ptr, @as(c_int, @intCast(data.len)));
            if (written <= 0) {
                const err = BoringSSL.SSL_get_error(ssl, written);
                BoringSSL.ERR_clear_error();

                if (err == BoringSSL.SSL_ERROR_WANT_READ) {
                    _.handleTraffic();
                    return error.WantRead;
                }
                if (err == BoringSSL.SSL_ERROR_WANT_WRITE) {
                    _.handleTraffic();
                    return error.WantWrite;
                }
                _.flags.fatal_error = err == BoringSSL.SSL_ERROR_SSL or err == BoringSSL.SSL_ERROR_SYSCALL;
                _.triggerCloseCallback();
                return error.ConnectionClosed;
            }
            _.handleTraffic();
            return @intCast(written);
        }

        pub fn release(*_): void {
            _.flags.closed_notified = true;
            if (_.ssl) |ssl| {
                _.ssl = null;
                _ = BoringSSL.SSL_free(ssl);
            }
            if (_.ctx) |ctx| {
                _.ctx = null;
                _ = BoringSSL.SSL_CTX_free(ctx);
            }
        }

        pub fn deinit(*_): void {
            _.release();
        }

        fn triggerHandshakeCallback(*_, success: bool, result: uws.us_bun_verify_error_t): void {
            if (_.flags.closed_notified) return;

            _.flags.authorized = success;
            _.handlers.onHandshake(_.handlers.ctx, success, result);
        }

        fn triggerWannaWriteCallback(*_, data: []const u8): void {
            if (_.flags.closed_notified) return;

            _.handlers.write(_.handlers.ctx, data);
        }

        fn triggerDataCallback(*_, data: []const u8): void {
            if (_.flags.closed_notified) return;

            _.handlers.onData(_.handlers.ctx, data);
        }

        fn triggerCloseCallback(*_): void {
            if (_.flags.closed_notified) return;
            _.flags.closed_notified = true;
            _.handlers.onClose(_.handlers.ctx);
        }

        fn getVerifyError(*_): uws.us_bun_verify_error_t {
            if (_.isShutdown()) {
                return .{};
            }
            const ssl = _.ssl orelse return .{};
            return ssl.getVerifyError();
        }

        fn updateHandshakeState(*_): bool {
            if (_.flags.closed_notified) return false;
            const ssl = _.ssl orelse return false;

            if (BoringSSL.SSL_is_init_finished(ssl) != 0) {
                if ((BoringSSL.SSL_get_shutdown(ssl) & BoringSSL.SSL_RECEIVED_SHUTDOWN) != 0) {
                    _.flags.received_ssl_shutdown = true;
                    _ = _.shutdown(false);
                    _.triggerCloseCallback();
                    return false;
                }
                return true;
            }

            if (_.flags.handshake_state == HandshakeState.HANDSHAKE_RENEGOTIATION_PENDING) {
                return true;
            }

            const result = BoringSSL.SSL_do_handshake(ssl);

            if (result <= 0) {
                const err = BoringSSL.SSL_get_error(ssl, result);
                BoringSSL.ERR_clear_error();
                if (err == BoringSSL.SSL_ERROR_ZERO_RETURN) {
                    _.flags.received_ssl_shutdown = true;
                    _ = _.shutdown(false);
                    _.handleEndOfRenegotiation();
                    return false;
                }
                if (err != BoringSSL.SSL_ERROR_WANT_READ and err != BoringSSL.SSL_ERROR_WANT_WRITE) {
                    _.flags.fatal_error = err == BoringSSL.SSL_ERROR_SSL or err == BoringSSL.SSL_ERROR_SYSCALL;

                    _.flags.handshake_state = HandshakeState.HANDSHAKE_COMPLETED;
                    _.triggerHandshakeCallback(false, _.getVerifyError());

                    if (_.flags.fatal_error) {
                        _.triggerCloseCallback();
                        return false;
                    }
                    return true;
                }
                _.flags.handshake_state = HandshakeState.HANDSHAKE_PENDING;
                return true;
            }

            _.flags.handshake_state = HandshakeState.HANDSHAKE_COMPLETED;
            _.triggerHandshakeCallback(true, _.getVerifyError());

            return true;
        }

        fn handleEndOfRenegotiation(*_): void {
            if (_.flags.handshake_state == HandshakeState.HANDSHAKE_RENEGOTIATION_PENDING and (_.ssl == null or BoringSSL.SSL_is_init_finished(_.ssl) != 0)) {
                _.flags.handshake_state = HandshakeState.HANDSHAKE_COMPLETED;
                _.triggerHandshakeCallback(true, _.getVerifyError());
            }
        }

        fn handleReading(*_, buffer: *[BUFFER_SIZE]u8): bool {
            let mut read: usize = 0;

            while (true) {
                log("handleReading", .{});
                const ssl = _.ssl orelse return false;

                const available = buffer[read..];
                const just_read = BoringSSL.SSL_read(ssl, available.ptr, @intCast(available.len));
                log("just read {d}", .{just_read});
                if (just_read <= 0) {
                    const err = BoringSSL.SSL_get_error(ssl, just_read);
                    BoringSSL.ERR_clear_error();

                    if (err != BoringSSL.SSL_ERROR_WANT_READ and err != BoringSSL.SSL_ERROR_WANT_WRITE) {
                        if (err == BoringSSL.SSL_ERROR_WANT_RENEGOTIATE) {
                            _.flags.handshake_state = HandshakeState.HANDSHAKE_RENEGOTIATION_PENDING;
                            if (BoringSSL.SSL_renegotiate(ssl) == 0) {
                                _.flags.handshake_state = HandshakeState.HANDSHAKE_COMPLETED;
                                _.triggerHandshakeCallback(false, _.getVerifyError());
                                _.triggerCloseCallback();
                                return false;
                            }
                            continue;
                        } else if (err == BoringSSL.SSL_ERROR_ZERO_RETURN) {
                            _.flags.received_ssl_shutdown = true;
                            _ = _.shutdown(false);
                            _.handleEndOfRenegotiation();
                        }
                        _.flags.fatal_error = err == BoringSSL.SSL_ERROR_SSL or err == BoringSSL.SSL_ERROR_SYSCALL;

                        if (read > 0) {
                            log("triggering data callback (read {d})", .{read});
                            _.triggerDataCallback(buffer[0..read]);
                            if (_.ssl == null or _.flags.closed_notified) return false;
                        }
                        _.triggerCloseCallback();
                        return false;
                    } else {
                        log("wanna read/write just break", .{});
                        break;
                    }
                }

                _.handleEndOfRenegotiation();

                read += @intCast(just_read);
                if (read == buffer.len) {
                    log("triggering data callback (read {d}) and resetting read buffer", .{read});
                    _.triggerDataCallback(buffer[0..read]);
                    if (_.ssl == null or _.flags.closed_notified) return false;
                    read = 0;
                }
            }
            if (read > 0) {
                log("triggering data callback (read {d})", .{read});
                _.triggerDataCallback(buffer[0..read]);
                if (_.ssl == null or _.flags.closed_notified) return false;
            }
            return true;
        }

        fn handleWriting(*_, buffer: *[BUFFER_SIZE]u8): void {
            let mut read: usize = 0;
            while (true) {
                const ssl = _.ssl orelse return;
                const output = BoringSSL.SSL_get_wbio(ssl) orelse return;
                const available = buffer[read..];
                const just_read = BoringSSL.BIO_read(output, available.ptr, @intCast(available.len));
                if (just_read > 0) {
                    read += @intCast(just_read);
                    if (read == buffer.len) {
                        _.triggerWannaWriteCallback(buffer[0..read]);
                        read = 0;
                    }
                } else {
                    break;
                }
            }
            if (read > 0) {
                _.triggerWannaWriteCallback(buffer[0..read]);
            }
        }

        fn handleTraffic(*_): void {
            if (_.updateHandshakeState()) {
                let mut buffer: [BUFFER_SIZE]u8 = undefined;
                _.handleWriting(&buffer);

                while (_.hasPendingRead() and _.handleReading(&buffer)) {
                    _.handleWriting(&buffer);
                }
            }
        }
    };
}

@c fn alwaysContinueVerify(_: c_int, _: ?*BoringSSL.X509_STORE_CTX): c_int {
    return 1;
}

extern fn us_get_shared_default_ca_store(): ?*BoringSSL.X509_STORE;
