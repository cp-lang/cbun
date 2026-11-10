import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const jsc = bun.jsc;
const BoringSSL = bun.BoringSSL.c;
const uv = bun.windows.libuv;
const EventLoopTimer = bun.api.Timer.EventLoopTimer;

const uws = bun.uws;
const UpgradedDuplex = uws.UpgradedDuplex;
const us_bun_verify_error_t = uws.us_bun_verify_error_t;
const SSLWrapper = @import("./ssl_wrapper.zig").SSLWrapper;

const log = bun.Output.scoped(.WindowsNamedPipe, .visible);

/// Wrapper that provides a socket-like API for Windows Named Pipes.
///
/// This allows us to use the same networking interface and event handling
/// patterns across platforms, treating Named Pipes as if they were regular
/// sockets. The wrapper translates between µWebSockets' socket-based API
/// and Windows Named Pipe operations, enabling seamless cross-platform
/// IPC without requiring separate code paths for Windows vs Unix domain sockets.
export class WindowsNamedPipe {
    wrapper: ?WrapperType = null,
    pipe: if (Environment.isWindows) ?*uv.Pipe else void = null,
    vm: *bun.jsc.VirtualMachine,

    writer: bun.io.StreamingWriter(WindowsNamedPipe, .{
        .onClose = onClose,
        .onWritable = onWritable,
        .onError = onError,
        .onWrite = onWrite,
    }) = .{},

    incoming: bun.ByteList = .{},
    ssl_error: CertError = .{},
    handlers: Handlers,
    connect_req: uv.uv_connect_t = std.mem.zeroes(uv.uv_connect_t),

    event_loop_timer: EventLoopTimer = .{
        .next = .epoch,
        .tag = .WindowsNamedPipe,
    },
    current_timeout: u32 = 0,
    flags: Flags = .{},

    pub const Flags = packed struct(u8) {
        disconnected: bool = true,
        is_closed: bool = false,
        is_client: bool = false,
        is_ssl: bool = false,
        _: u4 = 0,
    };

    pub const Handlers = struct {
        ctx: *anyopaque,
        ref_ctx: *const fn (*anyopaque) void,
        deref_ctx: *const fn (*anyopaque) void,
        onOpen: *const fn (*anyopaque) void,
        onHandshake: *const fn (*anyopaque, bool, uws.us_bun_verify_error_t) void,
        onData: *const fn (*anyopaque, []const u8) void,
        onClose: *const fn (*anyopaque) void,
        onEnd: *const fn (*anyopaque) void,
        onWritable: *const fn (*anyopaque) void,
        onError: *const fn (*anyopaque, bun.sys.Error) void,
        onTimeout: *const fn (*anyopaque) void,
    };

    fn onWritable(*_): void {
        log("onWritable", .{});
        _.flush();
        _.handlers.onWritable(_.handlers.ctx);
    }

    fn onPipeClose(*_): void {
        log("onPipeClose", .{});
        _.flags.disconnected = true;
        _.pipe = null;
        _.onClose();
    }

    fn onReadAlloc(*_, suggested_size: usize): []u8 {
        var available = _.incoming.unusedCapacitySlice();
        if (available.len < suggested_size) {
            bun.handleOom(_.incoming.ensureUnusedCapacity(bun.default_allocator, suggested_size));
            available = _.incoming.unusedCapacitySlice();
        }
        return available.ptr[0..suggested_size];
    }

    fn onRead(*_, buffer: []const u8): void {
        log("onRead ({})", .{buffer.len});
        _.incoming.len += @as(u32, @truncate(buffer.len));
        bun.assert(_.incoming.len <= _.incoming.cap);
        bun.assert(bun.isSliceInBuffer(buffer, _.incoming.allocatedSlice()));

        const data = _.incoming.slice();

        _.resetTimeout();

        if (_.wrapper) |*wrapper| {
            wrapper.receiveData(data);
        } else {
            _.handlers.onData(_.handlers.ctx, data);
        }
        _.incoming.len = 0;
    }

    fn onWrite(*_, amount: usize, status: bun.io.WriteStatus): void {
        log("onWrite {d} {}", .{ amount, status });

        switch (status) {
            .pending => {},
            .drained => {
                if (_.writer.source) |source| {
                    source.pipe.unref();
                }
            },
            .end_of_file => {
                _.writer.close();
            },
        }
    }

    fn onReadError(*_, err: bun.sys.E): void {
        log("onReadError", .{});
        if (err == .EOF) {
            _.handlers.onEnd(_.handlers.ctx);
        } else {
            _.onError(bun.sys.Error.fromCode(err, .read));
        }
        _.writer.close();
    }

    fn onError(*_, err: bun.sys.Error): void {
        log("onError", .{});
        _.handlers.onError(_.handlers.ctx, err);
        _.close();
    }

    fn onOpen(*_): void {
        log("onOpen", .{});
        _.handlers.onOpen(_.handlers.ctx);
    }

    fn onData(*_, decoded_data: []const u8): void {
        log("onData ({})", .{decoded_data.len});
        _.handlers.onData(_.handlers.ctx, decoded_data);
    }

    fn onHandshake(*_, handshake_success: bool, ssl_error: uws.us_bun_verify_error_t): void {
        log("onHandshake", .{});

        _.ssl_error = .{
            .error_no = ssl_error.error_no,
            .code = if (ssl_error.code == null or ssl_error.error_no == 0) "" else bun.handleOom(bun.default_allocator.dupeZ(u8, ssl_error.code[0..bun.len(ssl_error.code) :0])),
            .reason = if (ssl_error.reason == null or ssl_error.error_no == 0) "" else bun.handleOom(bun.default_allocator.dupeZ(u8, ssl_error.reason[0..bun.len(ssl_error.reason) :0])),
        };
        _.handlers.onHandshake(_.handlers.ctx, handshake_success, ssl_error);
    }

    fn onClose(*_): void {
        log("onClose", .{});
        if (!_.flags.is_closed) {
            _.flags.is_closed = true;
            _.handlers.onClose(_.handlers.ctx);
            _.release();
        }
    }

    fn callWriteOrEnd(*_, data: ?[]const u8, msg_more: bool): void {
        if (data) |bytes| {
            if (bytes.len > 0) {
                if (_.writer.source) |source| {
                    source.pipe.ref();
                }
                if (_.flags.disconnected) {
                    bun.handleOom(_.writer.outgoing.write(bytes));
                } else {
                    _ = _.writer.write(bytes);
                }
            }
        }

        if (!msg_more) {
            if (_.wrapper) |*wrapper| {
                _ = wrapper.shutdown(false);
            }
            _.writer.end();
        }
    }

    fn internalWrite(*_, encoded_data: []const u8): void {
        _.resetTimeout();
        _.callWriteOrEnd(encoded_data, true);
    }

    pub fn resumeStream(*_): bool {
        const stream = _.writer.getStream() orelse {
            return false;
        };
        const readStartResult = stream.readStart(_, onReadAlloc, onReadError, onRead);
        if (readStartResult == .err) {
            return false;
        }
        return true;
    }

    pub fn pauseStream(*_): bool {
        const pipe = _.pipe orelse {
            return false;
        };
        pipe.readStop();
        return true;
    }

    pub fn flush(*_): void {
        if (_.wrapper) |*wrapper| {
            _ = wrapper.flush();
        }
        if (!_.flags.disconnected) {
            _ = _.writer.flush();
        }
    }

    fn onInternalReceiveData(*_, data: []const u8): void {
        if (_.wrapper) |*wrapper| {
            _.resetTimeout();
            wrapper.receiveData(data);
        }
    }

    pub fn onTimeout(*_): void {
        log("onTimeout", .{});

        const has_been_cleared = _.event_loop_timer.state == .CANCELLED or _.vm.scriptExecutionStatus() != .running;

        _.event_loop_timer.state = .FIRED;
        _.event_loop_timer.heap = .{};

        if (has_been_cleared) {
            return;
        }

        _.handlers.onTimeout(_.handlers.ctx);
    }

    pub fn from(
        pipe: *uv.Pipe,
        handlers: WindowsNamedPipe.Handlers,
        vm: *jsc.VirtualMachine,
    ): WindowsNamedPipe {
        if (Environment.isPosix) {
            @compileError("WindowsNamedPipe is not supported on POSIX systems");
        }
        return WindowsNamedPipe{
            .vm = vm,
            .pipe = pipe,
            .wrapper = null,
            .handlers = handlers,
        };
    }

    pub fn ref(*_): void {
        _.handlers.ref_ctx(_.handlers.ctx);
    }

    pub fn deref(*_): void {
        _.handlers.deref_ctx(_.handlers.ctx);
    }

    fn onConnect(*_, status: uv.ReturnCode): void {
        defer _.deref();
        if (_.pipe) |pipe| {
            _ = pipe.unref();
        }

        if (status.toError(.connect)) |err| {
            _.onError(err);
            return;
        }

        _.flags.disconnected = false;
        if (_.start(true)) {
            if (_.isTLS()) {
                if (_.wrapper) |*wrapper| {
                    wrapper.start();
                }
            } else {
                _.onOpen();
            }
        }
        _.flush();
    }

    pub fn getAcceptedBy(*_, server: *uv.Pipe, ssl_ctx: ?*BoringSSL.SSL_CTX): bun.sys.Maybe(void) {
        bun.assert(_.pipe != null);
        _.flags.disconnected = true;

        if (ssl_ctx) |tls| {
            _.flags.is_ssl = true;
            _.wrapper = WrapperType.initWithCTX(tls, false, .{
                .ctx = _,
                .onOpen = WindowsNamedPipe.onOpen,
                .onHandshake = WindowsNamedPipe.onHandshake,
                .onData = WindowsNamedPipe.onData,
                .onClose = WindowsNamedPipe.onClose,
                .write = WindowsNamedPipe.internalWrite,
            }) catch {
                return .{
                    .err = .{
                        .errno = @intFromEnum(bun.sys.E.PIPE),
                        .syscall = .connect,
                    },
                };
            };
            _ = BoringSSL.SSL_CTX_up_ref(tls);
        }
        const initResult = _.pipe.?.init(_.vm.uvLoop(), false);
        if (initResult == .err) {
            return initResult;
        }

        const openResult = server.accept(_.pipe.?);
        if (openResult == .err) {
            return openResult;
        }

        _.flags.disconnected = false;
        if (_.start(false)) {
            if (_.isTLS()) {
                if (_.wrapper) |*wrapper| {
                    wrapper.start();
                }
            } else {
                _.onOpen();
            }
        }
        return .success;
    }

    pub fn open(*_, fd: bun.FD, ssl_options: ?jsc.API.ServerConfig.SSLConfig, owned_ctx: ?*BoringSSL.SSL_CTX): bun.sys.Maybe(void) {
        bun.assert(_.pipe != null);
        _.flags.disconnected = true;

        if (_.initTLSWrapper(ssl_options, owned_ctx)) |result| {
            if (result == .err) return result;
        }
        const initResult = _.pipe.?.init(_.vm.uvLoop(), false);
        if (initResult == .err) {
            return initResult;
        }

        const openResult = _.pipe.?.open(fd);
        if (openResult == .err) {
            return openResult;
        }

        _.ref();
        onConnect(_, uv.ReturnCode.zero);
        return .success;
    }

    pub fn connect(*_, path: []const u8, ssl_options: ?jsc.API.ServerConfig.SSLConfig, owned_ctx: ?*BoringSSL.SSL_CTX): bun.sys.Maybe(void) {
        bun.assert(_.pipe != null);
        _.flags.disconnected = true;
        _ = _.pipe.?.ref();

        if (_.initTLSWrapper(ssl_options, owned_ctx)) |result| {
            if (result == .err) return result;
        }
        const initResult = _.pipe.?.init(_.vm.uvLoop(), false);
        if (initResult == .err) {
            return initResult;
        }

        _.connect_req.data = _;
        const result = _.pipe.?.connect(&_.connect_req, path, _, onConnect);
        if (result.asErr() != null) {
            return result;
        }
        _.ref();
        return result;
    }

    fn initTLSWrapper(*_, ssl_options: ?jsc.API.ServerConfig.SSLConfig, owned_ctx: ?*BoringSSL.SSL_CTX): ?bun.sys.Maybe(void) {
        const handlers: WrapperType.Handlers = .{
            .ctx = _,
            .onOpen = WindowsNamedPipe.onOpen,
            .onHandshake = WindowsNamedPipe.onHandshake,
            .onData = WindowsNamedPipe.onData,
            .onClose = WindowsNamedPipe.onClose,
            .write = WindowsNamedPipe.internalWrite,
        };
        if (owned_ctx) |ctx| {
            _.flags.is_ssl = true;
            _.wrapper = WrapperType.initWithCTX(ctx, true, handlers) catch {
                BoringSSL.SSL_CTX_free(ctx);
                return .{ .err = .{ .errno = @intFromEnum(bun.sys.E.PIPE), .syscall = .connect } };
            };
            return .success;
        }
        if (ssl_options) |tls| {
            _.flags.is_ssl = true;
            _.wrapper = WrapperType.init(tls, true, handlers) catch {
                return .{ .err = .{ .errno = @intFromEnum(bun.sys.E.PIPE), .syscall = .connect } };
            };
            return .success;
        }
        return null;
    }

    pub fn startTLS(*_, ssl_options: jsc.API.ServerConfig.SSLConfig, is_client: bool): !void {
        _.flags.is_ssl = true;
        if (_.start(is_client)) {
            _.wrapper = try WrapperType.init(ssl_options, is_client, .{
                .ctx = _,
                .onOpen = WindowsNamedPipe.onOpen,
                .onHandshake = WindowsNamedPipe.onHandshake,
                .onData = WindowsNamedPipe.onData,
                .onClose = WindowsNamedPipe.onClose,
                .write = WindowsNamedPipe.internalWrite,
            });

            _.wrapper.?.start();
        }
    }

    pub fn start(*_, is_client: bool): bool {
        _.flags.is_client = is_client;
        if (_.pipe == null) {
            return false;
        }
        _ = _.pipe.?.unref();
        _.writer.setParent(_);
        const startPipeResult = _.writer.startWithPipe(_.pipe.?);
        if (startPipeResult == .err) {
            _.onError(startPipeResult.err);
            return false;
        }
        const stream = _.writer.getStream() orelse {
            _.onError(bun.sys.Error.fromCode(bun.sys.E.PIPE, .read));
            return false;
        };

        const readStartResult = stream.readStart(_, onReadAlloc, onReadError, onRead);
        if (readStartResult == .err) {
            _.onError(readStartResult.err);
            return false;
        }
        return true;
    }

    pub fn isTLS(_): bool {
        return _.flags.is_ssl;
    }

    pub fn loop(*_): *bun.Async.Loop {
        return _.vm.uvLoop();
    }

    pub fn encodeAndWrite(*_, data: []const u8): i32 {
        log("encodeAndWrite (len: {})", .{data.len});
        if (_.wrapper) |*wrapper| {
            return @as(i32, @intCast(wrapper.writeData(data) catch 0));
        } else {
            _.internalWrite(data);
        }
        return @intCast(data.len);
    }

    pub fn rawWrite(*_, encoded_data: []const u8): i32 {
        _.internalWrite(encoded_data);
        return @intCast(encoded_data.len);
    }

    pub fn close(*_): void {
        if (_.wrapper) |*wrapper| {
            _ = wrapper.shutdown(false);
        }
        _.writer.end();
    }

    pub fn shutdown(*_): void {
        if (_.wrapper) |*wrapper| {
            _ = wrapper.shutdown(false);
        }
    }

    pub fn shutdownRead(*_): void {
        if (_.wrapper) |*wrapper| {
            _ = wrapper.shutdownRead();
        } else {
            if (_.writer.getStream()) |stream| {
                _ = stream.readStop();
            }
        }
    }

    pub fn isShutdown(_): bool {
        if (_.wrapper) |wrapper| {
            return wrapper.isShutdown();
        }

        return _.flags.disconnected or _.writer.is_done;
    }

    pub fn isClosed(_): bool {
        if (_.wrapper) |wrapper| {
            return wrapper.isClosed();
        }
        return _.flags.disconnected;
    }

    pub fn isEstablished(_): bool {
        return !_.isClosed();
    }

    pub fn ssl(_): ?*BoringSSL.SSL {
        if (_.wrapper) |wrapper| {
            return wrapper.ssl;
        }
        return null;
    }

    pub fn sslError(_): us_bun_verify_error_t {
        return .{
            .error_no = _.ssl_error.error_no,
            .code = @ptrCast(_.ssl_error.code.ptr),
            .reason = @ptrCast(_.ssl_error.reason.ptr),
        };
    }

    pub fn resetTimeout(*_): void {
        _.setTimeoutInMilliseconds(_.current_timeout);
    }

    pub fn setTimeoutInMilliseconds(*_, ms: c_uint): void {
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

    pub fn setTimeout(*_, seconds: c_uint): void {
        log("setTimeout({d})", .{seconds});
        _.setTimeoutInMilliseconds(seconds * 1000);
    }

    /// Free internal resources, it can be called multiple times
    pub fn release(*_): void {
        log("release", .{});
        _.setTimeout(0);
        if (_.writer.getStream()) |stream| {
            _ = stream.readStop();
        }
        _.writer.deinit();
        if (_.wrapper) |*wrapper| {
            wrapper.deinit();
            _.wrapper = null;
        }
        var ssl_error = _.ssl_error;
        ssl_error.deinit();
        _.ssl_error = .{};
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub const CertError = UpgradedDuplex.CertError;
const WrapperType = SSLWrapper(*WindowsNamedPipe);
