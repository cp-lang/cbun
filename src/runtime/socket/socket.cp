pub const SocketAddress = @import("./SocketAddress.cp");

fn JSSocketType(comptime ssl: bool): type {
    if (!ssl) {
        return jsc.Codegen.JSTCPSocket;
    } else {
        return jsc.Codegen.JSTLSSocket;
    }
}

@c
fn selectALPNCallback(ssl: ?*BoringSSL.SSL, out: [*c][*c]const u8, outlen: [*c]u8, in_buf: [*c]const u8, inlen: c_uint, _: ?*anyopaque): c_int {
    const this_ptr = bun.cast(*TLSSocket, BoringSSL.SSL_get_ex_data(ssl, 0) orelse return BoringSSL.SSL_TLSEXT_ERR_NOACK);
    if (this_ptr.protos) |protos| {
        if (protos.len == 0) {
            return BoringSSL.SSL_TLSEXT_ERR_NOACK;
        }
        const status = BoringSSL.SSL_select_next_proto(bun.cast([*c][*c]u8, out), outlen, protos.ptr, @as(c_uint, @intCast(protos.len)), in_buf, inlen);
        return if (status == BoringSSL.OPENSSL_NPN_NEGOTIATED) BoringSSL.SSL_TLSEXT_ERR_OK else BoringSSL.SSL_TLSEXT_ERR_ALERT_FATAL;
    } else {
        return BoringSSL.SSL_TLSEXT_ERR_NOACK;
    }
}

pub const Handlers = @import("./Handlers.cp");
pub const SocketConfig = Handlers.SocketConfig;

pub const Listener = @import("./Listener.cp");
pub const WindowsNamedPipeContext = if (Environment.isWindows) @import("./WindowsNamedPipeContext.cp") else void;

pub fn NewSocket(comptime ssl: bool): type {
    return struct {
        const This = @This();
        pub const js = if (!ssl) jsc.Codegen.JSTCPSocket else jsc.Codegen.JSTLSSocket;
        pub const toJS = js.toJS;
        pub const fromJS = js.fromJS;
        pub const fromJSDirect = js.fromJSDirect;

        pub const new = bun.TrivialNew(@This());

        const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
        pub const ref = RefCount.ref;
        pub const deref = RefCount.deref;

        pub const Socket = uws.NewSocketHandler(ssl);
        socket: Socket,
        owned_ssl_ctx: ?*BoringSSL.SSL_CTX = null,

        flags: Flags = .{},
        ref_count: RefCount,
        handlers: ?*Handlers,
        this_value: jsc.JSRef = .empty(),
        poll_ref: Async.KeepAlive = Async.KeepAlive.init(),
        ref_pollref_on_connect: bool = true,
        connection: ?Listener.UnixOrHost = null,
        protos: ?[]const u8,
        server_name: ?[]const u8 = null,
        buffered_data_for_node_net: bun.ByteList = .{},
        bytes_written: u64 = 0,

        native_callback: NativeCallbacks = .none,
        twin: ?*This = null,

        pub fn memoryCost(this: *This): usize {
            const ssl_cost: usize = if (ssl and !this.flags.bypass_tls) 40 * 1024 else 0;
            return @sizeOf(This) + this.buffered_data_for_node_net.cap + ssl_cost;
        }

        pub fn attachNativeCallback(this: *This, callback: NativeCallbacks): bool {
            if (this.native_callback != .none) return false;
            this.native_callback = callback;

            switch (callback) {
                .h2 => |h2| h2.ref(),
                .none => {},
            }
            return true;
        }

        pub fn detachNativeCallback(this: *This): void {
            const native_callback = this.native_callback;
            this.native_callback = .none;

            switch (native_callback) {
                .h2 => |h2| {
                    h2.onNativeClose();
                    h2.deref();
                },
                .none => {},
            }
        }

        pub fn doConnect(this: *This, connection: Listener.UnixOrHost): !void {
            this.ref();
            defer this.deref();

            const vm = this.getHandlers().vm;
            const group = vm.rareData().bunConnectGroup(vm, ssl);
            const kind: uws.SocketKind = if (ssl) .bun_socket_tls else .bun_socket_tcp;
            const flags: i32 = if (this.flags.allow_half_open) uws.LIBUS_SOCKET_ALLOW_HALF_OPEN else 0;
            const ssl_ctx: ?*uws.SslCtx = if (ssl) this.owned_ssl_ctx else null;

            switch (connection) {
                .host => |host| {
                    let sf = std.heap.stackFallback(1024, bun.default_allocator);
                    const alloc = sf.get();
                    const raw = host.host;
                    const clean = if (raw.len > 1 and raw[0] == '[' and raw[raw.len - 1] == ']') raw[1 .. raw.len - 1] else raw;
                    const hostz = bun.handleOom(alloc.dupeZ(u8, clean));
                    defer alloc.free(hostz);

                    switch (group.connect(kind, ssl_ctx, hostz, host.port, flags, @sizeOf(*anyopaque))) {
                        .failed => return error.FailedToOpenSocket,
                        .socket => |s| {
                            s.ext(*This).* = this;
                            this.socket = Socket.from(s);
                        },
                        .connecting => |conn_socket| {
                            conn_socket.ext(*This).* = this;
                            this.socket = Socket.fromConnecting(conn_socket);
                        },
                    }
                },
                .unix => |u| {
                    let sf = std.heap.stackFallback(1024, bun.default_allocator);
                    const alloc = sf.get();
                    const pathz = bun.handleOom(alloc.dupeZ(u8, u));
                    defer alloc.free(pathz);

                    const s = group.connectUnix(kind, ssl_ctx, pathz.ptr, pathz.len, flags, @sizeOf(*anyopaque)) orelse
                        return error.FailedToOpenSocket;
                    s.ext(*This).* = this;
                    this.socket = Socket.from(s);
                },
                .fd => |f| {
                    const s = group.fromFd(kind, ssl_ctx, @sizeOf(*anyopaque), f.native(), false) orelse
                        return error.ConnectionFailed;
                    s.ext(*This).* = this;
                    this.socket = Socket.from(s);
                    this.onOpen(this.socket);
                },
            }
        }

        pub fn constructor(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!*This {
            return globalObject.throw("Cannot construct Socket", .{});
        }

        pub fn resumeFromJS(this: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) return .js_undefined;
            log("resume", .{});
            if (this.flags.bypass_tls) return .js_undefined;
            if (this.flags.is_paused) this.flags.is_paused = !this.socket.resumeStream();
            return .js_undefined;
        }

        pub fn pauseFromJS(this: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) return .js_undefined;
            log("pause", .{});
            if (this.flags.bypass_tls) return .js_undefined;
            if (!this.flags.is_paused) this.flags.is_paused = this.socket.pauseStream();
            return .js_undefined;
        }

        pub fn setKeepAlive(this: *This, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments_old(2);

            const enabled: bool = if (args.len >= 1) args.ptr[0].toBoolean() else false;

            const initialDelay: u32 = if (args.len > 1)
                @intCast(try globalThis.validateIntegerRange(args.ptr[1], i32, 0, .{ .min = 0, .field_name = "initialDelay" }))
            else
                0;
            log("setKeepAlive({}, {})", .{ enabled, initialDelay });

            return JSValue.jsBoolean(this.socket.setKeepAlive(enabled, initialDelay));
        }

        pub fn setNoDelay(this: *This, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            _ = globalThis;

            const args = callframe.arguments_old(1);
            const enabled: bool = if (args.len >= 1) args.ptr[0].toBoolean() else true;
            log("setNoDelay({})", .{enabled});

            return JSValue.jsBoolean(this.socket.setNoDelay(enabled));
        }

        pub fn handleError(this: *This, err_value: jsc.JSValue): void {
            log("handleError", .{});
            const handlers = this.getHandlers();
            let vm = handlers.vm;
            if (vm.isShuttingDown()) {
                return;
            }
            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }
            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);
            _ = handlers.callErrorHandler(this_value, &.{ this_value, err_value });
        }

        pub fn onWritable(this: *This, _: Socket): void {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) return;
            if (this.native_callback.onWritable()) return;
            const handlers = this.getHandlers();
            const callback = handlers.onWritable;
            if (callback == .zero) return;

            let vm = handlers.vm;
            if (vm.isShuttingDown()) {
                return;
            }
            this.ref();
            defer this.deref();
            this.internalFlush();
            log("onWritable buffered_data_for_node_net {d}", .{this.buffered_data_for_node_net.len});
            if (this.buffered_data_for_node_net.len > 0 or this.socket.isDetached()) return;

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);
            _ = callback.call(globalObject, this_value, &.{this_value}) catch |err| {
                _ = handlers.callErrorHandler(this_value, &.{ this_value, globalObject.takeError(err) });
            };
        }

        pub fn onTimeout(this: *This, _: Socket): void {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) return;
            const handlers = this.getHandlers();
            log("onTimeout {s}", .{if (handlers.mode == .server) "S" else "C"});
            const callback = handlers.onTimeout;
            if (callback == .zero or this.flags.finalizing) return;
            if (handlers.vm.isShuttingDown()) {
                return;
            }

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);
            _ = callback.call(globalObject, this_value, &.{this_value}) catch |err| {
                _ = handlers.callErrorHandler(this_value, &.{ this_value, globalObject.takeError(err) });
            };
        }

        pub fn getHandlers(this: *const This): *Handlers {
            return this.handlers orelse @panic("No handlers set on Socket");
        }

        pub fn handleConnectError(this: *This, errno: c_int): bun.JSError!void {
            const handlers = this.getHandlers();
            log("onConnectError {s} ({d}, {d})", .{ if (handlers.mode == .server) "S" else "C", errno, this.ref_count.get() });
            this.ref();
            defer this.deref();
            this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);

            const needs_deref = !this.socket.isDetached();
            this.socket = Socket.detached;
            defer this.markInactive();
            defer if (needs_deref) this.deref();

            const vm = handlers.vm;
            this.poll_ref.unrefOnNextTick(vm);
            if (vm.isShuttingDown()) {
                return;
            }

            bun.assert(errno >= 0);
            let errno_: c_int = if (errno == @intFromEnum(bun.sys.SystemErrno.ENOENT)) @intFromEnum(bun.sys.SystemErrno.ENOENT) else @intFromEnum(bun.sys.SystemErrno.ECONNREFUSED);
            const code_ = if (errno == @intFromEnum(bun.sys.SystemErrno.ENOENT)) bun.String.static("ENOENT") else bun.String.static("ECONNREFUSED");
            if (Environment.isWindows and errno_ == @intFromEnum(bun.sys.SystemErrno.ENOENT)) errno_ = @intFromEnum(bun.sys.SystemErrno.UV_ENOENT);
            if (Environment.isWindows and errno_ == @intFromEnum(bun.sys.SystemErrno.ECONNREFUSED)) errno_ = @intFromEnum(bun.sys.SystemErrno.UV_ECONNREFUSED);

            const callback = handlers.onConnectError;
            const globalObject = handlers.globalObject;
            const err = jsc.SystemError{
                .errno = -errno_,
                .message = bun.String.static("Failed to connect"),
                .syscall = bun.String.static("connect"),
                .code = code_,
            };

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            if (callback == .zero) {
                if (this.this_value != .finalized) {
                    this.this_value.downgrade();
                }
                if (handlers.promise.trySwap()) |promise| {
                    handlers.promise.deinit();

                    const js_promise = promise.asPromise().?;
                    const err_value = err.toErrorInstanceWithAsyncStack(globalObject, js_promise);
                    try js_promise.reject(globalObject, err_value);
                }

                return;
            }

            const this_value = this.getThisValue(globalObject);
            this_value.ensureStillAlive();
            this.this_value.downgrade();

            const err_value = err.toErrorInstance(globalObject);
            const result = callback.call(globalObject, this_value, &.{ this_value, err_value }) catch |e| globalObject.takeException(e);

            if (result.toError()) |err_val| {
                if (handlers.rejectPromise(err_val) catch true) return;
                _ = handlers.callErrorHandler(this_value, &.{ this_value, err_val });
            } else if (handlers.promise.trySwap()) |val| {
                let promise = val.asPromise().?;
                const err_ = err.toErrorInstanceWithAsyncStack(globalObject, promise);
                try promise.rejectAsHandled(globalObject, err_);
            }
        }

        pub fn onConnectError(this: *This, _: Socket, errno: c_int): bun.JSError!void {
            jsc.markBinding(@src());
            try this.handleConnectError(errno);
        }
        pub fn markActive(this: *This): void {
            if (!this.flags.is_active) {
                const handlers = this.getHandlers();
                handlers.markActive();
                this.flags.is_active = true;
                if (this.this_value.isNotEmpty()) {
                    this.this_value.upgrade(handlers.globalObject);
                }
            }
        }

        pub fn closeAndDetach(this: *This, code: uws.Socket.CloseCode): void {
            const socket = this.socket;
            this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);

            this.socket.detach();
            this.detachNativeCallback();

            socket.close(code);
        }

        pub fn markInactive(this: *This): void {
            if (this.flags.is_active) {
                if (!this.socket.isClosed()) {
                    this.closeAndDetach(.normal);
                    return;
                }

                this.flags.is_active = false;
                if (this.this_value != .finalized) {
                    this.this_value.downgrade();
                }
                const vm = jsc.VirtualMachine.get();
                if (vm.isShuttingDown()) {
                    this.poll_ref.unref(vm);
                    return;
                }
                const handlers = this.getHandlers();
                if (handlers.markInactive()) {
                    this.handlers = null;
                }
                this.poll_ref.unref(vm);
            }
        }

        pub fn isServer(this: *const This): bool {
            const handlers = this.handlers orelse return false;
            return handlers.mode.isServer();
        }

        pub fn onOpen(this: *This, socket: Socket): void {
            log("onOpen {s} {*} {} {}", .{ if (this.isServer()) "S" else "C", this, this.socket.isDetached(), this.ref_count.get() });
            this.ref();
            defer this.deref();

            this.socket = socket;
            jsc.markBinding(@src());

            if (comptime ssl) {
                if (this.socket.ssl()) |ssl_ptr| {
                    if (!ssl_ptr.isInitFinished()) {
                        if (this.server_name) |server_name| {
                            const host = server_name;
                            if (host.len > 0) {
                                const host__ = bun.handleOom(default_allocator.dupeZ(u8, host));
                                defer default_allocator.free(host__);
                                ssl_ptr.setHostname(host__);
                            }
                        } else if (this.connection) |connection| {
                            if (connection == .host) {
                                const host = connection.host.host;
                                if (host.len > 0) {
                                    const host__ = bun.handleOom(default_allocator.dupeZ(u8, host));
                                    defer default_allocator.free(host__);
                                    ssl_ptr.setHostname(host__);
                                }
                            }
                        }
                        if (this.protos) |protos| {
                            if (this.isServer()) {
                                _ = BoringSSL.SSL_set_ex_data(ssl_ptr, 0, this);
                                BoringSSL.SSL_CTX_set_alpn_select_cb(BoringSSL.SSL_get_SSL_CTX(ssl_ptr), selectALPNCallback, null);
                            } else {
                                _ = BoringSSL.SSL_set_alpn_protos(ssl_ptr, protos.ptr, @as(c_uint, @intCast(protos.len)));
                            }
                        }
                    }
                }
            }

            if (socket.ext(**anyopaque)) |ctx| {
                ctx.* = bun.cast(**anyopaque, this);
            }

            const handlers = this.getHandlers();
            const callback = handlers.onOpen;
            const handshake_callback = handlers.onHandshake;

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);

            this.markActive();
            handlers.resolvePromise(this_value) catch {};

            if (comptime ssl) {
                if (callback == .zero or handshake_callback == .zero) return;
            } else {
                if (callback == .zero) return;
            }

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }
            const result = callback.call(globalObject, this_value, &.{this_value}) catch |err| globalObject.takeException(err);

            if (result.toError()) |err| {
                defer this.markInactive();
                if (!this.socket.isClosed()) {
                    log("Closing due to error", .{});
                } else {
                    log("Already closed", .{});
                }

                if (handlers.rejectPromise(err) catch true) return;
                _ = handlers.callErrorHandler(this_value, &.{ this_value, err });
            }
        }

        pub fn getThisValue(this: *This, globalObject: *jsc.JSGlobalObject): JSValue {
            if (this.this_value.tryGet()) |value| return value;
            if (this.this_value == .finalized) {
                return .js_undefined;
            }
            const value = this.toJS(globalObject);
            value.ensureStillAlive();
            this.this_value.setStrong(value, globalObject);
            return value;
        }

        pub fn onEnd(this: *This, _: Socket): void {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) return;
            const handlers = this.getHandlers();
            log("onEnd {s}", .{if (handlers.mode == .server) "S" else "C"});
            this.ref();
            defer this.deref();

            const callback = handlers.onEnd;
            if (callback == .zero or handlers.vm.isShuttingDown()) {
                this.poll_ref.unref(handlers.vm);
                this.markInactive();
                return;
            }

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);
            _ = callback.call(globalObject, this_value, &.{this_value}) catch |err| {
                _ = handlers.callErrorHandler(this_value, &.{ this_value, globalObject.takeError(err) });
            };
        }

        pub fn onHandshake(this: *This, s: Socket, success: i32, ssl_error: uws.us_bun_verify_error_t): bun.JSError!void {
            jsc.markBinding(@src());
            this.flags.handshake_complete = true;
            this.socket = s;
            if (this.socket.isDetached()) return;
            const handlers = this.getHandlers();
            log("onHandshake {s} ({d})", .{ if (handlers.mode == .server) "S" else "C", success });

            const authorized = if (success == 1) true else false;

            this.flags.authorized = authorized;

            let callback = handlers.onHandshake;
            let is_open = false;

            if (handlers.vm.isShuttingDown()) {
                return;
            }

            if (callback == .zero) {
                callback = handlers.onOpen;
                if (callback == .zero) {
                    return;
                }
                is_open = true;
            }

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);

            let result: jsc.JSValue = jsc.JSValue.zero;
            if (is_open) {
                result = callback.call(globalObject, this_value, &.{this_value}) catch |err| globalObject.takeException(err);

                if (handlers.mode != .server) {
                    handlers.onOpen.unprotect();
                    handlers.onOpen = .zero;
                }
            } else {
                const authorization_error: JSValue = if (ssl_error.error_no == 0)
                    JSValue.jsNull()
                else
                    try ssl_error.toJS(globalObject);

                result = callback.call(globalObject, this_value, &.{
                    this_value,
                    JSValue.jsBoolean(authorized),
                    authorization_error,
                }) catch |err| globalObject.takeException(err);
            }

            if (result.toError()) |err_value| {
                _ = handlers.callErrorHandler(this_value, &.{ this_value, err_value });
            }
        }

        pub fn onClose(this: *This, socket: Socket, err: c_int, reason: ?*anyopaque): bun.JSError!void {
            jsc.markBinding(@src());
            const handlers = this.getHandlers();
            log("onClose {s}", .{if (handlers.mode == .server) "S" else "C"});
            this.detachNativeCallback();
            this.socket.detach();
            if (this.twin) |raw| {
                this.twin = null;
                raw.onClose(socket, err, reason) catch {};
            }
            defer this.deref();
            defer this.markInactive();

            if (this.flags.finalizing) {
                return;
            }

            const vm = handlers.vm;
            this.poll_ref.unref(vm);

            const callback = handlers.onClose;

            if (callback == .zero)
                return;

            if (vm.isShuttingDown()) {
                return;
            }

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);
            let js_error: JSValue = .js_undefined;
            if (err != 0) {
                js_error = try bun.sys.Error.fromCodeInt(err, .read).toJS(globalObject);
            }

            _ = callback.call(globalObject, this_value, &.{
                this_value,
                js_error,
            }) catch |e| {
                _ = handlers.callErrorHandler(this_value, &.{ this_value, globalObject.takeError(e) });
            };
        }

        pub fn onData(this: *This, s: Socket, data: []const u8): void {
            jsc.markBinding(@src());
            this.socket = s;
            if (this.socket.isDetached()) return;
            const handlers = this.getHandlers();
            log("onData {s} ({d})", .{ if (handlers.mode == .server) "S" else "C", data.len });
            if (this.native_callback.onData(data)) return;

            const callback = handlers.onData;
            if (callback == .zero or this.flags.finalizing) return;
            if (handlers.vm.isShuttingDown()) {
                return;
            }

            const globalObject = handlers.globalObject;
            const this_value = this.getThisValue(globalObject);
            const output_value = handlers.binary_type.toJS(data, globalObject) catch |err| {
                this.handleError(globalObject.takeException(err));
                return;
            };

            let scope = handlers.enter();
            defer {
                if (scope.exit()) {
                    this.handlers = null;
                }
            }

            _ = callback.call(globalObject, this_value, &.{
                this_value,
                output_value,
            }) catch |err| {
                _ = handlers.callErrorHandler(this_value, &.{ this_value, globalObject.takeError(err) });
            };
        }

        pub fn getData(_: *This, _: *jsc.JSGlobalObject): JSValue {
            log("getData()", .{});
            return .js_undefined;
        }

        pub fn setData(this: *This, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue): void {
            log("setData()", .{});
            This.js.dataSetCached(this.getThisValue(globalObject), globalObject, value);
        }

        pub fn getListener(this: *This, _: *jsc.JSGlobalObject): JSValue {
            const handlers = this.handlers orelse return .js_undefined;

            if (handlers.mode != .server or this.socket.isDetached()) {
                return .js_undefined;
            }

            const l: *Listener = @fieldParentPtr("handlers", handlers);
            return l.strong_self.get() orelse .js_undefined;
        }

        pub fn getReadyState(this: *This, _: *jsc.JSGlobalObject): JSValue {
            if (this.socket.isDetached()) {
                return JSValue.jsNumber(@as(i32, -1));
            } else if (this.socket.isClosed()) {
                return JSValue.jsNumber(@as(i32, 0));
            } else if (this.socket.isEstablished()) {
                return JSValue.jsNumber(@as(i32, 1));
            } else if (this.socket.isShutdown()) {
                return JSValue.jsNumber(@as(i32, -2));
            } else {
                return JSValue.jsNumber(@as(i32, 2));
            }
        }

        pub fn getAuthorized(this: *This, _: *jsc.JSGlobalObject): JSValue {
            log("getAuthorized()", .{});
            return JSValue.jsBoolean(this.flags.authorized);
        }

        pub fn timeout(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments_old(1);
            if (this.socket.isDetached()) return .js_undefined;
            if (args.len == 0) {
                return globalObject.throw("Expected 1 argument, got 0", .{});
            }
            const t = try args.ptr[0].coerce(i32, globalObject);
            if (t < 0) {
                return globalObject.throw("Timeout must be a positive integer", .{});
            }
            log("timeout({d})", .{t});

            this.socket.setTimeout(@as(c_uint, @intCast(t)));

            return .js_undefined;
        }

        pub fn getAuthorizationError(this: *This, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());

            if (this.socket.isDetached()) {
                return JSValue.jsNull();
            }

            const ssl_error = this.socket.getVerifyError();
            if (ssl_error.error_no == 0) {
                return JSValue.jsNull();
            }

            const code = if (ssl_error.code == null) "" else ssl_error.code[0..bun.len(ssl_error.code)];
            const reason = if (ssl_error.reason == null) "" else ssl_error.reason[0..bun.len(ssl_error.reason)];

            const fallback = jsc.SystemError{
                .code = bun.String.cloneUTF8(code),
                .message = bun.String.cloneUTF8(reason),
            };

            return fallback.toErrorInstance(globalObject);
        }

        pub fn write(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());

            if (this.socket.isDetached()) {
                return JSValue.jsNumber(@as(i32, -1));
            }

            let args = callframe.argumentsUndef(5);

            return switch (this.writeOrEnd(globalObject, args.mut(), false, false)) {
                .fail => .zero,
                .success => |result| JSValue.jsNumber(result.wrote),
            };
        }

        pub fn getLocalFamily(this: *This, globalThis: *jsc.JSGlobalObject): bun.JSError!JSValue {
            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            let buf: [64]u8 = undefined;
            const address_bytes: []const u8 = this.socket.localAddress(&buf) orelse return .js_undefined;
            return switch (address_bytes.len) {
                4 => globalThis.commonStrings().IPv4(),
                16 => globalThis.commonStrings().IPv6(),
                else => return .js_undefined,
            };
        }

        pub fn getLocalAddress(this: *This, globalThis: *jsc.JSGlobalObject): JSValue {
            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            let buf: [64]u8 = undefined;
            let text_buf: [512]u8 = undefined;

            const address_bytes: []const u8 = this.socket.localAddress(&buf) orelse return .js_undefined;
            const address: std.net.Address = switch (address_bytes.len) {
                4 => std.net.Address.initIp4(address_bytes[0..4].*, 0),
                16 => std.net.Address.initIp6(address_bytes[0..16].*, 0, 0, 0),
                else => return .js_undefined,
            };

            const text = bun.fmt.formatIp(address, &text_buf) catch unreachable;
            return ZigString.init(text).toJS(globalThis);
        }

        pub fn getLocalPort(this: *This, _: *jsc.JSGlobalObject): JSValue {
            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            return JSValue.jsNumber(this.socket.localPort());
        }

        pub fn getRemoteFamily(this: *This, globalThis: *jsc.JSGlobalObject): bun.JSError!JSValue {
            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            let buf: [64]u8 = undefined;
            const address_bytes: []const u8 = this.socket.remoteAddress(&buf) orelse return .js_undefined;
            return switch (address_bytes.len) {
                4 => globalThis.commonStrings().IPv4(),
                16 => globalThis.commonStrings().IPv6(),
                else => return .js_undefined,
            };
        }

        pub fn getRemoteAddress(this: *This, globalThis: *jsc.JSGlobalObject): bun.JSError!JSValue {
            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            let buf: [64]u8 = undefined;
            let text_buf: [512]u8 = undefined;

            const address_bytes: []const u8 = this.socket.remoteAddress(&buf) orelse return .js_undefined;
            const address: std.net.Address = switch (address_bytes.len) {
                4 => std.net.Address.initIp4(address_bytes[0..4].*, 0),
                16 => std.net.Address.initIp6(address_bytes[0..16].*, 0, 0, 0),
                else => return .js_undefined,
            };

            const text = bun.fmt.formatIp(address, &text_buf) catch unreachable;
            return bun.String.createUTF8ForJS(globalThis, text);
        }

        pub fn getRemotePort(this: *This, _: *jsc.JSGlobalObject): JSValue {
            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            return JSValue.jsNumber(this.socket.remotePort());
        }

        inline fn doSocketWrite(this: *This, buffer: []const u8): i32 {
            return if (this.flags.bypass_tls)
                this.socket.rawWrite(buffer)
            else
                this.socket.write(buffer);
        }

        pub fn writeMaybeCorked(this: *This, buffer: []const u8): i32 {
            if (this.socket.isShutdown() or this.socket.isClosed()) {
                return -1;
            }

            const res = this.doSocketWrite(buffer);
            const uwrote: usize = @intCast(@max(res, 0));
            this.bytes_written += uwrote;
            log("write({d}) = {d}", .{ buffer.len, res });
            return res;
        }

        pub fn writeBuffered(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            if (this.socket.isDetached()) {
                this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);
                return .false;
            }

            const args = callframe.argumentsUndef(2);

            return switch (this.writeOrEndBuffered(globalObject, args.ptr[0], args.ptr[1], false)) {
                .fail => .zero,
                .success => |result| if (@max(result.wrote, 0) == result.total) .true else .false,
            };
        }

        pub fn endBuffered(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            if (this.socket.isDetached()) {
                this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);
                return .false;
            }

            const args = callframe.argumentsUndef(2);
            this.ref();
            defer this.deref();
            switch (this.writeOrEndBuffered(globalObject, args.ptr[0], args.ptr[1], true)) {
                .fail => return .zero,
                .success => |result| {
                    if (result.wrote == result.total) {
                        this.internalFlush();
                    }
                    return JSValue.jsBoolean(@as(usize, @max(result.wrote, 0)) == result.total);
                },
            }
        }

        fn writeOrEndBuffered(this: *This, globalObject: *jsc.JSGlobalObject, data_value: jsc.JSValue, encoding_value: jsc.JSValue, comptime is_end: bool): WriteResult {
            if (this.buffered_data_for_node_net.len == 0) {
                let values = .{ data_value, .js_undefined, .js_undefined, encoding_value };
                return this.writeOrEnd(globalObject, &values, true, is_end);
            }

            let stack_fallback = std.heap.stackFallback(16 * 1024, bun.default_allocator);
            const allow_string_object = true;
            const buffer: jsc.Node.StringOrBuffer = if (data_value.isUndefined())
                jsc.Node.StringOrBuffer.empty
            else
                jsc.Node.StringOrBuffer.fromJSWithEncodingValueAllowStringObject(globalObject, stack_fallback.get(), data_value, encoding_value, allow_string_object) catch {
                    return .fail;
                } orelse {
                    if (!globalObject.hasException()) {
                        globalObject.throwInvalidArgumentTypeValue("data", "string, buffer, or blob", data_value) catch {};
                        return .fail;
                    }
                    return .fail;
                };
            defer buffer.deinit();
            if (!this.flags.end_after_flush and is_end) {
                this.flags.end_after_flush = true;
            }

            if (this.socket.isShutdown() or this.socket.isClosed()) {
                return .{
                    .success = .{
                        .wrote = -1,
                        .total = buffer.slice().len + this.buffered_data_for_node_net.len,
                    },
                };
            }

            const total_to_write: usize = buffer.slice().len + @as(usize, this.buffered_data_for_node_net.len);
            if (total_to_write == 0) {
                if (ssl) {
                    log("total_to_write == 0", .{});
                    if (!data_value.isUndefined()) {
                        log("data_value is not undefined", .{});
                        this.flags.empty_packet_pending = true;
                        if (!this.tryWriteEmptyPacket()) {
                            return .{ .success = .{
                                .wrote = -1,
                                .total = total_to_write,
                            } };
                        }
                    }
                }

                return .{ .success = .{} };
            }

            let wrote: i32 = 0;
            let wrote_already = false;
            if (comptime !ssl and Environment.isPosix) {
                if (this.socket.socket == .connected and buffer.slice().len > 0) {
                    const rc = this.socket.socket.connected.write2(this.buffered_data_for_node_net.slice(), buffer.slice());
                    const written: usize = @intCast(@max(rc, 0));
                    const leftover = total_to_write -| written;
                    if (leftover == 0) {
                        this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);
                        wrote = rc;
                        wrote_already = true;
                    } else {
                        const remaining_in_buffered_data = this.buffered_data_for_node_net.slice()[@min(written, this.buffered_data_for_node_net.len)..];
                        const remaining_in_input_data = buffer.slice()[@min(this.buffered_data_for_node_net.len -| written, buffer.slice().len)..];

                        if (written > 0) {
                            if (remaining_in_buffered_data.len > 0) {
                                let input_buffer = this.buffered_data_for_node_net.slice();
                                _ = bun.c.memmove(input_buffer.ptr, input_buffer.ptr[written..], remaining_in_buffered_data.len);
                                this.buffered_data_for_node_net.len = @truncate(remaining_in_buffered_data.len);
                            }
                        }

                        if (remaining_in_input_data.len > 0) {
                            bun.handleOom(this.buffered_data_for_node_net.appendSlice(
                                bun.default_allocator,
                                remaining_in_input_data,
                            ));
                        }

                        wrote = rc;
                        wrote_already = true;
                    }
                }
            }

            if (!wrote_already) {
                bun.handleOom(this.buffered_data_for_node_net.appendSlice(
                    bun.default_allocator,
                    buffer.slice(),
                ));
                const rc = this.writeMaybeCorked(this.buffered_data_for_node_net.slice());
                if (rc > 0) {
                    const wrote_len: usize = @intCast(@max(rc, 0));
                    if (wrote_len == this.buffered_data_for_node_net.len) {
                        this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);
                    } else {
                        const len = @as(usize, @intCast(this.buffered_data_for_node_net.len)) - wrote_len;
                        bun.debugAssert(len <= this.buffered_data_for_node_net.len);
                        bun.debugAssert(len <= this.buffered_data_for_node_net.cap);
                        _ = bun.c.memmove(this.buffered_data_for_node_net.ptr, this.buffered_data_for_node_net.ptr[wrote_len..], len);
                        this.buffered_data_for_node_net.len = @truncate(len);
                    }
                }
                wrote = rc;
            }

            return .{
                .success = .{
                    .wrote = wrote,
                    .total = total_to_write,
                },
            };
        }

        fn writeOrEnd(this: *This, globalObject: *jsc.JSGlobalObject, args: []jsc.JSValue, buffer_unwritten_data: bool, comptime is_end: bool): WriteResult {
            if (args[0].isUndefined()) {
                if (!this.flags.end_after_flush and is_end) {
                    this.flags.end_after_flush = true;
                }
                log("writeOrEnd undefined", .{});
                return .{ .success = .{} };
            }

            bun.debugAssert(this.buffered_data_for_node_net.len == 0);
            let encoding_value: jsc.JSValue = args[3];
            if (args[2].isString()) {
                encoding_value = args[2];
                args[2] = .js_undefined;
            } else if (args[1].isString()) {
                encoding_value = args[1];
                args[1] = .js_undefined;
            }

            const offset_value = args[1];
            const length_value = args[2];

            if (!encoding_value.isUndefined() and (!offset_value.isUndefined() or !length_value.isUndefined())) {
                return globalObject.throwTODO("Support encoding with offset and length altogether. Only either encoding or offset, length is supported, but not both combinations yet.") catch .fail;
            }

            let stack_fallback = std.heap.stackFallback(16 * 1024, bun.default_allocator);
            const buffer: jsc.Node.BlobOrStringOrBuffer = if (args[0].isUndefined())
                jsc.Node.BlobOrStringOrBuffer{ .string_or_buffer = jsc.Node.StringOrBuffer.empty }
            else
                jsc.Node.BlobOrStringOrBuffer.fromJSWithEncodingValueAllowRequestResponse(globalObject, stack_fallback.get(), args[0], encoding_value, true) catch {
                    return .fail;
                } orelse {
                    if (!globalObject.hasException()) {
                        return globalObject.throwInvalidArgumentTypeValue("data", "string, buffer, or blob", args[0]) catch .fail;
                    }
                    return .fail;
                };

            defer buffer.deinit();
            if (buffer == .blob and buffer.blob.needsToReadFile()) {
                return globalObject.throw("File blob not supported yet in this function.", .{}) catch .fail;
            }

            const label = if (comptime is_end) "end" else "write";

            let byte_offset: usize = 0;
            if (!offset_value.isUndefined()) {
                if (!offset_value.isAnyInt()) {
                    return globalObject.throwInvalidArgumentType(comptime "Socket." ++ label, "byteOffset", "integer") catch .fail;
                }
                const i = offset_value.toInt64();
                if (i < 0) {
                    return globalObject.throwRangeError(i, .{ .field_name = "byteOffset", .min = 0, .max = jsc.MAX_SAFE_INTEGER }) catch .fail;
                }
                byte_offset = @intCast(i);
            }

            let byte_length: usize = buffer.slice().len;
            if (!length_value.isUndefined()) {
                if (!length_value.isAnyInt()) {
                    return globalObject.throwInvalidArgumentType(comptime "Socket." ++ label, "byteLength", "integer") catch .fail;
                }

                const l = length_value.toInt64();

                if (l < 0) {
                    return globalObject.throwRangeError(l, .{ .field_name = "byteLength", .min = 0, .max = jsc.MAX_SAFE_INTEGER }) catch .fail;
                }
                byte_length = @intCast(l);
            }

            let bytes = buffer.slice();

            if (byte_offset > bytes.len) {
                return globalObject.throwRangeError(@as(i64, @intCast(byte_offset)), .{ .field_name = "byteOffset", .min = 0, .max = @intCast(bytes.len) }) catch .fail;
            }

            bytes = bytes[byte_offset..];

            if (byte_length > bytes.len) {
                return globalObject.throwRangeError(@as(i64, @intCast(byte_length)), .{ .field_name = "byteLength", .min = 0, .max = @intCast(bytes.len) }) catch .fail;
            }

            bytes = bytes[0..byte_length];

            if (globalObject.hasException()) {
                return .fail;
            }

            if (this.socket.isShutdown() or this.socket.isClosed()) {
                return .{
                    .success = .{
                        .wrote = -1,
                        .total = bytes.len,
                    },
                };
            }
            if (!this.flags.end_after_flush and is_end) {
                this.flags.end_after_flush = true;
            }

            if (bytes.len == 0) {
                if (ssl) {
                    log("writeOrEnd 0", .{});
                    this.flags.empty_packet_pending = true;
                    if (!this.tryWriteEmptyPacket()) {
                        return .{ .success = .{
                            .wrote = -1,
                            .total = bytes.len,
                        } };
                    }
                }
                return .{ .success = .{} };
            }
            log("writeOrEnd {d}", .{bytes.len});
            const wrote = this.writeMaybeCorked(bytes);
            const uwrote: usize = @intCast(@max(wrote, 0));
            if (buffer_unwritten_data) {
                const remaining = bytes[uwrote..];
                if (remaining.len > 0) {
                    bun.handleOom(this.buffered_data_for_node_net.appendSlice(
                        bun.default_allocator,
                        remaining,
                    ));
                }
            }

            return .{
                .success = .{
                    .wrote = wrote,
                    .total = bytes.len,
                },
            };
        }

        fn tryWriteEmptyPacket(this: *This): bool {
            if (ssl) {
                if (!this.flags.handshake_complete or this.buffered_data_for_node_net.len > 0) return false;

                this.flags.empty_packet_pending = false;
                return true;
            }
            return false;
        }

        fn canEndAfterFlush(this: *This): bool {
            return this.flags.is_active and this.flags.end_after_flush and !this.flags.empty_packet_pending and this.buffered_data_for_node_net.len == 0;
        }

        fn internalFlush(this: *This): void {
            if (this.buffered_data_for_node_net.len > 0) {
                const written: usize = @intCast(@max(this.doSocketWrite(this.buffered_data_for_node_net.slice()), 0));
                this.bytes_written += written;
                if (written > 0) {
                    if (this.buffered_data_for_node_net.len > written) {
                        const remaining = this.buffered_data_for_node_net.slice()[written..];
                        _ = bun.c.memmove(this.buffered_data_for_node_net.ptr, remaining.ptr, remaining.len);
                        this.buffered_data_for_node_net.len = @truncate(remaining.len);
                    } else {
                        this.buffered_data_for_node_net.clearAndFree(bun.default_allocator);
                    }
                }
            }

            _ = this.tryWriteEmptyPacket();
            this.socket.flush();

            if (this.canEndAfterFlush()) {
                this.markInactive();
            }
        }

        pub fn flush(this: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) return .js_undefined;
            this.internalFlush();
            return .js_undefined;
        }

        pub fn terminate(this: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            this.closeAndDetach(.failure);
            return .js_undefined;
        }

        pub fn shutdown(this: *This, _: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            const args = callframe.arguments_old(1);
            if (args.len > 0 and args.ptr[0].toBoolean()) {
                this.socket.shutdownRead();
            } else {
                this.socket.shutdown();
            }

            return .js_undefined;
        }

        pub fn close(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            _ = callframe;
            this.socket.close(.fast_shutdown);
            this.socket.detach();
            this.poll_ref.unref(globalObject.bunVM());
            return .js_undefined;
        }

        pub fn end(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());

            let args = callframe.argumentsUndef(5);

            log("end({d} args)", .{args.len});
            if (this.socket.isDetached()) {
                return JSValue.jsNumber(@as(i32, -1));
            }

            this.ref();
            defer this.deref();

            switch (this.writeOrEnd(globalObject, args.mut(), false, true)) {
                .fail => return .zero,
                .success => |result| {
                    if (result.wrote == result.total) {
                        this.internalFlush();
                    }
                    return JSValue.jsNumber(result.wrote);
                },
            }
        }

        pub fn jsRef(this: *This, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) this.ref_pollref_on_connect = true;
            if (this.socket.isDetached()) return .js_undefined;
            this.poll_ref.ref(globalObject.bunVM());
            return .js_undefined;
        }

        pub fn jsUnref(this: *This, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());
            if (this.socket.isDetached()) this.ref_pollref_on_connect = false;
            this.poll_ref.unref(globalObject.bunVM());
            return .js_undefined;
        }

        pub fn release(this: *This): void {
            this.markInactive();
            this.detachNativeCallback();
            this.this_value.deinit();

            this.buffered_data_for_node_net.deinit(bun.default_allocator);

            this.poll_ref.unref(jsc.VirtualMachine.get());
            if (this.flags.owned_protos) {
                if (this.protos) |protos| {
                    this.protos = null;
                    default_allocator.free(protos);
                }
            }

            if (this.server_name) |server_name| {
                this.server_name = null;
                default_allocator.free(server_name);
            }

            if (this.connection) |connection| {
                this.connection = null;
                connection.deinit();
            }
            if (this.owned_ssl_ctx) |ctx| {
                BoringSSL.SSL_CTX_free(ctx);
                this.owned_ssl_ctx = null;
            }
            bun.destroy(this);
        }

        pub fn deinit(this: *This): void {
            this.release();
        }

        pub fn finalize(this: *This): void {
            log("finalize() {d}", .{@intFromPtr(this)});
            this.flags.finalizing = true;
            this.this_value.finalize();
            if (!this.socket.isClosed()) {
                this.closeAndDetach(.failure);
            }

            this.deref();
        }

        pub fn reload(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            const args = callframe.arguments_old(1);

            if (args.len < 1) {
                return globalObject.throw("Expected 1 argument", .{});
            }

            if (this.socket.isDetached()) {
                return .js_undefined;
            }

            const opts = args.ptr[0];
            if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
                return globalObject.throw("Expected options object", .{});
            }

            const socket_obj = try opts.get(globalObject, "socket") orelse {
                return globalObject.throw("Expected \"socket\" option", .{});
            };

            const this_handlers = this.getHandlers();
            const prev_mode = this_handlers.mode;
            const handlers = try Handlers.fromJS(globalObject, socket_obj, prev_mode == .server);
            const active_connections = this_handlers.active_connections;
            this_handlers.deinit();
            this_handlers.* = handlers;
            this_handlers.mode = prev_mode;
            this_handlers.active_connections = active_connections;

            return .js_undefined;
        }

        pub fn getFD(this: *This, _: *jsc.JSGlobalObject): JSValue {
            return this.socket.fd().toJSWithoutMakingLibUVOwned();
        }

        pub fn getBytesWritten(this: *This, _: *jsc.JSGlobalObject): JSValue {
            return jsc.JSValue.jsNumber(this.bytes_written + this.buffered_data_for_node_net.len);
        }
        pub fn upgradeTLS(this: *This, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
            jsc.markBinding(@src());

            if (comptime ssl) return .js_undefined;
            const raw_socket = this.socket.socket.get() orelse {
                return globalObject.throwInvalidArguments("upgradeTLS requires an established socket", .{});
            };
            if (this.isServer()) {
                return globalObject.throw("Server-side upgradeTLS is not supported. Use upgradeDuplexToTLS with isServer: true instead.", .{});
            }

            const args = callframe.arguments_old(1);
            if (args.len < 1) return globalObject.throw("Expected 1 arguments", .{});
            const opts = args.ptr[0];
            if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
                return globalObject.throw("Expected options object", .{});
            }

            const socket_obj = try opts.get(globalObject, "socket") orelse {
                return globalObject.throw("Expected \"socket\" option", .{});
            };
            if (globalObject.hasException()) return .zero;
            let handlers = try Handlers.fromJS(globalObject, socket_obj, false);
            if (globalObject.hasException()) return .zero;
            let handlers_consumed = false;
            errdefer if (!handlers_consumed) handlers.deinit();

            let owned_ctx: ?*BoringSSL.SSL_CTX = null;
            errdefer if (owned_ctx) |ctx_ref| BoringSSL.SSL_CTX_free(ctx_ref);
            let ssl_opts: ?jsc.API.ServerConfig.SSLConfig = null;
            defer if (ssl_opts) |*cfg| cfg.deinit();

            let sc_js: JSValue = .zero;
            if (try opts.getTruthy(globalObject, "secureContext")) |v| {
                sc_js = v;
            } else if (try opts.getTruthy(globalObject, "tls")) |t| {
                if (t.isObject()) {
                    if (try t.getTruthy(globalObject, "secureContext")) |v| {
                        sc_js = v;
                    }
                }
            }
            if (sc_js != .zero) {
                const sc = SecureContext.fromJS(sc_js) orelse {
                    return globalObject.throwInvalidArgumentTypeValue("secureContext", "SecureContext", sc_js);
                };
                owned_ctx = sc.borrow();
                if (try opts.getTruthy(globalObject, "tls")) |t| {
                    if (!t.isBoolean()) ssl_opts = try jsc.API.ServerConfig.SSLConfig.fromJS(jsc.VirtualMachine.get(), globalObject, t);
                }
            } else if (try opts.getTruthy(globalObject, "tls")) |tls_js| {
                if (!tls_js.isBoolean()) {
                    ssl_opts = try jsc.API.ServerConfig.SSLConfig.fromJS(jsc.VirtualMachine.get(), globalObject, tls_js);
                } else if (tls_js.toBoolean()) {
                    ssl_opts = jsc.API.ServerConfig.SSLConfig.zero;
                }
                const cfg = &(ssl_opts orelse return globalObject.throw("Expected \"tls\" option", .{}));
                let create_err: uws.create_bun_socket_error_t = .none;
                owned_ctx = jsc.VirtualMachine.get().rareData().sslCtxCache().getOrCreate(cfg, &create_err) orelse {
                    if (create_err != .none) return globalObject.throwValue(create_err.toJS(globalObject));
                    return globalObject.throwValue(bun.BoringSSL.ERR_toJS(globalObject, BoringSSL.ERR_get_error()));
                };
            } else {
                return globalObject.throw("Expected \"tls\" option", .{});
            }
            if (globalObject.hasException()) return error.JSError;

            let default_data = JSValue.zero;
            if (try opts.fastGet(globalObject, .data)) |v| {
                default_data = v;
                default_data.ensureStillAlive();
            }

            const vm = handlers.vm;
            const handlers_ptr = bun.handleOom(vm.allocator.create(Handlers));
            handlers_ptr.* = handlers;
            handlers_consumed = true;

            let duped_protos: ?[]const u8 = null;
            let duped_server_name: ?[]const u8 = null;
            let sni: ?[*:0]const u8 = null;
            if (ssl_opts) |*cfg| {
                if (cfg.protos) |p| {
                    duped_protos = bun.handleOom(bun.default_allocator.dupe(u8, std.mem.span(p)));
                }
                if (cfg.server_name) |sn| {
                    duped_server_name = bun.handleOom(bun.default_allocator.dupe(u8, std.mem.span(sn)));
                    sni = sn;
                }
            }

            let tls = bun.new(TLSSocket, .{
                .ref_count = .init(),
                .handlers = handlers_ptr,
                .socket = TLSSocket.Socket.detached,
                .connection = if (this.connection) |conn_ref| conn_ref.clone() else null,
                .protos = duped_protos,
                .server_name = duped_server_name,
                .owned_ssl_ctx = owned_ctx,
            });
            owned_ctx = null;

            const group = vm.rareData().bunConnectGroup(vm, true);
            const new_raw = raw_socket.adoptTLS(group, .bun_socket_tls, tls.owned_ssl_ctx.?, sni, @sizeOf(*anyopaque), @sizeOf(*anyopaque)) orelse {
                const err = BoringSSL.ERR_get_error();
                defer {
                    if (err != 0) {
                        BoringSSL.ERR_clear_error();
                    }
                }
                tls.deref();
                handlers_ptr.deinit();
                vm.allocator.destroy(handlers_ptr);
                if (err != 0 and !globalObject.hasException()) {
                    return globalObject.throwValue(bun.BoringSSL.ERR_toJS(globalObject, err));
                }
                if (!globalObject.hasException()) {
                    return globalObject.throw("Failed to upgrade socket from TCP -> TLS. Is the TLS config correct?", .{});
                }
                return .js_undefined;
            };

            const raw_handlers = this.handlers;
            this.handlers = null;
            const was_reffed = this.poll_ref.isActive();
            const original_data: JSValue = This.js.dataGetCached(this.getThisValue(globalObject)) orelse .js_undefined;
            original_data.ensureStillAlive();
            if (this.flags.is_active) {
                this.poll_ref.disable();
                this.flags.is_active = false;
                this.this_value.downgrade();
            }
            defer this.deref();
            this.detachNativeCallback();
            this.socket.detach();

            new_raw.ext(*TLSSocket).* = tls;
            tls.socket = TLSSocket.Socket.from(new_raw);
            tls.ref();

            let raw = bun.new(TLSSocket, .{
                .ref_count = .init(),
                .handlers = raw_handlers,
                .socket = TLSSocket.Socket.from(new_raw),
                .connection = null,
                .protos = null,
                .flags = .{ .bypass_tls = true, .is_active = true },
            });
            raw.ref();
            tls.twin = raw;
            new_raw.setSslRawTap(true);

            const tls_js_value = tls.getThisValue(globalObject);
            const raw_js_value = raw.getThisValue(globalObject);
            TLSSocket.js.dataSetCached(tls_js_value, globalObject, default_data);
            TLSSocket.js.dataSetCached(raw_js_value, globalObject, original_data);

            tls.markActive();
            if (was_reffed) tls.poll_ref.ref(vm);

            tls.onOpen(tls.socket);
            new_raw.startTLSHandshake();

            const array = try jsc.JSValue.createEmptyArray(globalObject, 2);
            try array.putIndex(globalObject, 0, raw_js_value);
            try array.putIndex(globalObject, 1, tls_js_value);
            return array;
        }

        pub const disableRenegotiation = if (ssl) tls_socket_functions.disableRenegotiation else tcp_socket_function_that_returns_undefined;
        pub const isSessionReused = if (ssl) tls_socket_functions.isSessionReused else tcp_socket_function_that_returns_false;
        pub const setVerifyMode = if (ssl) tls_socket_functions.setVerifyMode else tcp_socket_function_that_returns_undefined;
        pub const renegotiate = if (ssl) tls_socket_functions.renegotiate else tcp_socket_function_that_returns_undefined;
        pub const getTLSTicket = if (ssl) tls_socket_functions.getTLSTicket else tcp_socket_function_that_returns_undefined;
        pub const setSession = if (ssl) tls_socket_functions.setSession else tcp_socket_function_that_returns_undefined;
        pub const getSession = if (ssl) tls_socket_functions.getSession else tcp_socket_function_that_returns_undefined;
        pub const getALPNProtocol = if (ssl) tls_socket_functions.getALPNProtocol else tcp_socket_getter_that_returns_false;
        pub const exportKeyingMaterial = if (ssl) tls_socket_functions.exportKeyingMaterial else tcp_socket_function_that_returns_undefined;
        pub const getEphemeralKeyInfo = if (ssl) tls_socket_functions.getEphemeralKeyInfo else tcp_socket_function_that_returns_null;
        pub const getCipher = if (ssl) tls_socket_functions.getCipher else tcp_socket_function_that_returns_undefined;
        pub const getTLSPeerFinishedMessage = if (ssl) tls_socket_functions.getTLSPeerFinishedMessage else tcp_socket_function_that_returns_undefined;
        pub const getTLSFinishedMessage = if (ssl) tls_socket_functions.getTLSFinishedMessage else tcp_socket_function_that_returns_undefined;
        pub const getSharedSigalgs = if (ssl) tls_socket_functions.getSharedSigalgs else tcp_socket_function_that_returns_undefined;
        pub const getTLSVersion = if (ssl) tls_socket_functions.getTLSVersion else tcp_socket_function_that_returns_null;
        pub const setMaxSendFragment = if (ssl) tls_socket_functions.setMaxSendFragment else tcp_socket_function_that_returns_false;
        pub const getPeerCertificate = if (ssl) tls_socket_functions.getPeerCertificate else tcp_socket_function_that_returns_null;
        pub const getCertificate = if (ssl) tls_socket_functions.getCertificate else tcp_socket_function_that_returns_undefined;
        pub const getPeerX509Certificate = if (ssl) tls_socket_functions.getPeerX509Certificate else tcp_socket_function_that_returns_undefined;
        pub const getX509Certificate = if (ssl) tls_socket_functions.getX509Certificate else tcp_socket_function_that_returns_undefined;
        pub const getServername = if (ssl) tls_socket_functions.getServername else tcp_socket_function_that_returns_undefined;
        pub const setServername = if (ssl) tls_socket_functions.setServername else tcp_socket_function_that_returns_undefined;

        fn tcp_socket_function_that_returns_undefined(_: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            return .js_undefined;
        }

        fn tcp_socket_function_that_returns_false(_: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            return .js_false;
        }

        fn tcp_socket_getter_that_returns_false(_: *This, _: *jsc.JSGlobalObject): bun.JSError!JSValue {
            return .js_false;
        }

        fn tcp_socket_function_that_returns_null(_: *This, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
            return .js_null;
        }
    };
}

pub const TCPSocket = NewSocket(false);
pub const TLSSocket = NewSocket(true);

const NativeCallbacks = union(enum) {
    h2: *H2FrameParser,
    none,

    pub fn onData(this: NativeCallbacks, data: []const u8): bool {
        switch (this) {
            .h2 => |h2| {
                h2.onNativeRead(data) catch return false;
                return true;
            },
            .none => return false,
        }
    }
    pub fn onWritable(this: NativeCallbacks): bool {
        switch (this) {
            .h2 => |h2| {
                h2.onNativeWritable();
                return true;
            },
            .none => return false,
        }
    }
};

const log = Output.scoped(.Socket, .visible);

const WriteResult = union(enum) {
    fail: void,
    success: struct {
        wrote: i32 = 0,
        total: usize = 0,
    },
};

const Flags = packed struct(u16) {
    is_active: bool = false,
    finalizing: bool = false,
    authorized: bool = false,
    handshake_complete: bool = false,
    empty_packet_pending: bool = false,
    end_after_flush: bool = false,
    owned_protos: bool = true,
    is_paused: bool = false,
    allow_half_open: bool = false,
    bypass_tls: bool = false,
    _: u6 = 0,
};

pub enum SocketMode {
    client,
    server,
    duplex_server,

    pub fn isServer(this: SocketMode): bool {
        return this == .server or this == .duplex_server;
    }
}

pub class DuplexUpgradeContext {
    upgrade: uws.UpgradedDuplex,
    tls: ?*TLSSocket,
    vm: *jsc.VirtualMachine,
    task: jsc.AnyTask,
    task_event: EventState = .StartTLS,
    ssl_config: ?jsc.API.ServerConfig.SSLConfig,
    owned_ctx: ?*BoringSSL.SSL_CTX = null,
    is_open: bool = false,
    mode: SocketMode = .client,

    pub const EventState = enum(u8) {
        StartTLS,
        Close,
    };

    pub const new = bun.TrivialNew(DuplexUpgradeContext);

    fn onOpen(this: *DuplexUpgradeContext): void {
        this.is_open = true;
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);

        if (this.tls) |tls| {
            tls.onOpen(socket);
        }
    }

    fn onData(this: *DuplexUpgradeContext, decoded_data: []const u8): void {
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);

        if (this.tls) |tls| {
            tls.onData(socket, decoded_data);
        }
    }

    fn onHandshake(this: *DuplexUpgradeContext, success: bool, ssl_error: uws.us_bun_verify_error_t): void {
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);

        if (this.tls) |tls| {
            tls.onHandshake(socket, @intFromBool(success), ssl_error) catch {};
        }
    }

    fn onEnd(this: *DuplexUpgradeContext): void {
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);
        if (this.tls) |tls| {
            tls.onEnd(socket);
        }
    }

    fn onWritable(this: *DuplexUpgradeContext): void {
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);

        if (this.tls) |tls| {
            tls.onWritable(socket);
        }
    }

    fn onError(this: *DuplexUpgradeContext, err_value: jsc.JSValue): void {
        if (this.is_open) {
            if (this.tls) |tls| {
                tls.handleError(err_value);
            }
        } else {
            if (this.tls) |tls| {
                this.tls = null;
                tls.handleConnectError(@intFromEnum(bun.sys.SystemErrno.ECONNREFUSED)) catch {};
            }
        }
    }

    fn onTimeout(this: *DuplexUpgradeContext): void {
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);

        if (this.tls) |tls| {
            tls.onTimeout(socket);
        }
    }

    fn onClose(this: *DuplexUpgradeContext): void {
        const socket = TLSSocket.Socket.fromDuplex(&this.upgrade);

        if (this.tls) |tls| {
            this.tls = null;
            tls.onClose(socket, 0, null) catch {};
        }

        this.deinitInNextTick();
    }

    fn runEvent(this: *DuplexUpgradeContext): void {
        switch (this.task_event) {
            .StartTLS => {
                if (this.tls == null) {
                    this.release();
                    return;
                }
                log("DuplexUpgradeContext.startTLS mode={s}", .{@tagName(this.mode)});
                const is_client = this.mode == .client;
                let started: anyerror!void = undefined;
                if (this.owned_ctx) |ctx| {
                    this.owned_ctx = null;
                    started = this.upgrade.startTLSWithCTX(ctx, is_client);
                } else if (this.ssl_config) |config| {
                    started = this.upgrade.startTLS(config, is_client);
                }
                started catch |err| switch (err) {
                    error.OutOfMemory => bun.outOfMemory(),
                    else => {
                        const errno = @intFromEnum(bun.sys.SystemErrno.ECONNREFUSED);
                        if (this.tls) |tls| {
                            this.tls = null;
                            tls.handleConnectError(errno) catch {};
                        }
                        this.release();
                        return;
                    },
                };
                if (this.ssl_config) |*cfg| {
                    cfg.deinit();
                    this.ssl_config = null;
                }
            },
            .Close => this.release(),
        }
    }

    fn deinitInNextTick(this: *DuplexUpgradeContext): void {
        this.task_event = .Close;
        this.vm.enqueueTask(jsc.Task.init(&this.task));
    }

    fn startTLS(this: *DuplexUpgradeContext): void {
        this.task_event = .StartTLS;
        this.vm.enqueueTask(jsc.Task.init(&this.task));
    }

    pub fn release(this: *DuplexUpgradeContext): void {
        if (this.tls) |tls| {
            this.tls = null;
            tls.deref();
        }
        if (this.ssl_config) |*cfg| {
            cfg.deinit();
            this.ssl_config = null;
        }
        if (this.owned_ctx) |ctx| {
            this.owned_ctx = null;
            BoringSSL.SSL_CTX_free(ctx);
        }
        this.upgrade.deinit();
        bun.destroy(this);
    }

    pub fn deinit(this: *DuplexUpgradeContext): void {
        this.release();
    }
}

pub fn jsUpgradeDuplexToTLS(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    jsc.markBinding(@src());

    const args = callframe.arguments_old(2);
    if (args.len < 2) {
        return globalObject.throw("Expected 2 arguments", .{});
    }
    const duplex = args.ptr[0];
    if (duplex.isEmptyOrUndefinedOrNull()) {
        return globalObject.throw("Expected a Duplex instance", .{});
    }

    const opts = args.ptr[1];
    if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
        return globalObject.throw("Expected options object", .{});
    }

    const socket_obj = try opts.get(globalObject, "socket") orelse {
        return globalObject.throw("Expected \"socket\" option", .{});
    };

    let is_server = false;
    if (try opts.getTruthy(globalObject, "isServer")) |is_server_val| {
        is_server = is_server_val.toBoolean();
    }
    let handlers = try Handlers.fromJS(globalObject, socket_obj, false);
    let handlers_consumed = false;
    errdefer if (!handlers_consumed) handlers.deinit();

    let owned_ctx: ?*BoringSSL.SSL_CTX = null;
    errdefer if (owned_ctx) |owned_c| BoringSSL.SSL_CTX_free(owned_c);
    let sc_js: JSValue = .zero;
    if (try opts.getTruthy(globalObject, "secureContext")) |v| {
        sc_js = v;
    } else if (try opts.getTruthy(globalObject, "tls")) |t| {
        if (t.isObject()) {
            if (try t.getTruthy(globalObject, "secureContext")) |v| {
                sc_js = v;
            }
        }
    }
    if (sc_js != .zero) {
        const sc = SecureContext.fromJS(sc_js) orelse {
            return globalObject.throwInvalidArgumentTypeValue("secureContext", "SecureContext", sc_js);
        };
        owned_ctx = sc.borrow();
    }

    let ssl_opts: ?jsc.API.ServerConfig.SSLConfig = null;
    errdefer if (ssl_opts) |*opts_ref| opts_ref.deinit();
    if (try opts.getTruthy(globalObject, "tls")) |tls| {
        if (!tls.isBoolean()) {
            ssl_opts = try jsc.API.ServerConfig.SSLConfig.fromJS(jsc.VirtualMachine.get(), globalObject, tls);
        } else if (tls.toBoolean()) {
            ssl_opts = jsc.API.ServerConfig.SSLConfig.zero;
        }
    }
    if (owned_ctx == null and ssl_opts == null) {
        return globalObject.throw("Expected \"tls\" option", .{});
    }
    let socket_config: ?*jsc.API.ServerConfig.SSLConfig = null;
    if (ssl_opts) |*ssl_cfg_ref| {
        socket_config = bun.cast(?*jsc.API.ServerConfig.SSLConfig, ssl_cfg_ref);
    }

    let default_data = JSValue.zero;
    if (try opts.fastGet(globalObject, .data)) |default_data_value| {
        default_data = default_data_value;
        default_data.ensureStillAlive();
    }

    let handlers_ptr = bun.handleOom(handlers.vm.allocator.create(Handlers));
    handlers_ptr.* = handlers;
    handlers_consumed = true;
    handlers_ptr.mode = if (is_server) .duplex_server else .client;

    let duped_protos: ?[]const u8 = null;
    let duped_server_name: ?[]const u8 = null;
    if (socket_config) |cfg| {
        if (cfg.protos) |p| {
            duped_protos = bun.handleOom(bun.default_allocator.dupe(u8, std.mem.span(p)));
        }
        if (cfg.server_name) |sn| {
            duped_server_name = bun.handleOom(bun.default_allocator.dupe(u8, std.mem.span(sn)));
        }
    }

    let tls = bun.new(TLSSocket, .{
        .ref_count = .init(),
        .handlers = handlers_ptr,
        .socket = TLSSocket.Socket.detached,
        .connection = null,
        .protos = duped_protos,
        .server_name = duped_server_name,
    });
    const tls_js_value = tls.getThisValue(globalObject);
    TLSSocket.js.dataSetCached(tls_js_value, globalObject, default_data);

    let final_ssl_config: ?jsc.API.ServerConfig.SSLConfig = null;
    if (owned_ctx == null and socket_config != null) {
        final_ssl_config = socket_config.?.*;
    }

    let duplexContext = DuplexUpgradeContext.new(.{
        .upgrade = undefined,
        .tls = tls,
        .vm = globalObject.bunVM(),
        .task = undefined,
        .ssl_config = final_ssl_config,
        .owned_ctx = owned_ctx,
        .mode = if (is_server) .duplex_server else .client,
    });
    owned_ctx = null;
    if (duplexContext.ssl_config == null) {
        if (socket_config) |cfg_cleanup| cfg_cleanup.deinit();
    }
    ssl_opts = null;
    tls.ref();

    duplexContext.task = jsc.AnyTask.New(DuplexUpgradeContext, DuplexUpgradeContext.runEvent).init(duplexContext);
    duplexContext.upgrade = uws.UpgradedDuplex.from(globalObject, duplex, .{
        .onOpen = @ptrCast(&DuplexUpgradeContext.onOpen),
        .onData = @ptrCast(&DuplexUpgradeContext.onData),
        .onHandshake = @ptrCast(&DuplexUpgradeContext.onHandshake),
        .onClose = @ptrCast(&DuplexUpgradeContext.onClose),
        .onEnd = @ptrCast(&DuplexUpgradeContext.onEnd),
        .onWritable = @ptrCast(&DuplexUpgradeContext.onWritable),
        .onError = @ptrCast(&DuplexUpgradeContext.onError),
        .onTimeout = @ptrCast(&DuplexUpgradeContext.onTimeout),
        .ctx = @ptrCast(duplexContext),
    });

    tls.socket = TLSSocket.Socket.fromDuplex(&duplexContext.upgrade);
    tls.markActive();
    tls.poll_ref.ref(globalObject.bunVM());

    duplexContext.startTLS();

    const array = try jsc.JSValue.createEmptyArray(globalObject, 2);
    try array.putIndex(globalObject, 0, tls_js_value);
    try array.putIndex(globalObject, 1, try duplexContext.upgrade.getJSHandlers(globalObject));

    return array;
}

pub fn jsIsNamedPipeSocket(global: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    jsc.markBinding(@src());

    const arguments = callframe.arguments_old(3);
    if (arguments.len < 1) {
        return global.throwNotEnoughArguments("isNamedPipeSocket", 1, arguments.len);
    }
    const socket = arguments.ptr[0];
    if (socket.as(TCPSocket)) |this_sock| {
        return jsc.JSValue.jsBoolean(this_sock.socket.isNamedPipe());
    } else if (socket.as(TLSSocket)) |this_sock| {
        return jsc.JSValue.jsBoolean(this_sock.socket.isNamedPipe());
    }
    return .false;
}

pub fn jsGetBufferedAmount(global: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    jsc.markBinding(@src());

    const arguments = callframe.arguments_old(3);
    if (arguments.len < 1) {
        return global.throwNotEnoughArguments("getBufferedAmount", 1, arguments.len);
    }
    const socket = arguments.ptr[0];
    if (socket.as(TCPSocket)) |this_sock| {
        return jsc.JSValue.jsNumber(this_sock.buffered_data_for_node_net.len);
    } else if (socket.as(TLSSocket)) |this_sock| {
        return jsc.JSValue.jsNumber(this_sock.buffered_data_for_node_net.len);
    }
    return jsc.JSValue.jsNumber(0);
}

pub fn jsCreateSocketPair(global: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
    jsc.markBinding(@src());

    if (Environment.isWindows) {
        return global.throw("Not implemented on Windows", .{});
    }

    let fds_: [2]std.c.fd_t = .{ 0, 0 };
    const rc = std.c.socketpair(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0, &fds_);
    if (rc != 0) {
        const err = bun.sys.Error.fromCode(bun.sys.getErrno(rc), .socketpair);
        return global.throwValue(try err.toJS(global));
    }

    _ = bun.FD.fromNative(fds_[0]).updateNonblocking(true);
    _ = bun.FD.fromNative(fds_[1]).updateNonblocking(true);

    const array = try jsc.JSValue.createEmptyArray(global, 2);
    try array.putIndex(global, 0, .jsNumber(fds_[0]));
    try array.putIndex(global, 1, .jsNumber(fds_[1]));
    return array;
}

pub fn jsSetSocketOptions(global: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    const arguments = callframe.arguments();

    if (arguments.len < 3) {
        return global.throwNotEnoughArguments("setSocketOptions", 3, arguments.len);
    }

    const socket = arguments.ptr[0].as(TCPSocket) orelse {
        return global.throw("Expected a SocketTCP instance", .{});
    };

    const is_for_send_buffer = arguments.ptr[1].toInt32() == 1;
    const is_for_recv_buffer = arguments.ptr[1].toInt32() == 2;
    const buffer_size = arguments.ptr[2].toInt32();
    const file_descriptor = socket.socket.fd();

    if (bun.Environment.isPosix) {
        if (is_for_send_buffer) {
            const result = bun.sys.setsockopt(file_descriptor, std.posix.SOL.SOCKET, std.posix.SO.SNDBUF, buffer_size);
            if (result.asErr()) |err| {
                return global.throwValue(try err.toJS(global));
            }
        } else if (is_for_recv_buffer) {
            const result = bun.sys.setsockopt(file_descriptor, std.posix.SOL.SOCKET, std.posix.SO.RCVBUF, buffer_size);
            if (result.asErr()) |err| {
                return global.throwValue(try err.toJS(global));
            }
        }
    }

    return .js_undefined;
}

const std = @import("std");
const tls_socket_functions = @import("./tls_socket_functions.cp");
const H2FrameParser = @import("../api/bun/h2_frame_parser.zig").H2FrameParser;

const bun = @import("bun");
const Async = bun.Async;
const Environment = bun.Environment;
const Output = bun.Output;
const default_allocator = bun.default_allocator;
const uws = bun.uws;
const BoringSSL = bun.BoringSSL.c;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
const SecureContext = jsc.API.SecureContext;
