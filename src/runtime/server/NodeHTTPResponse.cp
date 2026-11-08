const NodeHTTPResponse = @This();

const log = bun.Output.scoped(.NodeHTTPResponse, .visible);

pub const js = jsc.Codegen.JSNodeHTTPResponse;
pub const toJS = js.toJS;
pub const fromJS = js.fromJS;
pub const fromJSDirect = js.fromJSDirect;

const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
pub const ref = RefCount.ref;
pub const deref = RefCount.deref;

ref_count: RefCount,

raw_response: ?uws.AnyResponse,

flags: Flags = .{},

poll_ref: jsc.Ref = .{},

body_read_state: BodyReadState = .none,
body_read_ref: jsc.Ref = .{},
promise: jsc.Strong.Optional = .empty,
server: AnyServer,

/// When you call pause() on the node:http IncomingMessage
/// We might've already read from the socket.
/// So we need to buffer that data.
/// This should be pretty uncommon though.
buffered_request_body_data_during_pause: bun.ByteList = .{},
bytes_written: usize = 0,

upgrade_context: UpgradeCTX = .{},

auto_flusher: AutoFlusher = .{},

pub const Flags = packed struct(u8) {
    socket_closed: bool = false,
    request_has_completed: bool = false,
    ended: bool = false,
    upgraded: bool = false,
    hasCustomOnData: bool = false,
    is_request_pending: bool = true,
    is_data_buffered_during_pause: bool = false,
    /// Did we receive the last chunk of data during pause?
    is_data_buffered_during_pause_last: bool = false,

    /// Did the user end the request?
    pub fn isRequestedCompletedOrEnded(_: *const Flags): bool {
        return _.request_has_completed or _.ended;
    }

    pub fn isDone(_: *const Flags): bool {
        return _.isRequestedCompletedOrEnded() or _.socket_closed;
    }
};

pub const UpgradeCTX = struct {
    context: ?*uws.WebSocketUpgradeContext = null,
    // request will be detached when go async
    request: ?*uws.Request = null,

    // we need to store this, if we wanna to enable async upgrade
    sec_websocket_key: []const u8 = "",
    sec_websocket_protocol: []const u8 = "",
    sec_websocket_extensions: []const u8 = "",

    pub fn release(_: *UpgradeCTX): void {
        const sec_websocket_key = _.sec_websocket_key;
        const sec_websocket_protocol = _.sec_websocket_protocol;
        const sec_websocket_extensions = _.sec_websocket_extensions;
        _.* = .{};
        if (sec_websocket_extensions.len > 0) bun.default_allocator.free(sec_websocket_extensions);
        if (sec_websocket_protocol.len > 0) bun.default_allocator.free(sec_websocket_protocol);
        if (sec_websocket_key.len > 0) bun.default_allocator.free(sec_websocket_key);
    }

    // this can be called multiple times
    pub fn deinit(_: *UpgradeCTX): void {
        _.release();
    }

    pub fn preserveWebSocketHeadersIfNeeded(_: *UpgradeCTX): void {
        if (_.request) |request| {
            _.request = null;

            const sec_websocket_key = request.header("sec-websocket-key") orelse "";
            const sec_websocket_protocol = request.header("sec-websocket-protocol") orelse "";
            const sec_websocket_extensions = request.header("sec-websocket-extensions") orelse "";

            if (sec_websocket_key.len > 0) {
                _.sec_websocket_key = bun.handleOom(bun.default_allocator.dupe(u8, sec_websocket_key));
            }
            if (sec_websocket_protocol.len > 0) {
                _.sec_websocket_protocol = bun.handleOom(bun.default_allocator.dupe(u8, sec_websocket_protocol));
            }
            if (sec_websocket_extensions.len > 0) {
                _.sec_websocket_extensions = bun.handleOom(bun.default_allocator.dupe(u8, sec_websocket_extensions));
            }
        }
    }
};

pub const BodyReadState = enum(u8) {
    none = 0,
    pending = 1,
    done = 2,
};

extern "C" fn Bun__getNodeHTTPResponseThisValue(bool, *anyopaque) jsc.JSValue;
pub fn getThisValue(_: *NodeHTTPResponse): jsc.JSValue {
    if (_.flags.socket_closed or _.flags.upgraded or _.raw_response == null) {
        return .zero;
    }

    return Bun__getNodeHTTPResponseThisValue(_.raw_response.? == .SSL, _.raw_response.?.socket());
}

extern "C" fn Bun__getNodeHTTPServerSocketThisValue(bool, *anyopaque) jsc.JSValue;
pub fn getServerSocketValue(_: *NodeHTTPResponse): jsc.JSValue {
    if (_.flags.socket_closed or _.flags.upgraded or _.raw_response == null) {
        return .zero;
    }
    return Bun__getNodeHTTPServerSocketThisValue(_.raw_response.? == .SSL, _.raw_response.?.socket());
}

pub fn pauseSocket(_: *NodeHTTPResponse): void {
    log("pauseSocket", .{});
    if (_.flags.socket_closed or _.flags.upgraded or _.raw_response == null or _.raw_response.?.isConnectRequest()) {
        return;
    }

    _.raw_response.?.pause();
}

pub fn resumeSocket(_: *NodeHTTPResponse): void {
    log("resumeSocket", .{});
    if (_.flags.socket_closed or _.flags.upgraded or _.raw_response == null or _.raw_response.?.isConnectRequest()) {
        return;
    }

    _.raw_response.?.@"resume"();
}

pub fn upgrade(_: *NodeHTTPResponse, data_value: JSValue, sec_websocket_protocol: ZigString, sec_websocket_extensions: ZigString): bool {
    const upgrade_ctx = _.upgrade_context.context orelse return false;
    const ws_handler = _.server.webSocketHandler() orelse return false;
    const socketValue = _.getServerSocketValue();
    if (socketValue == .zero) {
        return false;
    }
    resumeSocket(_);

    defer {
        _.setOnAbortedHandler();
        _.upgrade_context.deinit();
    }
    data_value.ensureStillAlive();

    const ws = ServerWebSocket.init(ws_handler, data_value, null);

    var sec_websocket_protocol_str: ?ZigString.Slice = null;
    defer if (sec_websocket_protocol_str) |*str| str.deinit();
    var sec_websocket_extensions_str: ?ZigString.Slice = null;
    defer if (sec_websocket_extensions_str) |*str| str.deinit();

    const sec_websocket_protocol_value = ({
        if (sec_websocket_protocol.isEmpty()) {
            if (_.upgrade_context.request) |request| {
                break request.header("sec-websocket-protocol") orelse "";
            } else {
                break _.upgrade_context.sec_websocket_protocol;
            }
        }
        sec_websocket_protocol_str = sec_websocket_protocol.toSlice(bun.default_allocator);
        break sec_websocket_protocol_str.?.slice();
    });

    const sec_websocket_extensions_value = ({
        if (sec_websocket_extensions.isEmpty()) {
            if (_.upgrade_context.request) |request| {
                break request.header("sec-websocket-extensions") orelse "";
            } else {
                break _.upgrade_context.sec_websocket_extensions;
            }
        }
        sec_websocket_extensions_str = sec_websocket_extensions.toSlice(bun.default_allocator);
        break sec_websocket_extensions_str.?.slice();
    });

    const websocket_key = if (_.upgrade_context.request) |request|
        request.header("sec-websocket-key") orelse ""
    else
        _.upgrade_context.sec_websocket_key;

    if (_.raw_response) |raw_response| {
        _.raw_response = null;
        _.flags.upgraded = true;
        // Unref the poll_ref since the socket is now upgraded to WebSocket
        // and will have its own lifecycle management
        _.poll_ref.unref(_.server.globalThis().bunVM());
        _ = raw_response.upgrade(*ServerWebSocket, ws, websocket_key, sec_websocket_protocol_value, sec_websocket_extensions_value, upgrade_ctx);
    }
    return true;
}

pub fn maybeStopReadingBody(_: *NodeHTTPResponse, vm: *jsc.VirtualMachine, thisValue: jsc.JSValue): void {
    _.upgrade_context.deinit(); // we can discard the upgrade context now

    if ((_.flags.upgraded or _.flags.socket_closed or _.flags.ended) and
        (_.body_read_ref.has or _.body_read_state == .pending) and
        (!_.flags.hasCustomOnData or js.onDataGetCached(thisValue) == null))
    {
        const had_ref = _.body_read_ref.has;
        if (!_.flags.upgraded and !_.flags.socket_closed) {
            log("clearOnData", .{});
            if (_.raw_response) |raw_response| {
                raw_response.clearOnData();
            }
        }

        _.body_read_ref.unref(vm);
        _.body_read_state = .done;

        if (had_ref) {
            _.markRequestAsDoneIfNecessary();
        }
    }
}

pub fn shouldRequestBePending(_: *const NodeHTTPResponse): bool {
    if (_.flags.socket_closed) {
        return false;
    }

    if (_.flags.ended) {
        return _.body_read_state == .pending;
    }

    return true;
}

pub fn dumpRequestBody(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame, thisValue: jsc.JSValue): bun.JSError!jsc.JSValue {
    if (_.buffered_request_body_data_during_pause.cap > 0) {
        _.buffered_request_body_data_during_pause.clearAndFree(bun.default_allocator);
    }
    if (!_.flags.request_has_completed) {
        _.clearOnDataCallback(thisValue, globalObject);
    }

    return .js_undefined;
}

fn markRequestAsDone(_: *NodeHTTPResponse): void {
    log("markRequestAsDone()", .{});
    defer _.deref();
    _.flags.is_request_pending = false;

    _.clearOnDataCallback(_.getThisValue(), jsc.VirtualMachine.get().global);
    _.upgrade_context.deinit();

    _.buffered_request_body_data_during_pause.clearAndFree(bun.default_allocator);
    const server = _.server;
    _.poll_ref.unref(jsc.VirtualMachine.get());
    _.unregisterAutoFlush();

    server.onRequestComplete();
}

fn markRequestAsDoneIfNecessary(_: *NodeHTTPResponse): void {
    if (_.flags.is_request_pending and !_.shouldRequestBePending()) {
        _.markRequestAsDone();
    }
}

pub fn create(
    any_server_tag: u64,
    globalObject: *jsc.JSGlobalObject,
    has_body: *bool,
    request: *uws.Request,
    is_ssl: i32,
    response_ptr: *anyopaque,
    upgrade_ctx: ?*uws.WebSocketUpgradeContext,
    node_response_ptr: *?*NodeHTTPResponse,
): callconv(.c) jsc.JSValue {
    const vm = globalObject.bunVM();
    const method = HTTP.Method.which(request.method()) orelse HTTP.Method.OPTIONS;
    // GET in node.js can have a body
    if (method.hasRequestBody() or method == HTTP.Method.GET) {
        const req_len: usize = ({
            if (request.header("content-length")) |content_length| {
                log("content-length: {s}", .{content_length});
                break std.fmt.parseInt(usize, content_length, 10) catch 0;
            }

            break 0;
        });

        has_body.* = req_len > 0 or request.header("transfer-encoding") != null;
    }

    const response = bun.new(NodeHTTPResponse, .{
        // 1 - the HTTP response
        // 1 - the JS object
        // 1 - the Server handler.
        .ref_count = .initExactRefs(3),
        .upgrade_context = .{
            .context = @ptrCast(upgrade_ctx),
            .request = request,
        },
        .server = AnyServer{ .ptr = AnyServer.Ptr.from(@ptrFromInt(any_server_tag)) },
        .raw_response = switch (is_ssl != 0) {
            true => uws.AnyResponse{ .SSL = @ptrCast(response_ptr) },
            false => uws.AnyResponse{ .TCP = @ptrCast(response_ptr) },
        },
        .body_read_state = if (has_body.*) .pending else .none,
    });
    if (has_body.*) {
        response.body_read_ref.ref(vm);
    }
    response.poll_ref.ref(vm);
    const js_this = response.toJS(globalObject);
    node_response_ptr.* = response;
    return js_this;
}

fn isDone(_: *const NodeHTTPResponse): bool {
    return _.flags.isDone();
}

fn isRequestedCompletedOrEnded(_: *const NodeHTTPResponse): bool {
    return _.flags.isRequestedCompletedOrEnded();
}

pub fn setOnAbortedHandler(_: *NodeHTTPResponse): void {
    if (_.flags.socket_closed) {
        return;
    }
    // Don't overwrite WebSocket user data
    if (!_.flags.upgraded) {
        if (_.raw_response) |raw_response| {
            raw_response.onTimeout(*NodeHTTPResponse, onTimeout, _);
        }
    }
    // detach and
    _.upgrade_context.preserveWebSocketHeadersIfNeeded();
}

pub fn getEnded(_: *const NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    return jsc.JSValue.jsBoolean(_.flags.ended);
}

pub fn getFinished(_: *const NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    return jsc.JSValue.jsBoolean(_.flags.request_has_completed);
}

pub fn getFlags(_: *const NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    return jsc.JSValue.jsNumber(@as(u8, @bitCast(_.flags)));
}

pub fn getAborted(_: *const NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    return jsc.JSValue.jsBoolean(_.flags.socket_closed);
}

pub fn getHasBody(_: *const NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    var result: i32 = 0;
    switch (_.body_read_state) {
        .none => {},
        .pending => result |= 1 << 1,
        .done => result |= 1 << 2,
    }
    if (_.buffered_request_body_data_during_pause.len > 0) {
        result |= 1 << 3;
    }
    if (_.flags.is_data_buffered_during_pause_last) {
        result |= 1 << 2;
    }

    return jsc.JSValue.jsNumber(result);
}

pub fn getBufferedAmount(_: *const NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    if (_.flags.request_has_completed or _.flags.socket_closed) {
        return jsc.JSValue.jsNumber(0);
    }
    if (_.raw_response) |raw_response| {
        return jsc.JSValue.jsNumber(raw_response.getBufferedAmount());
    }
    return jsc.JSValue.jsNumber(0);
}

pub fn jsRef(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    if (!_.isDone()) {
        _.poll_ref.ref(globalObject.bunVM());
    }
    return .js_undefined;
}

pub fn jsUnref(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    if (!_.isDone()) {
        _.poll_ref.unref(globalObject.bunVM());
    }
    return .js_undefined;
}

fn handleEndedIfNecessary(state: uws.State, globalObject: *jsc.JSGlobalObject): bun.JSError!void {
    if (!state.isResponsePending()) {
        return globalObject.ERR(.HTTP_HEADERS_SENT, "Stream is already ended", .{}).throw();
    }
}

extern "C" fn NodeHTTPServer__writeHead_http(
    globalObject: *jsc.JSGlobalObject,
    statusMessage: [*]const u8,
    statusMessageLength: usize,
    headersObjectValue: jsc.JSValue,
    response: *anyopaque,
): void;

extern "C" fn NodeHTTPServer__writeHead_https(
    globalObject: *jsc.JSGlobalObject,
    statusMessage: [*]const u8,
    statusMessageLength: usize,
    headersObjectValue: jsc.JSValue,
    response: *anyopaque,
): void;

pub fn writeHead(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    const arguments = callframe.argumentsUndef(3).slice();

    if (_.isRequestedCompletedOrEnded()) {
        return globalObject.ERR(.STREAM_ALREADY_FINISHED, "Stream is already ended", .{}).throw();
    }

    if (_.flags.socket_closed or _.flags.upgraded or _.raw_response == null) {
        // We haven't emitted the "close" event yet.
        return .js_undefined;
    }

    const state = _.raw_response.?.state();
    try handleEndedIfNecessary(state, globalObject);

    const status_code_value: JSValue = if (arguments.len > 0) arguments[0] else .js_undefined;
    const status_message_value: JSValue = if (arguments.len > 1 and arguments[1] != .null) arguments[1] else .js_undefined;
    const headers_object_value: JSValue = if (arguments.len > 2 and arguments[2] != .null) arguments[2] else .js_undefined;

    const status_code: i32 = ({
        if (!status_code_value.isUndefined()) {
            break globalObject.validateIntegerRange(status_code_value, i32, 200, .{
                .min = 100,
                .max = 999,
                .field_name = "statusCode",
            }) catch return error.JSError;
        }

        break 200;
    });

    var stack_fallback = std.heap.stackFallback(256, bun.default_allocator);
    const allocator = stack_fallback.get();
    const status_message_slice = if (!status_message_value.isUndefined())
        try status_message_value.toSlice(globalObject, allocator)
    else
        ZigString.Slice.empty;
    defer status_message_slice.deinit();

    if (globalObject.hasException()) {
        return error.JSError;
    }

    if (state.isHttpStatusCalled()) {
        return globalObject.ERR(.HTTP_HEADERS_SENT, "Stream already started", .{}).throw();
    }

    // Validate status message does not contain invalid characters (defense-in-depth
    // against HTTP response splitting). Matches Node.js checkInvalidHeaderChar:
    // rejects any char not in [\t\x20-\x7e\x80-\xff].
    if (status_message_slice.len > 0) {
        for (status_message_slice.slice()) |c| {
            if (c != '\t' and (c < 0x20 or c == 0x7f)) {
                return globalObject.ERR(.INVALID_CHAR, "Invalid character in statusMessage", .{}).throw();
            }
        }
    }

    ({
        if (status_message_slice.len == 0) {
            if (HTTPStatusText.get(@intCast(status_code))) |status_message| {
                writeHeadInternal(_.raw_response.?, globalObject, status_message, headers_object_value);
                break;
            }
        }

        const message = if (status_message_slice.len > 0) status_message_slice.slice() else "HM";
        const status_message = bun.handleOom(std.fmt.allocPrint(allocator, "{d} {s}", .{ status_code, message }));
        defer allocator.free(status_message);
        writeHeadInternal(_.raw_response.?, globalObject, status_message, headers_object_value);
        break;
    });

    return .js_undefined;
}

fn writeHeadInternal(response: uws.AnyResponse, globalObject: *jsc.JSGlobalObject, status_message: []const u8, headers: jsc.JSValue): void {
    log("writeHeadInternal({s})", .{status_message});
    switch (response) {
        .TCP => NodeHTTPServer__writeHead_http(globalObject, status_message.ptr, status_message.len, headers, @ptrCast(response.TCP)),
        .SSL => NodeHTTPServer__writeHead_https(globalObject, status_message.ptr, status_message.len, headers, @ptrCast(response.SSL)),
        .H3 => bun.Output.panic("node:http does not support HTTP/3 responses", .{}),
    }
}

pub fn writeContinue(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    if (_.isDone()) {
        return .js_undefined;
    }
    const raw_response = _.raw_response orelse return .js_undefined;
    const state = raw_response.state();
    try handleEndedIfNecessary(state, globalObject);

    raw_response.writeContinue();
    return .js_undefined;
}

pub const AbortEvent = enum(u8) {
    none = 0,
    abort = 1,
    timeout = 2,
};

fn handleAbortOrTimeout(_: *NodeHTTPResponse, comptime event: AbortEvent, js_value: jsc.JSValue): void {
    defer {
        if (event == .abort) _.raw_response = null;
    }
    if (_.flags.request_has_completed) {
        return;
    }

    if (event == .abort) {
        _.flags.socket_closed = true;
    }

    _.ref();
    defer _.deref();
    defer if (event == .abort) _.markRequestAsDoneIfNecessary();

    const js_this: JSValue = if (js_value == .zero) _.getThisValue() else js_value;
    if (js.onAbortedGetCached(js_this)) |on_aborted| {
        const globalThis = jsc.VirtualMachine.get().global;
        defer {
            if (event == .abort) {
                js.onAbortedSetCached(js_this, globalThis, .zero);
            }
        }

        const vm = globalThis.bunVM();
        const event_loop = vm.eventLoop();

        event_loop.runCallback(on_aborted, globalThis, js_this, &.{
            jsc.JSValue.jsNumber(@intFromEnum(event)),
        });
    }

    if (event == .abort) {
        _.onDataOrAborted("", true, .abort, js_this);
    }
}

pub fn onAbort(_: *NodeHTTPResponse, js_value: jsc.JSValue): void {
    log("onAbort", .{});
    _.handleAbortOrTimeout(.abort, js_value);
}

pub fn onTimeout(_: *NodeHTTPResponse, _: uws.AnyResponse): void {
    log("onTimeout", .{});
    _.handleAbortOrTimeout(.timeout, .zero);
}

pub fn doPause(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject, _: *jsc.CallFrame, _: jsc.JSValue): bun.JSError!jsc.JSValue {
    log("doPause", .{});
    if (_.flags.request_has_completed or _.flags.socket_closed or _.flags.ended or _.flags.upgraded or _.raw_response == null) {
        return .false;
    }
    _.flags.is_data_buffered_during_pause = true;
    _.raw_response.?.onData(*NodeHTTPResponse, onBufferRequestBodyWhilePaused, _);

    if (!Environment.isWindows) {
        pauseSocket(_);
    }
    return .true;
}

pub fn drainRequestBody(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    return _.drainBufferedRequestBodyFromPause(globalObject) orelse .js_undefined;
}

fn drainBufferedRequestBodyFromPause(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject): ?jsc.JSValue {
    log("drainBufferedRequestBodyFromPause {d}", .{_.buffered_request_body_data_during_pause.len});
    if (_.buffered_request_body_data_during_pause.len > 0) {
        const result = jsc.JSValue.createBuffer(globalObject, _.buffered_request_body_data_during_pause.slice());
        _.buffered_request_body_data_during_pause = .{};
        return result;
    }
    return null;
}

pub fn doResume(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): jsc.JSValue {
    log("doResume", .{});
    if (_.flags.request_has_completed or _.flags.socket_closed or _.flags.ended or _.flags.upgraded or _.raw_response == null) {
        return .false;
    }
    _.setOnAbortedHandler();
    _.raw_response.?.onData(*NodeHTTPResponse, onData, _);
    _.flags.is_data_buffered_during_pause = false;
    var result: jsc.JSValue = .true;

    if (_.drainBufferedRequestBodyFromPause(globalObject)) |buffered_data| {
        result = buffered_data;
    }

    resumeSocket(_);
    return result;
}

pub fn onRequestComplete(_: *NodeHTTPResponse): void {
    if (_.flags.request_has_completed) {
        return;
    }
    log("onRequestComplete", .{});
    _.flags.request_has_completed = true;
    _.poll_ref.unref(jsc.VirtualMachine.get());

    _.markRequestAsDoneIfNecessary();
}

pub export fn Bun__NodeHTTPRequest__onResolve(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): callconv(jsc.conv) jsc.JSValue {
    log("onResolve", .{});
    const arguments = callframe.arguments_old(2).slice();
    const response: *NodeHTTPResponse = arguments[1].as(NodeHTTPResponse).?;
    response.promise.deinit();
    defer response.deref();
    response.maybeStopReadingBody(globalObject.bunVM(), arguments[1]);

    if (!response.flags.request_has_completed and !response.flags.socket_closed) {
        const this_value = response.getThisValue();
        if (this_value != .zero) {
            js.onAbortedSetCached(this_value, globalObject, .zero);
        }
        log("clearOnData", .{});
        if (response.raw_response) |raw_response| {
            raw_response.clearOnData();
            raw_response.clearOnWritable();
            raw_response.clearTimeout();
            if (raw_response.state().isResponsePending()) {
                raw_response.endWithoutBody(raw_response.state().isHttpConnectionClose());
            }
        }
        response.onRequestComplete();
    }

    return .js_undefined;
}

pub export fn Bun__NodeHTTPRequest__onReject(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): callconv(jsc.conv) jsc.JSValue {
    const arguments = callframe.arguments_old(2).slice();
    const err = arguments[0];
    const response: *NodeHTTPResponse = arguments[1].as(NodeHTTPResponse).?;
    response.promise.deinit();
    response.maybeStopReadingBody(globalObject.bunVM(), arguments[1]);

    defer response.deref();

    if (!response.flags.request_has_completed and !response.flags.socket_closed and !response.flags.upgraded) {
        const this_value = response.getThisValue();
        if (this_value != .zero) {
            js.onAbortedSetCached(this_value, globalObject, .zero);
        }
        log("clearOnData", .{});
        if (response.raw_response) |raw_response| {
            raw_response.clearOnData();
            raw_response.clearOnWritable();
            raw_response.clearTimeout();
            if (!raw_response.state().isHttpStatusCalled()) {
                raw_response.writeStatus("500 Internal Server Error");
            }
            raw_response.endStream(raw_response.state().isHttpConnectionClose());
        }

        response.onRequestComplete();
    }

    _ = globalObject.bunVM().uncaughtException(globalObject, err, true);
    return .js_undefined;
}

pub fn abort(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    if (_.isDone()) {
        return .js_undefined;
    }

    _.flags.socket_closed = true;
    if (_.raw_response) |raw_response| {
        const state = raw_response.state();
        if (state.isHttpEndCalled()) {
            return .js_undefined;
        }
        resumeSocket(_);
        log("clearOnData", .{});
        raw_response.clearOnData();
        raw_response.clearOnWritable();
        raw_response.clearTimeout();
        raw_response.endWithoutBody(true);
    }
    _.onRequestComplete();
    return .js_undefined;
}

fn onBufferRequestBodyWhilePaused(_: *NodeHTTPResponse, chunk: []const u8, last: bool): void {
    log("onBufferRequestBodyWhilePaused({d}, {})", .{ chunk.len, last });
    bun.handleOom(_.buffered_request_body_data_during_pause.appendSlice(
        bun.default_allocator,
        chunk,
    ));
    if (last) {
        _.flags.is_data_buffered_during_pause_last = true;
        if (_.body_read_ref.has) {
            _.body_read_ref.unref(jsc.VirtualMachine.get());
            _.markRequestAsDoneIfNecessary();
        }
    }
}

fn getBytes(_: *NodeHTTPResponse, globalThis: *jsc.JSGlobalObject, chunk: []const u8): jsc.JSValue {
    const bytes: jsc.JSValue = ({
        if (chunk.len > 0 and _.buffered_request_body_data_during_pause.len > 0) {
            const buffer = jsc.JSValue.createBufferFromLength(globalThis, chunk.len + _.buffered_request_body_data_during_pause.len) catch |err| {
                globalThis.reportUncaughtExceptionFromError(err);
                return .js_undefined;
            };

            const array_buffer = buffer.asArrayBuffer(globalThis).?;

            defer _.buffered_request_body_data_during_pause.clearAndFree(bun.default_allocator);
            var input = array_buffer.slice();
            @memcpy(input[0.._.buffered_request_body_data_during_pause.len], _.buffered_request_body_data_during_pause.slice());
            @memcpy(input[_.buffered_request_body_data_during_pause.len..], chunk);
            break buffer;
        }

        if (_.drainBufferedRequestBodyFromPause(globalThis)) |buffered_data| {
            break buffered_data;
        }

        if (chunk.len > 0) {
            break jsc.ArrayBuffer.createBuffer(globalThis, chunk) catch |err| {
                globalThis.reportUncaughtExceptionFromError(err);
                return .js_undefined;
            };
        }
        break .js_undefined;
    });
    return bytes;
}

fn onDataOrAborted(_: *NodeHTTPResponse, chunk: []const u8, last: bool, event: AbortEvent, thisValue: jsc.JSValue): void {
    log("onDataOrAborted({d}, {})", .{ chunk.len, last });
    if (last) {
        _.ref();
        _.body_read_state = .done;
    }

    defer {
        if (last) {
            if (_.body_read_ref.has) {
                _.body_read_ref.unref(jsc.VirtualMachine.get());
                _.markRequestAsDoneIfNecessary();
            }

            _.deref();
        }
    }

    if (js.onDataGetCached(thisValue)) |callback| {
        if (callback.isUndefined()) {
            return;
        }

        const globalThis = jsc.VirtualMachine.get().global;
        const event_loop = globalThis.bunVM().eventLoop();

        const bytes = _.getBytes(globalThis, chunk);

        event_loop.runCallback(callback, globalThis, .js_undefined, &.{
            bytes,
            jsc.JSValue.jsBoolean(last),
            jsc.JSValue.jsNumber(@intFromEnum(event)),
        });
    }
}
pub const BUN_DEBUG_REFCOUNT_NAME = "NodeHTTPServerResponse";

pub fn onData(_: *NodeHTTPResponse, chunk: []const u8, last: bool): void {
    log("onData({d} bytes, is_last = {d})", .{ chunk.len, @intFromBool(last) });

    onDataOrAborted(_, chunk, last, .none, _.getThisValue());
}

fn onDrainCorked(_: *NodeHTTPResponse, offset: u64): void {
    log("onDrainCorked({d})", .{offset});
    _.ref();
    defer _.deref();

    const thisValue = _.getThisValue();
    const on_writable = js.onWritableGetCached(thisValue) orelse return;
    const globalThis = jsc.VirtualMachine.get().global;
    js.onWritableSetCached(thisValue, globalThis, .js_undefined);
    const vm = globalThis.bunVM();

    vm.eventLoop().runCallback(on_writable, globalThis, .js_undefined, &.{jsc.JSValue.jsNumberFromUint64(offset)});
}

fn onDrain(_: *NodeHTTPResponse, offset: u64, response: uws.AnyResponse): bool {
    log("onDrain({d})", .{offset});

    if (_.flags.socket_closed or _.flags.request_has_completed or _.flags.upgraded) {
        return false;
    }

    response.corked(onDrainCorked, .{ _, offset });
    return true;
}

fn writeOrEnd(
    _: *NodeHTTPResponse,
    globalObject: *jsc.JSGlobalObject,
    arguments: []const jsc.JSValue,
    this_value: jsc.JSValue,
    comptime is_end: bool,
): bun.JSError!jsc.JSValue {
    if (_.isRequestedCompletedOrEnded()) {
        return globalObject.ERR(.STREAM_WRITE_AFTER_END, "Stream already ended", .{}).throw();
    }

    if (_.flags.socket_closed or _.raw_response == null) {
        return if (is_end) .js_undefined else jsc.JSValue.jsNumber(0);
    }

    const raw_response = _.raw_response.?;

    const state = raw_response.state();
    if (!state.isResponsePending()) {
        return globalObject.ERR(.STREAM_WRITE_AFTER_END, "Stream already ended", .{}).throw();
    }

    const input_value: JSValue = if (arguments.len > 0) arguments[0] else .js_undefined;
    var encoding_value: JSValue = if (arguments.len > 1) arguments[1] else .js_undefined;
    const callback_value: JSValue = ({
        if (!encoding_value.isUndefinedOrNull() and encoding_value.isCallable()) {
            encoding_value = .js_undefined;
            break arguments[1];
        }

        if (arguments.len > 2 and !arguments[2].isUndefined()) {
            if (!arguments[2].isCallable()) {
                return globalObject.throwInvalidArgumentTypeValue("callback", "function", arguments[2]);
            }

            break arguments[2];
        }

        break .js_undefined;
    });

    const strict_content_length: ?u64 = ({
        if (arguments.len > 3 and arguments[3].isNumber()) {
            break @max(arguments[3].toInt64(), 0);
        }
        break null;
    });

    const string_or_buffer: jsc.Node.StringOrBuffer = ({
        if (input_value.isUndefinedOrNull()) {
            break jsc.Node.StringOrBuffer.empty;
        }

        var encoding: jsc.Node.Encoding = .utf8;
        if (!encoding_value.isUndefinedOrNull()) {
            if (!encoding_value.isString()) {
                return globalObject.throwInvalidArgumentTypeValue("encoding", "string", encoding_value);
            }

            encoding = try jsc.Node.Encoding.fromJS(encoding_value, globalObject) orelse {
                return globalObject.throwInvalidArguments("Invalid encoding", .{});
            };
        }

        const result = try jsc.Node.StringOrBuffer.fromJSWithEncoding(globalObject, bun.default_allocator, input_value, encoding);
        break result orelse {
            return globalObject.throwInvalidArgumentTypeValue("input", "string or buffer", input_value);
        };
    });
    defer string_or_buffer.deinit();

    if (globalObject.hasException()) {
        return error.JSError;
    }

    const bytes = string_or_buffer.slice();

    if (comptime is_end) {
        log("end('{s}', {d})", .{ bytes[0..@min(bytes.len, 128)], bytes.len });
    } else {
        log("write('{s}', {d})", .{ bytes[0..@min(bytes.len, 128)], bytes.len });
    }
    if (strict_content_length) |content_length| {
        const bytes_written = _.bytes_written + bytes.len;

        if (is_end) {
            if (bytes_written != content_length) {
                return globalObject.ERR(.HTTP_CONTENT_LENGTH_MISMATCH, "Content-Length mismatch", .{}).throw();
            }
        } else if (bytes_written > content_length) {
            return globalObject.ERR(.HTTP_CONTENT_LENGTH_MISMATCH, "Content-Length mismatch", .{}).throw();
        }
        _.bytes_written = bytes_written;
    } else {
        _.bytes_written +|= bytes.len;
    }
    if (is_end) {
        if (_.body_read_ref.has and _.body_read_state == .pending and (!_.flags.hasCustomOnData or js.onDataGetCached(this_value) == null)) {
            _.body_read_ref.unref(jsc.VirtualMachine.get());
            _.body_read_state = .none;
        }

        if (this_value != .zero) {
            js.onAbortedSetCached(this_value, globalObject, .zero);
        }

        raw_response.clearAborted();
        raw_response.clearOnWritable();
        raw_response.clearTimeout();
        _.flags.ended = true;
        if (!state.isHttpWriteCalled() or bytes.len > 0) {
            raw_response.end(bytes, state.isHttpConnectionClose());
        } else {
            raw_response.endStream(state.isHttpConnectionClose());
        }
        _.onRequestComplete();

        return jsc.JSValue.jsNumberFromUint64(bytes.len);
    } else {
        const js_this = if (this_value != .zero) this_value else _.getThisValue();
        switch (raw_response.write(bytes)) {
            .want_more => |written| {
                raw_response.clearOnWritable();
                js.onWritableSetCached(js_this, globalObject, .js_undefined);
                return jsc.JSValue.jsNumberFromUint64(written);
            },
            .backpressure => |written| {
                if (!callback_value.isUndefined()) {
                    js.onWritableSetCached(js_this, globalObject, callback_value.withAsyncContextIfNeeded(globalObject));
                    raw_response.onWritable(*NodeHTTPResponse, onDrain, _);
                }

                return jsc.JSValue.jsNumberFromInt64(-@as(i64, @intCast(@min(written, std.math.maxInt(i64)))));
            },
        }
    }
}

pub fn setOnWritable(_: *NodeHTTPResponse, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: JSValue): void {
    if (_.isDone() or value.isUndefined()) {
        js.onWritableSetCached(thisValue, globalObject, .js_undefined);
    } else {
        js.onWritableSetCached(thisValue, globalObject, value.withAsyncContextIfNeeded(globalObject));
    }
}

pub fn getOnWritable(_: *NodeHTTPResponse, thisValue: jsc.JSValue, _: *jsc.JSGlobalObject): jsc.JSValue {
    return js.onWritableGetCached(thisValue) orelse .js_undefined;
}

pub fn getOnAbort(_: *NodeHTTPResponse, thisValue: jsc.JSValue, _: *jsc.JSGlobalObject): jsc.JSValue {
    if (_.flags.socket_closed or _.flags.upgraded) {
        return .js_undefined;
    }
    return js.onAbortedGetCached(thisValue) orelse .js_undefined;
}

pub fn setOnAbort(_: *NodeHTTPResponse, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: JSValue): void {
    if (_.flags.socket_closed or _.flags.upgraded) {
        return;
    }

    if (_.isRequestedCompletedOrEnded() or value.isUndefined()) {
        js.onAbortedSetCached(thisValue, globalObject, .zero);
    } else {
        js.onAbortedSetCached(thisValue, globalObject, value.withAsyncContextIfNeeded(globalObject));
    }
}

pub fn getOnData(_: *NodeHTTPResponse, thisValue: jsc.JSValue, _: *jsc.JSGlobalObject): jsc.JSValue {
    return js.onDataGetCached(thisValue) orelse .js_undefined;
}

pub fn getHasCustomOnData(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    return .jsBoolean(_.flags.hasCustomOnData);
}

pub fn getUpgraded(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject): jsc.JSValue {
    return .jsBoolean(_.flags.upgraded);
}

pub fn setHasCustomOnData(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject, value: JSValue): void {
    _.flags.hasCustomOnData = value.toBoolean();
}

fn clearOnDataCallback(_: *NodeHTTPResponse, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject): void {
    log("clearOnDataCallback", .{});
    if (_.body_read_state != .none) {
        if (thisValue != .zero) {
            js.onDataSetCached(thisValue, globalObject, .js_undefined);
        }
        if (!_.flags.socket_closed and !_.flags.upgraded) {
            log("clearOnData", .{});
            if (_.raw_response) |raw_response| {
                raw_response.clearOnData();
            }
        }
        if (_.body_read_state != .done) {
            _.body_read_state = .done;
        }
    }
}

pub fn setOnData(_: *NodeHTTPResponse, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: JSValue): void {
    if (value.isUndefined() or _.flags.ended or _.flags.socket_closed or _.body_read_state != .pending or _.flags.is_data_buffered_during_pause_last or _.flags.upgraded) {
        js.onDataSetCached(thisValue, globalObject, .js_undefined);
        defer {
            if (_.body_read_ref.has) {
                _.body_read_ref.unref(globalObject.bunVM());
            }
        }
        switch (_.body_read_state) {
            .pending, .done => {
                if (!_.flags.request_has_completed and !_.flags.socket_closed and !_.flags.upgraded) {
                    log("clearOnData", .{});
                    if (_.raw_response) |raw_response| {
                        raw_response.clearOnData();
                    }
                }
                _.body_read_state = .done;
            },
            .none => {},
        }
        return;
    }

    js.onDataSetCached(thisValue, globalObject, value.withAsyncContextIfNeeded(globalObject));
    _.flags.hasCustomOnData = true;
    if (_.raw_response) |raw_response| {
        raw_response.onData(*NodeHTTPResponse, onData, _);
    }
    _.flags.is_data_buffered_during_pause = false;

    bun.debugAssert(_.body_read_ref.has);
}

pub fn write(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    const arguments = callframe.arguments();

    return writeOrEnd(_, globalObject, arguments, .zero, false);
}

pub fn onAutoFlush(_: *NodeHTTPResponse): bool {
    defer _.deref();
    if (!_.flags.socket_closed and !_.flags.upgraded and _.raw_response != null) {
        _.raw_response.?.uncork();
    }
    _.auto_flusher.registered = false;
    return false;
}

fn registerAutoFlush(_: *NodeHTTPResponse): void {
    if (_.auto_flusher.registered) return;
    _.ref();
    AutoFlusher.registerDeferredMicrotaskWithTypeUnchecked(NodeHTTPResponse, _, jsc.VirtualMachine.get());
}

fn unregisterAutoFlush(_: *NodeHTTPResponse): void {
    if (!_.auto_flusher.registered) return;
    AutoFlusher.unregisterDeferredMicrotaskWithTypeUnchecked(NodeHTTPResponse, _, jsc.VirtualMachine.get());
    _.deref();
}

pub fn flushHeaders(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    if (!_.flags.socket_closed and !_.flags.upgraded and _.raw_response != null) {
        const raw_response = _.raw_response.?;
        raw_response.flushHeaders(false);
        if (raw_response.isCorked()) {
            _.registerAutoFlush();
        }
    }

    return .js_undefined;
}

pub fn end(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    const arguments = callframe.arguments();
    resumeSocket(_);
    return writeOrEnd(_, globalObject, arguments, callframe.this(), true);
}

pub fn getBytesWritten(_: *NodeHTTPResponse, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): jsc.JSValue {
    return jsc.JSValue.jsNumber(_.bytes_written);
}

fn handleCorked(globalObject: *jsc.JSGlobalObject, function: jsc.JSValue, result: *JSValue, is_exception: *bool): void {
    result.* = function.call(globalObject, .js_undefined, &.{}) catch |err| {
        result.* = globalObject.takeException(err);
        is_exception.* = true;
        return;
    };
}

pub fn setTimeout(_: *NodeHTTPResponse, seconds: u8): void {
    if (_.flags.request_has_completed or _.flags.socket_closed or _.flags.upgraded or _.raw_response == null) {
        return;
    }

    _.raw_response.?.timeout(seconds);
}

export fn NodeHTTPResponse__setTimeout(_: *NodeHTTPResponse, seconds: jsc.JSValue, globalThis: *jsc.JSGlobalObject): bool {
    if (!seconds.isNumber()) {
        globalThis.throwInvalidArgumentTypeValue("timeout", "number", seconds) catch {};
        return false;
    }

    if (_.flags.request_has_completed or _.flags.socket_closed or _.flags.upgraded or _.raw_response == null) {
        return false;
    }

    _.raw_response.?.timeout(@intCast(@min(seconds.to(c_uint), 255)));
    return true;
}

pub fn cork(_: *NodeHTTPResponse, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    const arguments = callframe.arguments_old(1).slice();
    if (arguments.len == 0) {
        return globalObject.throwNotEnoughArguments("cork", 1, 0);
    }

    if (!arguments[0].isCallable()) {
        return globalObject.throwInvalidArgumentTypeValue("cork", "function", arguments[0]);
    }

    if (_.flags.request_has_completed or _.flags.socket_closed or _.flags.upgraded) {
        return globalObject.ERR(.STREAM_ALREADY_FINISHED, "Stream is already ended", .{}).throw();
    }

    var result: jsc.JSValue = .zero;
    var is_exception: bool = false;
    _.ref();
    defer _.deref();

    if (_.raw_response) |raw_response| {
        raw_response.corked(handleCorked, .{ globalObject, arguments[0], &result, &is_exception });
    } else {
        handleCorked(globalObject, arguments[0], &result, &is_exception);
    }
    if (is_exception) {
        if (result != .zero) {
            return globalObject.throwValue(result);
        } else {
            return globalObject.throw("unknown error", .{});
        }
    }

    if (result == .zero) {
        return .js_undefined;
    }

    return result;
}

pub fn finalize(_: *NodeHTTPResponse): void {
    _.deref();
}

pub fn release(_: *NodeHTTPResponse): void {
    bun.debugAssert(!_.body_read_ref.has);
    bun.debugAssert(!_.poll_ref.has);
    bun.debugAssert(!_.flags.is_request_pending);
    bun.debugAssert(_.flags.socket_closed or _.flags.request_has_completed);

    _.buffered_request_body_data_during_pause.deinit(bun.default_allocator);
    _.poll_ref.unref(jsc.VirtualMachine.get());
    _.body_read_ref.unref(jsc.VirtualMachine.get());

    _.promise.deinit();
    bun.destroy(_);
}

fn deinit(_: *NodeHTTPResponse): void {
    _.release();
}

comptime {
    @export(&create, .{ .name = "NodeHTTPResponse__createForJS" });
}

pub export fn Bun__NodeHTTPResponse_onClose(response: *NodeHTTPResponse, js_value: jsc.JSValue): void {
    response.onAbort(js_value);
}

pub export fn Bun__NodeHTTPResponse_setClosed(response: *NodeHTTPResponse): void {
    response.flags.socket_closed = true;
}

const string = []const u8;

import std from "std";

import bun from "bun";
const Environment = bun.Environment;
const HTTP = bun.http;
const Output = bun.Output;
const uws = bun.uws;
const HTTPStatusText = bun.api.server.HTTPStatusText;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
const AutoFlusher = jsc.WebCore.AutoFlusher;

const AnyServer = jsc.API.AnyServer;
const ServerWebSocket = jsc.API.ServerWebSocket;
