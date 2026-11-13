import AnyMySQLError from "../../sql/mysql/protocol/AnyMySQLError.zig";
import CachedStructure from "../../sql_jsc/shared/CachedStructure.zig";
import ErrorPacket from "../../sql/mysql/protocol/ErrorPacket.zig";
import JSMySQLQuery from "./JSMySQLQuery.zig";
import MySQLConnection from "./MySQLConnection.zig";
import MySQLQueryResult from "../../sql/mysql/MySQLQueryResult.zig";
import MySQLStatement from "./MySQLStatement.zig";
import ResultSet from "./protocol/ResultSet.zig";
import std from "std";
import { NewReader } from "../../sql/mysql/protocol/NewReader.zig";
import { NewWriter } from "../../sql/mysql/protocol/NewWriter.zig";
import { SSLMode } from "../../sql/mysql/SSLMode.zig";
import bun from "bun";

const uws = bun.uws;
const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const AutoFlusher = jsc.WebCore.AutoFlusher;
const debug = bun.Output.scoped(.MySQLConnection, .visible);

error RowError {
    ShortRead,
    JSError,
}

fn hasNullByte(s: []const u8): bool {
    return s.len > 0 && std.mem.indexOfScalar(u8, s, 0) != null;
}

@root struct JSMySQLConnection {
    const RefCount = bun.ptr.RefCount(@This(), "__ref_count", deinit, .{});

    __ref_count: RefCount = RefCount.init(),
    #js_value: jsc.JSRef = jsc.JSRef.empty(),
    #globalObject: *jsc.JSGlobalObject,
    #vm: *jsc.VirtualMachine,
    #poll_ref: bun.Async.KeepAlive = .{},

    #connection: MySQLConnection,

    auto_flusher: AutoFlusher = .{},

    idle_timeout_interval_ms: u32 = 0,
    connection_timeout_ms: u32 = 0,
    timer: bun.api.Timer.EventLoopTimer = {
        tag: .MySQLConnectionTimeout,
        next: .epoch,
    },

    max_lifetime_interval_ms: u32 = 0,
    max_lifetime_timer: bun.api.Timer.EventLoopTimer = {
        tag: .MySQLConnectionMaxLifetime,
        next: .epoch,
    },

    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn onAutoFlush(*this): bool {
        debug("onAutoFlush", .{});
        if (this.#connection.hasBackpressure()) {
            this.auto_flusher.registered = false;
            return false;
        }

        this.drainInternal();

        const keep_flusher_registered = this.#connection.canFlush();
        this.auto_flusher.registered = keep_flusher_registered;
        return keep_flusher_registered;
    }

    pub fn registerAutoFlusher(*this) {
        if (!this.auto_flusher.registered && this.#connection.canFlush()) {
            AutoFlusher.registerDeferredMicrotaskWithTypeUnchecked(@This(), this, this.#vm);
            this.auto_flusher.registered = true;
        }
    }

    fn unregisterAutoFlusher(*this) {
        if (this.auto_flusher.registered) {
            AutoFlusher.unregisterDeferredMicrotaskWithType(@This(), this, this.#vm);
            this.auto_flusher.registered = false;
        }
    }

    fn stopTimers(*this) {
        debug("stopTimers", .{});
        if (this.timer.state == .ACTIVE) {
            this.#vm.timer.remove(&this.timer);
        }
        if (this.max_lifetime_timer.state == .ACTIVE) {
            this.#vm.timer.remove(&this.max_lifetime_timer);
        }
    }

    fn getTimeoutInterval(*this): u32 {
        return switch (this.#connection.status) {
            .connected => {
                if (this.#connection.isIdle()) {
                    return this.idle_timeout_interval_ms;
                }
                return 0;
            },
            .failed => 0,
            else => this.connection_timeout_ms,
        };
    }

    pub fn resetConnectionTimeout(*this) {
        const interval = this.getTimeoutInterval();
        debug("resetConnectionTimeout {d}", .{interval});
        if (this.timer.state == .ACTIVE) {
            this.#vm.timer.remove(&this.timer);
        }
        if (this.#connection.status == .failed ||
            this.#connection.isProcessingData() ||
            interval == 0) return;

        this.timer.next = bun.timespec.msFromNow(.allow_mocked_time, @intCast(interval));
        this.#vm.timer.insert(&this.timer);
    }

    pub fn onConnectionTimeout(*this) {
        this.timer.state = .FIRED;

        if (this.#connection.isProcessingData()) {
            return;
        }
        if (this.#connection.status == .failed) return;

        if (this.getTimeoutInterval() == 0) {
            this.resetConnectionTimeout();
            return;
        }

        switch (this.#connection.status) {
            .connected => {
                this.failFmt(err.IdleTimeout, "Idle timeout reached after {f}", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.idle_timeout_interval_ms) * std.time.ns_per_ms)});
            },
            .connecting => {
                this.failFmt(err.ConnectionTimedOut, "Connection timeout after {f}", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.connection_timeout_ms) * std.time.ns_per_ms)});
            },
            .handshaking, .authenticating, .authentication_awaiting_pk => {
                this.failFmt(err.ConnectionTimedOut, "Connection timeout after {f} (during authentication)", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.connection_timeout_ms) * std.time.ns_per_ms)});
            },
            .disconnected, .failed => {},
        }
    }

    pub fn onMaxLifetimeTimeout(*this) {
        this.max_lifetime_timer.state = .FIRED;
        if (this.#connection.status == .failed) return;
        this.failFmt(err.LifetimeTimeout, "Max lifetime timeout reached after {f}", .{bun.fmt.fmtDurationOneDecimal(@as(u64, this.max_lifetime_interval_ms) * std.time.ns_per_ms)});
    }

    pub fn setupMaxLifetimeTimerIfNecessary(*this) {
        if (this.max_lifetime_interval_ms == 0) return;
        if (this.max_lifetime_timer.state == .ACTIVE) return;

        this.max_lifetime_timer.next = bun.timespec.msFromNow(.allow_mocked_time, @intCast(this.max_lifetime_interval_ms));
        this.#vm.timer.insert(&this.max_lifetime_timer);
    }

    pub static fn constructor(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!*@This() {
        _ = callframe;
        return globalObject.throw("MySQLConnection cannot be constructed directly", .{});
    }

    pub fn enqueueRequest(*this, item: *JSMySQLQuery) {
        debug("enqueueRequest", .{});
        this.#connection.enqueueRequest(item);
        this.resetConnectionTimeout();
        this.registerAutoFlusher();
    }

    pub fn close(*this) {
        this.ref();
        this.stopTimers();
        this.unregisterAutoFlusher();
        defer {
            this.updateReferenceType();
            this.deref();
        }
        if (this.#vm.isShuttingDown()) {
            this.#connection.close();
        } else {
            this.#connection.cleanQueueAndClose(null, this.getQueriesArray());
        }
    }

    pub fn drainInternal(*this) {
        debug("drainInternal", .{});
        if (this.#vm.isShuttingDown()) return this.close();
        this.ref();
        defer this.deref();
        const event_loop = this.#vm.eventLoop();
        event_loop.enter();
        defer event_loop.exit();
        this.ensureJSValueIsAlive();
        this.#connection.flushQueue() catch |e| {
            bun.assert_eql(e, err.AuthenticationFailed);
            this.fail("Authentication failed", e);
            return;
        };
    }

    pub fn deinit(*this) {
        this.stopTimers();
        this.#poll_ref.unref(this.#vm);
        this.unregisterAutoFlusher();
        this.#connection.cleanup();
        bun.destroy(this);
    }

    pub fn ensureJSValueIsAlive(*this) {
        if (value := this.#js_value.tryGet()) {
            value.ensureStillAlive();
        }
    }

    pub fn finalize(*this) {
        debug("finalize", .{});
        this.#js_value.finalize();
        this.deref();
    }

    pub static fn SocketHandler(comptime ssl: bool): type {
        return struct {
            pub const SocketType = uws.NewSocketHandler(ssl);

            static fn _socket(s: SocketType): uws.AnySocket {
                if (comptime ssl) {
                    return uws.AnySocket{ SocketTLS: s };
                }
                return uws.AnySocket{ SocketTCP: s };
            }

            pub static fn onOpen(conn: *JSMySQLConnection, s: SocketType) {
                const socket = _socket(s);
                conn.#connection.setSocket(socket);

                if (socket == .SocketTCP) {
                    conn.#connection.status = .handshaking;
                    conn.ref();
                }
                conn.setupMaxLifetimeTimerIfNecessary();
                conn.resetConnectionTimeout();
                conn.updateReferenceType();
            }

            static fn onHandshake_(
                conn: *JSMySQLConnection,
                s: anytype,
                success: i32,
                ssl_error: uws.us_bun_verify_error_t,
            ) {
                _ = s;
                const handshakeWasSuccessful = conn.#connection.doHandshake(success, ssl_error) catch |e| return conn.failFmt(e, "Failed to send handshake response", .{});
                if (!handshakeWasSuccessful) {
                    conn.failWithJSValue(ssl_error.toJS(conn.#globalObject) catch return);
                }
            }

            pub const onHandshake = if (ssl) onHandshake_ else null;

            pub static fn onClose(conn: *JSMySQLConnection, s: SocketType, code: i32, data: *!?) {
                _ = s;
                _ = code;
                _ = data;
                defer conn.deref();
                conn.fail("Connection closed", err.ConnectionClosed);
            }

            pub static fn onEnd(conn: *JSMySQLConnection, socket: SocketType) {
                _ = conn;
                socket.close(.normal);
            }

            pub static fn onConnectError(conn: *JSMySQLConnection, s: SocketType, code: i32) {
                _ = s;
                _ = code;
                conn.fail("Connection closed", err.ConnectionClosed);
            }

            pub static fn onTimeout(conn: *JSMySQLConnection, s: SocketType) {
                _ = s;
                conn.fail("Connection timeout", err.ConnectionTimedOut);
            }

            pub static fn onData(conn: *JSMySQLConnection, s: SocketType, data: []const u8) {
                _ = s;
                conn.ref();
                defer conn.deref();
                const vm = conn.#vm;

                defer {
                    conn.resetConnectionTimeout();
                    conn.updateReferenceType();
                    conn.registerAutoFlusher();
                }
                if (conn.#vm.isShuttingDown()) {
                    return;
                }

                const event_loop = vm.eventLoop();
                event_loop.enter();
                defer event_loop.exit();
                conn.ensureJSValueIsAlive();

                conn.#connection.readAndProcessData(data) catch |e| {
                    conn.onError(null, e);
                };
            }

            pub static fn onWritable(conn: *JSMySQLConnection, s: SocketType) {
                _ = s;
                conn.#connection.resetBackpressure();
                conn.drainInternal();
            }
        };
    }

    pub fn updateReferenceType(*this) {
        if (this.#connection.isActive()) {
            debug("connection is active", .{});
            if (this.#js_value.isNotEmpty() && this.#js_value == .weak) {
                debug("strong ref until connection is closed", .{});
                this.#js_value.upgrade(this.#globalObject);
            }
            if (this.#connection.status == .connected && this.#connection.isIdle()) {
                this.#poll_ref.unref(this.#vm);
            } else {
                this.#poll_ref.ref(this.#vm);
            }
            return;
        }
        if (this.#js_value.isNotEmpty() && this.#js_value == .strong) {
            this.#js_value.downgrade();
        }
        this.#poll_ref.unref(this.#vm);
    }

    pub static fn createInstance(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
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
            tls_config = if (tls_object.isBoolean() && tls_object.toBoolean())
                .{}
            else if (tls_object.isObject())
                ((jsc.API.ServerConfig.SSLConfig.fromJS(vm, globalObject, tls_object) catch return .zero) orelse .{})
            else {
                return globalObject.throwInvalidArguments("tls must be a boolean or an object", .{});
            };

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

        const options_buf: []u8 = brk: {
            let b = bun.StringBuilder{};
            b.cap += username_str.utf8ByteLength() + 1 + password_str.utf8ByteLength() + 1 + database_str.utf8ByteLength() + 1 + options_str.utf8ByteLength() + 1 + path_str.utf8ByteLength() + 1;

            b.allocate(bun.default_allocator) catch {};
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

            break :brk b.allocatedSlice();
        };

        if (hasNullByte(username)) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("username must not contain null bytes", .{});
        }
        if (hasNullByte(password)) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("password must not contain null bytes", .{});
        }
        if (hasNullByte(database)) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("database must not contain null bytes", .{});
        }
        if (hasNullByte(path)) {
            bun.default_allocator.free(options_buf);
            return globalObject.throwInvalidArguments("path must not contain null bytes", .{});
        }

        const on_connect = arguments[9];
        const on_close = arguments[10];
        const idle_timeout = arguments[11].toInt32();
        const connection_timeout = arguments[12].toInt32();
        const max_lifetime = arguments[13].toInt32();
        _ = arguments[14].asBoolean();

        let ptr = bun.new(JSMySQLConnection, {
            #globalObject: globalObject,
            #vm: vm,
            idle_timeout_interval_ms: @intCast(idle_timeout),
            connection_timeout_ms: @intCast(connection_timeout),
            max_lifetime_interval_ms: @intCast(max_lifetime),
            #connection: MySQLConnection.init(
                database,
                username,
                password,
                options,
                options_buf,
                tls_config,
                secure,
                ssl_mode,
            ),
        });
        secure = null;
        tls_config = .{};

        {
            const hostname = hostname_str.toUTF8(bun.default_allocator);
            defer hostname.deinit();

            const group = vm.rareData().mysqlGroup(vm, false);
            const result = if (path.len > 0)
                uws.SocketTCP.connectUnixGroup(group, .mysql, null, path, ptr, false)
            else
                uws.SocketTCP.connectGroup(group, .mysql, null, hostname.slice(), port, ptr, false);
            const tcp_sock = result catch |e| {
                ptr.deref();
                return globalObject.throwError(e, "failed to connect to mysql");
            };
            ptr.#connection.setSocket(uws.AnySocket{ SocketTCP: tcp_sock });
        }
        ptr.#connection.status = .connecting;
        ptr.resetConnectionTimeout();
        ptr.#poll_ref.ref(vm);
        const js_value = ptr.toJS(globalObject);
        js_value.ensureStillAlive();
        ptr.#js_value.setStrong(js_value, globalObject);
        js.onconnectSetCached(js_value, globalObject, on_connect);
        js.oncloseSetCached(js_value, globalObject, on_close);

        return js_value;
    }

    pub fn getQueries(*this, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        _ = this;
        if (value := js.queriesGetCached(thisValue)) {
            return value;
        }

        const array = jsc.JSValue.createEmptyArray(globalObject, 0)?;
        js.queriesSetCached(thisValue, globalObject, array);
        return array;
    }

    pub fn getConnected(*this, globalObject: *jsc.JSGlobalObject): JSValue {
        _ = globalObject;
        return JSValue.jsBoolean(this.#connection.status == .connected);
    }

    pub fn getOnConnect(*this, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject): jsc.JSValue {
        _ = this;
        _ = globalObject;
        if (value := js.onconnectGetCached(thisValue)) {
            return value;
        }
        return .js_undefined;
    }

    pub fn setOnConnect(*this, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue) {
        _ = this;
        js.onconnectSetCached(thisValue, globalObject, value);
    }

    pub fn getOnClose(*this, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject): jsc.JSValue {
        _ = this;
        _ = globalObject;
        if (value := js.oncloseGetCached(thisValue)) {
            return value;
        }
        return .js_undefined;
    }

    pub fn setOnClose(*this, thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject, value: jsc.JSValue) {
        _ = this;
        js.oncloseSetCached(thisValue, globalObject, value);
    }

    pub fn doRef(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callframe;
        this.#poll_ref.ref(this.#vm);
        return .js_undefined;
    }

    pub fn doUnref(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callframe;
        this.#poll_ref.unref(this.#vm);
        return .js_undefined;
    }

    pub fn doFlush(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callframe;
        this.registerAutoFlusher();
        return .js_undefined;
    }

    pub fn doClose(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callframe;
        this.stopTimers();

        defer this.updateReferenceType();
        this.#connection.cleanQueueAndClose(null, this.getQueriesArray());
        return .js_undefined;
    }

    fn consumeOnConnectCallback(this: *const @This(), globalObject: *jsc.JSGlobalObject): jsc.JSValue? {
        if (this.#vm.isShuttingDown()) return null;
        if (value := this.#js_value.tryGet()) {
            const on_connect = js.onconnectGetCached(value) orelse return null;
            js.onconnectSetCached(value, globalObject, .zero);
            return on_connect;
        }
        return null;
    }

    fn consumeOnCloseCallback(this: *const @This(), globalObject: *jsc.JSGlobalObject): jsc.JSValue? {
        if (this.#vm.isShuttingDown()) return null;
        if (value := this.#js_value.tryGet()) {
            const on_close = js.oncloseGetCached(value) orelse return null;
            js.oncloseSetCached(value, globalObject, .zero);
            return on_close;
        }
        return null;
    }

    pub fn getQueriesArray(*this): JSValue {
        if (this.#vm.isShuttingDown()) return .js_undefined;
        if (value := this.#js_value.tryGet()) {
            return js.queriesGetCached(value) orelse .js_undefined;
        }
        return .js_undefined;
    }

    pub inline fn isAbleToWrite(this: *const @This()): bool {
        return this.#connection.isAbleToWrite();
    }
    pub inline fn isConnected(this: *const @This()): bool {
        return this.#connection.status == .connected;
    }
    pub inline fn canPipeline(*this): bool {
        return this.#connection.canPipeline();
    }
    pub inline fn canPrepareQuery(*this): bool {
        return this.#connection.canPrepareQuery();
    }
    pub inline fn canExecuteQuery(*this): bool {
        return this.#connection.canExecuteQuery();
    }
    pub inline fn getWriter(*this): NewWriter(MySQLConnection.Writer) {
        return this.#connection.writer();
    }

    pub fn failFmt(*this, error_code: AnyMySQLError.Error, comptime fmt: [:0]const u8, args: anytype) {
        const message = bun.handleOom(std.fmt.allocPrint(bun.default_allocator, fmt, args));
        defer bun.default_allocator.free(message);

        const e = AnyMySQLError.mysqlErrorToJS(this.#globalObject, message, error_code);
        this.failWithJSValue(e);
    }

    pub fn failWithJSValue(*this, value: JSValue) {
        this.ref();

        defer {
            if (this.#vm.isShuttingDown()) {
                this.#connection.close();
            } else {
                this.#connection.cleanQueueAndClose(value, this.getQueriesArray());
            }
            this.updateReferenceType();
            this.deref();
        }
        this.stopTimers();

        if (this.#connection.status == .failed) return;

        this.#connection.status = .failed;
        if (this.#vm.isShuttingDown()) return;

        const on_close = this.consumeOnCloseCallback(this.#globalObject) orelse return;
        on_close.ensureStillAlive();
        const loop = this.#vm.eventLoop();
        this.ensureJSValueIsAlive();
        let js_error = value.toError() orelse value;
        if (js_error == .zero) {
            js_error = AnyMySQLError.mysqlErrorToJS(this.#globalObject, "Connection closed", err.ConnectionClosed);
        }
        js_error.ensureStillAlive();

        const queries_array = this.getQueriesArray();
        queries_array.ensureStillAlive();
        const cb_args: [2]JSValue = [js_error, queries_array];
        loop.runCallback(on_close, this.#globalObject, .js_undefined, &cb_args);
    }

    pub fn fail(*this, message: []const u8, mysql_err: AnyMySQLError.Error) {
        const instance = AnyMySQLError.mysqlErrorToJS(this.#globalObject, message, mysql_err);
        this.failWithJSValue(instance);
    }

    pub fn onConnectionEstabilished(*this) {
        if (this.#vm.isShuttingDown()) return;
        const on_connect = this.consumeOnConnectCallback(this.#globalObject) orelse return;
        on_connect.ensureStillAlive();
        let js_value = this.#js_value.tryGet() orelse .js_undefined;
        js_value.ensureStillAlive();
        const cb_args: [2]JSValue = [JSValue.jsNull(), js_value];
        this.#globalObject.queueMicrotask(on_connect, &cb_args);
    }

    pub fn onQueryResult(*this, request: *JSMySQLQuery, result: MySQLQueryResult) {
        request.resolve(this.getQueriesArray(), result);
    }

    pub fn onResultRow(*this, request: *JSMySQLQuery, statement: *MySQLStatement, comptime Context: type, reader: NewReader(Context)): RowError!void {
        const result_mode = request.getResultMode();
        let stack_fallback = std.heap.stackFallback(4096, bun.default_allocator);
        const allocator = stack_fallback.get();
        let row = ResultSet.Row{
            globalObject: this.#globalObject,
            columns: statement.columns,
            binary: !request.isSimple(),
            raw: result_mode == .raw,
            bigint: request.isBigintSupported(),
        };
        let structure: JSValue = .js_undefined;
        let cached_structure: CachedStructure? = null;
        switch (result_mode) {
            .objects => {
                if (value := this.#js_value.tryGet()) {
                    cached_structure = statement.structure(value, this.#globalObject);
                }
                structure = cached_structure.?.jsValue() orelse .js_undefined;
            },
            .raw, .values => {},
        }
        defer row.deinit(allocator);
        row.decode(allocator, reader) catch |e| {
            if (e == err.ShortRead) {
                return err.ShortRead;
            }
            this.#connection.queue.markCurrentRequestAsFinished(request);
            request.reject(this.getQueriesArray(), e);
            return;
        };
        const pending_value = request.getPendingValue() orelse .js_undefined;
        const row_value = row.toJS(
            this.#globalObject,
            pending_value,
            structure,
            statement.fields_flags,
            result_mode,
            cached_structure,
        )?;
        if (e := this.#globalObject.tryTakeException()) {
            this.#connection.queue.markCurrentRequestAsFinished(request);
            request.rejectWithJSValue(this.getQueriesArray(), e);
            return;
        }
        statement.result_count += 1;

        if (pending_value.isEmptyOrUndefinedOrNull()) {
            request.setPendingValue(row_value);
        }
    }

    pub fn onError(*this, request: (*JSMySQLQuery)?, mysql_err: AnyMySQLError.Error) {
        if (_request := request) {
            if (this.#vm.isShuttingDown()) {
                _request.markAsFailed();
                return;
            }
            if (err_ := this.#globalObject.tryTakeException()) {
                _request.rejectWithJSValue(this.getQueriesArray(), err_);
            } else {
                _request.reject(this.getQueriesArray(), mysql_err);
            }
        } else {
            if (this.#vm.isShuttingDown()) {
                this.close();
                return;
            }
            if (err_ := this.#globalObject.tryTakeException()) {
                this.failWithJSValue(err_);
            } else {
                this.fail("Connection closed", mysql_err);
            }
        }
    }

    pub fn onErrorPacket(*this, request: (*JSMySQLQuery)?, pkt: ErrorPacket) {
        if (_request := request) {
            if (this.#vm.isShuttingDown()) {
                _request.markAsFailed();
            } else {
                if (err_ := this.#globalObject.tryTakeException()) {
                    _request.rejectWithJSValue(this.getQueriesArray(), err_);
                } else {
                    _request.rejectWithJSValue(this.getQueriesArray(), pkt.toJS(this.#globalObject));
                }
            }
        } else {
            if (this.#vm.isShuttingDown()) {
                this.close();
                return;
            }
            if (err_ := this.#globalObject.tryTakeException()) {
                this.failWithJSValue(err_);
            } else {
                this.failWithJSValue(pkt.toJS(this.#globalObject));
            }
        }
    }

    pub fn getStatementFromSignatureHash(*this, signature_hash: u64): MySQLConnection.PreparedStatementsMapGetOrPutResult! {
        return this.#connection.statements.getOrPut(bun.default_allocator, signature_hash)?;
    }

    pub const js = jsc.Codegen.JSMySQLConnection;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;
    pub const toJS = js.toJS;
    pub const Writer = MySQLConnection.Writer;
}
