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

    pub const ListenerType = union(enum) {
        uws: *uws.ListenSocket,
        namedPipe: *WindowsNamedPipeListeningContext,
        none: void,
    };

    pub fn getData(*_, _: *jsc.JSGlobalObject): JSValue {
        log("getData()", .{});
        return _.strong_data.get() orelse .js_undefined;
    }

    pub fn setData(*_, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue): void {
        log("setData()", .{});
        _.strong_data.set(globalObject, value);
    }

    pub fn reload(*_, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const args = callframe.arguments_old(1);

        if (args.len < 1 or (_.listener == .none and _.handlers.active_connections == 0)) {
            return globalObject.throw("Expected 1 argument", .{});
        }

        const opts = args.ptr[0];
        if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
            return globalObject.throwValue(globalObject.toInvalidArguments("Expected options object", .{}));
        }

        const socket_obj = try opts.get(globalObject, "socket") orelse {
            return globalObject.throw("Expected \"socket\" object", .{});
        };

        const handlers = try Handlers.fromJS(globalObject, socket_obj, _.handlers.mode == .server);
        const active_connections = _.handlers.active_connections;
        _.handlers.deinit();
        _.handlers = handlers;
        _.handlers.active_connections = active_connections;

        return .js_undefined;
    }

    pub fn addServerName(*_, global: *jsc.JSGlobalObject, hostname: JSValue, tls: JSValue): bun.JSError!JSValue {
        if (!_.ssl) {
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

        if (_.listener != .uws) return .js_undefined;
        const ls = _.listener.uws;

        const sni_ctx: *BoringSSL.SSL_CTX = if (SecureContext.fromJS(tls)) |sc|
            sc.borrow()
        else if (try SSLConfig.fromJS(jsc.VirtualMachine.get(), global, tls)) |ssl_config| ({
            let mut cfg = ssl_config;
            defer cfg.deinit();
            let mut create_err: uws.create_bun_socket_error_t = .none;
            break jsc.VirtualMachine.get().rareData().sslCtxCache().getOrCreate(&cfg, &create_err) orelse {
                if (create_err != .none) return global.throwValue(create_err.toJS(global));
                return global.throwValue(bun.BoringSSL.ERR_toJS(global, BoringSSL.ERR_get_error()));
            };
        }) else return .js_undefined;

        ls.removeServerName(server_name);
        const ok = ls.addServerName(server_name, sni_ctx, null);
        BoringSSL.SSL_CTX_free(sni_ctx);
        if (!ok) {
            return global.throwValue(global.ERR(.BORINGSSL, "Failed to register SNI for '{s}'", .{server_name}).toJS());
        }

        return .js_undefined;
    }

    pub fn dispose(*_, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        _.doStop(true);
        return .js_undefined;
    }

    pub fn stop(*_, _: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const arguments = callframe.arguments_old(1);
        log("close", .{});

        _.doStop(if (arguments.len > 0 and arguments.ptr[0].isBoolean()) arguments.ptr[0].toBoolean() else false);

        return .js_undefined;
    }

    fn doStop(*_, force_close: bool): void {
        if (_.listener == .none) return;
        const listener = _.listener;

        if (listener == .uws) _.unlinkUnixSocketPath();

        defer switch (listener) {
            .uws => |socket| socket.close(),
            .namedPipe => |namedPipe| if (Environment.isWindows) namedPipe.closePipeAndDeinit(),
            .none => {},
        };
        _.listener = .none;

        if (_.handlers.active_connections == 0) {
            _.poll_ref.unref(_.handlers.vm);
            _.strong_self.clearWithoutDeallocation();
            _.strong_data.clearWithoutDeallocation();
        } else if (force_close) {
            _.group.closeAll();
        }
    }

    pub fn finalize(*_): void {
        log("finalize", .{});
        const listener = _.listener;
        _.listener = .none;
        switch (listener) {
            .uws => |socket| {
                _.unlinkUnixSocketPath();
                socket.close();
            },
            .namedPipe => |namedPipe| if (Environment.isWindows) namedPipe.closePipeAndDeinit(),
            .none => {},
        }
        _.release();
    }

    fn unlinkUnixSocketPath(_: *const Listener): void {
        if (_.connection != .unix) return;
        const path = _.connection.unix;
        if (path.len == 0 or path[0] == 0) return;
        const buf = bun.path_buffer_pool.get();
        defer bun.path_buffer_pool.put(buf);
        _ = bun.sys.unlink(bun.path.z(path, buf));
    }

    pub fn release(*_): void {
        log("deinit", .{});
        _.strong_self.deinit();
        _.strong_data.deinit();
        const vm = _.handlers.vm;
        _.poll_ref.unref(vm);
        bun.assert(_.listener == .none);

        if (_.handlers.active_connections > 0) {
            _.group.closeAll();
        }
        bun.asan.unregisterRootRegion(&_.group, @sizeOf(uws.SocketGroup));
        _.group.deinit();
        if (_.secure_ctx) |ctx| BoringSSL.SSL_CTX_free(ctx);

        _.connection.deinit();
        if (_.protos) |protos| {
            _.protos = null;
            bun.default_allocator.free(protos);
        }
        _.handlers.deinit();
        vm.allocator.destroy(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }

    pub fn getConnectionsCount(*_, _: *jsc.JSGlobalObject): JSValue {
        return JSValue.jsNumber(_.handlers.active_connections);
    }

    pub fn getUnix(*_, globalObject: *jsc.JSGlobalObject): JSValue {
        if (_.connection != .unix) {
            return .js_undefined;
        }

        return ZigString.init(_.connection.unix).withEncoding().toJS(globalObject);
    }

    pub fn getHostname(*_, globalObject: *jsc.JSGlobalObject): JSValue {
        if (_.connection != .host) {
            return .js_undefined;
        }
        return ZigString.init(_.connection.host.host).withEncoding().toJS(globalObject);
    }

    pub fn getPort(*_, _: *jsc.JSGlobalObject): JSValue {
        if (_.connection != .host) {
            return .js_undefined;
        }
        return JSValue.jsNumber(_.connection.host.port);
    }

    pub fn getFD(*_, _: *jsc.JSGlobalObject): JSValue {
        switch (_.listener) {
            .uws => |uws_listener| {
                return uws_listener.socket(false).fd().toJSWithoutMakingLibUVOwned();
            },
            else => return JSValue.jsNumber(-1),
        }
    }

    pub fn ref(*_, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const this_value = callframe.this();
        if (_.listener == .none) return .js_undefined;
        _.poll_ref.ref(globalObject.bunVM());
        _.strong_self.set(globalObject, this_value);
        return .js_undefined;
    }

    pub fn unref(*_, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        _.poll_ref.unref(globalObject.bunVM());
        if (_.handlers.active_connections == 0) {
            _.strong_self.clearWithoutDeallocation();
        }
        return .js_undefined;
    }

    pub fn getsockname(*_, globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        if (_.listener != .uws) {
            return .js_undefined;
        }

        const out = callFrame.argumentsAsArray(1)[0];
        if (!out.isObject()) {
            return globalThis.throwInvalidArguments("Expected object", .{});
        }
        const socket = _.listener.uws;

        let mut buf: [64]u8 = [_]u8{0} ** 64;
        let mut text_buf: [512]u8 = undefined;
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

    pub fn clone(_: UnixOrHost): UnixOrHost {
        switch (_) {
            .unix => |u| {
                return .{
                    .unix = bun.handleOom(bun.default_allocator.dupe(u8, u)),
                };
            },
            .host => |h| {
                return .{
                    .host = .{
                        .host = bun.handleOom(bun.default_allocator.dupe(u8, h.host)),
                        .port = _.host.port,
                    },
                };
            },
            .fd => |f| return .{ .fd = f },
        }
    }

    pub fn release(_: UnixOrHost): void {
        switch (_) {
            .unix => |u| {
                bun.default_allocator.free(u);
            },
            .host => |h| {
                bun.default_allocator.free(h.host);
            },
            .fd => {},
        }
    }

    pub fn deinit(_: UnixOrHost): void {
        _.release();
    }
};

pub fn listen(globalObject: *jsc.JSGlobalObject, opts: JSValue): bun.JSError!JSValue {
    log("listen", .{});
    if (opts.isEmptyOrUndefinedOrNull() or opts.isBoolean() or !opts.isObject()) {
        return globalObject.throwInvalidArguments("Expected object", .{});
    }

    const vm = jsc.VirtualMachine.get();

    let mut socket_config = try SocketConfig.fromJS(vm, opts, globalObject, true);
    defer socket_config.deinitExcludingHandlers();

    const handlers = &socket_config.handlers;
    errdefer handlers.deinit();

    const hostname_or_unix = &socket_config.hostname_or_unix;
    const port = socket_config.port;
    const ssl = if (socket_config.ssl) |*s| s else null;
    const ssl_enabled = ssl != null;
    const socket_flags = socket_config.socketFlags();

    if (Environment.isWindows and port == null) {
        let mut buf: bun.PathBuffer = undefined;
        if (normalizePipeName(hostname_or_unix.slice(), buf[0..])) |pipe_name| {
            const connection: Listener.UnixOrHost = .{
                .unix = bun.handleOom(hostname_or_unix.intoOwnedSlice(bun.default_allocator)),
            };

            let mut socket: Listener = .{
                .handlers = handlers.*,
                .connection = connection,
                .ssl = ssl_enabled,
                .listener = .none,
                .protos = if (ssl) |s| s.takeProtos() else null,
            };

            vm.eventLoop().ensureWaker();

            if (socket_config.default_data != .zero) {
                socket.strong_data = .create(socket_config.default_data, globalObject);
            }

            const this_ptr: *Listener = bun.handleOom(handlers.vm.allocator.create(Listener));
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

    let mut this_ptr: *Listener = bun.handleOom(handlers.vm.allocator.create(Listener));
    this_ptr.* = .{
        .handlers = handlers.*,
        .connection = undefined,
        .ssl = ssl_enabled,
        .protos = if (ssl) |s| s.takeProtos() else null,
    };
    this_ptr.group.init(uws.Loop.get(), null, this_ptr);
    bun.asan.registerRootRegion(&this_ptr.group, @sizeOf(uws.SocketGroup));
    let mut listener_allocated = true;
    errdefer if (listener_allocated) {
        if (this_ptr.secure_ctx) |c| BoringSSL.SSL_CTX_free(c);
        if (this_ptr.protos) |p| bun.default_allocator.free(p);
        bun.asan.unregisterRootRegion(&this_ptr.group, @sizeOf(uws.SocketGroup));
        this_ptr.group.deinit();
        handlers.vm.allocator.destroy(this_ptr);
    };

    if (ssl) |ssl_cfg| {
        let mut create_err: uws.create_bun_socket_error_t = .none;
        this_ptr.secure_ctx = ssl_cfg.asUSockets().createSSLContext(&create_err) orelse {
            return globalObject.throwValue(create_err.toJS(globalObject));
        };
    }
    const kind: uws.SocketKind = if (ssl_enabled) .bun_listener_tls else .bun_listener_tcp;

    const hostname = bun.handleOom(hostname_or_unix.intoOwnedSlice(bun.default_allocator));
    errdefer bun.default_allocator.free(hostname);
    let mut connection: Listener.UnixOrHost = if (port) |port_| .{
        .host = .{ .host = hostname, .port = port_ },
    } else if (socket_config.fd) |fd| .{ .fd = fd } else .{ .unix = hostname };

    let mut errno: c_int = 0;
    const listen_socket: *uws.ListenSocket = ({
        switch (connection) {
            .host => |host| {
                const hostz = bun.handleOom(bun.default_allocator.dupeZ(u8, host.host));
                defer bun.default_allocator.free(hostz);
                const ls = this_ptr.group.listen(kind, this_ptr.secure_ctx, hostz.ptr, host.port, socket_flags, @sizeOf(?*anyopaque), &errno);
                if (ls) |s| connection.host.port = @intCast(s.getLocalPort());
                break ls;
            },
            .unix => |u| {
                const pathz = bun.handleOom(bun.default_allocator.dupeZ(u8, u));
                defer bun.default_allocator.free(pathz);
                break this_ptr.group.listenUnix(kind, this_ptr.secure_ctx, pathz.ptr, pathz.len, socket_flags, @sizeOf(?*anyopaque), &errno);
            },
            .fd => |fd| {
                const err: bun.jsc.SystemError = .{
                    .errno = @intFromEnum(bun.sys.SystemErrno.EINVAL),
                    .code = .static("EINVAL"),
                    .message = .static("Bun does not support listening on a file descriptor."),
                    .syscall = .static("listen"),
                    .fd = fd.uv(),
                };
                return globalObject.throwValue(err.toErrorInstance(globalObject));
            },
        }
    }) orelse {
        const err = globalObject.createErrorInstance("Failed to listen at {s}", .{hostname});
        log("Failed to listen {d}", .{errno});
        if (errno != 0) {
            err.put(globalObject, ZigString.static("syscall"), try bun.String.createUTF8ForJS(globalObject, "listen"));
            err.put(globalObject, ZigString.static("errno"), JSValue.jsNumber(errno));
            err.put(globalObject, ZigString.static("address"), ZigString.initUTF8(hostname).toJS(globalObject));
            if (port) |p| err.put(globalObject, ZigString.static("port"), .jsNumber(p));
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

    let mut this_socket = Socket.new(.{
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

    let mut socket_config = try SocketConfig.fromJS(vm, opts, globalObject, false);
    defer socket_config.deinitExcludingHandlers();

    const handlers = &socket_config.handlers;
    errdefer handlers.deinit();

    const hostname_or_unix = &socket_config.hostname_or_unix;
    const port = socket_config.port;
    const ssl = if (socket_config.ssl) |*s| s else null;
    const ssl_enabled = ssl != null;
    const default_data = socket_config.default_data;

    vm.eventLoop().ensureWaker();

    let mut connection: Listener.UnixOrHost = ({
        if (try opts.getTruthy(globalObject, "fd")) |fd_| {
            if (fd_.isNumber()) {
                const fd = fd_.asFileDescriptor();
                break .{ .fd = fd };
            }
        }
        const host = bun.handleOom(hostname_or_unix.intoOwnedSlice(bun.default_allocator));
        break if (port) |port_| .{
            .host = .{
                .host = host,
                .port = port_,
            },
        } else .{ .unix = host };
    });
    errdefer connection.deinit();

    let mut owned_ssl_ctx: ?*BoringSSL.SSL_CTX = null;
    if (ssl_enabled) {
        const native_sc: ?*SecureContext = ({
            const tls_js = (try opts.getTruthy(globalObject, "tls")) orelse break null;
            if (!tls_js.isObject()) break null;
            const sc_js = (try tls_js.getTruthy(globalObject, "secureContext")) orelse break null;
            break SecureContext.fromJS(sc_js);
        });
        if (native_sc) |sc| owned_ssl_ctx = sc.borrow();
    }
    errdefer if (owned_ssl_ctx) |c| BoringSSL.SSL_CTX_free(c);

    if (Environment.isWindows) {
        let mut buf: bun.PathBuffer = undefined;
        let mut pipe_name: ?[]const u8 = null;
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

            const handlers_ptr = bun.handleOom(handlers.vm.allocator.create(Handlers));
            handlers_ptr.* = handlers.*;
            handlers_ptr.mode = .client;

            let mut promise = jsc.JSPromise.create(globalObject);
            const promise_value = promise.toJS();
            handlers_ptr.promise.set(globalObject, promise_value);

            if (ssl_enabled) {
                let mut tls = if (prev_maybe_tls) |prev| ({
                    if (prev.handlers) |prev_handlers| {
                        prev_handlers.deinit();
                        handlers.vm.allocator.destroy(prev_handlers);
                    }
                    bun.assert(prev.this_value.isNotEmpty());
                    prev.handlers = handlers_ptr;
                    bun.assert(prev.socket.socket == .detached);
                    if (prev.connection) |old_connection| {
                        old_connection.deinit();
                    }
                    prev.connection = connection;
                    if (prev.flags.owned_protos) {
                        if (prev.protos) |old_protos| {
                            bun.default_allocator.free(old_protos);
                        }
                    }
                    prev.protos = if (ssl) |s| s.takeProtos() else null;
                    if (prev.server_name) |old_server_name| {
                        bun.default_allocator.free(old_server_name);
                    }
                    prev.server_name = if (ssl) |s| s.takeServerName() else null;
                    break prev;
                }) else TLSSocket.new(.{
                    .ref_count = .init(),
                    .handlers = handlers_ptr,
                    .socket = TLSSocket.Socket.detached,
                    .connection = connection,
                    .protos = if (ssl) |s| s.takeProtos() else null,
                    .server_name = if (ssl) |s| s.takeServerName() else null,
                });

                TLSSocket.js.dataSetCached(tls.getThisValue(globalObject), globalObject, default_data);
                tls.poll_ref.ref(handlers.vm);
                tls.ref();

                const ctx_for_pipe = owned_ssl_ctx;
                owned_ssl_ctx = null;
                const named_pipe = switch (connection) {
                    .unix => WindowsNamedPipeContext.connect(
                        globalObject,
                        pipe_name.?,
                        if (ssl) |s| s.* else null,
                        ctx_for_pipe,
                        .{ .tls = tls },
                    ) catch return promise_value,
                    .fd => |fd| WindowsNamedPipeContext.open(
                        globalObject,
                        fd,
                        if (ssl) |s| s.* else null,
                        ctx_for_pipe,
                        .{ .tls = tls },
                    ) catch return promise_value,
                    else => unreachable,
                };
                tls.socket = TLSSocket.Socket.fromNamedPipe(named_pipe);
            } else {
                let mut tcp = if (prev_maybe_tcp) |prev| ({
                    bun.assert(prev.this_value.isNotEmpty());
                    if (prev.handlers) |prev_handlers| {
                        prev_handlers.deinit();
                        handlers.vm.allocator.destroy(prev_handlers);
                    }
                    prev.handlers = handlers_ptr;
                    bun.assert(prev.socket.socket == .detached);
                    if (prev.connection) |old_connection| {
                        old_connection.deinit();
                    }
                    prev.connection = connection;
                    bun.assert(prev.protos == null);
                    bun.assert(prev.server_name == null);
                    break prev;
                }) else TCPSocket.new(.{
                    .ref_count = .init(),
                    .handlers = handlers_ptr,
                    .socket = TCPSocket.Socket.detached,
                    .connection = connection,
                    .protos = null,
                    .server_name = null,
                });
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
            let mut create_err: uws.create_bun_socket_error_t = .none;
            owned_ssl_ctx = vm.rareData().sslCtxCache().getOrCreate(ssl_cfg, &create_err) orelse {
                return globalObject.throwValue(create_err.toJS(globalObject));
            };
        }
    }

    default_data.ensureStillAlive();

    const handlers_ptr = bun.handleOom(handlers.vm.allocator.create(Handlers));
    handlers_ptr.* = handlers.*;
    handlers_ptr.mode = .client;

    let mut promise = jsc.JSPromise.create(globalObject);
    const promise_value = promise.toJS();
    handlers_ptr.promise.set(globalObject, promise_value);

    switch (ssl_enabled) {
        inline else => |is_ssl_enabled| {
            const SocketType = NewSocket(is_ssl_enabled);
            const maybe_previous: ?*SocketType = if (is_ssl_enabled)
                prev_maybe_tls
            else
                prev_maybe_tcp;

            const socket = if (maybe_previous) |prev| ({
                bun.assert(prev.this_value.isNotEmpty());
                if (prev.handlers) |prev_handlers| {
                    prev_handlers.deinit();
                    handlers.vm.allocator.destroy(prev_handlers);
                }
                prev.handlers = handlers_ptr;
                bun.assert(prev.socket.socket == .detached);
                if (prev.connection) |old_connection| {
                    old_connection.deinit();
                }
                prev.connection = connection;
                if (prev.flags.owned_protos) {
                    if (prev.protos) |old_protos| {
                        bun.default_allocator.free(old_protos);
                    }
                }
                prev.protos = if (ssl) |s| s.takeProtos() else null;
                if (prev.server_name) |old_server_name| {
                    bun.default_allocator.free(old_server_name);
                }
                prev.server_name = if (ssl) |s| s.takeServerName() else null;
                if (prev.owned_ssl_ctx) |old| BoringSSL.SSL_CTX_free(old);
                prev.owned_ssl_ctx = owned_ssl_ctx;
                break prev;
            }) else bun.new(SocketType, .{
                .ref_count = .init(),
                .handlers = handlers_ptr,
                .socket = SocketType.Socket.detached,
                .connection = connection,
                .protos = if (ssl) |s| s.takeProtos() else null,
                .server_name = if (ssl) |s| s.takeServerName() else null,
                .owned_ssl_ctx = owned_ssl_ctx,
            });
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
        },
    }
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

    fn onClientConnect(_: *WindowsNamedPipeListeningContext, status: uv.ReturnCode): void {
        if (status != uv.ReturnCode.zero or _.vm.isShuttingDown() or _.listener == null) {
            return;
        }
        const listener = _.listener.?;
        const socket: WindowsNamedPipeContext.SocketType = ({
            if (_.ctx) |_| {
                break .{ .tls = Listener.onNamePipeCreated(true, listener) };
            } else {
                break .{ .tcp = Listener.onNamePipeCreated(false, listener) };
            }
        });

        const client = WindowsNamedPipeContext.create(_.globalThis, socket);

        const result = client.named_pipe.getAcceptedBy(&_.uvPipe, _.ctx);
        if (result == .err) {
            client.deinit();
        }
    }

    @c fn onPipeClosed(pipe: *uv.Pipe): void {
        const this_ctx: *WindowsNamedPipeListeningContext = @ptrCast(@alignCast(pipe.data));
        this_ctx.release();
    }

    pub fn closePipeAndDeinit(_: *WindowsNamedPipeListeningContext): void {
        _.listener = null;
        _.uvPipe.data = _;
        _.uvPipe.close(onPipeClosed);
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
        let mut pipe_initialized = false;
        errdefer {
            if (pipe_initialized) this_ctx.closePipeAndDeinit() else this_ctx.release();
        }

        if (ssl_config) |ssl_options| {
            bun.BoringSSL.load();

            const ctx_opts: uws.SocketContext.BunSocketContextOptions = ssl_options.asUSockets();
            let mut err: uws.create_bun_socket_error_t = .none;
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
            let mut path_buf: bun.PathBuffer = undefined;
            const len = @min(path.len, path_buf.len - 1);

            @memcpy(path_buf[0..len], path[0..len]);
            path_buf[len] = 0;
            const slice_z = path_buf[0..len :0];
            this_ctx.uvPipe.listenNamedPipe(slice_z, backlog, this_ctx, onClientConnect).unwrap() catch return error.FailedToBindPipe;
        }

        return this_ctx;
    }

    pub fn release(_: *WindowsNamedPipeListeningContext): void {
        _.listener = null;
        if (_.ctx) |ctx| {
            _.ctx = null;
            BoringSSL.SSL_CTX_free(ctx);
        }
        bun.destroy(_);
    }

    pub fn deinit(_: *WindowsNamedPipeListeningContext): void {
        _.release();
    }
} else void;
