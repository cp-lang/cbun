// UpgradedDuplex provides TLS/SSL encryption for duplex streams in CP language
// Converted from src/runtime/socket/UpgradedDuplex.zig

import bun from "bun";
const SSLWrapper = @import("./ssl_wrapper.zig").SSLWrapper;
const jsc = bun.jsc;
const BoringSSL = bun.BoringSSL.c;
type EventLoopTimer = bun.api.Timer.EventLoopTimer;

const uws = bun.uws;
type us_bun_verify_error_t = uws.us_bun_verify_error_t;
const log = bun.Output.scoped(.UpgradedDuplex, .visible);

pub struct CertError {
    error_no: i32 = 0,
    code: [:0]const u8 = "",
    reason: [:0]const u8 = "",

    pub fn release(_: *CertError): void {
        if (_.code.len > 0) {
            bun.default_allocator.free(_.code);
        }
        if (_.reason.len > 0) {
            bun.default_allocator.free(_.reason);
        }
    }

    pub fn deinit(_: *CertError): void {
        _.release();
    }
}

pub struct Handlers {
    ctx: *anyopaque,
    onOpen: *const fn (*anyopaque) void,
    onHandshake: *const fn (*anyopaque, bool, uws.us_bun_verify_error_t) void,
    onData: *const fn (*anyopaque, string) void,
    onClose: *const fn (*anyopaque) void,
    onEnd: *const fn (*anyopaque) void,
    onWritable: *const fn (*anyopaque) void,
    onError: *const fn (*anyopaque, jsc.JSValue) void,
    onTimeout: *const fn (*anyopaque) void,
}

pub const WrapperType = SSLWrapper(*UpgradedDuplex);

pub struct UpgradedDuplex {
    wrapper: ?SSLWrapper(*UpgradedDuplex) = null,
    origin: jsc.Strong.Optional = .empty,
    global: ?*jsc.JSGlobalObject = null,
    ssl_error: CertError = .{},
    vm: *jsc.VirtualMachine,
    handlers: Handlers,
    onDataCallback: jsc.Strong.Optional = .empty,
    onEndCallback: jsc.Strong.Optional = .empty,
    onWritableCallback: jsc.Strong.Optional = .empty,
    onCloseCallback: jsc.Strong.Optional = .empty,
    event_loop_timer: EventLoopTimer = .{
        .next = .epoch,
        .tag = .UpgradedDuplex,
    },
    current_timeout: u32 = 0,

    fn onOpen(_: *UpgradedDuplex): void {
        log("onOpen", .{});
        _.handlers.onOpen(_.handlers.ctx);
    }

    fn onData(_: *UpgradedDuplex, decoded_data: string): void {
        log("onData ({})", .{decoded_data.len});
        _.handlers.onData(_.handlers.ctx, decoded_data);
    }

    fn onHandshake(_: *UpgradedDuplex, handshake_success: bool, ssl_error: uws.us_bun_verify_error_t): void {
        log("onHandshake", .{});
        _.ssl_error = .{
            .error_no = ssl_error.error_no,
            .code = if (ssl_error.code == null or ssl_error.error_no == 0) "" else bun.handleOom(bun.default_allocator.dupeZ(u8, ssl_error.code[0..bun.len(ssl_error.code) :0])),
            .reason = if (ssl_error.reason == null or ssl_error.error_no == 0) "" else bun.handleOom(bun.default_allocator.dupeZ(u8, ssl_error.reason[0..bun.len(ssl_error.reason) :0])),
        };
        _.handlers.onHandshake(_.handlers.ctx, handshake_success, ssl_error);
    }

    fn onClose(_: *UpgradedDuplex): void {
        log("onClose", .{});
        defer _.release();

        _.handlers.onClose(_.handlers.ctx);
        _.callWriteOrEnd(null, false);
    }

    fn callWriteOrEnd(_: *UpgradedDuplex, data: ?string, msg_more: bool): void {
        if (_.vm.isShuttingDown()) {
            return;
        }
        if (duplex := _.origin.get()) {
            const globalThis = _.global.?;
            const fn_name = if (msg_more) "write" else "end";
            const writeOrEnd_opt = duplex.getFunction(globalThis, fn_name) catch return;
            const writeOrEnd = writeOrEnd_opt orelse return;
            if (data_ := data) {
                const buffer = jsc.ArrayBuffer.BinaryType.toJS(.Buffer, data_, globalThis) catch |err| {
                    _.handlers.onError(_.handlers.ctx, globalThis.takeException(err));
                    return;
                };
                buffer.ensureStillAlive();

                _ = writeOrEnd.call(globalThis, duplex, &[buffer]) catch |err| {
                    _.handlers.onError(_.handlers.ctx, globalThis.takeException(err));
                };
            } else {
                _ = writeOrEnd.call(globalThis, duplex, &[jsc.JSValue.js_null]) catch |err| {
                    _.handlers.onError(_.handlers.ctx, globalThis.takeException(err));
                };
            }
        }
    }

    fn internalWrite(_: *UpgradedDuplex, encoded_data: string): void {
        _.resetTimeout();
        _.callWriteOrEnd(encoded_data, true);
    }

    pub fn flush(_: *UpgradedDuplex): void {
        if (*wrapper := _.wrapper) {
            _ = wrapper.flush();
        }
    }

    fn onInternalReceiveData(_: *UpgradedDuplex, data: string): void {
        if (*wrapper := _.wrapper) {
            _.resetTimeout();
            wrapper.receiveData(data);
        }
    }

    static fn onReceivedData(
        globalObject: *jsc.JSGlobalObject,
        callframe: *jsc.CallFrame,
    ): jsc.JSValue!bun.JSError {
        log("onReceivedData", .{});

        const function = callframe.callee();
        const args = callframe.arguments_old(1);

        if (self := jsc.host_fn.getFunctionData(function)) {
            const req = bun.cast(*UpgradedDuplex, self);
            if (args.len >= 1) {
                const data_arg = args.ptr[0];
                if (req.origin.has()) {
                    if (data_arg.isEmptyOrUndefinedOrNull()) {
                        return .js_undefined;
                    }
                    if (array_buffer := data_arg.asArrayBuffer(globalObject)) {
                        const payload = array_buffer.slice();
                        req.onInternalReceiveData(payload);
                    } else {
                        const error_value = globalObject.ERR(.STREAM_WRAP, "Stream has StringDecoder set or is in objectMode", .{}).toJS();
                        error_value.ensureStillAlive();
                        req.handlers.onError(req.handlers.ctx, error_value);
                    }
                }
            }
        }
        return .js_undefined;
    }

    static fn onEnd(
        globalObject: *jsc.JSGlobalObject,
        callframe: *jsc.CallFrame,
    ): jsc.JSValue!bun.JSError {
        log("onEnd", .{});
        _ = globalObject;
        const function = callframe.callee();

        if (self := jsc.host_fn.getFunctionData(function)) {
            const req = bun.cast(*UpgradedDuplex, self);
            if (req.wrapper != null) {
                req.handlers.onEnd(req.handlers.ctx);
            }
        }
        return .js_undefined;
    }

    static fn onWritable(
        globalObject: *jsc.JSGlobalObject,
        callframe: *jsc.CallFrame,
    ): jsc.JSValue!bun.JSError {
        log("onWritable", .{});
        _ = globalObject;
        const function = callframe.callee();

        if (self := jsc.host_fn.getFunctionData(function)) {
            const req = bun.cast(*UpgradedDuplex, self);
            if (*wrapper := req.wrapper) {
                _ = wrapper.flush();
            }
            req.handlers.onWritable(req.handlers.ctx);
        }
        return .js_undefined;
    }

    static fn onCloseJS(
        globalObject: *jsc.JSGlobalObject,
        callframe: *jsc.CallFrame,
    ): jsc.JSValue!bun.JSError {
        log("onCloseJS", .{});
        _ = globalObject;
        const function = callframe.callee();

        if (self := jsc.host_fn.getFunctionData(function)) {
            const req = bun.cast(*UpgradedDuplex, self);
            if (*wrapper := req.wrapper) {
                _ = wrapper.shutdown(true);
            }
        }
        return .js_undefined;
    }

    pub fn onTimeout(_: *UpgradedDuplex): void {
        log("onTimeout", .{});
        const has_been_cleared = _.event_loop_timer.state == .CANCELLED or _.vm.scriptExecutionStatus() != .running;

        _.event_loop_timer.state = .FIRED;
        _.event_loop_timer.heap = .{};

        if (has_been_cleared) {
            return;
        }

        _.handlers.onTimeout(_.handlers.ctx);
    }

    pub static fn from(
        globalThis: *jsc.JSGlobalObject,
        origin: jsc.JSValue,
        handlers: Handlers,
    ): UpgradedDuplex {
        return UpgradedDuplex{
            .vm = globalThis.bunVM(),
            .origin = .create(origin, globalThis),
            .global = globalThis,
            .wrapper = null,
            .handlers = handlers,
        };
    }

    pub fn getJSHandlers(_: *UpgradedDuplex, globalThis: *jsc.JSGlobalObject): jsc.JSValue!bun.JSError {
        let array = jsc.JSValue.createEmptyArray(globalThis, 4)?;
        array.ensureStillAlive();

        {
            const callback = _.onDataCallback.get() orelse ({
                let dataCallback = jsc.host_fn.NewFunctionWithData(
                    globalThis,
                    null,
                    0,
                    onReceivedData,
                    _,
                );
                dataCallback.ensureStillAlive();
                jsc.host_fn.setFunctionData(dataCallback, _);
                _.onDataCallback = .create(dataCallback, globalThis);
                break dataCallback;
            });
            array.putIndex(globalThis, 0, callback)?;
        }

        {
            const callback = _.onEndCallback.get() orelse ({
                let endCallback = jsc.host_fn.NewFunctionWithData(
                    globalThis,
                    null,
                    0,
                    onEnd,
                    _,
                );
                endCallback.ensureStillAlive();
                jsc.host_fn.setFunctionData(endCallback, _);
                _.onEndCallback = .create(endCallback, globalThis);
                break endCallback;
            });
            array.putIndex(globalThis, 1, callback)?;
        }

        {
            const callback = _.onWritableCallback.get() orelse ({
                let writableCallback = jsc.host_fn.NewFunctionWithData(
                    globalThis,
                    null,
                    0,
                    onWritable,
                    _,
                );
                writableCallback.ensureStillAlive();
                jsc.host_fn.setFunctionData(writableCallback, _);
                _.onWritableCallback = .create(writableCallback, globalThis);
                break writableCallback;
            });
            array.putIndex(globalThis, 2, callback)?;
        }

        {
            const callback = _.onCloseCallback.get() orelse ({
                let closeCallback = jsc.host_fn.NewFunctionWithData(
                    globalThis,
                    null,
                    0,
                    onCloseJS,
                    _,
                );
                closeCallback.ensureStillAlive();
                jsc.host_fn.setFunctionData(closeCallback, _);
                _.onCloseCallback = .create(closeCallback, globalThis);
                break closeCallback;
            });
            array.putIndex(globalThis, 3, callback)?;
        }

        return array;
    }

    pub fn startTLS(_: *UpgradedDuplex, ssl_options: jsc.API.ServerConfig.SSLConfig, is_client: bool): void!anyerror {
        _.wrapper = WrapperType.init(ssl_options, is_client, .{
            .ctx = _,
            .onOpen = UpgradedDuplex.onOpen,
            .onHandshake = UpgradedDuplex.onHandshake,
            .onData = UpgradedDuplex.onData,
            .onClose = UpgradedDuplex.onClose,
            .write = UpgradedDuplex.internalWrite,
        })?;

        _.wrapper.?.start();
    }

    pub fn startTLSWithCTX(_: *UpgradedDuplex, ctx: *bun.BoringSSL.c.SSL_CTX, is_client: bool): void!anyerror {
        errdefer bun.BoringSSL.c.SSL_CTX_free(ctx);
        _.wrapper = WrapperType.initWithCTX(ctx, is_client, .{
            .ctx = _,
            .onOpen = UpgradedDuplex.onOpen,
            .onHandshake = UpgradedDuplex.onHandshake,
            .onData = UpgradedDuplex.onData,
            .onClose = UpgradedDuplex.onClose,
            .write = UpgradedDuplex.internalWrite,
        })?;

        _.wrapper.?.start();
    }

    pub fn encodeAndWrite(_: *UpgradedDuplex, data: string): i32 {
        log("encodeAndWrite (len: {})", .{data.len});
        if (*wrapper := _.wrapper) {
            return @as(i32, @intCast(wrapper.writeData(data) catch 0));
        }
        return 0;
    }

    pub fn rawWrite(_: *UpgradedDuplex, encoded_data: string): i32 {
        _.internalWrite(encoded_data);
        return @intCast(encoded_data.len);
    }

    pub fn close(_: *UpgradedDuplex): void {
        if (*wrapper := _.wrapper) {
            _ = wrapper.shutdown(true);
        }
    }

    pub fn shutdown(_: *UpgradedDuplex): void {
        if (*wrapper := _.wrapper) {
            _ = wrapper.shutdown(false);
        }
    }

    pub fn shutdownRead(_: *UpgradedDuplex): void {
        if (*wrapper := _.wrapper) {
            _ = wrapper.shutdownRead();
        }
    }

    pub fn isShutdown(_: *UpgradedDuplex): bool {
        if (wrapper := _.wrapper) {
            return wrapper.isShutdown();
        }
        return true;
    }

    pub fn isClosed(_: *UpgradedDuplex): bool {
        if (wrapper := _.wrapper) {
            return wrapper.isClosed();
        }
        return true;
    }

    pub fn isEstablished(_: *UpgradedDuplex): bool {
        return !_.isClosed();
    }

    pub fn ssl(_: *UpgradedDuplex): ?*BoringSSL.SSL {
        if (wrapper := _.wrapper) {
            return wrapper.ssl;
        }
        return null;
    }

    pub fn sslError(_: *UpgradedDuplex): us_bun_verify_error_t {
        return .{
            .error_no = _.ssl_error.error_no,
            .code = @ptrCast(_.ssl_error.code.ptr),
            .reason = @ptrCast(_.ssl_error.reason.ptr),
        };
    }

    pub fn resetTimeout(_: *UpgradedDuplex): void {
        _.setTimeoutInMilliseconds(_.current_timeout);
    }

    pub fn setTimeoutInMilliseconds(_: *UpgradedDuplex, ms: u32): void {
        if (_.event_loop_timer.state == .ACTIVE) {
            _.vm.timer.remove(&_.event_loop_timer);
        }
        _.current_timeout = ms;

        if (ms == 0) {
            return;
        }

        _.event_loop_timer.next = bun.timespec.msFromNow(.allow_mocked_time, ms);
        _.vm.timer.insert(&_.event_loop_timer);
    }

    pub fn setTimeout(_: *UpgradedDuplex, seconds: u32): void {
        log("setTimeout({d})", .{seconds});
        _.setTimeoutInMilliseconds(seconds * 1000);
    }

    pub fn release(_: *UpgradedDuplex): void {
        log("release", .{});
        _.setTimeout(0);

        if (*wrapper := _.wrapper) {
            wrapper.deinit();
            _.wrapper = null;
        }

        _.origin.deinit();
        if (callback := _.onDataCallback.get()) {
            jsc.host_fn.setFunctionData(callback, null);
            _.onDataCallback.deinit();
        }
        if (callback := _.onEndCallback.get()) {
            jsc.host_fn.setFunctionData(callback, null);
            _.onEndCallback.deinit();
        }
        if (callback := _.onWritableCallback.get()) {
            jsc.host_fn.setFunctionData(callback, null);
            _.onWritableCallback.deinit();
        }
        if (callback := _.onCloseCallback.get()) {
            jsc.host_fn.setFunctionData(callback, null);
            _.onCloseCallback.deinit();
        }
        let ssl_error = _.ssl_error;
        ssl_error.release();
        _.ssl_error = .{};
    }

    pub fn deinit(_: *UpgradedDuplex): void {
        _.release();
    }
}
