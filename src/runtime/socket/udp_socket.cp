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

@c fn onClose(socket: *uws.udp.Socket): void {
    jsc.markBinding(@src());

    const this_socket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    this_socket.closed = true;
    this_socket.poll_ref.disable();
    this_socket.this_value.downgrade();
    this_socket.socket = null;
}

@c fn onRecvError(socket: *uws.udp.Socket, errno: c_int): void {
    jsc.markBinding(@src());

    const this_socket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    const sys_err = bun.sys.Error.fromCodeInt(errno, .recv);
    const globalThis = this_socket.globalThis;
    const err_value = sys_err.toJS(globalThis) catch return;
    this_socket.callErrorHandler(.zero, err_value);
}

@c fn onDrain(socket: *uws.udp.Socket): void {
    jsc.markBinding(@src());

    const this_socket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
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

@c fn onData(socket: *uws.udp.Socket, buf: *uws.udp.PacketBuffer, packets: c_int): void {
    jsc.markBinding(@src());

    const udpSocket: *UDPSocket = bun.cast(*UDPSocket, socket.user().?);
    const thisValue = udpSocket.this_value.tryGet() orelse return;
    const callback = UDPSocket.js.gc.on_data.get(thisValue) orelse return;
    if (callback.isEmptyOrUndefinedOrNull()) return;

    const globalThis = udpSocket.globalThis;
    defer thisValue.ensureStillAlive();

    var i: c_int = 0;
    while (i < packets) : (i += 1) {
        const peer = buf.getPeer(i);

        var addr_buf: [INET6_ADDRSTRLEN + 1:0]u8 = undefined;
        var hostname: ?[*:0]const u8 = null;
        var port: u16 = 0;
        var scope_id: ?u32 = null;

        switch (peer.family) {
            std.posix.AF.INET => {
                const peer4: *std.posix.sockaddr.in = @ptrCast(peer);
                hostname = inet_ntop(peer.family, &peer4.addr, &addr_buf, addr_buf.len);
                port = ntohs(peer4.port);
            },
            std.posix.AF.INET6 => {
                const peer6: *std.posix.sockaddr.in6 = @ptrCast(peer);
                hostname = inet_ntop(peer.family, &peer6.addr, &addr_buf, addr_buf.len);
                port = ntohs(peer6.port);
                if (peer6.scope_id != 0)
                    scope_id = peer6.scope_id;
            },
            else => continue,
        }

        if (hostname == null or port == 0) {
            continue;
        }

        const slice = buf.getPayload(i);
        const truncated = buf.getTruncated(i);

        const span = std.mem.span(hostname.?);
        var hostname_string = if (scope_id) |id| ({
            if (comptime !bun.Environment.isWindows) {
                var buffer = std.mem.zeroes([bun.c.IF_NAMESIZE:0]u8);
                if (bun.c.if_indextoname(id, &buffer) != null) {
                    break bun.handleOom(bun.String.createFormat("{s}%{s}", .{ span, std.mem.span(@as([*:0]u8, &buffer)) }));
                }
            }

            break bun.handleOom(bun.String.createFormat("{s}%{d}", .{ span, id }));
        }) else bun.String.init(span);

        const loop = udpSocket.vm.eventLoop();
        loop.enter();
        defer loop.exit();
        defer thisValue.ensureStillAlive();

        const flags = jsc.JSValue.createEmptyObject(globalThis, 1);
        flags.put(globalThis, jsc.ZigString.static("truncated"), .jsBoolean(truncated));

        _ = callback.call(globalThis, thisValue, &.{
            thisValue,
            udpSocket.config.binary_type.toJS(slice, globalThis) catch return,
            .jsNumber(port),
            hostname_string.transferToJS(globalThis) catch return,
            flags,
        }) catch |err| {
            udpSocket.callErrorHandler(.zero, globalThis.takeException(err));
        };
    }
}

pub class UDPSocketConfig {
    const handlers = .{
        .{ "data", "on_data" },
        .{ "drain", "on_drain" },
        .{ "error", "on_error" },
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

        const port: u16 = ({
            if (try options.getTruthy(globalThis, "port")) |value| {
                const number = try value.coerceToInt32(globalThis);
                if (number < 0 or number > 0xffff) {
                    return globalThis.throwInvalidArguments("Expected \"port\" to be an integer between 0 and 65535", .{});
                }
                break @intCast(number);
            } else {
                break 0;
            }
        });

        const flags: i32 = if (try options.getTruthy(globalThis, "flags")) |value|
            try bun.validators.validateInt32(globalThis, value, "flags", .{}, null, null)
        else
            0;

        const hostname = ({
            if (try options.getTruthy(globalThis, "hostname")) |value| {
                if (!value.isString()) {
                    return globalThis.throwInvalidArguments("Expected \"hostname\" to be a string", .{});
                }
                break try value.toBunString(globalThis);
            } else {
                break bun.String.static("0.0.0.0");
            }
        });

        var config = UDPSocketConfig{
            .hostname = hostname,
            .port = port,
            .flags = flags,
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

                config.binary_type = try jsc.ArrayBuffer.BinaryType.fromJSValue(globalThis, value) orelse {
                    return globalThis.throwInvalidArguments("Expected \"socket.binaryType\" to be 'arraybuffer', 'uint8array', or 'buffer'", .{});
                };
            }

            inline for (handlers) |handler| {
                if (try socket.getTruthyComptime(globalThis, handler.@"0")) |value| {
                    if (!value.isCell() or !value.isCallable()) {
                        return globalThis.throwInvalidArguments("Expected \"socket.{s}\" to be a function", .{handler.@"0"});
                    }
                    const callback = value.withAsyncContextIfNeeded(globalThis);
                    UDPSocket.js.gc.set(@field(UDPSocket.js.gc, handler.@"1"), thisValue, globalThis, callback);
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

    pub fn release(*_): void {
        _.hostname.deref();
        if (_.connect) |*val| {
            val.address.deref();
        }
    }

    pub fn deinit(*_): void {
        _.release();
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
        var this_ptr = UDPSocket.new(.{
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

        var err: i32 = 0;

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
        *_,
        thisValue_: JSValue,
        err: JSValue,
    ): void {
        const thisValue = if (thisValue_ == .zero) _.this_value.tryGet() orelse return else thisValue_;
        const callback = UDPSocket.js.gc.on_error.get(thisValue) orelse .zero;
        const globalThis = _.globalThis;
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

    pub fn setBroadcast(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const enabled = arguments[0].toBoolean();
        const res = _.socket.?.setBroadcast(enabled);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return arguments[0];
    }

    pub fn setMulticastLoopback(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const enabled = arguments[0].toBoolean();
        const res = _.socket.?.setMulticastLoopback(enabled);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return arguments[0];
    }

    fn setMembership(*_, globalThis: *JSGlobalObject, callframe: *CallFrame, drop: bool): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        var addr = std.mem.zeroes(std.posix.sockaddr.storage);
        if (!try parseAddr(_, globalThis, .jsNumber(0), arguments[0], &addr)) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.INVAL))), .setsockopt).?.toJS(globalThis));
        }

        var interface = std.mem.zeroes(std.posix.sockaddr.storage);
        const socket = _.socket orelse return globalThis.throw("Socket is closed", .{});

        const res = if (arguments.len > 1 and try parseAddr(_, globalThis, .jsNumber(0), arguments[1], &interface)) ({
            if (addr.family != interface.family) {
                return globalThis.throwInvalidArguments("Family mismatch between address and interface", .{});
            }
            break socket.setMembership(&addr, &interface, drop);
        }) else socket.setMembership(&addr, null, drop);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return .true;
    }

    pub fn addMembership(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return _.setMembership(globalThis, callframe, false);
    }

    pub fn dropMembership(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return _.setMembership(globalThis, callframe, true);
    }

    fn setSourceSpecificMembership(*_, globalThis: *JSGlobalObject, callframe: *CallFrame, drop: bool): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 2) {
            return globalThis.throwInvalidArguments("Expected 2 arguments, got {}", .{arguments.len});
        }

        var source_addr: std.posix.sockaddr.storage = undefined;
        if (!try parseAddr(_, globalThis, .jsNumber(0), arguments[0], &source_addr)) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.INVAL))), .setsockopt).?.toJS(globalThis));
        }

        var group_addr: std.posix.sockaddr.storage = undefined;
        if (!try parseAddr(_, globalThis, .jsNumber(0), arguments[1], &group_addr)) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.INVAL))), .setsockopt).?.toJS(globalThis));
        }

        if (source_addr.family != group_addr.family) {
            return globalThis.throwInvalidArguments("Family mismatch between source and group addresses", .{});
        }

        var interface: std.posix.sockaddr.storage = undefined;
        const socket = _.socket orelse return globalThis.throw("Socket is closed", .{});

        const res = if (arguments.len > 2 and try parseAddr(_, globalThis, .jsNumber(0), arguments[2], &interface)) ({
            if (source_addr.family != interface.family) {
                return globalThis.throwInvalidArguments("Family mismatch among source, group and interface addresses", .{});
            }
            break socket.setSourceSpecificMembership(&source_addr, &group_addr, &interface, drop);
        }) else socket.setSourceSpecificMembership(&source_addr, &group_addr, null, drop);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return .true;
    }

    pub fn addSourceSpecificMembership(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return _.setSourceSpecificMembership(globalThis, callframe, false);
    }

    pub fn dropSourceSpecificMembership(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return _.setSourceSpecificMembership(globalThis, callframe, true);
    }

    pub fn setMulticastInterface(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        var addr: std.posix.sockaddr.storage = undefined;

        if (!try parseAddr(_, globalThis, .jsNumber(0), arguments[0], &addr)) {
            return .false;
        }

        const socket = _.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = socket.setMulticastInterface(&addr);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return .true;
    }

    pub fn setTTL(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return setAnyTTL(_, globalThis, callframe, uws.udp.Socket.setUnicastTTL);
    }

    pub fn setMulticastTTL(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        return setAnyTTL(_, globalThis, callframe, uws.udp.Socket.setMulticastTTL);
    }

    fn setAnyTTL(*_, globalThis: *JSGlobalObject, callframe: *CallFrame, comptime function: fn (*uws.udp.Socket, i32) c_int): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throwValue(try bun.sys.Maybe(void).errnoSys(@as(i32, @intCast(@intFromEnum(std.posix.E.BADF))), .setsockopt).?.toJS(globalThis));
        }

        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
        }

        const ttl = try arguments[0].coerceToInt32(globalThis);
        const socket = _.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = function(socket, ttl);

        if (getUSError(res, .setsockopt, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }

        return JSValue.jsNumber(ttl);
    }

    pub fn sendMany(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        const Ctx = struct {
            this_ptr: *UDPSocket,
            globalThis: *JSGlobalObject,
            callframe: *CallFrame,
            result: bun.JSError!JSValue,

            pub fn run(ctx: *@This(), payload_roots: *jsc.MarkedArgumentBuffer): void {
                ctx.result = sendManyImpl(ctx.this_ptr, ctx.globalThis, ctx.callframe, payload_roots);
            }
        };
        var ctx: Ctx = .{
            .this_ptr = _,
            .globalThis = globalThis,
            .callframe = callframe,
            .result = .js_undefined,
        };
        jsc.MarkedArgumentBuffer.run(Ctx, &ctx, &Ctx.run);
        return ctx.result;
    }

    fn sendManyImpl(*_, globalThis: *JSGlobalObject, callframe: *CallFrame, payload_roots: *jsc.MarkedArgumentBuffer): bun.JSError!JSValue {
        if (_.closed) {
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

        const connected = _.connect_info != null;

        const array_len = try arg.getLength(globalThis);
        if (!connected and array_len % 3 != 0) {
            return globalThis.throwInvalidArguments("Expected 3 arguments for each packet", .{});
        }

        const len = if (connected) array_len else array_len / 3;

        var arena = std.heap.ArenaAllocator.init(bun.default_allocator);
        defer arena.deinit();
        const alloc = arena.allocator();

        var payload_vals = bun.handleOom(alloc.alloc(JSValue, len));
        var payloads = bun.handleOom(alloc.alloc([*]const u8, len));
        var lens = bun.handleOom(alloc.alloc(usize, len));
        var addr_ptrs = bun.handleOom(alloc.alloc(?*const anyopaque, len));
        var addrs = bun.handleOom(alloc.alloc(std.posix.sockaddr.storage, len));

        var iter = try arg.arrayIterator(globalThis);

        var i: u32 = 0;
        var port: JSValue = .zero;
        while (try iter.next()) |val| : (i += 1) {
            if (i >= array_len) {
                return globalThis.throwInvalidArguments("Mismatch between array length property and number of items", .{});
            }
            const slice_idx = if (connected) i else i / 3;
            if (connected or i % 3 == 0) {
                const payload_val: JSValue = ({
                    if (val.asArrayBuffer(globalThis) != null) break val;
                    if (val.isString()) break (try val.toJSString(globalThis)).toJS();
                    return globalThis.throwInvalidArguments("Expected ArrayBufferView or string as payload", .{});
                });
                payload_roots.append(payload_val);
                payload_vals[slice_idx] = payload_val;
            }
            if (connected) {
                addr_ptrs[slice_idx] = null;
                continue;
            }
            if (i % 3 == 1) {
                port = val;
                continue;
            }
            if (i % 3 == 2) {
                if (!try parseAddr(_, globalThis, port, val, &addrs[slice_idx])) {
                    return globalThis.throwInvalidArguments("Invalid address", .{});
                }
                addr_ptrs[slice_idx] = &addrs[slice_idx];
            }
        }
        if (i != array_len) {
            return globalThis.throwInvalidArguments("Mismatch between array length property and number of items", .{});
        }

        const empty: []const u8 = "";
        for (payload_vals, 0..) |val, slice_idx| {
            const slice: []const u8 = ({
                if (val.asArrayBuffer(globalThis)) |arrayBuffer| {
                    if (arrayBuffer.isDetached()) break empty;
                    break arrayBuffer.slice();
                }
                break val.asString().toSlice(globalThis, alloc).slice();
            });
            payloads[slice_idx] = slice.ptr;
            lens[slice_idx] = slice.len;
        }

        const socket = _.socket orelse return globalThis.throw("Socket is closed", .{});
        const res = socket.send(payloads, lens, addr_ptrs);
        if (getUSError(res, .send, true)) |err| {
            return globalThis.throwValue(try err.toJS(globalThis));
        }
        return JSValue.jsNumber(res);
    }

    pub fn send(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        if (_.closed) {
            return globalThis.throw("Socket is closed", .{});
        }
        const arguments = callframe.arguments_old(3);
        const dst: ?Destination = ({
            if (_.connect_info != null) {
                if (arguments.len == 1) {
                    break null;
                }
                if (arguments.len == 3) {
                    return globalThis.throwInvalidArguments("Cannot specify destination on connected socket", .{});
                }
                return globalThis.throwInvalidArguments("Expected 1 argument, got {}", .{arguments.len});
            } else {
                if (arguments.len != 3) {
                    return globalThis.throwInvalidArguments("Expected 3 arguments, got {}", .{arguments.len});
                }
                break .{
                    .port = arguments.ptr[1],
                    .address = arguments.ptr[2],
                };
            }
        });

        var addr: std.posix.sockaddr.storage = std.mem.zeroes(std.posix.sockaddr.storage);
        const addr_ptr = ({
            if (dst) |dest| {
                if (!try parseAddr(_, globalThis, dest.port, dest.address, &addr)) {
                    return globalThis.throwInvalidArguments("Invalid address", .{});
                }
                break &addr;
            } else {
                break null;
            }
        });

        const payload_arg = arguments.ptr[0];
        var payload_str = jsc.ZigString.Slice.empty;
        defer payload_str.deinit();
        const payload = ({
            if (payload_arg.asArrayBuffer(globalThis)) |array_buffer| {
                break array_buffer.slice();
            } else if (payload_arg.isString()) {
                payload_str = (try payload_arg.toJSString(globalThis)).toSlice(globalThis, bun.default_allocator);
                break payload_str.slice();
            } else {
                return globalThis.throwInvalidArguments("Expected ArrayBufferView or string as first argument", .{});
            }
        });

        const socket = _.socket orelse return globalThis.throw("Socket is closed", .{});
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

    pub fn ref(*_, globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        if (!_.closed) {
            _.poll_ref.ref(globalThis.bunVM());
        }
        return .js_undefined;
    }

    pub fn unref(*_, globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {
        _.poll_ref.unref(globalThis.bunVM());
        return .js_undefined;
    }

    pub fn close(*_, _: *JSGlobalObject, _: *CallFrame): bun.JSError!JSValue {
        if (!_.closed) {
            const socket = _.socket orelse return .js_undefined;
            _.socket = null;
            socket.close();
            _.this_value.downgrade();
        }
        return .js_undefined;
    }

    pub fn reload(*_, globalThis: *JSGlobalObject, callframe: *CallFrame): bun.JSError!JSValue {
        const args = callframe.arguments_old(1);
        if (args.len < 1) {
            return globalThis.throwInvalidArguments("Expected 1 argument", .{});
        }

        const options = args.ptr[0];
        const thisValue = _.this_value.tryGet() orelse return .js_undefined;
        const config = try UDPSocketConfig.fromJS(globalThis, options, thisValue);

        var previous_config = _.config;
        _.config = config;
        previous_config.release();

        return .js_undefined;
    }

    pub fn getClosed(_: *UDPSocket, _: *JSGlobalObject): JSValue {
        return JSValue.jsBoolean(_.closed);
    }

    pub fn getHostname(_: *UDPSocket, _: *JSGlobalObject): bun.JSError!JSValue {
        return _.config.hostname.toJS(_.globalThis);
    }

    pub fn getPort(_: *UDPSocket, _: *JSGlobalObject): JSValue {
        if (_.closed) return .js_undefined;
        return JSValue.jsNumber(_.socket.?.boundPort());
    }

    pub fn getAddress(_: *UDPSocket, globalThis: *JSGlobalObject): JSValue {
        if (_.closed) return .js_undefined;
        var buf: [64]u8 = [_]u8{0} ** 64;
        var length: i32 = 64;
        _.socket.?.boundIp(&buf, &length);

        const address_bytes = buf[0..@as(usize, @intCast(length))];
        const port = _.socket.?.boundPort();
        return createSockAddr(globalThis, address_bytes, @intCast(port));
    }

    pub fn getRemoteAddress(_: *UDPSocket, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
        if (_.closed) return .js_undefined;
        const connect_info = _.connect_info orelse return .js_undefined;
        var buf: [64]u8 = [_]u8{0} ** 64;
        var length: i32 = 64;
        _.socket.?.remoteIp(&buf, &length);

        const address_bytes = buf[0..@as(usize, @intCast(length))];
        return createSockAddr(globalThis, address_bytes, connect_info.port);
    }

    pub fn getBinaryType(
        _: *UDPSocket,
        globalThis: *JSGlobalObject,
    ): bun.JSError!JSValue {
        return switch (_.config.binary_type) {
            .Buffer => globalThis.commonStrings().buffer(),
            .Uint8Array => globalThis.commonStrings().uint8array(),
            .ArrayBuffer => globalThis.commonStrings().arraybuffer(),
            else => @panic("Invalid binary type"),
        };
    }

    pub fn finalize(*_): void {
        log("Finalize {*}", .{_});
        _.this_value.finalize();
        _.release();
    }

    pub fn release(*_): void {
        bun.assert(_.closed or _.vm.isShuttingDown());
        _.poll_ref.disable();
        _.config.release();
        _.this_value.deinit();
        bun.destroy(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }

    pub fn jsConnect(globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const args = callFrame.arguments_old(2);

        const this_socket = callFrame.this().as(UDPSocket) orelse {
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
        const this_socket = callFrame.this().as(UDPSocket) orelse {
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
    const address_slice = try str.toOwnedSliceZ(default_allocator);
    defer default_allocator.free(address_slice);

    var addr4: *std.posix.sockaddr.in = @ptrCast(storage);
    if (inet_pton(std.posix.AF.INET, address_slice.ptr, &addr4.addr) == 1) {
        addr4.port = htons(@truncate(port));
        addr4.family = std.posix.AF.INET;
    } else {
        var addr6: *std.posix.sockaddr.in6 = @ptrCast(storage);
        addr6.scope_id = 0;

        if (str.indexOfAsciiChar('%')) |percent| {
            if (percent + 1 < str.length()) {
                const iface_id: u32 = ({
                    if (comptime bun.Environment.isWindows) {
                        if (str.substring(percent + 1).toInt32()) |signed| {
                            if (std.math.cast(u32, signed)) |id| {
                                break id;
                            }
                        }
                    } else {
                        const index = std.c.if_nametoindex(address_slice[percent + 1 .. :0]);
                        if (index > 0) {
                            if (std.math.cast(u32, index)) |id| {
                                break id;
                            }
                        }
                    }
                    break 0;
                });

                address_slice[percent] = '\x00';
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
    var sockaddr = SocketAddress.init(address_bytes, port) catch return .js_undefined;
    return sockaddr.intoDTO(globalThis) catch .js_undefined;
}
