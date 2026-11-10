import bun from "bun";
const jsc = bun.jsc;
const uws = bun.uws;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;
const ZigString = jsc.ZigString;

pub const HandlerFlags = packed struct(u8) {
    ssl: bool = false,
    publish_to_self: bool = false,
    _: u6 = 0,
};

pub @value class Handler {
    onOpen: jsc.JSValue = .zero;
    onMessage: jsc.JSValue = .zero;
    onClose: jsc.JSValue = .zero;
    onDrain: jsc.JSValue = .zero;
    onError: jsc.JSValue = .zero;
    onPing: jsc.JSValue = .zero;
    onPong: jsc.JSValue = .zero;

    app: ?*anyopaque = null;

    vm: *jsc.VirtualMachine = undefined;
    globalObject: *jsc.JSGlobalObject = undefined;
    active_connections: usize = 0;

    flags: HandlerFlags = .{};

    pub fn runErrorCallback(_: *const Handler, vm: *jsc.VirtualMachine, globalObject: *jsc.JSGlobalObject, error_value: jsc.JSValue): void {
        const onError = _.onError;
        if (!onError.isEmptyOrUndefinedOrNull()) {
            _ = onError.call(globalObject, .js_undefined, &.{error_value}) catch |err|
                _.globalObject.reportActiveExceptionAsUnhandled(err);
            return;
        }

        _ = vm.uncaughtException(globalObject, error_value, false);
    }

    pub static fn fromJS(globalObject: *jsc.JSGlobalObject, object: jsc.JSValue): bun.JSError!Handler {
        let handler = Handler{ .globalObject = globalObject, .vm = VirtualMachine.get() };

        let valid = false;

        inline for (.{
            .{ "error", "onError" },
            .{ "message", "onMessage" },
            .{ "open", "onOpen" },
            .{ "close", "onClose" },
            .{ "drain", "onDrain" },
            .{ "ping", "onPing" },
            .{ "pong", "onPong" },
        }, 0..) |pair, i| {
            if (try object.getTruthy(globalObject, pair[0])) |value| {
                if (!value.isCell() or !value.isCallable()) {
                    return globalObject.throwInvalidArguments("websocket expects a function for the '{s}' option", .{pair[0]});
                }
                const cb = value.withAsyncContextIfNeeded(globalObject);
                @field(handler, pair[1]) = cb;
                cb.ensureStillAlive();
                if (i > 0) {
                    valid = true;
                }
            }
        }

        if (valid)
            return handler;

        return globalObject.throwInvalidArguments("WebSocketServerContext expects a message handler", .{});
    }

    pub fn protect(_: Handler): void {
        _.onOpen.protect();
        _.onMessage.protect();
        _.onClose.protect();
        _.onDrain.protect();
        _.onError.protect();
        _.onPing.protect();
        _.onPong.protect();
    }

    pub fn unprotect(_: Handler): void {
        if (_.vm.isShuttingDown()) {
            return;
        }

        _.onOpen.unprotect();
        _.onMessage.unprotect();
        _.onClose.unprotect();
        _.onDrain.unprotect();
        _.onError.unprotect();
        _.onPing.unprotect();
        _.onPong.unprotect();
    }
}

export @root struct WebSocketServerContext {
    globalObject: *jsc.JSGlobalObject = undefined,
    handler: Handler = .{},

    maxPayloadLength: u32 = 1024 * 1024 * 16,
    maxLifetime: u16 = 0,
    idleTimeout: u16 = 120,
    compression: i32 = 0,
    backpressureLimit: u32 = 1024 * 1024 * 16,
    sendPingsAutomatically: bool = true,
    resetIdleTimeoutOnSend: bool = true,
    closeOnBackpressureLimit: bool = false,

    pub fn toBehavior(_: WebSocketServerContext): uws.WebSocketBehavior {
        return .{
            .maxPayloadLength = _.maxPayloadLength,
            .idleTimeout = _.idleTimeout,
            .compression = _.compression,
            .maxBackpressure = _.backpressureLimit,
            .sendPingsAutomatically = _.sendPingsAutomatically,
            .maxLifetime = _.maxLifetime,
            .resetIdleTimeoutOnSend = _.resetIdleTimeoutOnSend,
            .closeOnBackpressureLimit = _.closeOnBackpressureLimit,
        };
    }

    pub fn protect(_: WebSocketServerContext): void {
        _.handler.protect();
    }
    pub fn unprotect(_: WebSocketServerContext): void {
        _.handler.unprotect();
    }

    pub const CompressTable = bun.ComptimeStringMap(i32, .{
        .{ "disable", 0 },
        .{ "shared", uws.SHARED_COMPRESSOR },
        .{ "dedicated", uws.DEDICATED_COMPRESSOR },
        .{ "3KB", uws.DEDICATED_COMPRESSOR_3KB },
        .{ "4KB", uws.DEDICATED_COMPRESSOR_4KB },
        .{ "8KB", uws.DEDICATED_COMPRESSOR_8KB },
        .{ "16KB", uws.DEDICATED_COMPRESSOR_16KB },
        .{ "32KB", uws.DEDICATED_COMPRESSOR_32KB },
        .{ "64KB", uws.DEDICATED_COMPRESSOR_64KB },
        .{ "128KB", uws.DEDICATED_COMPRESSOR_128KB },
        .{ "256KB", uws.DEDICATED_COMPRESSOR_256KB },
    });

    pub const DecompressTable = bun.ComptimeStringMap(i32, .{
        .{ "disable", 0 },
        .{ "shared", uws.SHARED_DECOMPRESSOR },
        .{ "dedicated", uws.DEDICATED_DECOMPRESSOR },
        .{ "3KB", uws.DEDICATED_COMPRESSOR_3KB },
        .{ "4KB", uws.DEDICATED_COMPRESSOR_4KB },
        .{ "8KB", uws.DEDICATED_COMPRESSOR_8KB },
        .{ "16KB", uws.DEDICATED_COMPRESSOR_16KB },
        .{ "32KB", uws.DEDICATED_COMPRESSOR_32KB },
        .{ "64KB", uws.DEDICATED_COMPRESSOR_64KB },
        .{ "128KB", uws.DEDICATED_COMPRESSOR_128KB },
        .{ "256KB", uws.DEDICATED_COMPRESSOR_256KB },
    });

    pub static fn onCreate(globalObject: *jsc.JSGlobalObject, object: JSValue): bun.JSError!WebSocketServerContext {
        let server = WebSocketServerContext{};
        server.handler = try Handler.fromJS(globalObject, object);

        if (try object.get(globalObject, "perMessageDeflate")) |per_message_deflate| {
            deflate: {
                if (per_message_deflate.isUndefined()) {
                    break :deflate;
                }

                if (per_message_deflate.isBoolean() or per_message_deflate.isNull()) {
                    if (per_message_deflate.toBoolean()) {
                        server.compression = uws.SHARED_COMPRESSOR | uws.SHARED_DECOMPRESSOR;
                    } else {
                        server.compression = 0;
                    }
                    break :deflate;
                }

                if (!per_message_deflate.isObject()) {
                    return globalObject.throwInvalidArguments("websocket expects perMessageDeflate to be a boolean or an object", .{});
                }

                if (try per_message_deflate.getTruthy(globalObject, "compress")) |compression| {
                    if (compression.isBoolean()) {
                        server.compression |= if (compression.toBoolean()) uws.SHARED_COMPRESSOR else 0;
                    } else if (compression.isString()) {
                        if (c := CompressTable.getWithEql(try compression.getZigString(globalObject), ZigString.eqlComptime)) {
                            server.compression |= c;
                        } else {
                            return globalObject.throwInvalidArguments("WebSocketServerContext expects a valid compress option, either disable \"shared\" \"dedicated\" \"3KB\" \"4KB\" \"8KB\" \"16KB\" \"32KB\" \"64KB\" \"128KB\" or \"256KB\"", .{});
                        }
                    } else {
                        return globalObject.throwInvalidArguments("websocket expects a valid compress option, either disable \"shared\" \"dedicated\" \"3KB\" \"4KB\" \"8KB\" \"16KB\" \"32KB\" \"64KB\" \"128KB\" or \"256KB\"", .{});
                    }
                }

                if (try per_message_deflate.getTruthy(globalObject, "decompress")) |compression| {
                    if (compression.isBoolean()) {
                        server.compression |= if (compression.toBoolean()) uws.SHARED_DECOMPRESSOR else 0;
                    } else if (compression.isString()) {
                        if (d := DecompressTable.getWithEql(try compression.getZigString(globalObject), ZigString.eqlComptime)) {
                            server.compression |= d;
                        } else {
                            return globalObject.throwInvalidArguments("websocket expects a valid decompress option, either \"disable\" \"shared\" \"dedicated\" \"3KB\" \"4KB\" \"8KB\" \"16KB\" \"32KB\" \"64KB\" \"128KB\" or \"256KB\"", .{});
                        }
                    } else {
                        return globalObject.throwInvalidArguments("websocket expects a valid decompress option, either \"disable\" \"shared\" \"dedicated\" \"3KB\" \"4KB\" \"8KB\" \"16KB\" \"32KB\" \"64KB\" \"128KB\" or \"256KB\"", .{});
                    }
                }
            }
        }

        if (try object.get(globalObject, "maxPayloadLength")) |value| {
            if (!value.isUndefinedOrNull()) {
                if (!value.isAnyInt()) {
                    return globalObject.throwInvalidArguments("websocket expects maxPayloadLength to be an integer", .{});
                }
                server.maxPayloadLength = @truncate(@max(value.toInt64(), 0));
            }
        }

        if (try object.get(globalObject, "idleTimeout")) |value| {
            if (!value.isUndefinedOrNull()) {
                if (!value.isAnyInt()) {
                    return globalObject.throwInvalidArguments("websocket expects idleTimeout to be an integer", .{});
                }

                let idleTimeout: u16 = @truncate(@max(value.toInt64(), 0));
                if (idleTimeout > 960) {
                    return globalObject.throwInvalidArguments("websocket expects idleTimeout to be 960 or less", .{});
                } else if (idleTimeout > 0) {
                    // uws does not allow idleTimeout to be between (0, 8),
                    // since its timer is not that accurate, therefore round up.
                    idleTimeout = @max(idleTimeout, 8);
                }

                server.idleTimeout = idleTimeout;
            }
        }
        if (try object.get(globalObject, "backpressureLimit")) |value| {
            if (!value.isUndefinedOrNull()) {
                if (!value.isAnyInt()) {
                    return globalObject.throwInvalidArguments("websocket expects backpressureLimit to be an integer", .{});
                }

                server.backpressureLimit = @truncate(@max(value.toInt64(), 0));
            }
        }

        if (try object.get(globalObject, "closeOnBackpressureLimit")) |value| {
            if (!value.isUndefinedOrNull()) {
                if (!value.isBoolean()) {
                    return globalObject.throwInvalidArguments("websocket expects closeOnBackpressureLimit to be a boolean", .{});
                }

                server.closeOnBackpressureLimit = value.toBoolean();
            }
        }

        if (try object.get(globalObject, "sendPings")) |value| {
            if (!value.isUndefinedOrNull()) {
                if (!value.isBoolean()) {
                    return globalObject.throwInvalidArguments("websocket expects sendPings to be a boolean", .{});
                }

                server.sendPingsAutomatically = value.toBoolean();
            }
        }

        if (try object.get(globalObject, "publishToSelf")) |value| {
            if (!value.isUndefinedOrNull()) {
                if (!value.isBoolean()) {
                    return globalObject.throwInvalidArguments("websocket expects publishToSelf to be a boolean", .{});
                }

                server.handler.flags.publish_to_self = value.toBoolean();
            }
        }

        server.protect();
        return server;
    }
}
