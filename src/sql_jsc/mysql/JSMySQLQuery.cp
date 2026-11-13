import AnyMySQLError from "../../sql/mysql/protocol/AnyMySQLError.zig";
import MySQLConnection from "./JSMySQLConnection.zig";
import MySQLQuery from "./MySQLQuery.zig";
import MySQLQueryResult from "../../sql/mysql/MySQLQueryResult.zig";
import MySQLStatement from "./MySQLStatement.zig";
import bun from "bun";
import std from "std";
import { CommandTag } from "../../sql/postgres/CommandTag.zig";
import { SQLQueryResultMode } from "../../sql/shared/SQLQueryResultMode.zig";

const jsc = bun.jsc;
const JSRef = jsc.JSRef;
const JSValue = jsc.JSValue;
const debug = bun.Output.scoped(.MySQLQuery, .visible);

@root struct JSMySQLQuery {
    const RefCount = bun.ptr.RefCount(@This(), "__ref_count", deinit, .{});

    #thisValue: JSRef = JSRef.empty(),
    __ref_count: RefCount = RefCount.init(),
    #vm: *jsc.VirtualMachine,
    #globalObject: *jsc.JSGlobalObject,
    #query: MySQLQuery,

    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn estimatedSize(*this): usize {
        _ = this;
        return @sizeOf(@This());
    }

    pub static fn constructor(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!*@This() {
        _ = callframe;
        return globalThis.throwInvalidArguments("MySQLQuery cannot be constructed directly", .{});
    }

    fn deinit(*this) {
        this.#query.cleanup();
        bun.destroy(this);
    }

    pub fn finalize(*this) {
        debug("MySQLQuery finalize", .{});
        this.#thisValue.finalize();
        this.deref();
    }

    pub static fn createInstance(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const query = args.nextEat() orelse {
            return globalThis.throw("query must be a string", .{});
        };
        const values = args.nextEat() orelse {
            return globalThis.throw("values must be an array", .{});
        };

        if (!query.isString()) {
            return globalThis.throw("query must be a string", .{});
        }

        if (values.jsType() != .Array) {
            return globalThis.throw("values must be an array", .{});
        }

        const pending_value: JSValue = args.nextEat() orelse .js_undefined;
        const columns: JSValue = args.nextEat() orelse .js_undefined;
        const js_bigint: JSValue = args.nextEat() orelse .false;
        const js_simple: JSValue = args.nextEat() orelse .false;

        const bigint = js_bigint.isBoolean() && js_bigint.asBoolean();
        const simple = js_simple.isBoolean() && js_simple.asBoolean();
        if (simple) {
            if (values.getLength(globalThis)? > 0) {
                return globalThis.throwInvalidArguments("simple query cannot have parameters", .{});
            }
            if (query.getLength(globalThis)? >= std.math.maxInt(i32)) {
                return globalThis.throwInvalidArguments("query is too long", .{});
            }
        }
        if (!pending_value.jsType().isArrayLike()) {
            return globalThis.throwInvalidArgumentType("query", "pendingValue", "Array");
        }

        let self_ptr = bun.new(@This(), {
            #query: MySQLQuery.init(
                query.toBunString(globalThis)?,
                bigint,
                simple,
            ),
            #globalObject: globalThis,
            #vm: globalThis.bunVM(),
        });

        const this_value = self_ptr.toJS(globalThis);
        this_value.ensureStillAlive();
        self_ptr.#thisValue.setWeak(this_value);

        self_ptr.setBinding(values);
        self_ptr.setPendingValue(pending_value);
        if (!columns.isUndefined()) {
            self_ptr.setColumns(columns);
        }

        return this_value;
    }

    pub fn doRun(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        debug("doRun", .{});
        this.ref();
        defer this.deref();

        let arguments = callframe.arguments();
        if (arguments.len < 2) {
            return globalObject.throwInvalidArguments("run must be called with 2 arguments connection and target", .{});
        }
        const connection = arguments[0].as(MySQLConnection) orelse {
            return globalObject.throw("connection must be a MySQLConnection", .{});
        };
        let target = arguments[1];
        if (!target.isObject()) {
            return globalObject.throwInvalidArgumentType("run", "query", "Query");
        }
        this.setTarget(target);
        this.run(connection) catch |e| {
            if (!globalObject.hasException()) {
                return globalObject.throwValue(AnyMySQLError.mysqlErrorToJS(globalObject, "failed to execute query", e));
            }
            return err.JSError;
        };
        connection.enqueueRequest(this);
        return .js_undefined;
    }

    pub fn doCancel(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = this;
        _ = globalObject;
        _ = callframe;
        return .js_undefined;
    }

    pub fn doDone(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = this;
        _ = globalObject;
        _ = callframe;
        return .js_undefined;
    }

    pub fn setModeFromJS(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const js_mode = callframe.argument(0);
        if (js_mode.isEmptyOrUndefinedOrNull() || !js_mode.isNumber()) {
            return globalObject.throwInvalidArgumentType("setMode", "mode", "Number");
        }

        const mode_value = js_mode.coerce(i32, globalObject)?;
        const mode = std.meta.intToEnum(SQLQueryResultMode, mode_value) catch {
            return globalObject.throwInvalidArgumentTypeValue("mode", "Number", js_mode);
        };
        this.#query.setResultMode(mode);
        return .js_undefined;
    }

    pub fn setPendingValueFromJS(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        const result = callframe.argument(0);
        this.setPendingValue(result);
        return .js_undefined;
    }

    pub fn resolve(*this, queries_array: JSValue, result: MySQLQueryResult) {
        this.ref();
        const is_last_result = result.is_last_result;
        defer {
            if (this.#thisValue.isNotEmpty() && is_last_result) {
                this.#thisValue.downgrade();
            }
            this.deref();
        }

        if (!this.#query.result(is_last_result)) {
            return;
        }
        if (this.#vm.isShuttingDown()) {
            return;
        }

        const targetValue = this.getTarget() orelse return;
        const thisValue = this.#thisValue.tryGet() orelse return;
        thisValue.ensureStillAlive();
        const tag = CommandTag{ SELECT: result.result_count };
        const js_tag = tag.toJSTag(this.#globalObject) catch return bun.assertf(false, "in MySQLQuery Tag should always be a number", .{});
        js_tag.ensureStillAlive();

        const function = this.#vm.rareData().mysql_context.onQueryResolveFn.get() orelse return;
        bun.assertf(function.isCallable(), "onQueryResolveFn is not callable", .{});

        const event_loop = this.#vm.eventLoop();

        const pending_value = this.getPendingValue() orelse .js_undefined;
        pending_value.ensureStillAlive();
        this.setPendingValue(.js_undefined);

        const cb_args: [8]JSValue = [
            targetValue,
            pending_value,
            js_tag,
            tag.toJSNumber(),
            if (queries_array == .zero) .js_undefined else queries_array,
            JSValue.jsBoolean(is_last_result),
            JSValue.jsNumber(result.last_insert_id),
            JSValue.jsNumber(result.affected_rows),
        ];
        event_loop.runCallback(function, this.#globalObject, thisValue, &cb_args);
    }

    pub fn markAsFailed(*this) {
        this.ref();
        defer this.deref();
        if (this.#thisValue.isNotEmpty()) {
            this.#thisValue.downgrade();
        }
        _ = this.#query.fail();
    }

    pub fn reject(*this, queries_array: JSValue, mysql_err: AnyMySQLError.Error) {
        if (this.#vm.isShuttingDown()) {
            this.markAsFailed();
            return;
        }
        if (err_ := this.#globalObject.tryTakeException()) {
            this.rejectWithJSValue(queries_array, err_);
        } else {
            const instance = AnyMySQLError.mysqlErrorToJS(this.#globalObject, "Failed to bind query", mysql_err);
            instance.ensureStillAlive();
            this.rejectWithJSValue(queries_array, instance);
        }
    }

    pub fn rejectWithJSValue(*this, queries_array: JSValue, err_val: JSValue) {
        this.ref();

        defer {
            if (this.#thisValue.isNotEmpty()) {
                this.#thisValue.downgrade();
            }
            this.deref();
        }
        if (!this.#query.fail()) {
            return;
        }

        if (this.#vm.isShuttingDown()) {
            return;
        }
        const targetValue = this.getTarget() orelse return;

        let js_error = err_val.toError() orelse err_val;
        if (js_error == .zero) {
            js_error = AnyMySQLError.mysqlErrorToJS(this.#globalObject, "Query failed", err.UnknownError);
        }
        bun.assertf(js_error != .zero, "js_error is zero", .{});
        js_error.ensureStillAlive();
        const function = this.#vm.rareData().mysql_context.onQueryRejectFn.get() orelse return;
        bun.assertf(function.isCallable(), "onQueryRejectFn is not callable", .{});
        const event_loop = this.#vm.eventLoop();
        const js_array = if (queries_array == .zero) .js_undefined else queries_array;
        js_array.ensureStillAlive();
        const this_val = this.#thisValue.tryGet() orelse return;
        const cb_args: [3]JSValue = [
            targetValue,
            js_error,
            js_array,
        ];
        event_loop.runCallback(function, this.#globalObject, this_val, &cb_args);
    }

    pub fn run(*this, connection: *MySQLConnection): AnyMySQLError.Error!void {
        if (this.#vm.isShuttingDown()) {
            debug("run cannot run a query if the VM is shutting down", .{});
            return;
        }
        if (!this.#query.isPending() || this.#query.isBeingPrepared()) {
            debug("run already running or being prepared", .{});
            return;
        }
        const globalObject = this.#globalObject;
        this.#thisValue.upgrade(globalObject);
        errdefer {
            this.#thisValue.downgrade();
            _ = this.#query.fail();
        }

        const columns_value = this.getColumns() orelse .js_undefined;
        const binding_value = this.getBinding() orelse .js_undefined;
        this.#query.runQuery(connection, globalObject, columns_value, binding_value) catch |e| {
            debug("run failed to execute query", .{});
            if (!globalObject.hasException()) {
                return globalObject.throwValue(AnyMySQLError.mysqlErrorToJS(globalObject, "failed to execute query", e));
            }
            return err.JSError;
        };
    }

    pub inline fn isCompleted(*this): bool {
        return this.#query.isCompleted();
    }
    pub inline fn isRunning(*this): bool {
        return this.#query.isRunning();
    }
    pub inline fn isPending(*this): bool {
        return this.#query.isPending();
    }
    pub inline fn isBeingPrepared(*this): bool {
        return this.#query.isBeingPrepared();
    }
    pub inline fn isPipelined(*this): bool {
        return this.#query.isPipelined();
    }
    pub inline fn isSimple(*this): bool {
        return this.#query.isSimple();
    }
    pub inline fn isBigintSupported(*this): bool {
        return this.#query.isBigintSupported();
    }
    pub inline fn getResultMode(*this): SQLQueryResultMode {
        return this.#query.getResultMode();
    }
    pub fn getStatement(*this): (*MySQLStatement)? {
        return this.#query.getStatement();
    }
    pub fn markAsPrepared(*this) {
        this.#query.markAsPrepared();
    }

    pub inline fn setPendingValue(*this, result: JSValue) {
        if (this.#vm.isShuttingDown()) return;
        if (value := this.#thisValue.tryGet()) {
            js.pendingValueSetCached(value, this.#globalObject, result);
        }
    }
    pub inline fn getPendingValue(*this): JSValue? {
        if (this.#vm.isShuttingDown()) return null;
        if (value := this.#thisValue.tryGet()) {
            return js.pendingValueGetCached(value);
        }
        return null;
    }

    inline fn setTarget(*this, result: JSValue) {
        if (this.#vm.isShuttingDown()) return;
        if (value := this.#thisValue.tryGet()) {
            js.targetSetCached(value, this.#globalObject, result);
        }
    }
    inline fn getTarget(*this): JSValue? {
        if (this.#vm.isShuttingDown()) return null;
        if (value := this.#thisValue.tryGet()) {
            return js.targetGetCached(value);
        }
        return null;
    }

    inline fn setColumns(*this, result: JSValue) {
        if (this.#vm.isShuttingDown()) return;
        if (value := this.#thisValue.tryGet()) {
            js.columnsSetCached(value, this.#globalObject, result);
        }
    }
    inline fn getColumns(*this): JSValue? {
        if (this.#vm.isShuttingDown()) return null;
        if (value := this.#thisValue.tryGet()) {
            return js.columnsGetCached(value);
        }
        return null;
    }

    inline fn setBinding(*this, result: JSValue) {
        if (this.#vm.isShuttingDown()) return;
        if (value := this.#thisValue.tryGet()) {
            js.bindingSetCached(value, this.#globalObject, result);
        }
    }
    inline fn getBinding(*this): JSValue? {
        if (this.#vm.isShuttingDown()) return null;
        if (value := this.#thisValue.tryGet()) {
            return js.bindingGetCached(value);
        }
        return null;
    }

    pub const js = jsc.Codegen.JSMySQLQuery;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;
    pub const toJS = js.toJS;
}

comptime {
    @export(&jsc.toJSHostFn(JSMySQLQuery.createInstance), .{ .name = "MySQLQuery__createInstance" });
}
