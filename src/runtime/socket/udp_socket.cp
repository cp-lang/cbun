import std from "std";
import bun from "bun";

const Async = bun.Async;
const Output = bun.Output;
const default_allocator = bun.default_allocator;
const jsc = bun.jsc;
const CallFrame = jsc.CallFrame;
const JSGlobalObject = jsc.JSGlobalObject;
const JSRef = jsc.JSRef;
const JSValue = jsc.JSValue;
const SocketAddress = jsc.API.SocketAddress;
const uws = @import("../../uws/uws.zig");

const log = Output.scoped(.UdpSocket, .visible);

const INET6_ADDRSTRLEN = if (bun.Environment.isWindows) 65 else 46;

extern fn ntohs(nshort: u16): u16;
extern fn htonl(hlong: u32): u32;
extern fn htons(hshort: u16): u16;
extern fn inet_ntop(af: c_int, src: ?*const anyopaque, dst: [*c]u8, size: c_int): ?[*:0]const u8;
extern fn inet_pton(af: c_int, src: [*c]const u8, dst: ?*anyopaque): c_int;

@c
fn onClose(socket: *uws.udp.Socket): void {
    jsc.markBinding(@src());

    let this_socket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    this_socket.closed = true;
    this_socket.poll_ref.disable();
    this_socket.this_value.downgrade();
    this_socket.socket = null;
}

@c
fn onRecvError(socket: *uws.udp.Socket, errno: c_int): void {
    jsc.markBinding(@src());

    let this_socket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    const sys_err = bun.sys.Error.fromCodeInt(errno, .recv);
    const globalThis = this_socket.globalThis;
    const err_value = sys_err.toJS(globalThis) catch return;
    this_socket.callErrorHandler(.zero, err_value);
}

@c
fn onDrain(socket: *uws.udp.Socket): void {
    jsc.markBinding(@src());

    let this_socket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    const thisValue = this_socket.this_value.tryGet() orelse return;
    const callback = UDPSocket.js.gc.on_drain.get(thisValue) orelse return;
    if (callback.isEmptyOrUndefinedOrNull()) return;

    const vm = jsc.VirtualMachine.get();
    const event_loop = vm.eventLoop();
    event_loop.enter();
    defer event_loop.exit();
    _ = callback.call(this_socket.globalThis, thisValue, &.{thisValue}) catch |err| {
        this_socket.callErrorHandler(.zero, this_socket.globalThis.takeException(err));
    };
}

@c
fn onData(socket: *uws.udp.Socket, buf: *uws.udp.PacketBuffer, packets: c_int): void {
    jsc.markBinding(@src());

    const udpSocket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    const thisValue = udpSocket.this_value.tryGet() orelse return;
    const callback = UDPSocket.js.gc.on_data.get(thisValue) orelse return;
    if (callback.isEmptyOrUndefinedOrNull()) return;

    const globalThis = udpSocket.globalThis;
    defer thisValue.ensureStillAlive();

    let i: c_int = 0;
    while (i < packets) : (i += 1) {
        const peer = buf.getPeer(i);

        let addr_buf: [67]u8 = undefined;
        let hostname: ?[*:0]const u8 = null;
        let port: u16 = 0;
        let scope_id: ?u32 = null;

        if (peer.family == std.posix.AF.INET) {
            const peer4: *std.posix.sockaddr.in = bun.cast(*std.posix.sockaddr.in, peer);
            hostname = inet_ntop(peer.family, &peer4.addr, &addr_buf, addr_buf.len);
            port = ntohs(peer4.port);
        } else if (peer.family == std.posix.AF.INET6) {
            const peer6: *std.posix.sockaddr.in6 = bun.cast(*std.posix.sockaddr.in6, peer);
            hostname = inet_ntop(peer.family, &peer6.addr, &addr_buf, addr_buf.len);
            port = ntohs(peer6.port);
            if (peer6.scope_id != 0) {
                scope_id = peer6.scope_id;
            }
        } else {
            continue;
        }

        if (hostname == null or port == 0) {
            continue;
        }

        const slice = buf.getPayload(i);
        const truncated = buf.getTruncated(i);

        const span = std.mem.span(hostname.?);
        let hostname_string: bun.String = bun.String.init(span);
        if (scope_id) |id| {
            if (comptime !bun.Environment.isWindows) {
                let buffer = std.mem.zeroes([64]u8);
                if (bun.c.if_indextoname(id, &buffer) != null) {
                    hostname_string = bun.handleOom(bun.String.createFormat("{s}%{s}", .{ span, std.mem.span(bun.cast([*:0]u8, &buffer)) }));
                } else {
                    hostname_string = bun.handleOom(bun.String.createFormat("{s}%{d}", .{ span, id }));
                }
            } else {
                hostname_string = bun.handleOom(bun.String.createFormat("{s}%{d}", .{ span, id }));
            }
        }

        const loop = udpSocket.vm.eventLoop();
        loop.enter();
        defer loop.exit();
        defer thisValue.ensureStillAlive();

        const flags = jsc.JSValue.createEmptyObject(globalThis, 1);
        flags.put(globalThis, jsc.ZigString.static("truncated"), .jsBoolean(truncated));

        const js_payload = udpSocket.config.binary_type.toJS(slice, globalThis) catch return;
        const js_hostname = hostname_string.transferToJS(globalThis) catch return;

        _ = callback.call(globalThis, thisValue, &.{
            thisValue,
            js_payload,
            .jsNumber(port),
            js_hostname,
            flags,
        }) catch |err| {
            udpSocket.callErrorHandler(.zero, globalThis.takeException(err));
        };
    }
}

pub class UDPSocketConfig {
    const HandlerEntry = struct {
        name: []const u8,
        gc_field: []const u8,
    };
    const handlers = [_]HandlerEntry{
        .{ .name = "data", .gc_field = "on_data" },
        .{ .name = "drain", .gc_field = "on_drain" },
        .{ .name = "error", .gc_field = "on_error" },
    };

    pub const ConnectConfig = struct {
        port: u16,
        address: bun.String,
    };

    hostname: bun.String = .empty,
    connect: ?ConnectConfig = null,
    port: u16 = 0,
    flags: i32 = 0,
    binary_type: jsc.ArrayBuffer.BinaryType = .Buffer,

    pub fn fromJS(globalThis: *JSGlobalObject, options: JSValue, thisValue: JSValue): bun.JSError!UDPSocketConfig {
        if (options.isEmptyOrUndefinedOrNull() or !options.isObject()) {
            return globalThis.throwInvalidArguments("Expected an object", .{});
        }

        let parsed_port: u16 = 0;
        if (try options.getTruthy(globalThis, "port")) |value| {
            const number = try value.coerceToInt32(globalThis);
            if (number < 0 or number > 0xffff) {
                return globalThis.throwInvalidArguments("Expected \"port\" to be an integer between 0 and 65535", .{});
            }
            parsed_port = @intCast(number);
        }

        let parsed_flags: i32 = 0;
        if (try options.getTruthy(globalThis, "flags")) |value| {
            parsed_flags = try bun.validators.validateInt32(globalThis, value, "flags", .{}, null, null);
        }

        let parsed_hostname = bun.String.static("0.0.0.0");
        if (try options.getTruthy(globalThis, "hostname")) |value| {
            if (!value.isString()) {
                return globalThis.throwInvalidArguments("Expected \"hostname\" to be a string", .{});
            }
            parsed_hostname = try value.toBunString(globalThis);
        }

        let config = UDPSocketConfig{
            .hostname = parsed_hostname,
            .port = parsed_port,
            .flags = parsed_flags,
        };

        errdefer config.release();

        if (try options.getTruthy(globalThis, "socket")) |socket| {
            if (!socket.isObject()) {
                return globalThis.throwInvalidArguments("Expected \"socket\" to be an object", .{});
            }

            if (try options.getTruthy(globalThis, "binaryType")) |value| {
                if (!value.isString()) {
                    return globalThis.throwInvalidArguments("Expected \"socket.binaryType\" to be a string", .{});
                }

                const maybe_binary_type = try jsc.ArrayBuffer.BinaryType.fromJSValue(globalThis, value);
                if (maybe_binary_type == null) {
                    return globalThis.throwInvalidArguments("Expected \"socket.binaryType\" to be 'arraybuffer', 'uint8array', or 'buffer'", .{});
                }
                config.binary_type = maybe_binary_type.?;
            }

            inline for (handlers) |handler| {
                if (try socket.getTruthyComptime(globalThis, handler.name)) |value| {
                    if (!value.isCell() or !value.isCallable()) {
                        return globalThis.throwInvalidArguments("Expected \"socket.{s}\" to be a function", .{handler.name});
                    }
                    const callback = value.withAsyncContextIfNeeded(globalThis);
                    UDPSocket.js.gc.set(@field(UDPSocket.js.gc, handler.gc_field), thisValue, globalThis, callback);
                }
            }
        }

        if (try options.getTruthy(globalThis, "connect")) |connect| {
            if (!connect.isObject()) {
                return globalThis.throwInvalidArguments("Expected \"connect\" to be an object", .{});
            }

            const connect_host_js = try connect.getTruthy(globalThis, "hostname") orelse {
                return globalThis.throwInvalidArguments("Expected \"connect.hostname\" to be a string", .{});
            };

            if (!connect_host_js.isString()) {
                return globalThis.throwInvalidArguments("Expected \"connect.hostname\" to be a string", .{});
            }

            const connect_port_js = try connect.getTruthy(globalThis, "port") orelse {
                return globalThis.throwInvalidArguments("Expected \"connect.port\" to be an integer", .{});
            };
            const connect_port = try connect_port_js.coerceToInt32(globalThis);

            const connect_host = try connect_host_js.toBunString(globalThis);

            config.connect = .{
                .port = if (connect_port < 1 or connect_port > 0xffff) 0 else @as(u16, @intCast(connect_port)),
                .address = connect_host,
            };
        }

        return config;
    }

    pub fn release(*this): void {
        this.hostname.deref();
        if (this.connect) |*val| {
            val.address.deref();
        }
    }

    pub fn deinit(*this): void {
        this.release();
    }
}

export class UDPSocket {
    config: UDPSocketConfig = .{},
    socket: ?*uws.udp.Socket = null,
    loop: *uws.Loop,

    globalThis: *JSGlobalObject,
    this_value: JSRef = JSRef.empty(),

    jsc_ref: jsc.Ref = jsc.Ref.init(),
    poll_ref: Async.KeepAlive = Async.KeepAlive.init(),
    closed: bool = false,
    connect_info: ?ConnectInfo = null,
    vm: *jsc.VirtualMachine,

    pub const ConnectInfo = struct {
        port: u16,
    };

    pub const js = jsc.Codegen.JSUDPSocket;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;
    pub const new = bun.TrivialNew(@This());

    pub fn udpSocket(globalThis: *JSGlobalObject, options: JSValue): bun.JSError!JSValue {
        log("udpSocket", .{});

        const vm = globalThis.bunVM();
        let this_ptr = UDPSocket.new(.{
            .socket = null,
            .config = .{},
            .globalThis = globalThis,
            .loop = uws.Loop.get(),
            .vm = vm,
        });
        errdefer {
            this_ptr.closed = true;
            if (this_ptr.socket) |socket| {
                this_ptr.socket = null;
                socket.close();
            }
            this_ptr.this_value.downgrade();
        }
        const thisValue = this_ptr.toJS(globalThis);
        thisValue.ensureStillAlive();
        this_ptr.this_value.setStrong(thisValue, globalThis);

        this_ptr.config = try UDPSocketConfig.fromJS(globalThis, options, thisValue);

        let err: i32 = 0;

        const hostname_slice = this_ptr.config.hostname.toUTF8(bun.default_allocator);
        defer hostname_slice.deinit();
        const hostname_z = bun.handleOom(bun.default_allocator.dupeZ(u8, hostname_slice.slice()));
        defer bun.default_allocator.free(hostname_z);

        this_ptr.socket = uws.udp.Socket.create(
            this_ptr.loop,
            onData,
            onDrain,
            onClose,
            onRecvError,
            hostname_z,
            this_ptr.config.port,
            this_ptr.config.flags,
            &err,
            this_ptr,
        ) orelse {
            this_ptr.closed = true;
            if (err != 0) {
                const code = @tagName(bun.sys.SystemErrno.init(@as(c_int, @intCast(err))).?);
                const sys_err = jsc.SystemError{
                    .errno = err,
                    .code = bun.String.static(code),
                    .message = bun.handleOom(bun.String.createFormat("bind {s} {f}", .{ code, this_ptr.config.hostname })),
                };
                const error_value = sys_err.toErrorInstance(globalThis);
                error_value.put(globalThis, "address", try this_ptr.config.hostname.toJS(globalThis));

                return globalThis.throwValue(error_value);
            }

            return globalThis.throw("Failed to bind socket", .{});
        };

        if (this_ptr.config.connect) |*connect| {
            const address_slice = connect.address.toUTF8(bun.default_allocator);
            defer address_slice.deinit();
            const address_z = bun.handleOom(bun.default_allocator.dupeZ(u8, address_slice.slice()));
            defer bun.default_allocator.free(address_z);
            const ret = this_ptr.socket.?.connect(address_z, connect.port);
            if (ret != 0) {
                if (bun.sys.Maybe(void).errnoSys(ret, .connect)) |*sys_err| {
                    return globalThis.throwValue(try sys_err.err.toJS(globalThis));
                }

                if (bun.c_ares.Error.initEAI(ret)) |eai_err| {
                    return globalThis.throwValue(try eai_err.toJSWithSyscallAndHostname(globalThis, "connect", address_slice.slice()));
                }
            }
            this_ptr.connect_info = .{ .port = connect.port };
        }

        this_ptr.poll_ref.ref(vm);
        return jsc.JSPromise.resolvedPromiseValue(globalThis, thisValue);
    }

    pub fn callErrorHandler(
        *this,
        thisValue_: JSValue,
        err: JSValue,
    ): void {
        const thisValue = if (thisValue_ == .zero) (this.this_value.tryGet() orelse return) else thisValue_;
        const callback = UDPSocket.js.gc.on_error.get(thisValue) orelse .zero;
        const globalThis = this.globalThis;
        const vm = globalThis.bunVM();

        if (err.isTerminationException()) {
            return;
        }
        if (callback.isEmptyOrUndefinedOrNull()) {
            _ = vm.uncaughtException(globalThis, err, false);
            return;
        }

        const event_loop = vm.eventLoop();
        event_loop.enter();
        defer event_loop.exit();
        _ = callback.call(globalThis, thisValue, &.{err.toError() orelse err}) catch |e| globalThis.reportActiveExceptionAsUnhandled(e);
    }

    pub fn setBroadcast(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const enabled = arguments[0].toBoolean();
        const res = this.socket.?.setBroadcast(enabled);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return arguments[0];
    }

    pub fn setMulticastLoopback(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const enabled = arguments[0].toBoolean();
        const res = this.socket.?.setMulticastLoopback(enabled);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return arguments[0];
    }

    fn setMembership(*this, globalThis: *JSGlobalObject, callframe: *CallFrame, drop: bool): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        let addr = std.mem.zeroes(std.posix.sockaddr.storage);
        if (!try parseAddr(this, globalThis, .jsNumber(0), arguments[0], &addr)) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.INVAL))), .setsockopt).?.toJS(globalThis));
        }

        let iface = std.mem.zeroes(std.posix.sockaddr.storage);
        const socket = this.socket orelse return globalThis.throw("Socket is closed", .{});

        let res: c_int = 0;
        if (arguments.len > 1 and try parseAddr(this, globalThis, .jsNumber(0), arguments[1], &iface)) {
            if (addr.family != iface.family) {
                return globalThis.throwInvalidArguments("Family mismatch between address and interface", .{});
            }
            res = socket.setMembership(&addr, &iface, drop);
        } else {
            res = socket.setMembership(&addr, null, drop);
        }

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return .true;
    }

    pub fn addMembership(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return this.setMembership(globalThis, callframe, false);
    }

    pub fn dropMembership(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return this.setMembership(globalThis, callframe, true);
    }

    fn setSourceSpecificMembership(*this, globalThis: *JSGlobalObject, callframe: *CallFrame, drop: bool): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 2) {
            return globalThis.throwInvalidArguments("Expected 2 arguments, got {}", .{arguments.len});
        }

        let source_addr: std.posix.sockaddr.storage = undefined;
        if (!try parseAddr(this, globalThis, .jsNumber(0), arguments[0], &source_addr)) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.INVAL))), .setsockopt).?.toJS(globalThis));
        }

        let group_addr: std.posix.sockaddr.storage = undefined;
        if (!try parseAddr(this, globalThis, .jsNumber(0), arguments[1], &group_addr)) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.INVAL))), .setsockopt).?.toJS(globalThis));
        }

        if (source_addr.family != group_addr.family) {
            return globalThis.throwInvalidArguments("Family mismatch between source and group addresses", .{});
        }

        let iface: std.posix.sockaddr.storage = undefined;
        const socket = this.socket orelse return globalThis.throw("Socket is closed", .{});

        let res: c_int = 0;
        if (arguments.len > 2 and try parseAddr(this, globalThis, .jsNumber(0), arguments[2], &iface)) {
            if (source_addr.family != iface.family) {
                return globalThis.throwInvalidArguments("Family mismatch among source, group and interface addresses", .{});
            }
            res = socket.setSourceSpecificMembership(&source_addr, &group_addr, &iface, drop);
        } else {
            res = socket.setSourceSpecificMembership(&source_addr, &group_addr, null, drop);
        }

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return .true;
    }

    pub fn addSourceSpecificMembership(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return this.setSourceSpecificMembership(globalThis, callframe, false);
    }

    pub fn dropSourceSpecificMembership(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return this.setSourceSpecificMembership(globalThis, callframe, true);
    }

    pub fn setMulticastInterface(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        let addr: std.posix.sockaddr.storage = undefined;

        if (!try parseAddr(this, globalThis, .jsNumber(0), arguments[0], &addr)) {
            return .false;
        }

        const socket = this.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = socket.setMulticastInterface(&addr);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return .true;
    }

    pub fn setTTL(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return this.setAnyTTL(globalThis, callframe, uws.udp.Socket.setUnicastTTL);
    }

    pub fn setMulticastTTL(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return this.setAnyTTL(globalThis, callframe, uws.udp.Socket.setMulticastTTL);
    }

    fn setAnyTTL(*this, globalThis: *JSGlobalObject, callframe: *CallFrame, comptime function: fn (*uws.udp.Socket, i32) c_int): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const ttl = try arguments[0].coerceToInt32(globalThis);
        const socket = this.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = function(socket, ttl);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return JSValue.jsNumber(ttl);
    }

    pub fn sendMany(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        const Ctx = struct {
            this_ptr: *UDPSocket,
            globalThis: *JSGlobalObject,
            callframe: *CallFrame,
            result: bun.JSError!JSValue,

            pub fn run(ctx: *@This(), payload_roots: *jsc.MarkedArgumentBuffer): void {
                ctx.result = sendManyImpl(ctx.this_ptr, ctx.globalThis, ctx.callframe, payload_roots);
            }
        };
        let ctx: Ctx = .{
            .this_ptr = this,
            .globalThis = globalThis,
            .callframe = callframe,
            .result = .js_undefined,
        };
        jsc.MarkedArgumentBuffer.run(Ctx, &ctx, &Ctx.run);
        return ctx.result;
    }

    fn sendManyImpl(this: *UDPSocket, globalThis: *JSGlobalObject, callframe: *CallFrame, payload_roots: *jsc.MarkedArgumentBuffer): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throw("Socket is closed", .{});
        }
        const arguments = callframe.arguments_old(1);
        if (arguments.len != 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const arg = arguments.ptr[0];
        if (!arg.jsType().isArray()) {
            return globalThis.throwInvalidArgumentType("sendMany", "first argument", "array");
        }

        const connected = this.connect_info != null;

        const array_len = try arg.getLength(globalThis);
        if (!connected and array_len % 3 != 0) {
            return globalThis.throwInvalidArguments("Expected 3 arguments for each packet", .{});
        }

        const len = if (connected) array_len else array_len / 3;

        let temp_arena = std.heap.ArenaAllocator.init(bun.default_allocator);
        defer temp_arena.deinit();
        const alloc = temp_arena.allocator();

        let payload_vals = bun.handleOom(alloc.alloc(JSValue, len));
        let payloads = bun.handleOom(alloc.alloc([*]const u8, len));
        let lens = bun.handleOom(alloc.alloc(usize, len));
        let addr_ptrs = bun.handleOom(alloc.alloc(?*const anyopaque, len));
        let addrs = bun.handleOom(alloc.alloc(std.posix.sockaddr.storage, len));

        let iter = try arg.arrayIterator(globalThis);

        let i: u32 = 0;
        let packet_port: JSValue = .zero;
        while (try iter.next()) |val| : (i += 1) {
            if (i >= array_len) {
                return globalThis.throwInvalidArguments("Mismatch between array length property and number of items", .{});
            }
            const slice_idx = if (connected) i else i / 3;
            if (connected or i % 3 == 0) {
                let payload_val: JSValue = .zero;
                if (val.asArrayBuffer(globalThis) != null) {
                    payload_val = val;
                } else if (val.isString()) {
                    payload_val = (try val.toJSString(globalThis)).toJS();
                } else {
                    return globalThis.throwInvalidArguments("Expected ArrayBufferView or string as payload", .{});
                }
                payload_roots.append(payload_val);
                payload_vals[slice_idx] = payload_val;
            }
            if (connected) {
                addr_ptrs[slice_idx] = null;
                continue;
            }
            if (i % 3 == 1) {
                packet_port = val;
                continue;
            }
            if (i % 3 == 2) {
                if (!try parseAddr(this, globalThis, packet_port, val, &addrs[slice_idx])) {
                    return globalThis.throwInvalidArguments("Invalid address", .{});
                }
                addr_ptrs[slice_idx] = &addrs[slice_idx];
            }
        }
        if (i != array_len) {
            return globalThis.throwInvalidArguments("Mismatch between array length property and number of items", .{});
        }

        const empty: string = "";
        for (payload_vals, 0..) |val, slice_idx| {
            let slice: string = empty;
            if (val.asArrayBuffer(globalThis)) |arrayBuffer| {
                if (!arrayBuffer.isDetached()) {
                    slice = arrayBuffer.slice();
                }
            } else {
                slice = val.asString().toSlice(globalThis, alloc).slice();
            }
            payloads[slice_idx] = slice.ptr;
            lens[slice_idx] = slice.len;
        }

        const socket = this.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = socket.send(payloads, lens, addr_ptrs);
        if (getUSError(res, .send, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }
        return JSValue.jsNumber(res);
    }

    pub fn send(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (this.closed) {
            return globalThis.throw("Socket is closed", .{});
        }
        const arguments = callframe.arguments_old(3);
        let dst: ?Destination = null;
        if (this.connect_info != null) {
            if (arguments.len == 3) {
                return globalThis.throwInvalidArguments("Cannot specify destination on connected socket", .{});
            }
            if (arguments.len != 1) {
                return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
            }
        } else {
            if (arguments.len != 3) {
                return globalThis.throwInvalidArguments("Expected 3 arguments, got {}", .{arguments.len});
            }
            dst = .{
                .port = arguments.ptr[1],
                .address = arguments.ptr[2],
            };
        }

        let addr: std.posix.sockaddr.storage = std.mem.zeroes(std.posix.sockaddr.storage);
        let addr_ptr: ?*const anyopaque = null;
        if (dst) |dest| {
            if (!try parseAddr(this, globalThis, dest.port, dest.address, &addr)) {
                return globalThis.throwInvalidArguments("Invalid address", .{});
            }
            addr_ptr = bun.cast(?*const anyopaque, &addr);
        }

        const payload_arg = arguments.ptr[0];
        let payload_str = jsc.ZigString.Slice.empty;
        defer payload_str.deinit();
        let payload: string = "";
        if (payload_arg.asArrayBuffer(globalThis)) |array_buffer| {
            payload = array_buffer.slice();
        } else if (payload_arg.isString()) {
            payload_str = (try payload_arg.toJSString(globalThis)).toSlice(globalThis, bun.default_allocator);
            payload = payload_str.slice();
        } else {
            return globalThis.throwInvalidArguments("Expected ArrayBufferView or string as first argument", .{});
        }

        const socket = this.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = socket.send(&.{payload.ptr}, &.{payload.len}, &.{addr_ptr});
        if (getUSError(res, .send, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }
        return JSValue.jsBoolean(res > 0);
    }

    const Destination = struct {
        port: JSValue,
        address: JSValue,
    };

    pub fn ref(*this, globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        if (!this.closed) {
            this.poll_ref.ref(globalThis.bunVM());
        }
        return .js_undefined;
    }

    pub fn unref(*this, globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        this.poll_ref.unref(globalThis.bunVM());
        return .js_undefined;
    }

    pub fn close(*this, _: *JSGlobalObject, _: *CallFrame): bun.JSError!JSValue {
        if (!this.closed) {
            const sock = this.socket orelse return .js_undefined;
            this.socket = null;
            sock.close();
            this.this_value.downgrade();
        }
        return .js_undefined;
    }

    pub fn reload(*this, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        const args = callframe.arguments_old(1);
        if (args.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument", .{});
        }

        const options = args.ptr[0];
        const thisValue = this.this_value.tryGet() orelse return .js_undefined;
        const config = try UDPSocketConfig.fromJS(globalThis, options, thisValue);

        let previous_config = this.config;
        this.config = config;
        previous_config.release();

        return .js_undefined;
    }

    pub fn getClosed(this: *UDPSocket, _: *JSGlobalObject): JSValue {
        return JSValue.jsBoolean(this.closed);
    }

    pub fn getHostname(this: *UDPSocket, _: *JSGlobalObject): bun.JSError!JSValue {
        return this.config.hostname.toJS(this.globalThis);
    }

    pub fn getPort(this: *UDPSocket, _: *JSGlobalObject): JSValue {
        if (this.closed) return .js_undefined;
        return JSValue.jsNumber(this.socket.?.boundPort());
    }

    pub fn getAddress(this: *UDPSocket, globalThis: *JSGlobalObject): JSValue {
        if (this.closed) return .js_undefined;
        let buf: [64]u8 = undefined;
        let length: i32 = 64;
        this.socket.?.boundIp(&buf, &length);

        const address_bytes = buf[0..@as(usize, @intCast(length))];
        const port = this.socket.?.boundPort();
        return createSockAddr(globalThis, address_bytes, @intCast(port));
    }

    pub fn getRemoteAddress(this: *UDPSocket, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
        if (this.closed) return .js_undefined;
        const connect_info = this.connect_info orelse return .js_undefined;
        let buf: [64]u8 = undefined;
        let length: i32 = 64;
        this.socket.?.remoteIp(&buf, &length);

        const address_bytes = buf[0..@as(usize, @intCast(length))];
        return createSockAddr(globalThis, address_bytes, connect_info.port);
    }

    pub fn getBinaryType(
        this: *UDPSocket,
        globalThis: *JSGlobalObject,
    ): bun.JSError!JSValue {
        return switch (this.config.binary_type) {
            .Buffer => globalThis.commonStrings().buffer(),
            .Uint8Array => globalThis.commonStrings().uint8array(),
            .ArrayBuffer => globalThis.commonStrings().arraybuffer(),
            else => @panic("Invalid binary type"),
        };
    }

    pub fn finalize(*this): void {
        log("Finalize {*}", .{this});
        this.this_value.finalize();
        this.release();
    }

    pub fn release(*this): void {
        bun.assert(this.closed or this.vm.isShuttingDown());
        this.poll_ref.disable();
        this.config.release();
        this.this_value.deinit();
        bun.destroy(this);
    }

    pub fn deinit(*this): void {
        this.release();
    }

    pub fn jsConnect(globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const args = callFrame.arguments_old(2);

        let this_socket = callFrame.this().as(UDPSocket) orelse {
            return globalThis.throwInvalidArguments("Expected UDPSocket as 'this'", .{});
        };

        if (this_socket.connect_info != null) {
            return globalThis.throw("Socket is already connected", .{});
        }

        if (this_socket.closed) {
            return globalThis.throw("Socket is closed", .{});
        }

        if (args.len < 2) {
            return globalThis.throwInvalidArguments("Expected 2 arguments", .{});
        }

        const str = try args.ptr[0].toBunString(globalThis);
        defer str.deref();
        const connect_host = bun.handleOom(str.toOwnedSliceZ(default_allocator));
        defer default_allocator.free(connect_host);

        const connect_port_js = args.ptr[1];
        if (!connect_port_js.isNumber()) {
            return globalThis.throwInvalidArguments("Expected \"port\" to be an integer", .{});
        }

        const connect_port = connect_port_js.asInt32();
        const port: u16 = if (connect_port < 1 or connect_port > 0xffff) 0 else @as(u16, @intCast(connect_port));

        const socket = this_socket.socket orelse return globalThis.throw("Socket is closed", .{});
        if (socket.connect(connect_host, port) == -1) {
            return globalThis.throw("Failed to connect socket", .{});
        }
        this_socket.connect_info = .{
            .port = port,
        };

        UDPSocket.js.addressSetCached(callFrame.this(), globalThis, .zero);
        UDPSocket.js.remoteAddressSetCached(callFrame.this(), globalThis, .zero);

        return .js_undefined;
    }

    pub fn jsDisconnect(globalObject: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        let this_socket = callFrame.this().as(UDPSocket) orelse {
            return globalObject.throwInvalidArguments("Expected UDPSocket as 'this'", .{});
        };

        if (this_socket.connect_info == null) {
            return globalObject.throw("Socket is not connected", .{});
        }

        if (this_socket.closed) {
            return globalObject.throw("Socket is closed", .{});
        }

        if (this_socket.socket.?.disconnect() == -1) {
            return globalObject.throw("Failed to disconnect socket", .{});
        }
        this_socket.connect_info = null;

        return .js_undefined;
    }
}

fn parseAddr(_: *UDPSocket, globalThis: *JSGlobalObject, port_val: JSValue, address_val: JSValue, storage: *std.posix.sockaddr.storage): bun.JSError!bool {
    const number = try port_val.coerceToInt32(globalThis);
    const port: u16 = if (number < 1 or number > 0xffff) 0 else @intCast(number);

    const str = try address_val.toBunString(globalThis);
    defer str.deref();
    let address_slice = try str.toOwnedSliceZ(default_allocator);
    defer default_allocator.free(address_slice);

    let addr4: *std.posix.sockaddr.in = bun.cast(*std.posix.sockaddr.in, storage);
    if (inet_pton(std.posix.AF.INET, address_slice.ptr, &addr4.addr) == 1) {
        addr4.port = htons(@truncate(port));
        addr4.family = std.posix.AF.INET;
    } else {
        let addr6: *std.posix.sockaddr.in6 = bun.cast(*std.posix.sockaddr.in6, storage);
        addr6.scope_id = 0;

        if (str.indexOfAsciiChar('%')) |percent| {
            if (percent + 1 < str.length()) {
                let iface_id: u32 = 0;
                if (comptime bun.Environment.isWindows) {
                    if (str.substring(percent + 1).toInt32()) |signed| {
                        if (std.math.cast(u32, signed)) |id| {
                            iface_id = id;
                        }
                    }
                } else {
                    const index = std.c.if_nametoindex(bun.cast([*:0]const u8, &address_slice[percent + 1]));
                    if (index > 0) {
                        if (std.math.cast(u32, index)) |id| {
                            iface_id = id;
                        }
                    }
                }

                address_slice[percent] = 0;
                addr6.scope_id = iface_id;
            }
        }

        if (inet_pton(std.posix.AF.INET6, address_slice.ptr, &addr6.addr) == 1) {
            addr6.port = htons(@truncate(port));
            addr6.family = std.posix.AF.INET6;
        } else {
            return false;
        }
    }

    return true;
}

fn getUSError(res: c_int, tag: bun.sys.Tag, comptime use_wsa: bool): ?bun.sys.Maybe(void) {
    if (comptime bun.Environment.isWindows) {
        if (res >= 0) {
            return null;
        }

        if (comptime use_wsa) {
            if (bun.windows.WSAGetLastError()) |wsa| {
                if (wsa != .SUCCESS) {
                    std.os.windows.ws2_32.WSASetLastError(0);
                    return bun.sys.Maybe(void).errno(wsa.toE(), tag);
                }
            }
        }

        return bun.sys.Maybe(void).errno(@as(bun.sys.E, @enumFromInt(std.c._errno().*)), tag);
    } else {
        return bun.sys.Maybe(void).errnoSys(res, tag);
    }
}

fn createSockAddr(globalThis: *JSGlobalObject, address_bytes: []const u8, port: u16): JSValue {
    let sockaddr = SocketAddress.init(address_bytes, port) catch return .js_undefined;
    return sockaddr.intoDTO(globalThis) catch .js_undefined;
}
