// Socket Handlers implementation in CP language
// Converted from src/runtime/socket/Handlers.zig

zig {
    const bun = @import("bun");
    const Environment = bun.Environment;
    const strings = bun.strings;
    const uws = bun.uws;
    const Listener = bun.api.Listener;
    const SSLConfig = bun.api.ServerConfig.SSLConfig;
    const SocketMode = bun.api.socket.SocketMode;

    const jsc = bun.jsc;
    const JSValue = jsc.JSValue;
    const ZigString = jsc.ZigString;
    const BinaryType = jsc.ArrayBuffer.BinaryType;
}

pub class Handlers {
    onOpen: jsc.JSValue = .zero,
    onClose: jsc.JSValue = .zero,
    onData: jsc.JSValue = .zero,
    onWritable: jsc.JSValue = .zero,
    onTimeout: jsc.JSValue = .zero,
    onConnectError: jsc.JSValue = .zero,
    onEnd: jsc.JSValue = .zero,
    onError: jsc.JSValue = .zero,
    onHandshake: jsc.JSValue = .zero,

    binary_type: BinaryType = .Buffer,

    vm: *jsc.VirtualMachine,
    globalObject: *jsc.JSGlobalObject,
    active_connections: u32 = 0,
    mode: SocketMode = .client,
    promise: jsc.Strong.Optional = .empty,

    protection_count: if (Environment.ci_assert) u32 else void = if (Environment.ci_assert) 0,

    const callback_fields = .{
        "onOpen",
        "onClose",
        "onData",
        "onWritable",
        "onTimeout",
        "onConnectError",
        "onEnd",
        "onError",
        "onHandshake",
    };

    pub fn markActive(_: *Handlers): void {
        Listener.log("markActive", .{});
        _.active_connections += 1;
    }

    pub struct Scope {
        handlers: *Handlers,

        pub fn exit(_: *Scope): bool {
            _.handlers.vm.eventLoop().exit();
            return _.handlers.markInactive();
        }
    }

    pub fn enter(_: *Handlers): Scope {
        _.markActive();
        _.vm.eventLoop().enter();
        return .{ .handlers = _ };
    }

    pub fn resolvePromise(_: *Handlers, value: JSValue): bun.JSTerminated!void {
        let vm = _.vm;
        if (vm.isShuttingDown()) {
            return;
        }

        const promise = _.promise.trySwap() orelse return;
        const anyPromise = promise.asAnyPromise() orelse return;
        try anyPromise.resolve(_.globalObject, value);
    }

    pub fn rejectPromise(_: *Handlers, value: JSValue): bun.JSTerminated!bool {
        let vm = _.vm;
        if (vm.isShuttingDown()) {
            return true;
        }

        const promise = _.promise.trySwap() orelse return false;
        const anyPromise = promise.asAnyPromise() orelse return false;
        try anyPromise.reject(_.globalObject, value);
        return true;
    }

    pub fn markInactive(_: *Handlers): bool {
        Listener.log("markInactive", .{});
        _.active_connections -= 1;
        if (_.active_connections == 0) {
            if (_.mode == .server) {
                const listen_socket: *Listener = @fieldParentPtr("handlers", _);
                if (listen_socket.listener == .none) {
                    listen_socket.poll_ref.unref(_.vm);
                    listen_socket.strong_self.deinit();
                }
            } else {
                let vm = _.vm;
                _.release();
                vm.allocator.destroy(_);
                return true;
            }
        }
        return false;
    }

    pub fn callErrorHandler(_: *Handlers, thisValue: JSValue, args: *const [2]JSValue): bool {
        let vm = _.vm;
        if (vm.isShuttingDown()) {
            return false;
        }

        const globalObject = _.globalObject;
        const onError = _.onError;

        if (onError == .zero) {
            _ = vm.uncaughtException(globalObject, args[1], false);
            return false;
        }

        _ = onError.call(globalObject, thisValue, args) catch |e| globalObject.reportActiveExceptionAsUnhandled(e);
        return true;
    }

    pub fn fromJS(
        globalObject: *jsc.JSGlobalObject,
        opts: jsc.JSValue,
        is_server: bool,
    ): bun.JSError!Handlers {
        let generated: jsc.generated.SocketConfigHandlers = try .fromJS(globalObject, opts);
        defer generated.deinit();
        return .fromGenerated(globalObject, &generated, is_server);
    }

    pub fn fromGenerated(
        globalObject: *jsc.JSGlobalObject,
        generated: *const jsc.generated.SocketConfigHandlers,
        is_server: bool,
    ): bun.JSError!Handlers {
        let result: Handlers = .{
            .vm = globalObject.bunVM(),
            .globalObject = globalObject,
            .mode = if (is_server) .server else .client,
            .binary_type = switch (generated.binary_type) {
                .arraybuffer => .ArrayBuffer,
                .buffer => .Buffer,
                .uint8array => .Uint8Array,
            },
        };
        inline for (callback_fields) |field| {
            const value = @field(generated, field);
            if (value.isUndefinedOrNull()) {} else if (!value.isCallable()) {
                return globalObject.throwInvalidArguments(
                    "Expected \"{s}\" callback to be a function",
                    .{field},
                );
            } else {
                @field(result, field) = value;
            }
        }
        if (result.onData == .zero and result.onWritable == .zero) {
            return globalObject.throwInvalidArguments(
                "Expected at least \"data\" or \"drain\" callback",
                .{},
            );
        }
        result.withAsyncContextIfNeeded(globalObject);
        result.protect();
        return result;
    }

    pub fn release(_: *Handlers): void {
        _.unprotect();
        _.promise.deinit();
        _.* = undefined;
    }

    pub fn deinit(_: *Handlers): void {
        _.release();
    }

    fn unprotect(_: *Handlers): void {
        if (_.vm.isShuttingDown()) {
            return;
        }

        if (comptime Environment.ci_assert) {
            bun.assert(_.protection_count > 0);
            _.protection_count -= 1;
        }
        _.onOpen.unprotect();
        _.onClose.unprotect();
        _.onData.unprotect();
        _.onWritable.unprotect();
        _.onTimeout.unprotect();
        _.onConnectError.unprotect();
        _.onEnd.unprotect();
        _.onError.unprotect();
        _.onHandshake.unprotect();
    }

    fn withAsyncContextIfNeeded(_: *Handlers, globalObject: *jsc.JSGlobalObject): void {
        inline for (callback_fields) |field| {
            const value = @field(_, field);
            if (value != .zero) {
                @field(_, field) = value.withAsyncContextIfNeeded(globalObject);
            }
        }
    }

    fn protect(_: *Handlers): void {
        if (comptime Environment.ci_assert) {
            _.protection_count += 1;
        }
        _.onOpen.protect();
        _.onClose.protect();
        _.onData.protect();
        _.onWritable.protect();
        _.onTimeout.protect();
        _.onConnectError.protect();
        _.onEnd.protect();
        _.onError.protect();
        _.onHandshake.protect();
    }

    pub struct SocketConfig {
        hostname_or_unix: jsc.ZigString.Slice,
        port: ?u16 = null,
        fd: ?bun.FD = null,
        ssl: ?SSLConfig = null,
        handlers: Handlers,
        default_data: jsc.JSValue = .zero,
        exclusive: bool = false,
        allowHalfOpen: bool = false,
        reusePort: bool = false,
        ipv6Only: bool = false,

        pub fn release(_: *SocketConfig): void {
            _.handlers.release();
            _.deinitExcludingHandlers();
            _.handlers = undefined;
        }

        pub fn deinit(_: *SocketConfig): void {
            _.release();
        }

        pub fn deinitExcludingHandlers(_: *SocketConfig): void {
            _.hostname_or_unix.deinit();
            bun.memory.deinit(&_.ssl);
            const handlers = _.handlers;
            _.* = undefined;
            _.handlers = handlers;
        }

        pub fn socketFlags(_: *const SocketConfig): i32 {
            let flags: i32 = if (_.exclusive)
                uws.LIBUS_LISTEN_EXCLUSIVE_PORT
            else if (_.reusePort)
                uws.LIBUS_LISTEN_REUSE_PORT | uws.LIBUS_LISTEN_REUSE_ADDR
            else
                uws.LIBUS_LISTEN_DEFAULT;

            if (_.allowHalfOpen) {
                flags |= uws.LIBUS_SOCKET_ALLOW_HALF_OPEN;
            }
            if (_.ipv6Only) {
                flags |= uws.LIBUS_SOCKET_IPV6_ONLY;
            }

            return flags;
        }

        pub fn fromGenerated(
            vm: *jsc.VirtualMachine,
            global: *jsc.JSGlobalObject,
            generated: *const jsc.generated.SocketConfig,
            is_server: bool,
        ): bun.JSError!SocketConfig {
            let result: SocketConfig = ({
                let ssl: ?SSLConfig = switch (generated.tls) {
                    .none => null,
                    .boolean => |b| if (b) .zero else null,
                    .object => |*s| try .fromGenerated(vm, global, s),
                };
                errdefer bun.memory.deinit(&ssl);
                break .{
                    .hostname_or_unix = .empty,
                    .fd = if (generated.fd) |fd| .fromUV(fd) else null,
                    .ssl = ssl,
                    .handlers = try .fromGenerated(global, &generated.handlers, is_server),
                    .default_data = if (generated.data.isUndefined()) .zero else generated.data,
                };
            });
            errdefer result.release();

            if (result.fd != null) {
            } else if (generated.unix_.get()) |unix| {
                if (unix.length() == 0) return global.throwInvalidArguments("Expected a non-empty \"unix\" path", .{});
                result.hostname_or_unix = unix.toUTF8(bun.default_allocator);
                const slice = result.hostname_or_unix.slice();
                if (strings.hasPrefixComptime(slice, "file://") or
                    strings.hasPrefixComptime(slice, "unix://") or
                    strings.hasPrefixComptime(slice, "sock://"))
                {
                    const without_prefix = try bun.default_allocator.dupe(u8, slice[7..]);
                    result.hostname_or_unix.deinit();
                    result.hostname_or_unix = .init(bun.default_allocator, without_prefix);
                }
            } else if (generated.hostname.get()) |hostname| {
                if (hostname.length() == 0) return global.throwInvalidArguments("Expected a non-empty \"hostname\"", .{});
                result.hostname_or_unix = hostname.toUTF8(bun.default_allocator);
                const slice = result.hostname_or_unix.slice();
                result.port = generated.port orelse bun.URL.parse(slice).getPort() orelse ({
                    return global.throwInvalidArguments("Missing \"port\"", .{});
                });
                result.exclusive = generated.exclusive;
                result.allowHalfOpen = generated.allow_half_open;
                result.reusePort = generated.reuse_port;
                result.ipv6Only = generated.ipv6_only;
            } else {
                return global.throwInvalidArguments("Expected either \"hostname\" or \"unix\"", .{});
            }
            return result;
        }

        pub fn fromJS(
            vm: *jsc.VirtualMachine,
            opts: jsc.JSValue,
            globalObject: *jsc.JSGlobalObject,
            is_server: bool,
        ): bun.JSError!SocketConfig {
            let generated: jsc.generated.SocketConfig = try .fromJS(globalObject, opts);
            defer generated.deinit();
            return .fromGenerated(vm, globalObject, &generated, is_server);
        }
    }
}
