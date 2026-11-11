import std from "std";
import bun from "bun";

const Async = bun.Async;
const Environment = bun.Environment;
const Output = bun.Output;
const default_allocator = bun.default_allocator;
const strings = bun.strings;
const uws = bun.uws;
const BoringSSL = bun.BoringSSL.c;
const uv = bun.windows.libuv;

const api = bun.api;
const Handlers = bun.api.SocketHandlers;
const SecureContext = bun.api.SecureContext;
const TCPSocket = bun.api.TCPSocket;
const TLSSocket = bun.api.TLSSocket;
const SSLConfig = bun.api.ServerConfig.SSLConfig;

const NewSocket = api.socket.NewSocket;
const SocketConfig = api.socket.SocketConfig;
const WindowsNamedPipeContext = api.socket.WindowsNamedPipeContext;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
const NodePath = jsc.Node.path;

export const log = Output.scoped(.Listener, .visible);

pub const ListenerType = union(enum) {
    uws: *uws.ListenSocket,
    namedPipe: *WindowsNamedPipeListeningContext,
    none: void,
};

export class Listener {
    handlers: Handlers,
    listener: ListenerType = .none,

    poll_ref: Async.KeepAlive = Async.KeepAlive.init(),
    connection: UnixOrHost,
    group: uws.SocketGroup = .{},
    secure_ctx: ?*BoringSSL.SSL_CTX = null,
    ssl: bool = false,
    protos: ?[]const u8 = null,

    strong_data: jsc.Strong.Optional = .empty,
    strong_self: jsc.Strong.Optional = .empty,

    pub const js = jsc.Codegen.JSListener;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;


    pub fn getData(*this, _: *jsc.JSGlobalObject): JSValue {
        log("getData()", .{});
        return this.strong_data.get() orelse .js_undefined;
    }

    pub fn setData(*this, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue): void {
        log("setData()", .{});
        this.strong_data.set(globalObject, value);
    }

    pub fn reload(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const args = callframe.arguments_old(1);

        if (args.len < 1 or (this.listener == .none and this.handlers.active_connections == 0)) {
            return globalObject.throw("Expected 1 argument", .{});
        }

        const opts = args.ptr[0];
        if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
            return globalObject.throwValue(globalObject.toInvalidArguments("Expected options object", .{}));
        }

        const socket_obj = try opts.get(globalObject, "socket") orelse {
            return globalObject.throw("Expected \"socket\" object", .{});
        };

        const handlers = try Handlers.fromJS(globalObject, socket_obj, this.handlers.mode == .server);
        const active_connections = this.handlers.active_connections;
        this.handlers.deinit();
        this.handlers = handlers;
        this.handlers.active_connections = active_connections;

        return .js_undefined;
    }

    pub fn addServerName(*this, global: *jsc.JSGlobalObject, hostname: JSValue, tls: JSValue): bun.JSError!JSValue {
        if (!this.ssl) {
            return global.throwInvalidArguments("addServerName requires SSL support", .{});
        }
        if (!hostname.isString()) {
            return global.throwInvalidArguments("hostname pattern expects a string", .{});
        }
        const host_str = try hostname.toSlice(global, bun.default_allocator);
        defer host_str.deinit();
        const server_name = bun.handleOom(bun.default_allocator.dupeZ(u8, host_str.slice()));
        defer bun.default_allocator.free(server_name);
        if (server_name.len == 0) {
            return global.throwInvalidArguments("hostname pattern cannot be empty", .{});
        }

        if (this.listener != .uws) return .js_undefined;
        const ls = this.listener.uws;

        let sni_ctx: ?*BoringSSL.SSL_CTX = null;
        if (SecureContext.fromJS(tls)) |sc| {
            sni_ctx = sc.borrow();
        } else if (try SSLConfig.fromJS(jsc.VirtualMachine.get(), global, tls)) |ssl_config| {
            let cfg = ssl_config;
            defer cfg.deinit();
            let create_err: uws.create_bun_socket_error_t = .none;
            const cached_ctx = jsc.VirtualMachine.get().rareData().sslCtxCache().getOrCreate(&cfg, &create_err);
            if (cached_ctx == null) {
                if (create_err != .none) return global.throwValue(create_err.toJS(global));
                return global.throwValue(bun.BoringSSL.ERR_toJS(global, BoringSSL.ERR_get_error()));
            }
            sni_ctx = cached_ctx;
        } else {
            return .js_undefined;
        }
        const final_sni_ctx = sni_ctx orelse return .js_undefined;

        ls.removeServerName(server_name);
        const ok = ls.addServerName(server_name, final_sni_ctx, null);
        BoringSSL.SSL_CTX_free(final_sni_ctx);
        if (!ok) {
            return global.throwValue(global.ERR(.BORINGSSL, "Failed to register SNI for '{s}'", .{server_name}).toJS());
        }

        return .js_undefined;
    }

    pub fn dispose(*this, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        this.doStop(true);
        return .js_undefined;
    }

    pub fn stop(*this, _: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const arguments = callframe.arguments_old(1);
        log("close", .{});

        this.doStop(if (arguments.len > 0 and arguments.ptr[0].isBoolean()) arguments.ptr[0].toBoolean() else false);

        return .js_undefined;
    }

    fn doStop(*this, force_close: bool): void {
        if (this.listener == .none) return;
        const listener = this.listener;

        if (listener == .uws) this.unlinkUnixSocketPath();

        defer switch (listener) {
            .uws => |socket| socket.close(),
            .namedPipe => |namedPipe| if (Environment.isWindows) namedPipe.closePipeAndDeinit(),
            .none => {},
        };
        this.listener = .none;

        if (this.handlers.active_connections == 0) {
            this.poll_ref.unref(this.handlers.vm);
            this.strong_self.clearWithoutDeallocation();
            this.strong_data.clearWithoutDeallocation();
        } else if (force_close) {
            this.group.closeAll();
        }
    }

    pub fn finalize(*this): void {
        log("finalize", .{});
        const listener = this.listener;
        this.listener = .none;
        switch (listener) {
            .uws => |socket| {
                this.unlinkUnixSocketPath();
                socket.close();
            },
            .namedPipe => |namedPipe| if (Environment.isWindows) namedPipe.closePipeAndDeinit(),
            .none => {},
        }
        this.release();
    }

    fn unlinkUnixSocketPath(this: *const Listener): void {
        if (this.connection != .unix) return;
        const path = this.connection.unix;
        if (path.len == 0 or path[0] == 0) return;
        const buf = bun.path_buffer_pool.get();
        defer bun.path_buffer_pool.put(buf);
        _ = bun.sys.unlink(bun.path.z(path, buf));
    }

    pub fn release(*this): void {
        log("deinit", .{});
        this.strong_self.deinit();
        this.strong_data.deinit();
        const vm = this.handlers.vm;
        this.poll_ref.unref(vm);
        bun.assert(this.listener == .none);

        if (this.handlers.active_connections > 0) {
            this.group.closeAll();
        }
        bun.asan.unregisterRootRegion(&this.group, @sizeOf(uws.SocketGroup));
        this.group.deinit();
        if (this.secure_ctx) |ctx| BoringSSL.SSL_CTX_free(ctx);

        this.connection.deinit();
        if (this.protos) |protos| {
            this.protos = null;
            bun.default_allocator.free(protos);
        }
        this.handlers.deinit();
        vm.allocator.destroy(this);
    }

    pub fn deinit(*this): void {
        this.release();
    }

    pub fn getConnectionsCount(*this, _: *jsc.JSGlobalObject): JSValue {
        return JSValue.jsNumber(this.handlers.active_connections);
    }

    pub fn getUnix(*this, globalObject: *jsc.JSGlobalObject): JSValue {
        if (this.connection != .unix) {
            return .js_undefined;
        }

        return ZigString.init(this.connection.unix).withEncoding().toJS(globalObject);
    }

    pub fn getHostname(*this, globalObject: *jsc.JSGlobalObject): JSValue {
        if (this.connection != .host) {
            return .js_undefined;
        }
        return ZigString.init(this.connection.host.host).withEncoding().toJS(globalObject);
    }

    pub fn getPort(*this, _: *jsc.JSGlobalObject): JSValue {
        if (this.connection != .host) {
            return .js_undefined;
        }
        return JSValue.jsNumber(this.connection.host.port);
    }

    pub fn getFD(*this, _: *jsc.JSGlobalObject): JSValue {
        switch (this.listener) {
            .uws => |uws_listener| {
                return uws_listener.socket(false).fd().toJSWithoutMakingLibUVOwned();
            },
            else => return JSValue.jsNumber(-1),
        }
    }

    pub fn ref(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const this_value = callframe.this();
        if (this.listener == .none) return .js_undefined;
        this.poll_ref.ref(globalObject.bunVM());
        this.strong_self.set(globalObject, this_value);
        return .js_undefined;
    }

    pub fn unref(*this, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        this.poll_ref.unref(globalObject.bunVM());
        if (this.handlers.active_connections == 0) {
            this.strong_self.clearWithoutDeallocation();
        }
        return .js_undefined;
    }

    pub fn getsockname(*this, globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        if (this.listener != .uws) {
            return .js_undefined;
        }

        const out = callFrame.argumentsAsArray(1)[0];
        if (!out.isObject()) {
            return globalThis.throwInvalidArguments("Expected object", .{});
        }
        const socket = this.listener.uws;

        let buf: [64]u8 = undefined;
        let text_buf: [512]u8 = undefined;
        const address_bytes: []const u8 = socket.getLocalAddress(&buf) catch return .js_undefined;
        const address_zig: std.net.Address = switch (address_bytes.len) {
            4 => std.net.Address.initIp4(address_bytes[0..4].*, 0),
            16 => std.net.Address.initIp6(address_bytes[0..16].*, 0, 0, 0),
            else => return .js_undefined,
        };
        const family_js = switch (address_bytes.len) {
            4 => globalThis.commonStrings().IPv4(),
            16 => globalThis.commonStrings().IPv6(),
            else => return .js_undefined,
        };
        const address_js = ZigString.init(bun.fmt.formatIp(address_zig, &text_buf) catch unreachable).toJS(globalThis);
        const port_js: JSValue = .jsNumber(socket.getLocalPort());

        out.put(globalThis, bun.String.static("family"), family_js);
        out.put(globalThis, bun.String.static("address"), address_js);
        out.put(globalThis, bun.String.static("port"), port_js);
        return .js_undefined;
    }
}

pub const UnixOrHost = union(enum) {
    unix: []const u8,
    host: struct {
        host: []const u8,
        port: u16,
    },
    fd: bun.FD,

    pub fn clone(this: UnixOrHost): UnixOrHost {
        switch (this) {
            .unix => |u| {
                return .{
                    .unix = bun.handleOom(bun.default_allocator.dupe(u8, u)),
                };
            },
            .host => |h| {
                return .{
                    .host = .{
                        .host = bun.handleOom(bun.default_allocator.dupe(u8, h.host)),
                        .port = h.port,
                    },
                };
            },
            .fd => |f| return .{ .fd = f },
        }
    }

    pub fn release(this: UnixOrHost): void {
        switch (this) {
            .unix => |u| {
                bun.default_allocator.free(u);
            },
            .host => |h| {
                bun.default_allocator.free(h.host);
            },
            .fd => {},
        }
    }

    pub fn deinit(this: UnixOrHost): void {
        this.release();
    }
};

pub fn listen(globalObject: *jsc.JSGlobalObject, opts: JSValue): bun.JSError!JSValue {
    log("listen", .{});
    if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
        return globalObject.throwInvalidArguments("Expected object", .{});
    }

    const vm = jsc.VirtualMachine.get();

    let socket_config = try SocketConfig.fromJS(vm, opts, globalObject, true);
    defer socket_config.deinitExcludingHandlers();

    const handlers = &socket_config.handlers;
    errdefer handlers.deinit();

    const hostname_or_unix = &socket_config.hostname_or_unix;
    const port = socket_config.port;
    const ssl = if (socket_config.ssl != null) &socket_config.ssl.? else null;
    const ssl_enabled = ssl != null;
    const socket_flags = socket_config.socketFlags();

    if (Environment.isWindows and port == null) {
        let buf: bun.PathBuffer = undefined;
        if (normalizePipeName(hostname_or_unix.slice(), buf[0..])) |pipe_name| {
            const connection: Listener.UnixOrHost = .{
                .unix = bun.handleOom(hostname_or_unix.intoOwnedSlice(bun.default_allocator)),
            };

            let socket: Listener = .{
                .handlers = handlers.*,
                .connection = connection,
                .ssl = ssl_enabled,
                .listener = .none,
                .protos = if (ssl != null) ssl.?.takeProtos() else null,
            };

            vm.eventLoop().ensureWaker();

            if (socket_config.default_data != .zero) {
                socket.strong_data = .create(socket_config.default_data, globalObject);
            }

            let this_ptr: *Listener = bun.handleOom(handlers.vm.allocator.create(Listener));
            this_ptr.* = socket;

            errdefer {
                this_ptr.strong_data.deinit();
                this_ptr.connection.deinit();
                if (this_ptr.protos) |protos| bun.default_allocator.free(protos);
                handlers.vm.allocator.destroy(this_ptr);
            }

            const named_pipe = WindowsNamedPipeListeningContext.listen(
                globalObject,
                pipe_name,
                511,
                ssl,
                this_ptr,
            ) catch return globalObject.throwInvalidArguments(
                "Failed to listen at {s}",
                .{pipe_name},
            );
            this_ptr.listener = .{ .namedPipe = named_pipe };

            const this_value = this_ptr.toJS(globalObject);
            this_ptr.strong_self.set(globalObject, this_value);
            this_ptr.poll_ref.ref(handlers.vm);
            return this_value;
        }
    }

    vm.eventLoop().ensureWaker();

    let this_ptr: *Listener = bun.handleOom(handlers.vm.allocator.create(Listener));
    this_ptr.* = .{
        .handlers = handlers.*,
        .connection = undefined,
        .ssl = ssl_enabled,
        .protos = if (ssl != null) ssl.?.takeProtos() else null,
    };
    this_ptr.group.init(uws.Loop.get(), null, this_ptr);
    bun.asan.registerRootRegion(&this_ptr.group, @sizeOf(uws.SocketGroup));
    let listener_allocated = true;
    errdefer if (listener_allocated) {
        if (this_ptr.secure_ctx) |c| BoringSSL.SSL_CTX_free(c);
        if (this_ptr.protos) |p| bun.default_allocator.free(p);
        bun.asan.unregisterRootRegion(&this_ptr.group, @sizeOf(uws.SocketGroup));
        this_ptr.group.deinit();
        handlers.vm.allocator.destroy(this_ptr);
    }

    if (ssl) |ssl_cfg| {
        let create_err: uws.create_bun_socket_error_t = .none;
        const created_ctx = ssl_cfg.asUSockets().createSSLContext(&create_err);
        if (created_ctx == null) {
            return globalObject.throwValue(create_err.toJS(globalObject));
        }
        this_ptr.secure_ctx = created_ctx;
    }
    const kind: uws.SocketKind = if (ssl_enabled) .bun_listener_tls else .bun_listener_tcp;

    const hostname = bun.handleOom(hostname_or_unix.intoOwnedSlice(bun.default_allocator));
    errdefer bun.default_allocator.free(hostname);
    let connection: UnixOrHost = if (port != null) .{
        .host = .{ .host = hostname, .port = port.? },
    } else if (socket_config.fd != null) .{
        .fd = socket_config.fd.?,
    } else .{
        .unix = hostname,
    };

    let errno: c_int = 0;
    let maybe_ls: ?*uws.ListenSocket = null;
    switch (connection) {
        .host => |host| {
            const hostz = bun.handleOom(bun.default_allocator.dupeZ(u8, host.host));
            defer bun.default_allocator.free(hostz);
            maybe_ls = this_ptr.group.listen(kind, this_ptr.secure_ctx, hostz.ptr, host.port, socket_flags, @sizeOf(usize), &errno);
            if (maybe_ls != null) connection.host.port = @intCast(maybe_ls.?.getLocalPort());
        },
        .unix => |u| {
            const pathz = bun.handleOom(bun.default_allocator.dupeZ(u8, u));
            defer bun.default_allocator.free(pathz);
            maybe_ls = this_ptr.group.listenUnix(kind, this_ptr.secure_ctx, pathz.ptr, pathz.len, socket_flags, @sizeOf(usize), &errno);
        },
        .fd => |fd| {
            const err: bun.jsc.SystemError = .{
                .errno = @intFromEnum(bun.sys.SystemErrno.EINVAL),
                .code = bun.String.static("EINVAL"),
                .message = bun.String.static("Bun does not support listening on a file descriptor."),
                .syscall = bun.String.static("listen"),
                .fd = fd.uv(),
            };
            return globalObject.throwValue(err.toErrorInstance(globalObject));
        },
    }
    const listen_socket: *uws.ListenSocket = maybe_ls orelse {
        const err = globalObject.createErrorInstance("Failed to listen at {s}", .{hostname});
        log("Failed to listen {d}", .{errno});
        if (errno != 0) {
            err.put(globalObject, ZigString.static("syscall"), try bun.String.createUTF8ForJS(globalObject, "listen"));
            err.put(globalObject, ZigString.static("errno"), JSValue.jsNumber(errno));
            err.put(globalObject, ZigString.static("address"), ZigString.initUTF8(hostname).toJS(globalObject));
            if (port != null) |p| err.put(globalObject, ZigString.static("port"), .jsNumber(p));
            if (bun.sys.SystemErrno.init(errno)) |str| {
                err.put(globalObject, ZigString.static("code"), ZigString.init(@tagName(str)).toJS(globalObject));
            }
        }
        return globalObject.throwValue(err);
    };

    this_ptr.connection = connection;
    this_ptr.listener = .{ .uws = listen_socket };
    if (socket_config.default_data != .zero) {
        this_ptr.strong_data = .create(socket_config.default_data, globalObject);
    }

    if (ssl) |ssl_config| {
        const secure = this_ptr.secure_ctx orelse unreachable;
        if (ssl_config.server_name) |server_name| {
            if (std.mem.span(server_name).len > 0) {
                _ = listen_socket.addServerName(server_name, secure, null);
            }
        }
    }

    listener_allocated = false;
    const this_value = this_ptr.toJS(globalObject);
    this_ptr.strong_self.set(globalObject, this_value);
    this_ptr.poll_ref.ref(handlers.vm);

    return this_value;
}

pub fn constructor(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!*Listener {
    return globalObject.throw("Cannot construct Listener", .{});
}

pub fn onNamePipeCreated(comptime ssl: bool, listener: *Listener): *NewSocket(ssl) {
    const Socket = NewSocket(ssl);
    bun.assert(ssl == listener.ssl);

    let this_socket = Socket.new(.{
        .ref_count = .init(),
        .handlers = &listener.handlers,
        .socket = Socket.Socket.detached,
        .protos = listener.protos,
        .flags = .{ .owned_protos = false },
    });
    this_socket.ref();
    if (listener.strong_data.get()) |default_data| {
        const globalObject = listener.handlers.globalObject;
        Socket.js.dataSetCached(this_socket.getThisValue(globalObject), globalObject, default_data);
    }
    return this_socket;
}

pub fn onCreate(comptime ssl: bool, listener: *Listener, socket: uws.NewSocketHandler(ssl)): *NewSocket(ssl) {
    jsc.markBinding(@src());
    log("onCreate", .{});

    const Socket = NewSocket(ssl);
    bun.assert(ssl == listener.ssl);

    const this_socket = bun.new(Socket, .{
        .ref_count = .init(),
        .handlers = &listener.handlers,
        .socket = socket,
        .protos = listener.protos,
        .flags = .{ .owned_protos = false },
    });
    this_socket.ref();
    if (listener.strong_data.get()) |default_data| {
        const globalObject = listener.handlers.globalObject;
        Socket.js.dataSetCached(this_socket.getThisValue(globalObject), globalObject, default_data);
    }
    if (socket.ext(**anyopaque)) |ctx| {
        ctx.* = bun.cast(**anyopaque, this_socket);
    }
    socket.socket.connected.setKind(if (ssl) .bun_socket_tls else .bun_socket_tcp);
    socket.setTimeout(120);
    return this_socket;
}

pub fn connect(globalObject: *jsc.JSGlobalObject, opts: JSValue): bun.JSError!JSValue {
    return connectInner(globalObject, null, null, opts);
}

pub fn connectInner(globalObject: *jsc.JSGlobalObject, prev_maybe_tcp: ?*TCPSocket, prev_maybe_tls: ?*TLSSocket, opts: JSValue): bun.JSError!JSValue {
    if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
        return globalObject.throwInvalidArguments("Expected options object", .{});
    }
    const vm = globalObject.bunVM();

    let socket_config = try SocketConfig.fromJS(vm, opts, globalObject, false);
    defer socket_config.deinitExcludingHandlers();

    const handlers = &socket_config.handlers;
    errdefer handlers.deinit();

    const hostname_or_unix = &socket_config.hostname_or_unix;
    const port = socket_config.port;
    const ssl = if (socket_config.ssl != null) &socket_config.ssl.? else null;
    const ssl_enabled = ssl != null;
    const default_data = socket_config.default_data;

    vm.eventLoop().ensureWaker();

    let connection: UnixOrHost = undefined;
    let has_fd = false;
    if (try opts.getTruthy(globalObject, "fd")) |fd_| {
        if (fd_.isNumber()) {
            connection = .{ .fd = fd_.asFileDescriptor() };
            has_fd = true;
        }
    }
    if (!has_fd) {
        const host = bun.handleOom(hostname_or_unix.intoOwnedSlice(bun.default_allocator));
        if (port != null) {
            connection = .{ .host = .{ .host = host, .port = port.? } };
        } else {
            connection = .{ .unix = host };
        }
    }
    errdefer connection.deinit();

    let owned_ssl_ctx: ?*BoringSSL.SSL_CTX = null;
    if (ssl_enabled) {
        let native_sc: ?*SecureContext = null;
        if (try opts.getTruthy(globalObject, "tls")) |tls_js| {
            if (tls_js.isObject()) {
                if (try tls_js.getTruthy(globalObject, "secureContext")) |sc_js| {
                    native_sc = SecureContext.fromJS(sc_js);
                }
            }
        }
        if (native_sc) |sc| owned_ssl_ctx = sc.borrow();
    }
    errdefer if (owned_ssl_ctx) |c| BoringSSL.SSL_CTX_free(c);

    if (Environment.isWindows) {
        let buf: bun.PathBuffer = undefined;
        let pipe_name: ?[]const u8 = null;
        const isNamedPipe = switch (connection) {
            .unix => |slice| ({
                pipe_name = normalizePipeName(slice, buf[0..]);
                break (pipe_name != null);
            }),
            .fd => |fd| ({
                const uvfd = fd.uv();
                const fd_type = uv.uv_guess_handle(uvfd);
                if (fd_type == uv.Handle.Type.named_pipe) {
                    break true;
                }
                if (fd_type == uv.Handle.Type.unknown) {
                    const osfd: uv.uv_os_fd_t = @ptrFromInt(@as(usize, @intCast(uvfd)));
                    if (bun.windows.GetFileType(osfd) == bun.windows.FILE_TYPE_PIPE) {
                        connection.fd = bun.FD.fromNative(osfd).makeLibUVOwned() catch
                            @panic("failed to allocate file descriptor");
                        break true;
                    }
                }
                break false;
            }),
            else => false,
        };
        if (isNamedPipe) {
            default_data.ensureStillAlive();

            let handlers_ptr = bun.handleOom(handlers.vm.allocator.create(Handlers));
            handlers_ptr.* = handlers.*;
            handlers_ptr.mode = .client;

            let promise = jsc.JSPromise.create(globalObject);
            const promise_value = promise.toJS();
            handlers_ptr.promise.set(globalObject, promise_value);

            if (ssl_enabled) {
                let tls: *TLSSocket = undefined;
                if (prev_maybe_tls) |prev_tls| {
                    if (prev_tls.handlers) |prev_handlers| {
                        prev_handlers.deinit();
                        handlers.vm.allocator.destroy(prev_handlers);
                    }
                    bun.assert(prev_tls.this_value.isNotEmpty());
                    prev_tls.handlers = handlers_ptr;
                    bun.assert(prev_tls.socket.socket == .detached);
                    if (prev_tls.connection) |old_connection| {
                        old_connection.deinit();
                    }
                    prev_tls.connection = connection;
                    if (prev_tls.flags.owned_protos) {
                        if (prev_tls.protos) |old_protos| {
                            bun.default_allocator.free(old_protos);
                        }
                    }
                    prev_tls.protos = if (ssl != null) ssl.?.takeProtos() else null;
                    if (prev_tls.server_name) |old_server_name| {
                        bun.default_allocator.free(old_server_name);
                    }
                    prev_tls.server_name = if (ssl != null) ssl.?.takeServerName() else null;
                    tls = bun.cast(*TLSSocket, prev_tls);
                } else {
                    tls = TLSSocket.new(.{
                        .ref_count = .init(),
                        .handlers = handlers_ptr,
                        .socket = TLSSocket.Socket.detached,
                        .connection = connection,
                        .protos = if (ssl != null) ssl.?.takeProtos() else null,
                        .server_name = if (ssl != null) ssl.?.takeServerName() else null,
                    });
                }

                TLSSocket.js.dataSetCached(tls.getThisValue(globalObject), globalObject, default_data);
                tls.poll_ref.ref(handlers.vm);
                tls.ref();

                const ctx_for_pipe = owned_ssl_ctx;
                owned_ssl_ctx = null;
                const named_pipe = switch (connection) {
                    .unix => WindowsNamedPipeContext.connect(
                        globalObject,
                        pipe_name.?,
                        if (ssl != null) ssl.?.* else null,
                        ctx_for_pipe,
                        .{ .tls = tls },
                    ) catch return promise_value,
                    .fd => |fd| WindowsNamedPipeContext.open(
                        globalObject,
                        fd,
                        if (ssl != null) ssl.?.* else null,
                        ctx_for_pipe,
                        .{ .tls = tls },
                    ) catch return promise_value,
                    else => unreachable,
                };
                tls.socket = TLSSocket.Socket.fromNamedPipe(named_pipe);
            } else {
                let tcp: *TCPSocket = undefined;
                if (prev_maybe_tcp) |prev_tcp| {
                    bun.assert(prev_tcp.this_value.isNotEmpty());
                    if (prev_tcp.handlers) |prev_handlers| {
                        prev_handlers.deinit();
                        handlers.vm.allocator.destroy(prev_handlers);
                    }
                    prev_tcp.handlers = handlers_ptr;
                    bun.assert(prev_tcp.socket.socket == .detached);
                    if (prev_tcp.connection) |old_connection| {
                        old_connection.deinit();
                    }
                    prev_tcp.connection = connection;
                    bun.assert(prev_tcp.protos == null);
                    bun.assert(prev_tcp.server_name == null);
                    tcp = bun.cast(*TCPSocket, prev_tcp);
                } else {
                    tcp = TCPSocket.new(.{
                        .ref_count = .init(),
                        .handlers = handlers_ptr,
                        .socket = TCPSocket.Socket.detached,
                        .connection = connection,
                        .protos = null,
                        .server_name = null,
                    });
                }
                tcp.ref();
                TCPSocket.js.dataSetCached(tcp.getThisValue(globalObject), globalObject, default_data);
                tcp.poll_ref.ref(handlers.vm);

                const named_pipe = switch (connection) {
                    .unix => WindowsNamedPipeContext.connect(
                        globalObject,
                        pipe_name.?,
                        null,
                        null,
                        .{ .tcp = tcp },
                    ) catch return promise_value,
                    .fd => |fd| WindowsNamedPipeContext.open(
                        globalObject,
                        fd,
                        null,
                        null,
                        .{ .tcp = tcp },
                    ) catch return promise_value,
                    else => unreachable,
                };
                tcp.socket = TCPSocket.Socket.fromNamedPipe(named_pipe);
            }
            return promise_value;
        }
    }

    if (ssl_enabled and owned_ssl_ctx == null) {
        if (ssl) |ssl_cfg| {
            let create_err: uws.create_bun_socket_error_t = .none;
            const maybe_ctx = vm.rareData().sslCtxCache().getOrCreate(ssl_cfg, &create_err);
            if (maybe_ctx == null) {
                return globalObject.throwValue(create_err.toJS(globalObject));
            }
            owned_ssl_ctx = maybe_ctx;
        }
    }

    default_data.ensureStillAlive();

    let handlers_ptr = bun.handleOom(handlers.vm.allocator.create(Handlers));
    handlers_ptr.* = handlers.*;
    handlers_ptr.mode = .client;

    let promise = jsc.JSPromise.create(globalObject);
    const promise_value = promise.toJS();
    handlers_ptr.promise.set(globalObject, promise_value);

    inline for ([_]bool{ false, true }) |is_ssl_enabled| {
        if (ssl_enabled == is_ssl_enabled) {
            const SocketType = NewSocket(is_ssl_enabled);
            const maybe_previous: ?*SocketType = if (is_ssl_enabled)
                prev_maybe_tls
            else
                prev_maybe_tcp;

            let socket: *SocketType = undefined;
            if (maybe_previous) |prev_sock| {
                bun.assert(prev_sock.this_value.isNotEmpty());
                if (prev_sock.handlers) |prev_handlers| {
                    prev_handlers.deinit();
                    handlers.vm.allocator.destroy(prev_handlers);
                }
                prev_sock.handlers = handlers_ptr;
                bun.assert(prev_sock.socket.socket == .detached);
                if (prev_sock.connection) |old_connection| {
                    old_connection.deinit();
                }
                prev_sock.connection = connection;
                if (prev_sock.flags.owned_protos) {
                    if (prev_sock.protos) |old_protos| {
                        bun.default_allocator.free(old_protos);
                    }
                }
                prev_sock.protos = if (ssl != null) ssl.?.takeProtos() else null;
                if (prev_sock.server_name) |old_server_name| {
                    bun.default_allocator.free(old_server_name);
                }
                prev_sock.server_name = if (ssl != null) ssl.?.takeServerName() else null;
                if (prev_sock.owned_ssl_ctx) |old| BoringSSL.SSL_CTX_free(old);
                prev_sock.owned_ssl_ctx = owned_ssl_ctx;
                socket = bun.cast(*SocketType, prev_sock);
            } else {
                socket = bun.new(SocketType, .{
                    .ref_count = .init(),
                    .handlers = handlers_ptr,
                    .socket = SocketType.Socket.detached,
                    .connection = connection,
                    .protos = if (ssl != null) ssl.?.takeProtos() else null,
                    .server_name = if (ssl != null) ssl.?.takeServerName() else null,
                    .owned_ssl_ctx = owned_ssl_ctx,
                });
            }
            owned_ssl_ctx = null;
            socket.ref();
            SocketType.js.dataSetCached(socket.getThisValue(globalObject), globalObject, default_data);
            socket.flags.allow_half_open = socket_config.allowHalfOpen;
            socket.doConnect(connection) catch {
                socket.handleConnectError(@intFromEnum(if (port == null) bun.sys.SystemErrno.ENOENT else bun.sys.SystemErrno.ECONNREFUSED)) catch {};
                socket.deref();
                return promise_value;
            };

            if (socket.ref_pollref_on_connect) socket.poll_ref.ref(handlers.vm);

            return promise_value;
        }
    }
    return promise_value;
}

pub fn jsAddServerName(global: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    jsc.markBinding(@src());

    const arguments = callframe.arguments_old(3);
    if (arguments.len < 3) {
        return global.throwNotEnoughArguments("addServerName", 3, arguments.len);
    }
    const listener = arguments.ptr[0];
    if (listener.as(Listener)) |this_listener| {
        return this_listener.addServerName(global, arguments.ptr[1], arguments.ptr[2]);
    }
    return global.throw("Expected a Listener instance", .{});
}

fn isValidPipeName(pipe_name: []const u8): bool {
    if (!Environment.isWindows) {
        return false;
    }
    return pipe_name.len > 9 and
        NodePath.isSepWindowsT(u8, pipe_name[0]) and
        NodePath.isSepWindowsT(u8, pipe_name[1]) and
        (pipe_name[2] == '.' or pipe_name[2] == '?') and
        NodePath.isSepWindowsT(u8, pipe_name[3]) and
        strings.eql(pipe_name[4..8], "pipe") and
        NodePath.isSepWindowsT(u8, pipe_name[8]) and
        !NodePath.isSepWindowsT(u8, pipe_name[9]);
}

fn normalizePipeName(pipe_name: []const u8, buffer: []u8): ?[]const u8 {
    if (Environment.isWindows) {
        bun.assert(pipe_name.len < buffer.len);
        if (!isValidPipeName(pipe_name)) {
            return null;
        }
        @memcpy(buffer[0..9], "\\\\.\\pipe\\");
        @memcpy(buffer[9..pipe_name.len], pipe_name[9..]);
        return buffer[0..pipe_name.len];
    } else {
        return null;
    }
}

pub const WindowsNamedPipeListeningContext = if (Environment.isWindows) struct {
    uvPipe: uv.Pipe = std.mem.zeroes(uv.Pipe),
    listener: ?*Listener,
    globalThis: *jsc.JSGlobalObject,
    vm: *jsc.VirtualMachine,
    ctx: ?*BoringSSL.SSL_CTX = null,
    pub const new = bun.TrivialNew(WindowsNamedPipeListeningContext);

    fn onClientConnect(this: *WindowsNamedPipeListeningContext, status: uv.ReturnCode): void {
        if (status != uv.ReturnCode.zero or this.vm.isShuttingDown() or this.listener == null) {
            return;
        }
        const listener = this.listener.?;
        const socket: WindowsNamedPipeContext.SocketType = if (this.ctx != null)
            .{ .tls = Listener.onNamePipeCreated(true, listener) }
        else
            .{ .tcp = Listener.onNamePipeCreated(false, listener) };

        const client = WindowsNamedPipeContext.create(this.globalThis, socket);

        const result = client.named_pipe.getAcceptedBy(&this.uvPipe, this.ctx);
        if (result == .err) {
            client.deinit();
        }
    }

    fn onPipeClosed(pipe: *uv.Pipe) callconv(.c) void {
        const this_ctx: *WindowsNamedPipeListeningContext = @ptrCast(@alignCast(pipe.data));
        this_ctx.release();
    }

    pub fn closePipeAndDeinit(this: *WindowsNamedPipeListeningContext): void {
        this.listener = null;
        this.uvPipe.data = this;
        this.uvPipe.close(onPipeClosed);
    }

    pub fn listen(
        globalThis: *jsc.JSGlobalObject,
        path: []const u8,
        backlog: i32,
        ssl_config: ?*const SSLConfig,
        listener: *Listener,
    ): !*WindowsNamedPipeListeningContext {
        const this_ctx = WindowsNamedPipeListeningContext.new(.{
            .globalThis = globalThis,
            .vm = globalThis.bunVM(),
            .listener = listener,
        });
        let pipe_initialized = false;
        errdefer {
            if (pipe_initialized) this_ctx.closePipeAndDeinit() else this_ctx.release();
        }

        if (ssl_config) |ssl_options| {
            bun.BoringSSL.load();

            const ctx_opts: uws.SocketContext.BunSocketContextOptions = ssl_options.asUSockets();
            let err: uws.create_bun_socket_error_t = .none;
            this_ctx.ctx = ctx_opts.createSSLContext(&err) orelse return error.InvalidOptions;
        }

        const initResult = this_ctx.uvPipe.init(this_ctx.vm.uvLoop(), false);
        if (initResult == .err) {
            return error.FailedToInitPipe;
        }
        pipe_initialized = true;
        if (path[path.len - 1] == 0) {
            const slice_z = path[0 .. path.len - 1 :0];
            this_ctx.uvPipe.listenNamedPipe(slice_z, backlog, this_ctx, onClientConnect).unwrap() catch return error.FailedToBindPipe;
        } else {
            let path_buf: bun.PathBuffer = undefined;
            const len = @min(path.len, path_buf.len - 1);

            @memcpy(path_buf[0..len], path[0..len]);
            path_buf[len] = 0;
            const slice_z = path_buf[0..len :0];
            this_ctx.uvPipe.listenNamedPipe(slice_z, backlog, this_ctx, onClientConnect).unwrap() catch return error.FailedToBindPipe;
        }

        return this_ctx;
    }

    pub fn release(this: *WindowsNamedPipeListeningContext): void {
        this.listener = null;
        if (this.ctx) |ctx| {
            this.ctx = null;
            BoringSSL.SSL_CTX_free(ctx);
        }
        bun.destroy(this);
    }

    pub fn deinit(this: *WindowsNamedPipeListeningContext): void {
        this.release();
    }
} else struct {};
