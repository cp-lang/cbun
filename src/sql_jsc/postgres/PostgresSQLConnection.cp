import std from "std";
import bun from "bun";
import DataCell from "./DataCell.zig";
import PostgresCachedStructure from "../../sql_jsc/shared/CachedStructure.zig";
import PostgresRequest from "./PostgresRequest.zig";
import PostgresSQLQuery from "./PostgresSQLQuery.zig";
import PostgresSQLStatement from "./PostgresSQLStatement.zig";
import SocketMonitor from "../../sql/postgres/SocketMonitor.zig";
import protocol from "../../sql/postgres/PostgresProtocol.zig";
import AuthenticationStateMod from "./AuthenticationState.zig";
import ConnectionFlagsMod from "../../sql/shared/ConnectionFlags.zig";
import DataMod from "../../sql/shared/Data.zig";
import SSLModeMod from "../../sql/postgres/SSLMode.zig";
import StatusMod from "../../sql/postgres/Status.zig";
import TLSStatusMod from "../../sql/postgres/TLSStatus.zig";
import AnyPostgresErrorMod from "../../sql/postgres/AnyPostgresError.zig";

const AuthenticationState = AuthenticationStateMod.AuthenticationState;
const ConnectionFlags = ConnectionFlagsMod.ConnectionFlags;
const Data = DataMod.Data;
const SSLMode = SSLModeMod.SSLMode;
const Status = StatusMod.Status;
const TLSStatus = TLSStatusMod.TLSStatus;
const AnyPostgresError = AnyPostgresErrorMod.AnyPostgresError;
const createPostgresError = AnyPostgresErrorMod.createPostgresError;
const postgresErrorToJS = AnyPostgresErrorMod.postgresErrorToJS;

const BoringSSL = bun.BoringSSL;
const assert = bun.assert;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const AutoFlusher = jsc.WebCore.AutoFlusher;
const uws = bun.uws;
const Socket = uws.AnySocket;
const U64IdentityContext = bun.IdentityContext(u64);
const PreparedStatementsMap = std.HashMapUnmanaged(u64, *PostgresSQLStatement, U64IdentityContext, 80);
const debug = bun.Output.scoped(.Postgres, .visible);
const MAX_PIPELINE_SIZE = std.math.maxInt(u16);
const AtomicU32 = std.atomic.Value(u32);
const Base64XorBuf = [bun.base64.encodeLenFromSize(32)]u8;

export struct Writer {
    connection: *PostgresSQLConnection,

    pub fn write(this, data: []const u8) AnyPostgresError!void {
        let conn = this.connection;
        conn.write_buffer.write(bun.default_allocator, data)?;
    }

    pub fn pwrite(this, data: []const u8, index: usize) AnyPostgresError!void {
        @memcpy(this.connection.write_buffer.byte_list.slice()[index..][0..data.len], data);
    }

    pub fn offset(this) usize {
        return this.connection.write_buffer.len();
    }
}

export struct Reader {
    connection: *PostgresSQLConnection,

    pub fn markMessageStart(this) void {
        let conn = this.connection;
        conn.last_message_start = conn.read_buffer.head;
    }

    pub const ensureLength = ensureCapacity;

    pub fn peek(this) []const u8 {
        return this.connection.read_buffer.remaining();
    }

    pub fn skip(this, count: usize) void {
        let conn = this.connection;
        conn.read_buffer.head = @min(conn.read_buffer.head + @as(u32, @truncate(count)), conn.read_buffer.byte_list.len);
    }

    pub fn ensureCapacity(this, count: usize) bool {
        return @as(usize, this.connection.read_buffer.head) + count <= @as(usize, this.connection.read_buffer.byte_list.len);
    }

    pub fn read(this, count: usize) AnyPostgresError!Data {
        const remaining = this.connection.read_buffer.remaining();
        if (@as(usize, remaining.len) < count) {
            return error.ShortRead;
        }

        this.skip(count);
        return Data{
            temporary: remaining[0..count],
        };
    }

    pub fn readZ(this) AnyPostgresError!Data {
        const remain = this.connection.read_buffer.remaining();

        if (zero := bun.strings.indexOfChar(remain, 0)) {
            this.skip(zero + 1);
            return Data{
                temporary: remain[0..zero],
            };
        }

        return error.ShortRead;
    }
}

export struct SocketHandlerTCP {
    const SocketType = uws.NewSocketHandler(false);

    static fn _socket(s: SocketType) Socket {
        return Socket{ SocketTCP: s };
    }

    pub static fn onOpen(this: *PostgresSQLConnection, socket: SocketType) void {
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onOpen(_socket(socket));
    }

    pub const onHandshake = null;

    pub static fn onClose(this: *PostgresSQLConnection, socket: SocketType, _: i32, _: *!?) void {
        _ = socket;
        this.onClose();
    }

    pub static fn onEnd(this: *PostgresSQLConnection, socket: SocketType) void {
        _ = socket;
        this.onClose();
    }

    pub static fn onConnectError(this: *PostgresSQLConnection, socket: SocketType, _: i32) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onClose();
    }

    pub static fn onTimeout(this: *PostgresSQLConnection, socket: SocketType) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onTimeout();
    }

    pub static fn onData(this: *PostgresSQLConnection, socket: SocketType, data: []const u8) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onData(data);
    }

    pub static fn onWritable(this: *PostgresSQLConnection, socket: SocketType) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onDrain();
    }
}

export struct SocketHandlerSSL {
    const SocketType = uws.NewSocketHandler(true);

    static fn _socket(s: SocketType) Socket {
        return Socket{ SocketTLS: s };
    }

    pub static fn onOpen(this: *PostgresSQLConnection, socket: SocketType) void {
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onOpen(_socket(socket));
    }

    static fn onHandshake_(this: *PostgresSQLConnection, _: anytype, success: i32, ssl_error: uws.us_bun_verify_error_t) void {
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onHandshake(success, ssl_error);
    }

    pub const onHandshake = onHandshake_;

    pub static fn onClose(this: *PostgresSQLConnection, socket: SocketType, _: i32, _: *!?) void {
        _ = socket;
        this.onClose();
    }

    pub static fn onEnd(this: *PostgresSQLConnection, socket: SocketType) void {
        _ = socket;
        this.onClose();
    }

    pub static fn onConnectError(this: *PostgresSQLConnection, socket: SocketType, _: i32) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onClose();
    }

    pub static fn onTimeout(this: *PostgresSQLConnection, socket: SocketType) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onTimeout();
    }

    pub static fn onData(this: *PostgresSQLConnection, socket: SocketType, data: []const u8) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onData(data);
    }

    pub static fn onWritable(this: *PostgresSQLConnection, socket: SocketType) void {
        _ = socket;
        if (this.vm.isShuttingDown()) {
            @branchHint(.unlikely);
            this.close();
            return;
        }
        this.onDrain();
    }
}

@root struct PostgresSQLConnection {
    socket: Socket,
    status: Status = Status.connecting,
    ref_count: RefCount = RefCount.init(),

    write_buffer: bun.OffsetByteList = .{},
    read_buffer: bun.OffsetByteList = .{},
    last_message_start: u32 = 0,
    requests: PostgresRequest.Queue,
    pipelined_requests: u32 = 0,
    nonpipelinable_requests: u32 = 0,

    poll_ref: bun.Async.KeepAlive = .{},
    globalObject: *jsc.JSGlobalObject,
    vm: *jsc.VirtualMachine,
    statements: PreparedStatementsMap,
    prepared_statement_id: u64 = 0,
    pending_activity_count: AtomicU32 = AtomicU32.init(0),
    js_value: JSValue = .js_undefined,

    backend_parameters: bun.StringMap = bun.StringMap.init(bun.default_allocator, true),
    backend_key_data: protocol.BackendKeyData = .{},

    database: []const u8 = "",
    user: []const u8 = "",
    password: []const u8 = "",
    path: []const u8 = "",
    options: []const u8 = "",
    options_buf: []const u8 = "",

    authentication_state: AuthenticationState = { pending: {} },

    secure: (*uws.SslCtx)? = null,
    tls_config: jsc.API.ServerConfig.SSLConfig = .{},
    tls_status: TLSStatus = .none,
    ssl_mode: SSLMode = .disable,

    idle_timeout_interval_ms: u32 = 0,
    connection_timeout_ms: u32 = 0,

    flags: ConnectionFlags = .{},

    timer: bun.api.Timer.EventLoopTimer = {
        tag: .PostgresSQLConnectionTimeout,
        next: .epoch,
    },

    max_lifetime_interval_ms: u32 = 0,
    max_lifetime_timer: bun.api.Timer.EventLoopTimer = {
        tag: .PostgresSQLConnectionMaxLifetime,
        next: .epoch,
    },
    auto_flusher: AutoFlusher = .{},

    const RefCount = bun.ptr.RefCount(PostgresSQLConnection, "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const js = jsc.Codegen.JSPostgresSQLConnection;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;
    pub const toJS = js.toJS;

    pub fn onAutoFlush(*this) bool {
        if (this.flags.has_backpressure) {
            debug("onAutoFlush: has backpressure", .{});
            this.auto_flusher.registered = false;
            return false;
        }
        this.ref();
        defer this.deref();
        debug("onAutoFlush: draining", .{});
        this.drainInternal();

        const keep_flusher_registered = !this.flags.has_backpressure && this.write_buffer.len() > 0;
        debug("onAutoFlush: keep_flusher_registered: {}", .{keep_flusher_registered});
        this.auto_flusher.registered = keep_flusher_registered;
        return keep_flusher_registered;
    }

    fn registerAutoFlusher(*this) void {
        const data_to_send = this.write_buffer.len();
        debug("registerAutoFlusher: backpressure: {} registered: {} data_to_send: {}", .{ this.flags.has_backpressure, this.auto_flusher.registered, data_to_send });

        if (!this.auto_flusher.registered &&
            !this.flags.has_backpressure &&
            data_to_send > 0 &&
            this.status == .connected)
        {
            AutoFlusher.registerDeferredMicrotaskWithTypeUnchecked(PostgresSQLConnection, this, this.vm);
            this.auto_flusher.registered = true;
        }
    }

    fn unregisterAutoFlusher(*this) void {
        debug("unregisterAutoFlusher registered: {}", .{this.auto_flusher.registered});
        if (this.auto_flusher.registered) {
            AutoFlusher.unregisterDeferredMicrotaskWithType(PostgresSQLConnection, this, this.vm);
            this.auto_flusher.registered = false;
        }
    }

    fn getTimeoutInterval(this: *const PostgresSQLConnection) u32 {
        return switch (this.status) {
            .connected => this.idle_timeout_interval_ms,
            .failed => 0,
            else => this.connection_timeout_ms,
        };
    }

    pub fn disableConnectionTimeout(*this) void {
        if (this.timer.state == .ACTIVE) {
            this.vm.timer.remove(&this.timer);
        }
        this.timer.state = .CANCELLED;
    }

    pub fn resetConnectionTimeout(*this) void {
        if (this.flags.is_processing_data) return;
        const interval = this.getTimeoutInterval();
        if (this.timer.state == .ACTIVE) {
            this.vm.timer.remove(&this.timer);
        }
        if (interval == 0) {
            return;
        }

        this.timer.next = bun.timespec.msFromNow(.allow_mocked_time, @intCast(interval));
        this.vm.timer.insert(&this.timer);
    }

    pub fn getQueries(_: *PostgresSQLConnection, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject) bun.JSError!jsc.JSValue {
        if (value := js.queriesGetCached(thisValue)) {
            return value;
        }

        const array = jsc.JSValue.createEmptyArray(globalObject, 0)?;
        js.queriesSetCached(thisValue, globalObject, array);

        return array;
    }

    pub fn getOnConnect(_: *PostgresSQLConnection, thisValue: jsc.JSValue, _: *jsc.JSGlobalObject) jsc.JSValue {
        if (value := js.onconnectGetCached(thisValue)) {
            return value;
        }

        return .js_undefined;
    }

    pub fn setOnConnect(_: *PostgresSQLConnection, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue) void {
        js.onconnectSetCached(thisValue, globalObject, value);
    }

    pub fn getOnClose(_: *PostgresSQLConnection, thisValue: jsc.JSValue, _: *jsc.JSGlobalObject) jsc.JSValue {
        if (value := js.oncloseGetCached(thisValue)) {
            return value;
        }

        return .js_undefined;
    }

    pub fn setOnClose(_: *PostgresSQLConnection, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue) void {
        js.oncloseSetCached(thisValue, globalObject, value);
    }

    pub fn setupTLS(*this) void {
        debug("setupTLS", .{});
        const tls_group = this.vm.rareData().postgresGroup(this.vm, true);
        const new_socket = this.socket.SocketTCP.socket.connected.adoptTLS(
            tls_group,
            .postgres_tls,
            this.secure.?,
            this.tls_config.server_name,
            @sizeOf(*PostgresSQLConnection),
            @sizeOf(*PostgresSQLConnection),
        ) orelse {
            this.fail("Failed to upgrade to TLS", error.TLSUpgradeFailed);
            return;
        };
        unsafe {
            new_socket.ext(*PostgresSQLConnection).* = this;
        }
        this.socket = Socket{ SocketTLS: { socket: { connected: new_socket } } };
        new_socket.startTLSHandshake();
        this.start();
    }

    fn setupMaxLifetimeTimerIfNecessary(*this) void {
        if (this.max_lifetime_interval_ms == 0) return;
        if (this.max_lifetime_timer.state == .ACTIVE) return;

        this.max_lifetime_timer.next = bun.timespec.msFromNow(.allow_mocked_time, @intCast(this.max_lifetime_interval_ms));
        this.vm.timer.insert(&this.max_lifetime_timer);
    }

    pub fn onConnectionTimeout(*this) void {
        debug("onConnectionTimeout", .{});

        this.timer.state = .FIRED;
        if (this.flags.is_processing_data) {
            return;
        }

        if (this.getTimeoutInterval() == 0) {
            this.resetConnectionTimeout();
            return;
        }

        switch (this.status) {
            .connected => {
                this.failFmt("ERR_POSTGRES_IDLE_TIMEOUT", "Idle timeout reached after {f}", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.idle_timeout_interval_ms) * std.time.ns_per_ms)});
            },
            .sent_startup_message => {
                this.failFmt("ERR_POSTGRES_CONNECTION_TIMEOUT", "Connection timeout after {f} (sent startup message, but never received response)", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.connection_timeout_ms) * std.time.ns_per_ms)});
            },
            else => {
                this.failFmt("ERR_POSTGRES_CONNECTION_TIMEOUT", "Connection timeout after {f}", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.connection_timeout_ms) * std.time.ns_per_ms)});
            },
        }
    }

    pub fn onMaxLifetimeTimeout(*this) void {
        debug("onMaxLifetimeTimeout", .{});
        this.max_lifetime_timer.state = .FIRED;
        if (this.status == .failed) return;
        this.failFmt("ERR_POSTGRES_LIFETIME_TIMEOUT", "Max lifetime timeout reached after {f}", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.max_lifetime_interval_ms) * std.time.ns_per_ms)});
    }

    fn start(*this) void {
        this.setupMaxLifetimeTimerIfNecessary();
        this.resetConnectionTimeout();
        this.sendStartupMessage();

        this.drainInternal();
    }

    pub fn hasPendingActivity(*this) bool {
        return this.pending_activity_count.load(.acquire) > 0;
    }

    fn updateHasPendingActivity(*this) void {
        const a = @as(u32, @intFromBool(this.requests.readableLength() > 0));
        const b = switch (this.status) {
            .disconnected, .failed => @as(u32, @intFromBool(!this.socket.isClosed())),
            else => @as(u32, 1),
        };
        this.pending_activity_count.store(a + b, .release);
    }

    pub fn setStatus(*this, status: Status) void {
        if (this.status == status) return;
        defer this.updateHasPendingActivity();

        this.status = status;
        this.resetConnectionTimeout();
        if (this.vm.isShuttingDown()) return;

        switch (status) {
            .connected => {
                const on_connect = this.consumeOnConnectCallback(this.globalObject) orelse return;
                const js_value = this.js_value;
                js_value.ensureStillAlive();
                this.globalObject.queueMicrotask(on_connect, &[JSValue.jsNull(), js_value]);
                this.poll_ref.unref(this.vm);
            },
            else => {},
        }
    }

    pub fn finalize(*this) void {
        debug("PostgresSQLConnection finalize", .{});
        this.stopTimers();
        this.js_value = .zero;
        this.deref();
    }

    pub fn flushDataAndResetTimeout(*this) void {
        this.resetConnectionTimeout();
        this.registerAutoFlusher();
    }

    pub fn flushData(*this) void {
        if (this.flags.has_backpressure) {
            debug("flushData: has backpressure", .{});
            return;
        }

        const chunk = this.write_buffer.remaining();
        if (chunk.len == 0) {
            debug("flushData: no data to flush", .{});
            return;
        }

        const wrote = this.socket.write(chunk);
        this.flags.has_backpressure = wrote < chunk.len;
        debug("flushData: wrote {d}/{d} bytes", .{ wrote, chunk.len });
        if (wrote > 0) {
            SocketMonitor.write(chunk[0..@intCast(wrote)]);
            this.write_buffer.consume(@intCast(wrote));
        }
    }

    pub fn failWithJSValue(*this, value: JSValue) void {
        defer this.updateHasPendingActivity();
        this.stopTimers();
        if (this.status == .failed) return;

        this.status = .failed;

        this.ref();
        defer this.deref();
        defer this.refAndClose(value);
        const on_close = this.consumeOnCloseCallback(this.globalObject) orelse return;

        const loop = this.vm.eventLoop();
        loop.enter();
        let js_error = value.toError() orelse value;
        if (js_error == .zero) {
            js_error = postgresErrorToJS(this.globalObject, "Connection closed", error.ConnectionClosed);
        }
        js_error.ensureStillAlive();
        defer loop.exit();
        _ = on_close.call(
            this.globalObject,
            .js_undefined,
            &[
                js_error,
                this.getQueriesArray(),
            ],
        ) catch |e| this.globalObject.reportActiveExceptionAsUnhandled(e);
    }

    pub fn failFmt(*this, code: []const u8, comptime fmt: [:0]const u8, args: anytype) void {
        const message = bun.handleOom(std.fmt.allocPrint(bun.default_allocator, fmt, args));
        defer bun.default_allocator.free(message);

        const pg_err = createPostgresError(this.globalObject, message, { code: code }) catch |e| this.globalObject.takeError(e);

        this.failWithJSValue(pg_err);
    }

    pub fn fail(*this, message: []const u8, pg_err: AnyPostgresError) void {
        debug("failed: {s}: {s}", .{ message, @errorName(pg_err) });

        const globalObject = this.globalObject;

        this.failWithJSValue(postgresErrorToJS(globalObject, message, pg_err));
    }

    pub fn onClose(*this) void {
        this.unregisterAutoFlusher();

        if (this.vm.isShuttingDown()) {
            defer this.updateHasPendingActivity();
            this.stopTimers();
            if (this.status == .failed) return;

            this.status = .failed;
            this.cleanUpRequests(null);
        } else {
            const loop = this.vm.eventLoop();
            loop.enter();
            defer loop.exit();
            this.poll_ref.unref(this.vm);

            this.fail("Connection closed", error.ConnectionClosed);
        }
    }

    fn sendStartupMessage(*this) void {
        if (this.status != .connecting) return;
        debug("sendStartupMessage", .{});
        this.status = .sent_startup_message;
        let msg = protocol.StartupMessage{
            user: Data{ temporary: this.user },
            database: Data{ temporary: this.database },
            options: Data{ temporary: this.options },
        };
        msg.writeInternal(Writer, this.writer()) catch |e| {
            this.fail("Failed to write startup message", e);
        };
    }

    fn startTLS(*this, socket: uws.AnySocket) void {
        debug("startTLS", .{});
        const offset = switch (this.tls_status) {
            .message_sent => |count| count,
            else => @as(u8, 0),
        };
        const ssl_request: [8]u8 = [
            0x00, 0x00, 0x00, 0x08,
            0x04, 0xD2, 0x16, 0x2F,
        ];

        const written = socket.write(ssl_request[offset..]);
        if (written > 0) {
            this.tls_status = {
                message_sent: offset + @as(u8, @intCast(written)),
            };
        } else {
            this.tls_status = {
                message_sent: offset,
            };
        }
    }

    pub fn onOpen(*this, socket: uws.AnySocket) void {
        this.socket = socket;

        this.poll_ref.ref(this.vm);
        this.updateHasPendingActivity();

        if (this.tls_status == .message_sent || this.tls_status == .pending) {
            this.startTLS(socket);
            return;
        }

        this.start();
    }

    pub fn onHandshake(*this, success: i32, ssl_error: uws.us_bun_verify_error_t) void {
        debug("onHandshake: {d} {d}", .{ success, ssl_error.error_no });
        const handshake_success = if (success == 1) true else false;
        if (handshake_success) {
            if (this.tls_config.reject_unauthorized != 0) {
                switch (this.ssl_mode) {
                    .verify_ca, .verify_full => {
                        if (ssl_error.error_no != 0) {
                            this.failWithJSValue(ssl_error.toJS(this.globalObject) catch return);
                            return;
                        }

                        const ssl_ptr: *BoringSSL.c.SSL = unsafe { @ptrCast(this.socket.getNativeHandle()) };
                        if (servername := BoringSSL.c.SSL_get_servername(ssl_ptr, 0)) {
                            const hostname = unsafe { servername[0..bun.len(servername)] };
                            if (!BoringSSL.checkServerIdentity(ssl_ptr, hostname)) {
                                this.failWithJSValue(ssl_error.toJS(this.globalObject) catch return);
                            }
                        }
                    },
                    .require, .prefer, .disable => {},
                }
            }
        } else {
            this.failWithJSValue(ssl_error.toJS(this.globalObject) catch return);
        }
    }

    pub fn onTimeout(*this) void {
        _ = this;
        debug("onTimeout", .{});
    }

    pub fn onDrain(*this) void {
        debug("onDrain", .{});
        this.flags.has_backpressure = false;
        if (this.tls_status == .message_sent) {
            if (this.tls_status.message_sent < 8) {
                this.startTLS(this.socket);
            }

            return;
        }

        this.drainInternal();
    }

    fn drainInternal(*this) void {
        debug("drainInternal", .{});
        if (this.vm.isShuttingDown()) return this.close();

        const event_loop = this.vm.eventLoop();
        event_loop.enter();
        defer event_loop.exit();

        this.flushData();

        if (!this.flags.has_backpressure && this.flags.is_ready_for_query) {
            this.advance();
            this.flushData();
        }
    }

    pub fn onData(*this, data: []const u8) void {
        this.ref();
        this.flags.is_processing_data = true;
        const vm = this.vm;

        this.disableConnectionTimeout();
        defer {
            if (this.status == .connected && !this.hasQueryRunning() && this.write_buffer.remaining().len == 0) {
                this.poll_ref.unref(vm);
            } else if (this.status == .connected) {
                this.poll_ref.ref(vm);
            }
            this.flags.is_processing_data = false;

            this.resetConnectionTimeout();
            this.deref();
        }

        const event_loop = vm.eventLoop();
        event_loop.enter();
        defer event_loop.exit();
        SocketMonitor.read(data);
        this.read_buffer.head = this.last_message_start;

        if (this.read_buffer.remaining().len == 0) {
            let consumed: usize = 0;
            let offset: usize = 0;
            const reader = protocol.StackReader.init(data, &consumed, &offset);
            PostgresRequest.onData(this, protocol.StackReader, reader) catch |e| {
                if (e == error.ShortRead) {
                    if (comptime bun.Environment.allow_assert) {
                        debug("read_buffer: empty and received short read: last_message_start: {d}, head: {d}, len: {d}", .{
                            offset,
                            consumed,
                            data.len,
                        });
                    }

                    this.read_buffer.head = 0;
                    this.last_message_start = 0;
                    this.read_buffer.byte_list.len = 0;
                    this.read_buffer.write(bun.default_allocator, data[offset..]) catch @panic("failed to write to read buffer");
                } else {
                    bun.handleErrorReturnTrace(e, @errorReturnTrace());

                    this.fail("Failed to read data", e);
                }
            };
            return;
        }
        this.read_buffer.write(bun.default_allocator, data) catch @panic("failed to write to read buffer");
        PostgresRequest.onData(this, Reader, this.bufferedReader()) catch |e| {
            if (e != error.ShortRead) {
                bun.handleErrorReturnTrace(e, @errorReturnTrace());
                this.fail("Failed to read data", e);
                return;
            }

            if (comptime bun.Environment.allow_assert) {
                debug("read_buffer: not empty and received short read: last_message_start: {d}, head: {d}, len: {d}", .{
                    this.last_message_start,
                    this.read_buffer.head,
                    this.read_buffer.byte_list.len,
                });
            }
            return;
        };

        debug("clean read_buffer", .{});
        this.last_message_start = 0;
        this.read_buffer.head = 0;
    }

    pub static fn constructor(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame) bun.JSError!*PostgresSQLConnection {
        _ = callframe;
        return globalObject.throw("PostgresSQLConnection cannot be constructed directly", .{});
    }

    pub static fn call(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame) bun.JSError!jsc.JSValue {
        let vm = globalObject.bunVM();
        const arguments = callframe.arguments();
        const hostname_str = arguments[0].toBunString(globalObject)?;
        defer hostname_str.deref();
        const port = arguments[1].coerce(i32, globalObject)?;

        const username_str = arguments[2].toBunString(globalObject)?;
        defer username_str.deref();
        const password_str = arguments[3].toBunString(globalObject)?;
        defer password_str.deref();
        const database_str = arguments[4].toBunString(globalObject)?;
        defer database_str.deref();
        const ssl_mode: SSLMode = switch (arguments[5].toInt32()) {
            0 => .disable,
            1 => .prefer,
            2 => .require,
            3 => .verify_ca,
            4 => .verify_full,
            else => .disable,
        };

        const tls_object = arguments[6];

        let tls_config: jsc.API.ServerConfig.SSLConfig = .{};
        let secure: (*uws.SslCtx)? = null;
        if (ssl_mode != .disable) {
            if (tls_object.isBoolean() && tls_object.toBoolean()) {
                tls_config = .{};
            } else if (tls_object.isObject()) {
                tls_config = (jsc.API.ServerConfig.SSLConfig.fromJS(vm, globalObject, tls_object) catch return .zero) orelse .{};
            } else {
                return globalObject.throwInvalidArguments("tls must be a boolean or an object", .{});
            }

            if (globalObject.hasException()) {
                tls_config.deinit();
                return .zero;
            }

            let sock_err: uws.create_bun_socket_error_t = .none;
            secure = vm.rareData().sslCtxCache().getOrCreateOpts(tls_config.asUSocketsForClientVerification(), &sock_err) orelse {
                tls_config.deinit();
                return globalObject.throwValue(sock_err.toJS(globalObject));
            };
        }
        errdefer {
            if (s := secure) bun.BoringSSL.c.SSL_CTX_free(s);
            tls_config.deinit();
        }

        let username: []const u8 = "";
        let password: []const u8 = "";
        let database: []const u8 = "";
        let options: []const u8 = "";
        let path: []const u8 = "";

        const options_str = arguments[7].toBunString(globalObject)?;
        defer options_str.deref();

        const path_str = arguments[8].toBunString(globalObject)?;
        defer path_str.deref();

        let b = bun.StringBuilder{};
        b.cap += username_str.utf8ByteLength() + 1 + password_str.utf8ByteLength() + 1 + database_str.utf8ByteLength() + 1 + options_str.utf8ByteLength() + 1 + path_str.utf8ByteLength() + 1;

        b.allocate(bun.default_allocator) catch {};
        {
            let u = username_str.toUTF8WithoutRef(bun.default_allocator);
            defer u.deinit();
            username = b.append(u.slice());

            let p = password_str.toUTF8WithoutRef(bun.default_allocator);
            defer p.deinit();
            password = b.append(p.slice());

            let d = database_str.toUTF8WithoutRef(bun.default_allocator);
            defer d.deinit();
            database = b.append(d.slice());

            let o = options_str.toUTF8WithoutRef(bun.default_allocator);
            defer o.deinit();
            options = b.append(o.slice());

            let _path = path_str.toUTF8WithoutRef(bun.default_allocator);
            defer _path.deinit();
            path = b.append(_path.slice());
        }
        const options_buf: []u8 = b.allocatedSlice();

        if (username.len > 0 && std.mem.indexOfScalar(u8, username, 0) != null) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("username must not contain null bytes", .{});
        }
        if (password.len > 0 && std.mem.indexOfScalar(u8, password, 0) != null) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("password must not contain null bytes", .{});
        }
        if (database.len > 0 && std.mem.indexOfScalar(u8, database, 0) != null) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("database must not contain null bytes", .{});
        }
        if (path.len > 0 && std.mem.indexOfScalar(u8, path, 0) != null) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("path must not contain null bytes", .{});
        }

        const on_connect = arguments[9];
        const on_close = arguments[10];
        const idle_timeout = arguments[11].toInt32();
        const connection_timeout = arguments[12].toInt32();
        const max_lifetime = arguments[13].toInt32();
        const use_unnamed_prepared_statements = arguments[14].asBoolean();

        let ptr: *PostgresSQLConnection = bun.default_allocator.create(PostgresSQLConnection)?;

        unsafe {
            ptr.* = PostgresSQLConnection{
                globalObject: globalObject,
                vm: vm,
                database: database,
                user: username,
                password: password,
                path: path,
                options: options,
                options_buf: options_buf,
                socket: Socket{ SocketTCP: { socket: { detached: {} } } },
                requests: PostgresRequest.Queue.init(bun.default_allocator),
                statements: PreparedStatementsMap{},
                tls_config: tls_config,
                secure: secure,
                ssl_mode: ssl_mode,
                tls_status: if (ssl_mode != .disable) .pending else .none,
                idle_timeout_interval_ms: @intCast(idle_timeout),
                connection_timeout_ms: @intCast(connection_timeout),
                max_lifetime_interval_ms: @intCast(max_lifetime),
                flags: {
                    use_unnamed_prepared_statements: use_unnamed_prepared_statements,
                },
            };
        }
        secure = null;
        tls_config = .{};

        {
            const hostname = hostname_str.toUTF8(bun.default_allocator);
            defer hostname.deinit();

            const group = vm.rareData().postgresGroup(vm, false);
            const result = if (path.len > 0)
                uws.SocketTCP.connectUnixGroup(group, .postgres, null, path, ptr, false)
            else
                uws.SocketTCP.connectGroup(group, .postgres, null, hostname.slice(), port, ptr, false);

            ptr.socket = { SocketTCP: result catch |e| {
                ptr.deinit();
                return globalObject.throwError(e, "failed to connect to postgresql");
            } };
        }

        ptr.updateHasPendingActivity();
        ptr.resetConnectionTimeout();
        ptr.poll_ref.ref(vm);
        const js_value = ptr.toJS(globalObject);
        js_value.ensureStillAlive();
        ptr.js_value = js_value;
        js.onconnectSetCached(js_value, globalObject, on_connect);
        js.oncloseSetCached(js_value, globalObject, on_close);
        bun.analytics.Features.postgres_connections += 1;
        return js_value;
    }

    pub static fn SocketHandler(comptime ssl: bool): type {
        return if (ssl) SocketHandlerSSL else SocketHandlerTCP;
    }

    pub fn doRef(*this, _: *jsc.JSGlobalObject, _: *jsc.CallFrame) bun.JSError!JSValue {
        this.poll_ref.ref(this.vm);
        this.updateHasPendingActivity();
        return .js_undefined;
    }

    pub fn doUnref(*this, _: *jsc.JSGlobalObject, _: *jsc.CallFrame) bun.JSError!JSValue {
        this.poll_ref.unref(this.vm);
        this.updateHasPendingActivity();
        return .js_undefined;
    }

    pub fn doFlush(*this, _: *jsc.JSGlobalObject, _: *jsc.CallFrame) bun.JSError!jsc.JSValue {
        this.registerAutoFlusher();
        return .js_undefined;
    }

    fn close(*this) void {
        this.disconnect();
        this.unregisterAutoFlusher();
        this.write_buffer.clearAndFree(bun.default_allocator);
    }

    pub fn doClose(*this, globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame) bun.JSError!JSValue {
        _ = globalObject;
        this.close();
        return .js_undefined;
    }

    pub fn stopTimers(*this) void {
        if (this.timer.state == .ACTIVE) {
            this.vm.timer.remove(&this.timer);
        }
        if (this.max_lifetime_timer.state == .ACTIVE) {
            this.vm.timer.remove(&this.max_lifetime_timer);
        }
    }

    pub fn deinit(*this) void {
        this.disconnect();
        this.stopTimers();
        let iter = this.statements.valueIterator();
        while (stmt_ptr := iter.next()) {
            let stmt = unsafe { stmt_ptr.* };
            stmt.deref();
        }
        this.statements.deinit(bun.default_allocator);
        this.requests.deinit();
        this.write_buffer.deinit(bun.default_allocator);
        this.read_buffer.deinit(bun.default_allocator);
        this.backend_parameters.deinit();

        bun.freeSensitive(bun.default_allocator, this.options_buf);

        this.tls_config.deinit();
        if (s := this.secure) bun.BoringSSL.c.SSL_CTX_free(s);
        bun.default_allocator.destroy(this);
    }

    fn cleanUpRequests(*this, js_reason: ?jsc.JSValue) void {
        while (request := this.current()) {
            switch (request.status) {
                .pending => {
                    let stmt = request.statement orelse continue;
                    stmt.error_response = { postgres_error: AnyPostgresError.ConnectionClosed };
                    stmt.status = .failed;
                    if (!this.vm.isShuttingDown()) {
                        if (reason := js_reason) {
                            request.onJSError(reason, this.globalObject);
                        } else {
                            request.onError({ postgres_error: AnyPostgresError.ConnectionClosed }, this.globalObject);
                        }
                    }
                },
                .binding, .running, .partial_response => {
                    this.finishRequest(request);
                    if (!this.vm.isShuttingDown()) {
                        if (reason := js_reason) {
                            request.onJSError(reason, this.globalObject);
                        } else {
                            request.onError({ postgres_error: AnyPostgresError.ConnectionClosed }, this.globalObject);
                        }
                    }
                },
                .success, .fail => {},
            }
            request.deref();
            this.requests.discard(1);
        }
    }

    fn refAndClose(*this, js_reason: ?jsc.JSValue) void {
        if (!this.socket.isClosed()) {
            this.poll_ref.ref(this.vm);
            this.socket.close();
        }

        this.cleanUpRequests(js_reason);
    }

    pub fn disconnect(*this) void {
        this.stopTimers();
        this.unregisterAutoFlusher();
        if (this.status == .connected) {
            this.status = .disconnected;
            this.refAndClose(null);
        }
    }

    fn current(*this) ?*PostgresSQLQuery {
        if (this.requests.readableLength() == 0) {
            return null;
        }

        return this.requests.peekItem(0);
    }

    pub fn hasQueryRunning(*this) bool {
        return !this.flags.is_ready_for_query || this.current() != null;
    }

    pub fn canPipeline(*this) bool {
        if (bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_SQL_AUTO_PIPELINING.get()) {
            @branchHint(.unlikely);
            return false;
        }

        return this.nonpipelinable_requests == 0 &&
            !this.flags.use_unnamed_prepared_statements &&
            !this.flags.waiting_to_prepare &&
            !this.flags.has_backpressure &&
            this.write_buffer.len() < MAX_PIPELINE_SIZE;
    }

    pub fn writer(*this) protocol.NewWriter(Writer) {
        return {
            wrapped: {
                connection: this,
            },
        };
    }

    pub fn bufferedReader(*this) protocol.NewReader(Reader) {
        return {
            wrapped: { connection: this },
        };
    }

    fn finishRequest(*this, item: *PostgresSQLQuery) void {
        switch (item.status) {
            .running, .binding, .partial_response => {
                if (item.flags.simple) {
                    this.nonpipelinable_requests -= 1;
                } else if (item.flags.pipelined) {
                    this.pipelined_requests -= 1;
                }
            },
            .success, .fail, .pending => {},
        }
    }

    pub fn canPrepareQuery(this: *const PostgresSQLConnection) bool {
        return this.flags.is_ready_for_query && !this.flags.waiting_to_prepare && this.pipelined_requests == 0;
    }

    pub fn advanceAndFlush(*this) void {
        if (!this.flags.has_backpressure && this.flags.is_ready_for_query) {
            this.advance();
            this.flushData();
        }
    }

    fn advance(*this) void {
        let offset: usize = 0;
        debug("advance", .{});
        defer {
            while (this.requests.readableLength() > 0) {
                const result = this.requests.peekItem(0);
                switch (result.status) {
                    .success => {
                        result.deref();
                        this.requests.discard(1);
                        continue;
                    },
                    .fail => {
                        result.deref();
                        this.requests.discard(1);
                        continue;
                    },
                    else => break,
                }
            }
        }
        while (this.requests.readableLength() > offset && !this.flags.has_backpressure) {
            if (this.vm.isShuttingDown()) return this.close();

            let req: *PostgresSQLQuery = this.requests.peekItem(offset);
            switch (req.status) {
                .pending => {
                    if (req.flags.simple) {
                        if (this.pipelined_requests > 0 || !this.flags.is_ready_for_query) {
                            debug("cannot execute simple query, pipelined_requests: {d}, is_ready_for_query: {}", .{ this.pipelined_requests, this.flags.is_ready_for_query });
                            return;
                        }
                        let query_str = req.query.toUTF8(bun.default_allocator);
                        defer query_str.deinit();
                        debug("execute simple query: {s}", .{query_str.slice()});
                        PostgresRequest.executeQuery(query_str.slice(), Writer, this.writer()) catch |e| {
                            if (err_ := this.globalObject.tryTakeException()) {
                                req.onJSError(err_, this.globalObject);
                            } else {
                                req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                            }
                            if (offset == 0) {
                                req.deref();
                                this.requests.discard(1);
                            } else {
                                req.status = .fail;
                            }
                            debug("executeQuery failed: {s}", .{@errorName(e)});
                            continue;
                        };
                        this.nonpipelinable_requests += 1;
                        this.flags.is_ready_for_query = false;
                        req.status = .running;
                        return;
                    } else {
                        if (statement := req.statement) {
                            switch (statement.status) {
                                .failed => {
                                    debug("stmt failed", .{});
                                    bun.assert(statement.error_response != null);
                                    req.onError(statement.error_response.?, this.globalObject);
                                    if (offset == 0) {
                                        req.deref();
                                        this.requests.discard(1);
                                    } else {
                                        req.status = .fail;
                                        offset += 1;
                                    }

                                    continue;
                                },
                                .prepared => {
                                    const thisValue = req.thisValue.tryGet() orelse {
                                        bun.assertf(false, "query value was freed earlier than expected", .{});
                                        if (offset == 0) {
                                            req.deref();
                                            this.requests.discard(1);
                                        } else {
                                            req.status = .fail;
                                            offset += 1;
                                        }
                                        continue;
                                    };
                                    const binding_value = PostgresSQLQuery.js.bindingGetCached(thisValue) orelse .zero;
                                    const columns_value = PostgresSQLQuery.js.columnsGetCached(thisValue) orelse .zero;
                                    req.flags.binary = statement.fields.len > 0;

                                    if (this.flags.use_unnamed_prepared_statements) {
                                        debug("parse, bind and execute unnamed stmt", .{});
                                        let query_str = req.query.toUTF8(bun.default_allocator);
                                        defer query_str.deinit();
                                        PostgresRequest.parseAndBindAndExecute(this.globalObject, query_str.slice(), statement, binding_value, columns_value, false, Writer, this.writer()) catch |e| {
                                            if (err_ := this.globalObject.tryTakeException()) {
                                                req.onJSError(err_, this.globalObject);
                                            } else {
                                                req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                                            }
                                            if (offset == 0) {
                                                req.deref();
                                                this.requests.discard(1);
                                            } else {
                                                req.status = .fail;
                                                offset += 1;
                                            }
                                            debug("parse, bind and execute failed: {s}", .{@errorName(e)});
                                            continue;
                                        };
                                    } else {
                                        debug("binding and executing stmt", .{});
                                        PostgresRequest.bindAndExecute(this.globalObject, statement, binding_value, columns_value, Writer, this.writer()) catch |e| {
                                            if (err_ := this.globalObject.tryTakeException()) {
                                                req.onJSError(err_, this.globalObject);
                                            } else {
                                                req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                                            }
                                            if (offset == 0) {
                                                req.deref();
                                                this.requests.discard(1);
                                            } else {
                                                req.status = .fail;
                                                offset += 1;
                                            }
                                            debug("bind and execute failed: {s}", .{@errorName(e)});
                                            continue;
                                        };
                                    }

                                    this.flags.is_ready_for_query = false;
                                    req.status = .binding;
                                    req.flags.pipelined = true;
                                    this.pipelined_requests += 1;

                                    if (this.flags.use_unnamed_prepared_statements || !this.canPipeline()) {
                                        debug("cannot pipeline more stmt", .{});
                                        return;
                                    }

                                    offset += 1;
                                    continue;
                                },
                                .pending => {
                                    if (!this.canPrepareQuery()) {
                                        debug("need to wait to finish the pipeline before starting a new query preparation", .{});
                                        return;
                                    }
                                    let query_str = req.query.toUTF8(bun.default_allocator);
                                    defer query_str.deinit();
                                    const has_params = statement.signature.fields.len > 0;
                                    if (!has_params) {
                                        const thisValue = req.thisValue.tryGet() orelse {
                                            bun.assertf(false, "query value was freed earlier than expected", .{});
                                            if (offset == 0) {
                                                req.deref();
                                                this.requests.discard(1);
                                            } else {
                                                req.status = .fail;
                                                offset += 1;
                                            }
                                            continue;
                                        };
                                        const binding_value = PostgresSQLQuery.js.bindingGetCached(thisValue) orelse .zero;
                                        debug("prepareAndQueryWithSignature", .{});
                                        PostgresRequest.prepareAndQueryWithSignature(this.globalObject, query_str.slice(), binding_value, Writer, this.writer(), &statement.signature) catch |e| {
                                            if (err_ := this.globalObject.tryTakeException()) {
                                                req.onJSError(err_, this.globalObject);
                                            } else {
                                                statement.status = .failed;
                                                statement.error_response = { postgres_error: e };
                                                req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                                            }
                                            if (offset == 0) {
                                                req.deref();
                                                this.requests.discard(1);
                                            } else {
                                                req.status = .fail;
                                            }
                                            debug("prepareAndQueryWithSignature failed: {s}", .{@errorName(e)});

                                            continue;
                                        };
                                        this.flags.is_ready_for_query = false;
                                        this.flags.waiting_to_prepare = true;
                                        req.status = .binding;
                                        statement.status = .parsing;
                                        this.flushDataAndResetTimeout();
                                        return;
                                    }

                                    if (this.flags.use_unnamed_prepared_statements) {
                                        const thisValue = req.thisValue.tryGet() orelse {
                                            bun.assertf(false, "query value was freed earlier than expected", .{});
                                            bun.assert(offset == 0);
                                            req.deref();
                                            this.requests.discard(1);
                                            continue;
                                        };
                                        const binding_value = PostgresSQLQuery.js.bindingGetCached(thisValue) orelse .zero;
                                        const columns_value = PostgresSQLQuery.js.columnsGetCached(thisValue) orelse .zero;
                                        debug("parseAndBindAndExecute (unnamed, first execution)", .{});
                                        PostgresRequest.parseAndBindAndExecute(this.globalObject, query_str.slice(), statement, binding_value, columns_value, true, Writer, this.writer()) catch |e| {
                                            if (err_ := this.globalObject.tryTakeException()) {
                                                req.onJSError(err_, this.globalObject);
                                            } else {
                                                statement.status = .failed;
                                                statement.error_response = { postgres_error: e };
                                                req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                                            }
                                            bun.assert(offset == 0);
                                            req.deref();
                                            this.requests.discard(1);
                                            debug("parseAndBindAndExecute failed: {s}", .{@errorName(e)});
                                            continue;
                                        };
                                        this.flags.is_ready_for_query = false;
                                        this.flags.waiting_to_prepare = true;
                                        req.status = .binding;
                                        statement.status = .parsing;
                                        req.flags.pipelined = true;
                                        this.pipelined_requests += 1;
                                        this.flushDataAndResetTimeout();
                                        return;
                                    }

                                    const connection_writer = this.writer();
                                    debug("writing query", .{});
                                    PostgresRequest.writeQuery(query_str.slice(), statement.signature.prepared_statement_name, statement.signature.fields, Writer, connection_writer) catch |e| {
                                        if (err_ := this.globalObject.tryTakeException()) {
                                            req.onJSError(err_, this.globalObject);
                                        } else {
                                            statement.error_response = { postgres_error: e };
                                            statement.status = .failed;
                                            req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                                        }
                                        bun.assert(offset == 0);
                                        req.deref();
                                        this.requests.discard(1);
                                        debug("write query failed: {s}", .{@errorName(e)});
                                        continue;
                                    };
                                    connection_writer.write(&protocol.Sync) catch |e| {
                                        if (err_ := this.globalObject.tryTakeException()) {
                                            req.onJSError(err_, this.globalObject);
                                        } else {
                                            statement.error_response = { postgres_error: e };
                                            statement.status = .failed;
                                            req.onWriteFail(e, this.globalObject, this.getQueriesArray());
                                        }
                                        bun.assert(offset == 0);
                                        req.deref();
                                        this.requests.discard(1);
                                        debug("write query (sync) failed: {s}", .{@errorName(e)});
                                        continue;
                                    };
                                    this.flags.is_ready_for_query = false;
                                    this.flags.waiting_to_prepare = true;
                                    statement.status = .parsing;
                                    this.flushDataAndResetTimeout();
                                    return;
                                },
                                .parsing => {
                                    offset += 1;
                                    continue;
                                },
                            }
                        } else {
                            offset += 1;
                            continue;
                        }
                    }
                },

                .running, .binding, .partial_response => {
                    if (this.flags.waiting_to_prepare || this.nonpipelinable_requests > 0) {
                        return;
                    }
                    const total_requests_running = this.pipelined_requests;
                    if (offset < total_requests_running) {
                        offset += total_requests_running;
                    } else {
                        offset += 1;
                    }
                    continue;
                },
                .success => {
                    if (offset > 0) {
                        req.status = .fail;
                        offset += 1;
                        continue;
                    }
                    req.deref();
                    this.requests.discard(1);
                    continue;
                },
                .fail => {
                    if (offset > 0) {
                        offset += 1;
                        continue;
                    }
                    req.deref();
                    this.requests.discard(1);
                    continue;
                },
            }
        }
    }

    pub fn getQueriesArray(this: *const PostgresSQLConnection) JSValue {
        if (this.js_value.isEmptyOrUndefinedOrNull()) {
            return .js_undefined;
        }
        return js.queriesGetCached(this.js_value) orelse .js_undefined;
    }

    pub fn on(*this, comptime MessageType: anytype, comptime Context: type, reader: protocol.NewReader(Context)) AnyPostgresError!void {
        debug("on({s})", .{@tagName(MessageType)});

        switch (comptime MessageType) {
            .DataRow => {
                const request = this.current() orelse return error.ExpectedRequest;

                let statement = request.statement orelse return error.ExpectedStatement;
                let structure: JSValue = .js_undefined;
                let cached_structure: ?PostgresCachedStructure = null;
                switch (request.flags.result_mode) {
                    .objects => {
                        cached_structure = statement.structure(this.js_value, this.globalObject);
                        structure = cached_structure.?.jsValue() orelse .js_undefined;
                    },
                    .raw, .values => {},
                }

                const empty_cells: [0]DataCell.SQLDataCell = [];
                let putter = DataCell.Putter{
                    list: empty_cells[0..],
                    fields: statement.fields,
                    binary: request.flags.binary,
                    bigint: request.flags.bigint,
                    globalObject: this.globalObject,
                };

                let stack_buf: [70]DataCell.SQLDataCell = undefined;
                let cells: []DataCell.SQLDataCell = stack_buf[0..@min(statement.fields.len, jsc.JSObject.maxInlineCapacity())];
                let free_cells = false;
                defer {
                    for (0..putter.count) |cell_idx| {
                        cells[cell_idx].deinit();
                    }
                    if (free_cells) bun.default_allocator.free(cells);
                }

                if (statement.fields.len >= jsc.JSObject.maxInlineCapacity()) {
                    cells = bun.default_allocator.alloc(DataCell.SQLDataCell, statement.fields.len)?;
                    free_cells = true;
                }
                @memset(cells, DataCell.SQLDataCell{ tag: .@"null", value: { @"null": 0 } });
                putter.list = cells;

                if (request.flags.result_mode == .raw) {
                    protocol.DataRow.decode(
                        &putter,
                        Context,
                        reader,
                        DataCell.Putter.putRaw,
                    )?;
                } else {
                    protocol.DataRow.decode(
                        &putter,
                        Context,
                        reader,
                        DataCell.Putter.put,
                    )?;
                }
                const thisValue = request.thisValue.tryGet() orelse {
                    bun.assertf(false, "query value was freed earlier than expected", .{});
                    return error.ExpectedRequest;
                };
                const pending_value = PostgresSQLQuery.js.pendingValueGetCached(thisValue) orelse .zero;
                pending_value.ensureStillAlive();
                const result = putter.toJS(
                    this.globalObject,
                    pending_value,
                    structure,
                    statement.fields_flags,
                    request.flags.result_mode,
                    cached_structure,
                )?;

                if (pending_value == .zero) {
                    PostgresSQLQuery.js.pendingValueSetCached(thisValue, this.globalObject, result);
                }
            },
            .CopyData => {
                let copy_data: protocol.CopyData = undefined;
                copy_data.decodeInternal(Context, reader)?;
                copy_data.data.deinit();
            },
            .ParameterStatus => {
                let parameter_status: protocol.ParameterStatus = undefined;
                parameter_status.decodeInternal(Context, reader)?;
                defer {
                    parameter_status.deinit();
                }
                this.backend_parameters.insert(parameter_status.name.slice(), parameter_status.value.slice())?;
            },
            .ReadyForQuery => {
                let ready_for_query: protocol.ReadyForQuery = undefined;
                ready_for_query.decodeInternal(Context, reader)?;

                this.setStatus(.connected);
                this.flags.waiting_to_prepare = false;
                this.flags.is_ready_for_query = true;
                this.socket.setTimeout(300);
                defer this.updateRef();

                if (request := this.current()) {
                    if (request.status == .partial_response) {
                        this.finishRequest(request);
                        request.onResult("", this.globalObject, this.js_value, true);
                    }
                }
                this.advance();

                this.registerAutoFlusher();
            },
            .CommandComplete => {
                let request = this.current() orelse return error.ExpectedRequest;

                let cmd: protocol.CommandComplete = undefined;
                cmd.decodeInternal(Context, reader)?;
                defer {
                    cmd.deinit();
                }
                debug("-> {s}", .{cmd.command_tag.slice()});
                defer this.updateRef();

                request.onResult(cmd.command_tag.slice(), this.globalObject, this.js_value, false);
            },
            .BindComplete => {
                reader.eatMessage(protocol.BindComplete)?;
                let request = this.current() orelse return error.ExpectedRequest;
                if (request.status == .binding) {
                    request.status = .running;
                }
            },
            .ParseComplete => {
                reader.eatMessage(protocol.ParseComplete)?;
                const request = this.current() orelse return error.ExpectedRequest;
                if (statement := request.statement) {
                    if (statement.status == .parsing && statement.signature.fields.len == 0) {
                        statement.status = .prepared;
                        this.flags.waiting_to_prepare = false;
                    }
                }
            },
            .ParameterDescription => {
                let description: protocol.ParameterDescription = undefined;
                description.decodeInternal(Context, reader)?;
                errdefer bun.default_allocator.free(description.parameters);
                const request = this.current() orelse return error.ExpectedRequest;
                let statement = request.statement orelse return error.ExpectedStatement;
                if (statement.parameters.len > 0) {
                    bun.default_allocator.free(statement.parameters);
                }
                statement.parameters = description.parameters;
                if (statement.status == .parsing) {
                    statement.status = .prepared;
                    this.flags.waiting_to_prepare = false;
                }
            },
            .RowDescription => {
                let description: protocol.RowDescription = undefined;
                description.decodeInternal(Context, reader)?;
                errdefer description.deinit();
                const request = this.current() orelse return error.ExpectedRequest;
                let statement = request.statement orelse return error.ExpectedStatement;
                if (statement.fields.len > 0) {
                    for (0..statement.fields.len) |field_idx| {
                        statement.fields[field_idx].deinit();
                    }
                    bun.default_allocator.free(statement.fields);
                    statement.cached_structure.deinit();
                    statement.cached_structure = .{};
                    statement.needs_duplicate_check = true;
                    statement.fields_flags = .{};
                }
                statement.fields = description.fields;
            },
            .Authentication => {
                let auth: protocol.Authentication = undefined;
                auth.decodeInternal(Context, reader)?;
                defer auth.deinit();

                switch (auth) {
                    .SASL => {
                        if (this.authentication_state != .SASL) {
                            this.authentication_state = { SASL: .{} };
                        }

                        let mechanism_buf: [128]u8 = undefined;
                        const mechanism = std.fmt.bufPrintZ(&mechanism_buf, "n,,n=*,r={s}", .{this.authentication_state.SASL.nonce()}) catch unreachable;
                        let response = protocol.SASLInitialResponse{
                            mechanism: {
                                temporary: "SCRAM-SHA-256",
                            },
                            data: {
                                temporary: mechanism,
                            },
                        };

                        response.writeInternal(Writer, this.writer())?;
                        debug("SASL", .{});
                        this.flushData();
                    },
                    .SASLContinue => {
                        let cont = &auth.SASLContinue;
                        if (this.authentication_state != .SASL) {
                            debug("Unexpected SASLContinue for authentiation state: {s}", .{@tagName(std.meta.activeTag(this.authentication_state))});
                            return error.UnexpectedMessage;
                        }
                        if (this.authentication_state.SASL.status != .init) {
                            debug("Unexpected SASLContinue for SASL state: {s}", .{@tagName(this.authentication_state.SASL.status)});
                            return error.UnexpectedMessage;
                        }
                        debug("SASLContinue", .{});

                        const iteration_count = cont.iterationCount()?;

                        const server_salt_decoded_base64 = bun.base64.decodeAlloc(bun.z_allocator, cont.s) catch |e| {
                            return switch (e) {
                                error.DecodingFailed => error.SASL_SIGNATURE_INVALID_BASE64,
                                else => e,
                            };
                        };
                        defer bun.z_allocator.free(server_salt_decoded_base64);
                        this.authentication_state.SASL.computeSaltedPassword(server_salt_decoded_base64, iteration_count, this)?;

                        const auth_string = std.fmt.allocPrint(
                            bun.z_allocator,
                            "n=*,r={s},r={s},s={s},i={s},c=biws,r={s}",
                            .{
                                this.authentication_state.SASL.nonce(),
                                cont.r,
                                cont.s,
                                cont.i,
                                cont.r,
                            },
                        )?;
                        defer bun.z_allocator.free(auth_string);
                        this.authentication_state.SASL.computeServerSignature(auth_string)?;

                        const client_key = this.authentication_state.SASL.clientKey();
                        const client_key_signature = this.authentication_state.SASL.clientKeySignature(&client_key, auth_string);
                        let client_key_xor_buffer: [32]u8 = undefined;
                        for (0..32) |idx| {
                            client_key_xor_buffer[idx] = client_key[idx] ^ client_key_signature[idx];
                        }

                        let client_key_xor_base64_buf = std.mem.zeroes(Base64XorBuf);
                        const xor_base64_len = bun.base64.encode(&client_key_xor_base64_buf, &client_key_xor_buffer);

                        const payload = std.fmt.allocPrint(
                            bun.z_allocator,
                            "c=biws,r={s},p={s}",
                            .{ cont.r, client_key_xor_base64_buf[0..xor_base64_len] },
                        )?;
                        defer bun.z_allocator.free(payload);

                        let response = protocol.SASLResponse{
                            data: {
                                temporary: payload,
                            },
                        };

                        response.writeInternal(Writer, this.writer())?;
                        this.authentication_state.SASL.status = .@"continue";
                        this.flushData();
                    },
                    .SASLFinal => |final| {
                        if (this.authentication_state != .SASL) {
                            debug("SASLFinal - Unexpected SASLContinue for authentiation state: {s}", .{@tagName(std.meta.activeTag(this.authentication_state))});
                            return error.UnexpectedMessage;
                        }

                        if (this.authentication_state.SASL.status != .@"continue") {
                            debug("SASLFinal - Unexpected SASLContinue for SASL state: {s}", .{@tagName(this.authentication_state.SASL.status)});
                            return error.UnexpectedMessage;
                        }

                        if (this.authentication_state.SASL.server_signature_len == 0) {
                            debug("SASLFinal - Server signature is empty", .{});
                            return error.UnexpectedMessage;
                        }

                        const server_signature = this.authentication_state.SASL.serverSignature();
                        const comparison_signature = final.data.slice();

                        if (comparison_signature.len < 2 ||
                            server_signature.len != comparison_signature.len - 2 ||
                            BoringSSL.c.CRYPTO_memcmp(server_signature.ptr, comparison_signature[2..].ptr, server_signature.len) != 0)
                        {
                            debug("SASLFinal - SASL Server signature mismatch\nExpected: {s}\nActual: {s}", .{ server_signature, comparison_signature[2..] });
                            this.fail("The server did not return the correct signature", error.SASL_SIGNATURE_MISMATCH);
                        } else {
                            debug("SASLFinal - SASL Server signature match", .{});
                            this.authentication_state.zero();
                        }
                    },
                    .Ok => {
                        debug("Authentication OK", .{});
                        this.authentication_state.zero();
                        this.authentication_state = { ok: {} };
                    },
                    .Unknown => {
                        this.fail("Unknown authentication method", error.UNKNOWN_AUTHENTICATION_METHOD);
                    },
                    .ClearTextPassword => {
                        debug("ClearTextPassword", .{});
                        let response = protocol.PasswordMessage{
                            password: {
                                temporary: this.password,
                            },
                        };

                        response.writeInternal(Writer, this.writer())?;
                        this.flushData();
                    },
                    .MD5Password => |md5| {
                        debug("MD5Password", .{});
                        let first_hash_buf: bun.sha.MD5.Digest = undefined;
                        let first_hash_str: [32]u8 = undefined;
                        let final_hash_buf: bun.sha.MD5.Digest = undefined;
                        let final_hash_str: [32]u8 = undefined;
                        let final_password_buf: [36]u8 = undefined;

                        let first_hasher = bun.sha.MD5.init();
                        first_hasher.update(this.password);
                        first_hasher.update(this.user);
                        first_hasher.final(&first_hash_buf);
                        const first_hash_str_output = std.fmt.bufPrint(&first_hash_str, "{x}", .{&first_hash_buf}) catch unreachable;

                        let final_hasher = bun.sha.MD5.init();
                        final_hasher.update(first_hash_str_output);
                        final_hasher.update(&md5.salt);
                        final_hasher.final(&final_hash_buf);
                        const final_hash_str_output = std.fmt.bufPrint(&final_hash_str, "{x}", .{&final_hash_buf}) catch unreachable;

                        const final_password = std.fmt.bufPrintZ(&final_password_buf, "md5{s}", .{final_hash_str_output}) catch unreachable;

                        let response = protocol.PasswordMessage{
                            password: {
                                temporary: final_password,
                            },
                        };

                        this.authentication_state = { md5: {} };
                        response.writeInternal(Writer, this.writer())?;
                        this.flushData();
                    },
                    else => {
                        debug("TODO auth: {s}", .{@tagName(std.meta.activeTag(auth))});
                        this.fail("TODO: support authentication method: {s}", error.UNSUPPORTED_AUTHENTICATION_METHOD);
                    },
                }
            },
            .NoData => {
                reader.eatMessage(protocol.NoData)?;
                let request = this.current() orelse return error.ExpectedRequest;
                if (request.status == .binding) {
                    request.status = .running;
                }
            },
            .BackendKeyData => {
                this.backend_key_data.decodeInternal(Context, reader)?;
            },
            .ErrorResponse => {
                let err_resp: protocol.ErrorResponse = undefined;
                err_resp.decodeInternal(Context, reader)?;

                if (this.status == .connecting || this.status == .sent_startup_message) {
                    defer {
                        err_resp.deinit();
                    }
                    this.failWithJSValue(err_resp.toJS(this.globalObject));

                    bun.assert(this.requests.count == 0);
                    return;
                }

                let request = this.current() orelse {
                    debug("ErrorResponse: {f}", .{err_resp});
                    return error.ExpectedRequest;
                };
                let is_error_owned = true;
                defer {
                    if (is_error_owned) {
                        err_resp.deinit();
                    }
                }
                if (stmt := request.statement) {
                    if (stmt.status == PostgresSQLStatement.Status.parsing) {
                        stmt.status = PostgresSQLStatement.Status.failed;
                        stmt.error_response = { protocol: err_resp };
                        is_error_owned = false;
                        if (this.statements.remove(bun.hash(stmt.signature.name))) {
                            stmt.deref();
                        }
                    }
                }

                this.finishRequest(request);
                this.updateRef();
                request.onError({ protocol: err_resp }, this.globalObject);
            },
            .PortalSuspended => {
                debug("TODO PortalSuspended", .{});
            },
            .CloseComplete => {
                reader.eatMessage(protocol.CloseComplete)?;
                let request = this.current() orelse return error.ExpectedRequest;
                defer this.updateRef();
                request.onResult("CLOSECOMPLETE", this.globalObject, this.js_value, false);
            },
            .CopyInResponse => {
                debug("TODO CopyInResponse", .{});
            },
            .NoticeResponse => {
                debug("UNSUPPORTED NoticeResponse", .{});
                let resp: protocol.NoticeResponse = undefined;

                resp.decodeInternal(Context, reader)?;
                resp.deinit();
            },
            .EmptyQueryResponse => {
                reader.eatMessage(protocol.EmptyQueryResponse)?;
                let request = this.current() orelse return error.ExpectedRequest;
                defer this.updateRef();
                request.onResult("", this.globalObject, this.js_value, false);
            },
            .CopyOutResponse => {
                debug("TODO CopyOutResponse", .{});
            },
            .CopyDone => {
                debug("TODO CopyDone", .{});
            },
            .CopyBothResponse => {
                debug("TODO CopyBothResponse", .{});
            },
            else => @compileError("Unknown message type"),
        }
    }

    pub fn updateRef(*this) void {
        this.updateHasPendingActivity();
        if (this.pending_activity_count.raw > 0) {
            this.poll_ref.ref(this.vm);
        } else {
            this.poll_ref.unref(this.vm);
        }
    }

    pub fn getConnected(*this, _: *jsc.JSGlobalObject) JSValue {
        return JSValue.jsBoolean(this.status == Status.connected);
    }

    pub fn consumeOnConnectCallback(this: *const PostgresSQLConnection, globalObject: *jsc.JSGlobalObject) ?jsc.JSValue {
        debug("consumeOnConnectCallback", .{});
        const on_connect = js.onconnectGetCached(this.js_value) orelse return null;
        debug("consumeOnConnectCallback exists", .{});

        js.onconnectSetCached(this.js_value, globalObject, .zero);
        return on_connect;
    }

    pub fn consumeOnCloseCallback(this: *const PostgresSQLConnection, globalObject: *jsc.JSGlobalObject) ?jsc.JSValue {
        debug("consumeOnCloseCallback", .{});
        const on_close = js.oncloseGetCached(this.js_value) orelse return null;
        debug("consumeOnCloseCallback exists", .{});
        js.oncloseSetCached(this.js_value, globalObject, .zero);
        return on_close;
    }
}

const jscall = jsc.toJSHostFn(PostgresSQLConnection.call);

comptime {
    @export(&jscall, { name: "PostgresSQLConnection__createInstance" });
}
