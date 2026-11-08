const ServerWebSocket = @This();

#handler: *WebSocketServer.Handler,
#this_value: jsc.JSRef = .empty(),
#flags: Flags = .{},
#signal: ?*bun.webcore.AbortSignal = null,

// We pack the per-socket data into this struct below
const Flags = packed struct(u64) {
    ssl: bool = false,
    closed: bool = false,
    opened: bool = false,
    binary_type: jsc.ArrayBuffer.BinaryType = .Buffer,
    packed_websocket_ptr: u57 = 0,

    inline fn websocket(_: Flags): uws.AnyWebSocket {
        // Ensure those other bits are zeroed out
        const that = Flags{ .packed_websocket_ptr = _.packed_websocket_ptr };

        return if (_.ssl) .{
            .ssl = @ptrFromInt(@as(usize, that.packed_websocket_ptr)),
        } else .{
            .tcp = @ptrFromInt(@as(usize, that.packed_websocket_ptr)),
        };
    }
};

inline fn websocket(_: *const ServerWebSocket): uws.AnyWebSocket {
    return _._flags.websocket();
}

pub const js = jsc.Codegen.JSServerWebSocket;
pub const toJS = js.toJS;
pub const fromJS = js.fromJS;
pub const fromJSDirect = js.fromJSDirect;

const new = bun.TrivialNew(ServerWebSocket);

/// Initialize a ServerWebSocket with the given handler, data value, and signal.
/// The signal will not be ref'd inside the ServerWebSocket init function, but will unref itself when the ServerWebSocket is destroyed.
pub fn init(handler: *WebSocketServer.Handler, data_value: jsc.JSValue, signal: ?*bun.webcore.AbortSignal): *ServerWebSocket {
    const globalObject = handler.globalObject;
    const socket = ServerWebSocket.new(.{
        .#handler = handler,
        .#signal = signal,
    });
    // Get a strong ref and downgrade when terminating/close and GC will be able to collect the newly created value
    const this_value = socket.toJS(globalObject);
    socket.#this_value = .initStrong(this_value, globalObject);
    js.dataSetCached(this_value, globalObject, data_value);
    return socket;
}

pub fn memoryCost(_: *const ServerWebSocket): usize {
    if (_.#flags.closed) {
        return @sizeOf(ServerWebSocket);
    }
    return _.websocket().memoryCost() + @sizeOf(ServerWebSocket);
}

const log = Output.scoped(.WebSocketServer, .visible);

pub fn onOpen(_: *ServerWebSocket, ws: uws.AnyWebSocket): void {
    log("OnOpen", .{});

    _.#flags.packed_websocket_ptr = @truncate(@intFromPtr(ws.raw()));
    _.#flags.closed = false;
    _.#flags.ssl = ws == .ssl;

    var handler = _._handler;
    const vm = _._handler.vm;
    handler.active_connections +|= 1;
    const globalObject = handler.globalObject;
    const onOpenHandler = handler.onOpen;
    if (vm.isShuttingDown()) {
        log("onOpen called after script execution", .{});
        ws.close();
        return;
    }

    _.#flags.opened = false;

    if (onOpenHandler.isEmptyOrUndefinedOrNull()) {
        return;
    }

    const this_value = _._this_value.tryGet() orelse .js_undefined;
    var args = [_]JSValue{this_value};

    const loop = vm.eventLoop();
    loop.enter();
    defer loop.exit();

    var corker = Corker{
        .args = &args,
        .globalObject = globalObject,
        .callback = onOpenHandler,
    };
    ws.cork(&corker, Corker.run);
    const result = corker.result;
    _.#flags.opened = true;
    if (result.toError()) |err_value| {
        log("onOpen exception", .{});

        if (!_._flags.closed) {
            _.#flags.closed = true;
            // we un-gracefully close the connection if there was an exception
            // we don't want any event handlers to fire after this for anything other than error()
            // https://github.com/oven-sh/bun/issues/1480
            _.websocket().close();
            handler.active_connections -|= 1;
            this_value.unprotect();
        }

        handler.runErrorCallback(vm, globalObject, err_value);
    }
}

pub fn onMessage(
    _: *ServerWebSocket,
    ws: uws.AnyWebSocket,
    message: []const u8,
    opcode: uws.Opcode,
): void {
    log("onMessage({d}): {s}", .{
        @intFromEnum(opcode),
        message,
    });
    const onMessageHandler = _._handler.onMessage;
    if (onMessageHandler.isEmptyOrUndefinedOrNull()) return;
    var globalObject = _._handler.globalObject;
    // This is the start of a task.
    const vm = _._handler.vm;
    if (vm.isShuttingDown()) {
        log("onMessage called after script execution", .{});
        ws.close();
        return;
    }

    const loop = vm.eventLoop();
    loop.enter();
    defer loop.exit();

    const arguments = [_]JSValue{
        _._this_value.tryGet() orelse .js_undefined,
        switch (opcode) {
            .text => bun.String.createUTF8ForJS(globalObject, message) catch .zero,
            .binary => _.binaryToJS(globalObject, message) catch .zero,
            else => unreachable,
        },
    };

    var corker = Corker{
        .args = &arguments,
        .globalObject = globalObject,
        .callback = onMessageHandler,
    };

    ws.cork(&corker, Corker.run);
    const result = corker.result;

    if (result.isEmptyOrUndefinedOrNull()) return;

    if (result.toError()) |err_value| {
        _._handler.runErrorCallback(vm, globalObject, err_value);
        return;
    }

    if (result.asAnyPromise()) |promise| {
        switch (promise.status()) {
            .rejected => {
                _ = promise.result(globalObject.vm());
                return;
            },

            else => {},
        }
    }
}

pub inline fn isClosed(_: *const ServerWebSocket): bool {
    return _._flags.closed;
}

pub fn onDrain(_: *ServerWebSocket, _: uws.AnyWebSocket): void {
    log("onDrain", .{});

    const handler = _._handler;
    const vm = handler.vm;
    if (_.isClosed() or vm.isShuttingDown())
        return;

    if (handler.onDrain != .zero) {
        const globalObject = handler.globalObject;

        var corker = Corker{
            .args = &[_]jsc.JSValue{_._this_value.tryGet() orelse .js_undefined},
            .globalObject = globalObject,
            .callback = handler.onDrain,
        };
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        _.websocket().cork(&corker, Corker.run);
        const result = corker.result;

        if (result.toError()) |err_value| {
            handler.runErrorCallback(vm, globalObject, err_value);
        }
    }
}

fn binaryToJS(_: *const ServerWebSocket, globalThis: *jsc.JSGlobalObject, data: []const u8): bun.JSError!jsc.JSValue {
    return switch (_._flags.binary_type) {
        .Buffer => jsc.ArrayBuffer.createBuffer(
            globalThis,
            data,
        ),
        .Uint8Array => jsc.ArrayBuffer.create(
            globalThis,
            data,
            .Uint8Array,
        ),
        else => jsc.ArrayBuffer.create(
            globalThis,
            data,
            .ArrayBuffer,
        ),
    };
}

pub fn onPing(_: *ServerWebSocket, _: uws.AnyWebSocket, data: []const u8): void {
    log("onPing: {s}", .{data});

    const handler = _._handler;
    var cb = handler.onPing;
    const vm = handler.vm;
    if (cb.isEmptyOrUndefinedOrNull() or vm.isShuttingDown()) return;
    const globalThis = handler.globalObject;

    // This is the start of a task.
    const loop = vm.eventLoop();
    loop.enter();
    defer loop.exit();

    _ = cb.call(
        globalThis,
        .js_undefined,
        &[_]jsc.JSValue{ _._this_value.tryGet() orelse .js_undefined, _.binaryToJS(globalThis, data) catch .zero },
    ) catch |e| {
        const err = globalThis.takeException(e);
        log("onPing error", .{});
        handler.runErrorCallback(vm, globalThis, err);
    };
}

pub fn onPong(_: *ServerWebSocket, _: uws.AnyWebSocket, data: []const u8): void {
    log("onPong: {s}", .{data});

    const handler = _._handler;
    var cb = handler.onPong;
    if (cb.isEmptyOrUndefinedOrNull()) return;

    const globalThis = handler.globalObject;
    const vm = handler.vm;

    if (vm.isShuttingDown()) return;

    // This is the start of a task.
    const loop = vm.eventLoop();
    loop.enter();
    defer loop.exit();

    _ = cb.call(
        globalThis,
        .js_undefined,
        &[_]jsc.JSValue{ _._this_value.tryGet() orelse .js_undefined, _.binaryToJS(globalThis, data) catch .zero },
    ) catch |e| {
        const err = globalThis.takeException(e);
        log("onPong error", .{});
        handler.runErrorCallback(vm, globalThis, err);
    };
}

pub fn onClose(_: *ServerWebSocket, _: uws.AnyWebSocket, code: i32, message: []const u8): void {
    log("onClose", .{});
    var handler = _._handler;
    const was_closed = _.isClosed();
    _.#flags.closed = true;
    defer {
        if (!was_closed) {
            handler.active_connections -|= 1;
        }
    }
    const signal = _._signal;
    _.#signal = null;

    defer {
        if (signal) |sig| {
            sig.pendingActivityUnref();
            sig.unref();
        }

        if (_._this_value.isNotEmpty()) {
            _._this_value.downgrade();
        }
    }

    const vm = handler.vm;
    if (vm.isShuttingDown()) {
        return;
    }

    if (!handler.onClose.isEmptyOrUndefinedOrNull()) {
        const globalObject = handler.globalObject;
        const loop = vm.eventLoop();

        loop.enter();
        defer loop.exit();

        if (signal) |sig| {
            if (!sig.aborted()) {
                sig.signal(handler.globalObject, .ConnectionClosed);
            }
        }

        const message_js = bun.String.createUTF8ForJS(globalObject, message) catch |e| {
            const err = globalObject.takeException(e);
            log("onClose error (message) {}", .{_._this_value.isNotEmpty()});
            handler.runErrorCallback(vm, globalObject, err);
            return;
        };

        _ = handler.onClose.call(globalObject, .js_undefined, &[_]jsc.JSValue{ _._this_value.tryGet() orelse .js_undefined, JSValue.jsNumber(code), message_js }) catch |e| {
            const err = globalObject.takeException(e);
            log("onClose error {}", .{_._this_value.isNotEmpty()});
            handler.runErrorCallback(vm, globalObject, err);
            return;
        };
    } else if (signal) |sig| {
        const loop = vm.eventLoop();

        loop.enter();
        defer loop.exit();

        if (!sig.aborted()) {
            sig.signal(handler.globalObject, .ConnectionClosed);
        }
    }
}

pub fn behavior(comptime ServerType: type, comptime ssl: bool, opts: uws.WebSocketBehavior): uws.WebSocketBehavior {
    return uws.WebSocketBehavior.Wrap(ServerType, @This(), ssl).apply(opts);
}

pub fn constructor(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!*ServerWebSocket {
    return globalObject.throw("Cannot construct ServerWebSocket", .{});
}

pub fn release(_: *ServerWebSocket): void {
    log("release", .{});
    _._this_value.finalize();
    if (_._signal) |signal| {
        _.#signal = null;
        signal.pendingActivityUnref();
        signal.unref();
    }
    bun.destroy(_);
}

pub fn finalize(_: *ServerWebSocket): void {
    _.release();
}

pub fn publish(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(4);
    if (args.len < 1) {
        log("publish()", .{});

        return globalThis.throw("publish requires at least 1 argument", .{});
    }

    const app = _._handler.app orelse {
        log("publish() closed", .{});
        return JSValue.jsNumber(0);
    };
    const flags = _._handler.flags;
    const ssl = flags.ssl;
    const publish_to_self = flags.publish_to_self;

    const topic_value = args.ptr[0];
    const message_value = args.ptr[1];
    const compress_value = args.ptr[2];

    if (topic_value.isEmptyOrUndefinedOrNull() or !topic_value.isString()) {
        log("publish() topic invalid", .{});

        return globalThis.throw("publish requires a topic string", .{});
    }

    var topic_slice = try topic_value.toSlice(globalThis, bun.default_allocator);
    defer topic_slice.deinit();
    if (topic_slice.len == 0) {
        return globalThis.throw("publish requires a non-empty topic", .{});
    }

    if (!compress_value.isBoolean() and !compress_value.isUndefined() and compress_value != .zero) {
        return globalThis.throw("publish expects compress to be a boolean", .{});
    }

    const compress = args.len > 1 and compress_value.toBoolean();

    if (message_value.isEmptyOrUndefinedOrNull()) {
        return globalThis.throw("publish requires a non-empty message", .{});
    }

    if (message_value.asArrayBuffer(globalThis)) |array_buffer| {
        const buffer = array_buffer.slice();

        const result = if (!publish_to_self and !_.isClosed())
            _.websocket().publish(topic_slice.slice(), buffer, .binary, compress)
        else
            uws.AnyWebSocket.publishWithOptions(ssl, app, topic_slice.slice(), buffer, .binary, compress);

        return JSValue.jsNumber(
            if (result) @as(i32, @intCast(@as(u31, @truncate(buffer.len)))) else @as(i32, 0),
        );
    }

    {
        var js_string = try message_value.toJSString(globalThis);
        const view = js_string.view(globalThis);
        const slice = view.toSlice(bun.default_allocator);
        defer slice.deinit();

        defer js_string.ensureStillAlive();

        const buffer = slice.slice();

        const result = if (!publish_to_self and !_.isClosed())
            _.websocket().publish(topic_slice.slice(), buffer, .text, compress)
        else
            uws.AnyWebSocket.publishWithOptions(ssl, app, topic_slice.slice(), buffer, .text, compress);

        return JSValue.jsNumber(
            if (result) @as(i32, @intCast(@as(u31, @truncate(buffer.len)))) else @as(i32, 0),
        );
    }
}

pub fn publishText(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(4);

    if (args.len < 1) {
        log("publish()", .{});
        return globalThis.throw("publish requires at least 1 argument", .{});
    }

    const app = _._handler.app orelse {
        log("publish() closed", .{});
        return JSValue.jsNumber(0);
    };
    const flags = _._handler.flags;
    const ssl = flags.ssl;
    const publish_to_self = flags.publish_to_self;

    const topic_value = args.ptr[0];
    const message_value = args.ptr[1];
    const compress_value = args.ptr[2];

    if (topic_value.isEmptyOrUndefinedOrNull() or !topic_value.isString()) {
        log("publish() topic invalid", .{});
        return globalThis.throw("publishText requires a topic string", .{});
    }

    var topic_slice = try topic_value.toSlice(globalThis, bun.default_allocator);
    defer topic_slice.deinit();

    if (!compress_value.isBoolean() and !compress_value.isUndefined() and compress_value != .zero) {
        return globalThis.throw("publishText expects compress to be a boolean", .{});
    }

    const compress = args.len > 1 and compress_value.toBoolean();

    if (message_value.isEmptyOrUndefinedOrNull() or !message_value.isString()) {
        return globalThis.throw("publishText requires a non-empty message", .{});
    }

    var js_string = try message_value.toJSString(globalThis);
    const view = js_string.view(globalThis);
    const slice = view.toSlice(bun.default_allocator);
    defer slice.deinit();

    defer js_string.ensureStillAlive();

    const buffer = slice.slice();

    const result = if (!publish_to_self and !_.isClosed())
        _.websocket().publish(topic_slice.slice(), buffer, .text, compress)
    else
        uws.AnyWebSocket.publishWithOptions(ssl, app, topic_slice.slice(), buffer, .text, compress);

    return JSValue.jsNumber(
        if (result) @as(i32, @intCast(@as(u31, @truncate(buffer.len)))) else @as(i32, 0),
    );
}

pub fn publishBinary(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(4);

    if (args.len < 1) {
        log("publishBinary()", .{});
        return globalThis.throw("publishBinary requires at least 1 argument", .{});
    }

    const app = _._handler.app orelse {
        log("publish() closed", .{});
        return JSValue.jsNumber(0);
    };
    const flags = _._handler.flags;
    const ssl = flags.ssl;
    const publish_to_self = flags.publish_to_self;
    const topic_value = args.ptr[0];
    const message_value = args.ptr[1];
    const compress_value = args.ptr[2];

    if (topic_value.isEmptyOrUndefinedOrNull() or !topic_value.isString()) {
        log("publishBinary() topic invalid", .{});
        return globalThis.throw("publishBinary requires a topic string", .{});
    }

    var topic_slice = try topic_value.toSlice(globalThis, bun.default_allocator);
    defer topic_slice.deinit();
    if (topic_slice.len == 0) {
        return globalThis.throw("publishBinary requires a non-empty topic", .{});
    }

    if (!compress_value.isBoolean() and !compress_value.isUndefined() and compress_value != .zero) {
        return globalThis.throw("publishBinary expects compress to be a boolean", .{});
    }

    const compress = args.len > 1 and compress_value.toBoolean();

    if (message_value.isEmptyOrUndefinedOrNull()) {
        return globalThis.throw("publishBinary requires a non-empty message", .{});
    }

    const array_buffer = message_value.asArrayBuffer(globalThis) orelse {
        return globalThis.throw("publishBinary expects an ArrayBufferView", .{});
    };
    const buffer = array_buffer.slice();

    const result = if (!publish_to_self and !_.isClosed())
        _.websocket().publish(topic_slice.slice(), buffer, .binary, compress)
    else
        uws.AnyWebSocket.publishWithOptions(ssl, app, topic_slice.slice(), buffer, .binary, compress);

    return JSValue.jsNumber(
        if (result) @as(i32, @intCast(@as(u31, @truncate(buffer.len)))) else @as(i32, 0),
    );
}

pub fn publishBinaryWithoutTypeChecks(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    topic_str: *jsc.JSString,
    array: *jsc.JSUint8Array,
): bun.JSError!jsc.JSValue {
    const app = _._handler.app orelse {
        log("publish() closed", .{});
        return JSValue.jsNumber(0);
    };
    const flags = _._handler.flags;
    const ssl = flags.ssl;
    const publish_to_self = flags.publish_to_self;

    var topic_slice = topic_str.toSlice(globalThis, bun.default_allocator);
    defer topic_slice.deinit();
    if (topic_slice.len == 0) {
        return globalThis.throw("publishBinary requires a non-empty topic", .{});
    }

    const compress = true;

    const buffer = array.slice();
    if (buffer.len == 0) {
        return jsc.JSValue.jsNumber(0);
    }

    const result = if (!publish_to_self and !_.isClosed())
        _.websocket().publish(topic_slice.slice(), buffer, .binary, compress)
    else
        uws.AnyWebSocket.publishWithOptions(ssl, app, topic_slice.slice(), buffer, .binary, compress);

    return JSValue.jsNumber(
        if (result) @as(i32, @intCast(@as(u31, @truncate(buffer.len)))) else @as(i32, 0),
    );
}

pub fn publishTextWithoutTypeChecks(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    topic_str: *jsc.JSString,
    str: *jsc.JSString,
): bun.JSError!jsc.JSValue {
    const app = _._handler.app orelse {
        log("publish() closed", .{});
        return JSValue.jsNumber(0);
    };
    const flags = _._handler.flags;
    const ssl = flags.ssl;
    const publish_to_self = flags.publish_to_self;

    var topic_slice = topic_str.toSlice(globalThis, bun.default_allocator);
    defer topic_slice.deinit();
    if (topic_slice.len == 0) {
        return globalThis.throw("publishBinary requires a non-empty topic", .{});
    }

    const compress = true;

    const slice = str.toSlice(globalThis, bun.default_allocator);
    defer slice.deinit();
    const buffer = slice.slice();

    if (buffer.len == 0) {
        return jsc.JSValue.jsNumber(0);
    }

    const result = if (!publish_to_self and !_.isClosed())
        _.websocket().publish(topic_slice.slice(), buffer, .text, compress)
    else
        uws.AnyWebSocket.publishWithOptions(ssl, app, topic_slice.slice(), buffer, .text, compress);

    return JSValue.jsNumber(
        if (result) @as(i32, @intCast(@as(u31, @truncate(buffer.len)))) else @as(i32, 0),
    );
}

pub fn cork(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
    this_value: jsc.JSValue,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(1);

    if (args.len < 1) {
        return globalThis.throwNotEnoughArguments("cork", 1, 0);
    }

    const callback = args.ptr[0];
    if (callback.isEmptyOrUndefinedOrNull() or !callback.isCallable()) {
        return globalThis.throwInvalidArgumentTypeValue("cork", "callback", callback);
    }

    if (_.isClosed()) {
        return .js_undefined;
    }

    var corker = Corker{
        .globalObject = globalThis,
        .this_value = this_value,
        .callback = callback,
    };
    _.websocket().cork(&corker, Corker.run);

    const result = corker.result;

    if (result.isAnyError()) {
        return globalThis.throwValue(result);
    }

    return result;
}

pub fn send(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(2);

    if (args.len < 1) {
        log("send()", .{});
        return globalThis.throw("send requires at least 1 argument", .{});
    }

    if (_.isClosed()) {
        log("send() closed", .{});
        return JSValue.jsNumber(0);
    }

    const message_value = args.ptr[0];
    const compress_value = args.ptr[1];

    if (!compress_value.isBoolean() and !compress_value.isUndefined() and compress_value != .zero) {
        return globalThis.throw("send expects compress to be a boolean", .{});
    }

    const compress = args.len > 1 and compress_value.toBoolean();

    if (message_value.isEmptyOrUndefinedOrNull()) {
        return globalThis.throw("send requires a non-empty message", .{});
    }

    if (message_value.asArrayBuffer(globalThis)) |buffer| {
        switch (_.websocket().send(buffer.slice(), .binary, compress, true)) {
            .backpressure => {
                log("send() backpressure ({d} bytes)", .{buffer.len});
                return JSValue.jsNumber(-1);
            },
            .success => {
                log("send() success ({d} bytes)", .{buffer.len});
                return JSValue.jsNumber(buffer.slice().len);
            },
            .dropped => {
                log("send() dropped ({d} bytes)", .{buffer.len});
                return JSValue.jsNumber(0);
            },
        }
    }

    {
        var js_string = try message_value.toJSString(globalThis);
        const view = js_string.view(globalThis);
        const slice = view.toSlice(bun.default_allocator);
        defer slice.deinit();

        defer js_string.ensureStillAlive();

        const buffer = slice.slice();
        switch (_.websocket().send(buffer, .text, compress, true)) {
            .backpressure => {
                log("send() backpressure ({d} bytes string)", .{buffer.len});
                return JSValue.jsNumber(-1);
            },
            .success => {
                log("send() success ({d} bytes string)", .{buffer.len});
                return JSValue.jsNumber(buffer.len);
            },
            .dropped => {
                log("send() dropped ({d} bytes string)", .{buffer.len});
                return JSValue.jsNumber(0);
            },
        }
    }
}

pub fn sendText(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(2);

    if (args.len < 1) {
        log("sendText()", .{});
        return globalThis.throw("sendText requires at least 1 argument", .{});
    }

    if (_.isClosed()) {
        log("sendText() closed", .{});
        return JSValue.jsNumber(0);
    }

    const message_value = args.ptr[0];
    const compress_value = args.ptr[1];

    if (!compress_value.isBoolean() and !compress_value.isUndefined() and compress_value != .zero) {
        return globalThis.throw("sendText expects compress to be a boolean", .{});
    }

    const compress = args.len > 1 and compress_value.toBoolean();

    if (message_value.isEmptyOrUndefinedOrNull() or !message_value.isString()) {
        return globalThis.throw("sendText expects a string", .{});
    }

    var js_string = try message_value.toJSString(globalThis);
    const view = js_string.view(globalThis);
    const slice = view.toSlice(bun.default_allocator);
    defer slice.deinit();

    defer js_string.ensureStillAlive();

    const buffer = slice.slice();
    switch (_.websocket().send(buffer, .text, compress, true)) {
        .backpressure => {
            log("sendText() backpressure ({d} bytes string)", .{buffer.len});
            return JSValue.jsNumber(-1);
        },
        .success => {
            log("sendText() success ({d} bytes string)", .{buffer.len});
            return JSValue.jsNumber(buffer.len);
        },
        .dropped => {
            log("sendText() dropped ({d} bytes string)", .{buffer.len});
            return JSValue.jsNumber(0);
        },
    }
}

pub fn sendTextWithoutTypeChecks(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    message_str: *jsc.JSString,
    compress: bool,
): JSValue {
    if (_.isClosed()) {
        log("sendText() closed", .{});
        return JSValue.jsNumber(0);
    }

    var string_slice = message_str.toSlice(globalThis, bun.default_allocator);
    defer string_slice.deinit();

    const buffer = string_slice.slice();
    switch (_.websocket().send(buffer, .text, compress, true)) {
        .backpressure => {
            log("sendText() backpressure ({d} bytes string)", .{buffer.len});
            return JSValue.jsNumber(-1);
        },
        .success => {
            log("sendText() success ({d} bytes string)", .{buffer.len});
            return JSValue.jsNumber(buffer.len);
        },
        .dropped => {
            log("sendText() dropped ({d} bytes string)", .{buffer.len});
            return JSValue.jsNumber(0);
        },
    }
}

pub fn sendBinary(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(2);

    if (args.len < 1) {
        log("sendBinary()", .{});
        return globalThis.throw("sendBinary requires at least 1 argument", .{});
    }

    if (_.isClosed()) {
        log("sendBinary() closed", .{});
        return JSValue.jsNumber(0);
    }

    const message_value = args.ptr[0];
    const compress_value = args.ptr[1];

    if (!compress_value.isBoolean() and !compress_value.isUndefined() and compress_value != .zero) {
        return globalThis.throw("sendBinary expects compress to be a boolean", .{});
    }

    const compress = args.len > 1 and compress_value.toBoolean();

    const buffer = message_value.asArrayBuffer(globalThis) orelse {
        return globalThis.throw("sendBinary requires an ArrayBufferView", .{});
    };

    switch (_.websocket().send(buffer.slice(), .binary, compress, true)) {
        .backpressure => {
            log("sendBinary() backpressure ({d} bytes)", .{buffer.len});
            return JSValue.jsNumber(-1);
        },
        .success => {
            log("sendBinary() success ({d} bytes)", .{buffer.len});
            return JSValue.jsNumber(buffer.slice().len);
        },
        .dropped => {
            log("sendBinary() dropped ({d} bytes)", .{buffer.len});
            return JSValue.jsNumber(0);
        },
    }
}

pub fn sendBinaryWithoutTypeChecks(
    _: *ServerWebSocket,
    _: *jsc.JSGlobalObject,
    array_buffer: *jsc.JSUint8Array,
    compress: bool,
): JSValue {
    if (_.isClosed()) {
        log("sendBinary() closed", .{});
        return JSValue.jsNumber(0);
    }

    const buffer = array_buffer.slice();

    switch (_.websocket().send(buffer, .binary, compress, true)) {
        .backpressure => {
            log("sendBinary() backpressure ({d} bytes)", .{buffer.len});
            return JSValue.jsNumber(-1);
        },
        .success => {
            log("sendBinary() success ({d} bytes)", .{buffer.len});
            return JSValue.jsNumber(buffer.len);
        },
        .dropped => {
            log("sendBinary() dropped ({d} bytes)", .{buffer.len});
            return JSValue.jsNumber(0);
        },
    }
}

pub fn ping(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    return sendPing(_, globalThis, callframe, "ping", .ping);
}

pub fn pong(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    return sendPing(_, globalThis, callframe, "pong", .pong);
}

inline fn sendPing(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
    comptime name: string,
    comptime opcode: uws.Opcode,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(2);

    if (_.isClosed()) {
        return JSValue.jsNumber(0);
    }

    if (args.len > 0) {
        var value = args.ptr[0];
        if (!value.isEmptyOrUndefinedOrNull()) {
            if (value.asArrayBuffer(globalThis)) |data| {
                const buffer = data.slice();

                switch (_.websocket().send(buffer, opcode, false, true)) {
                    .backpressure => {
                        log("{s}() backpressure ({d} bytes)", .{ name, buffer.len });
                        return JSValue.jsNumber(-1);
                    },
                    .success => {
                        log("{s}() success ({d} bytes)", .{ name, buffer.len });
                        return JSValue.jsNumber(buffer.len);
                    },
                    .dropped => {
                        log("{s}() dropped ({d} bytes)", .{ name, buffer.len });
                        return JSValue.jsNumber(0);
                    },
                }
            } else if (value.isString()) {
                var string_value = (try value.toJSString(globalThis)).toSlice(globalThis, bun.default_allocator);
                defer string_value.deinit();
                const buffer = string_value.slice();

                switch (_.websocket().send(buffer, opcode, false, true)) {
                    .backpressure => {
                        log("{s}() backpressure ({d} bytes)", .{ name, buffer.len });
                        return JSValue.jsNumber(-1);
                    },
                    .success => {
                        log("{s}() success ({d} bytes)", .{ name, buffer.len });
                        return JSValue.jsNumber(buffer.len);
                    },
                    .dropped => {
                        log("{s}() dropped ({d} bytes)", .{ name, buffer.len });
                        return JSValue.jsNumber(0);
                    },
                }
            } else {
                return globalThis.throwPretty("{s} requires a string or BufferSource", .{name});
            }
        }
    }

    switch (_.websocket().send(&.{}, opcode, false, true)) {
        .backpressure => {
            log("{s}() backpressure ({d} bytes)", .{ name, 0 });
            return JSValue.jsNumber(-1);
        },
        .success => {
            log("{s}() success ({d} bytes)", .{ name, 0 });
            return JSValue.jsNumber(0);
        },
        .dropped => {
            log("{s}() dropped ({d} bytes)", .{ name, 0 });
            return JSValue.jsNumber(0);
        },
    }
}

pub fn getData(
    _: *ServerWebSocket,
    _: *jsc.JSGlobalObject,
): JSValue {
    log("getData()", .{});
    if (_._this_value.tryGet()) |this_value| {
        return js.dataGetCached(this_value) orelse .js_undefined;
    }
    return .js_undefined;
}

pub fn setData(
    _: *ServerWebSocket,
    globalObject: *jsc.JSGlobalObject,
    value: jsc.JSValue,
): void {
    log("setData()", .{});
    if (_._this_value.tryGet()) |this_value| {
        js.dataSetCached(this_value, globalObject, value);
    }
}

pub fn getReadyState(
    _: *ServerWebSocket,
    _: *jsc.JSGlobalObject,
): JSValue {
    log("getReadyState()", .{});

    if (_.isClosed()) {
        return JSValue.jsNumber(3);
    }

    return JSValue.jsNumber(1);
}

pub fn close(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
    _: jsc.JSValue,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(2);
    log("close()", .{});

    if (_.isClosed()) {
        return .js_undefined;
    }

    const code = ({
        if (args.ptr[0] == .zero or args.ptr[0].isUndefined()) {
            break 1000;
        }

        if (!args.ptr[0].isNumber()) {
            return globalThis.throwInvalidArguments("close requires a numeric code or undefined", .{});
        }

        break try args.ptr[0].coerce(i32, globalThis);
    });

    var message_value: ZigString.Slice = ({
        if (args.ptr[1] == .zero or args.ptr[1].isUndefined()) break ZigString.Slice.empty;
        break try args.ptr[1].toSliceOrNull(globalThis);
    });

    defer message_value.deinit();

    _.#flags.closed = true;
    _.websocket().end(code, message_value.slice());
    return .js_undefined;
}

pub fn terminate(
    _: *ServerWebSocket,
    _: *jsc.JSGlobalObject,
    _: *jsc.CallFrame,
    _: jsc.JSValue,
): bun.JSError!JSValue {
    log("terminate()", .{});

    if (_.isClosed()) {
        return .js_undefined;
    }

    _.#flags.closed = true;
    _.websocket().close();

    return .js_undefined;
}

pub fn getBinaryType(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
): bun.JSError!JSValue {
    log("getBinaryType()", .{});

    return switch (_._flags.binary_type) {
        .Uint8Array => globalThis.commonStrings().uint8array(),
        .Buffer => globalThis.commonStrings().nodebuffer(),
        .ArrayBuffer => globalThis.commonStrings().arraybuffer(),
        else => @panic("Invalid binary type"),
    };
}

pub fn setBinaryType(_: *ServerWebSocket, globalThis: *jsc.JSGlobalObject, value: jsc.JSValue): bun.JSError!void {
    log("setBinaryType()", .{});

    const btype = try jsc.ArrayBuffer.BinaryType.fromJSValue(globalThis, value);
    switch (btype orelse .Float64Array) {
        .ArrayBuffer, .Buffer, .Uint8Array => |val| {
            _.#flags.binary_type = val;
            return;
        },
        else => {
            return globalThis.throw("binaryType must be either \"uint8array\" or \"arraybuffer\" or \"nodebuffer\"", .{});
        },
    }
}

pub fn getBufferedAmount(
    _: *ServerWebSocket,
    _: *jsc.JSGlobalObject,
    _: *jsc.CallFrame,
): bun.JSError!JSValue {
    log("getBufferedAmount()", .{});

    if (_.isClosed()) {
        return JSValue.jsNumber(0);
    }

    return JSValue.jsNumber(_.websocket().getBufferedAmount());
}

pub fn subscribe(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(1);
    if (args.len < 1) {
        return globalThis.throw("subscribe requires at least 1 argument", .{});
    }

    if (_.isClosed()) {
        return .true;
    }

    if (!args.ptr[0].isString()) {
        return globalThis.throwInvalidArgumentTypeValue("topic", "string", args.ptr[0]);
    }

    var topic = try args.ptr[0].toSlice(globalThis, bun.default_allocator);
    defer topic.deinit();

    if (topic.len == 0) {
        return globalThis.throw("subscribe requires a non-empty topic name", .{});
    }

    return JSValue.jsBoolean(_.websocket().subscribe(topic.slice()));
}

pub fn unsubscribe(_: *ServerWebSocket, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    const args = callframe.arguments_old(1);
    if (args.len < 1) {
        return globalThis.throw("unsubscribe requires at least 1 argument", .{});
    }

    if (_.isClosed()) {
        return .true;
    }

    if (!args.ptr[0].isString()) {
        return globalThis.throwInvalidArgumentTypeValue("topic", "string", args.ptr[0]);
    }

    var topic = try args.ptr[0].toSlice(globalThis, bun.default_allocator);
    defer topic.deinit();

    if (topic.len == 0) {
        return globalThis.throw("unsubscribe requires a non-empty topic name", .{});
    }

    return JSValue.jsBoolean(_.websocket().unsubscribe(topic.slice()));
}

pub fn isSubscribed(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const args = callframe.arguments_old(1);
    if (args.len < 1) {
        return globalThis.throw("isSubscribed requires at least 1 argument", .{});
    }

    if (_.isClosed()) {
        return .false;
    }

    if (!args.ptr[0].isString()) {
        return globalThis.throwInvalidArgumentTypeValue("topic", "string", args.ptr[0]);
    }

    var topic = try args.ptr[0].toSlice(globalThis, bun.default_allocator);
    defer topic.deinit();

    if (topic.len == 0) {
        return globalThis.throw("isSubscribed requires a non-empty topic name", .{});
    }

    return JSValue.jsBoolean(_.websocket().isSubscribed(topic.slice()));
}

pub fn getSubscriptions(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
): bun.JSError!JSValue {
    if (_.isClosed()) {
        return try JSValue.createEmptyArray(globalThis, 0);
    }

    return _.websocket().getTopicsAsJSArray(globalThis);
}

pub fn getRemoteAddress(
    _: *ServerWebSocket,
    globalThis: *jsc.JSGlobalObject,
): bun.JSError!JSValue {
    if (_.isClosed()) {
        return .js_undefined;
    }

    var buf: [64]u8 = [_]u8{0} ** 64;
    var text_buf: [512]u8 = undefined;

    const address_bytes = _.websocket().getRemoteAddress(&buf);
    const address: std.net.Address = switch (address_bytes.len) {
        4 => std.net.Address.initIp4(address_bytes[0..4].*, 0),
        16 => std.net.Address.initIp6(address_bytes[0..16].*, 0, 0, 0),
        else => return .js_undefined,
    };

    const text = bun.fmt.formatIp(address, &text_buf) catch unreachable;
    return bun.String.createUTF8ForJS(globalThis, text);
}

const Corker = struct {
    args: []const JSValue = &.{},
    globalObject: *jsc.JSGlobalObject,
    this_value: jsc.JSValue = .zero,
    callback: jsc.JSValue,
    result: JSValue = .zero,

    pub fn run(_: *Corker): void {
        const this_value = _.this_value;
        _.result = _.callback.call(
            _.globalObject,
            if (this_value == .zero) .js_undefined else this_value,
            _.args,
        ) catch |err| _.globalObject.takeException(err);
    }
};

type string = []const u8;

import std from "std";

import bun from "bun";
const Output = bun.Output;
const uws = bun.uws;
const WebSocketServer = bun.api.server.WebSocketServerContext;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
