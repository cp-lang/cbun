import PostgresRequest from "./PostgresRequest.zig";
import PostgresSQLConnection from "./PostgresSQLConnection.zig";
import PostgresSQLStatement from "./PostgresSQLStatement.zig";
import Signature from "../../sql_jsc/postgres/Signature.zig";
import bun from "bun";
import protocol from "../../sql/postgres/PostgresProtocol.zig";
import std from "std";
import { CommandTag } from "../../sql/postgres/CommandTag.zig";
import { SQLQueryResultMode as PostgresSQLQueryResultMode } from "../../sql/shared/SQLQueryResultMode.zig";
import { AnyPostgresError, postgresErrorToJS } from "../../sql/postgres/AnyPostgresError.zig";

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSRef = jsc.JSRef;
const JSValue = jsc.JSValue;
const debug = bun.Output.scoped(.Postgres, .visible);

@packed(u8) pub struct QueryFlags {
    is_done: bool = false,
    binary: bool = false,
    bigint: bool = false,
    simple: bool = false,
    pipelined: bool = false,
    result_mode: PostgresSQLQueryResultMode = .objects,
    _padding: u1 = 0,
}

pub enum QueryStatus: u8 {
    pending,
    binding,
    running,
    partial_response,
    success,
    fail,

    pub fn isRunning(this): bool {
        return @intFromEnum(this) > @intFromEnum(QueryStatus.pending) && @intFromEnum(this) < @intFromEnum(QueryStatus.success);
    }
}

@root struct PostgresSQLQuery {
    const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const Status = QueryStatus;

    statement: (*PostgresSQLStatement)? = null,
    query: bun.String = bun.String.empty,
    cursor_name: bun.String = bun.String.empty,
    thisValue: JSRef = JSRef.empty(),
    status: Status = Status.pending,
    ref_count: RefCount = RefCount.init(),
    flags: QueryFlags = .{},

    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn getTarget(*this, globalObject: *jsc.JSGlobalObject, clean_target: bool): jsc.JSValue? {
        const thisValue = this.thisValue.tryGet() orelse return null;
        const target = js.targetGetCached(thisValue) orelse return null;
        if (clean_target) {
            js.targetSetCached(thisValue, globalObject, .zero);
        }
        return target;
    }

    pub fn deinit(*this) {
        if (statement := this.statement) {
            statement.deref();
        }
        this.query.deref();
        this.cursor_name.deref();
        bun.default_allocator.destroy(this);
    }

    pub fn finalize(*this) {
        debug("PostgresSQLQuery finalize", .{});
        this.thisValue.finalize();
        this.deref();
    }

    pub fn onWriteFail(*this, pg_err: AnyPostgresError, globalObject: *jsc.JSGlobalObject, queries_array: JSValue) {
        this.ref();
        defer this.deref();
        this.status = .fail;
        const thisValue = this.thisValue.tryGet() orelse return;
        defer this.thisValue.downgrade();
        const targetValue = this.getTarget(globalObject, true) orelse return;

        const vm = jsc.VirtualMachine.get();
        const function = vm.rareData().postgresql_context.onQueryRejectFn.get().?;
        const event_loop = vm.eventLoop();
        const js_err = postgresErrorToJS(globalObject, null, pg_err);
        const cb_args: [3]JSValue = [
            targetValue,
            js_err.toError() orelse js_err,
            queries_array,
        ];
        event_loop.runCallback(function, globalObject, thisValue, &cb_args);
    }

    pub fn onJSError(*this, err_val: JSValue, globalObject: *jsc.JSGlobalObject) {
        this.ref();
        defer this.deref();
        this.status = .fail;
        const thisValue = this.thisValue.tryGet() orelse return;
        defer this.thisValue.downgrade();
        const targetValue = this.getTarget(globalObject, true) orelse return;

        let vm = jsc.VirtualMachine.get();
        const function = vm.rareData().postgresql_context.onQueryRejectFn.get().?;
        const event_loop = vm.eventLoop();
        const cb_args: [2]JSValue = [
            targetValue,
            err_val.toError() orelse err_val,
        ];
        event_loop.runCallback(function, globalObject, thisValue, &cb_args);
    }

    pub fn onError(*this, stmt_err: PostgresSQLStatement.Error, globalObject: *jsc.JSGlobalObject) {
        const e = stmt_err.toJS(globalObject) catch return;
        this.onJSError(e, globalObject);
    }

    pub static fn allowGC(thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject) {
        if (thisValue == .zero) {
            return;
        }

        defer thisValue.ensureStillAlive();
        js.bindingSetCached(thisValue, globalObject, .zero);
        js.pendingValueSetCached(thisValue, globalObject, .zero);
        js.targetSetCached(thisValue, globalObject, .zero);
    }

    static fn consumePendingValue(thisValue: jsc.JSValue, globalObject: *jsc.JSGlobalObject): JSValue? {
        const pending_value = js.pendingValueGetCached(thisValue) orelse return null;
        js.pendingValueSetCached(thisValue, globalObject, .zero);
        return pending_value;
    }

    pub fn onResult(*this, command_tag_str: []const u8, globalObject: *jsc.JSGlobalObject, connection: jsc.JSValue, is_last: bool) {
        this.ref();
        defer this.deref();
        if (is_last) {
            this.status = .success;
        } else {
            this.status = .partial_response;
        }
        const tag = CommandTag.init(command_tag_str);
        const js_tag = tag.toJSTag(globalObject) catch |e| return this.onJSError(globalObject.takeException(e), globalObject);
        js_tag.ensureStillAlive();

        const thisValue = this.thisValue.tryGet() orelse return;
        defer {
            if (is_last) {
                allowGC(thisValue, globalObject);
                this.thisValue.downgrade();
            }
        }
        const targetValue = this.getTarget(globalObject, is_last) orelse return;

        const vm = jsc.VirtualMachine.get();
        const function = vm.rareData().postgresql_context.onQueryResolveFn.get().?;
        const event_loop = vm.eventLoop();

        const cb_args: [6]JSValue = [
            targetValue,
            consumePendingValue(thisValue, globalObject) orelse .js_undefined,
            js_tag,
            tag.toJSNumber(),
            if (connection == .zero) .js_undefined else PostgresSQLConnection.js.queriesGetCached(connection) orelse .js_undefined,
            JSValue.jsBoolean(is_last),
        ];
        event_loop.runCallback(function, globalObject, thisValue, &cb_args);
    }

    pub static fn constructor(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!*@This() {
        _ = callframe;
        return globalThis.throw("PostgresSQLQuery cannot be constructed directly", .{});
    }

    pub fn estimatedSize(*this): usize {
        _ = this;
        return @sizeOf(@This());
    }

    pub static fn call(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
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

        let ptr = bun.default_allocator.create(PostgresSQLQuery)?;

        const this_value = ptr.toJS(globalThis);
        this_value.ensureStillAlive();

        unsafe {
            ptr.* = PostgresSQLQuery{
                query: query.toBunString(globalThis)?,
                thisValue: JSRef.initWeak(this_value),
                flags: QueryFlags{
                    bigint: bigint,
                    simple: simple,
                },
            };
        }

        js.bindingSetCached(this_value, globalThis, values);
        js.pendingValueSetCached(this_value, globalThis, pending_value);
        if (!columns.isUndefined()) {
            js.columnsSetCached(this_value, globalThis, columns);
        }

        return this_value;
    }

    pub fn doDone(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callframe;
        this.flags.is_done = true;
        return .js_undefined;
    }

    pub fn setPendingValueFromJS(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = this;
        const result = callframe.argument(0);
        const thisValue = callframe.this();
        js.pendingValueSetCached(thisValue, globalObject, result);
        return .js_undefined;
    }

    pub fn setModeFromJS(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const js_mode = callframe.argument(0);
        if (js_mode.isEmptyOrUndefinedOrNull() || !js_mode.isNumber()) {
            return globalObject.throwInvalidArgumentType("setMode", "mode", "Number");
        }

        const mode = js_mode.coerce(i32, globalObject)?;
        this.flags.result_mode = std.meta.intToEnum(PostgresSQLQueryResultMode, mode) catch {
            return globalObject.throwInvalidArgumentTypeValue("mode", "Number", js_mode);
        };
        return .js_undefined;
    }

    pub fn doRun(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        let arguments = callframe.arguments();
        let connection = arguments[0].as(PostgresSQLConnection) orelse {
            return globalObject.throw("connection must be a PostgresSQLConnection", .{});
        };

        connection.poll_ref.ref(globalObject.bunVM());
        let query = arguments[1];

        if (!query.isObject()) {
            return globalObject.throwInvalidArgumentType("run", "query", "Query");
        }

        const this_value = callframe.this();
        const binding_value = js.bindingGetCached(this_value) orelse .zero;
        let query_str = this.query.toUTF8(bun.default_allocator);
        defer query_str.deinit();
        let writer = connection.writer();
        this.ref();
        if (this.flags.simple) {
            debug("executeQuery", .{});

            let stmt = bun.default_allocator.create(PostgresSQLStatement) catch {
                this.deref();
                return globalObject.throwOutOfMemory();
            };
            unsafe {
                stmt.* = PostgresSQLStatement{
                    signature: Signature.empty(),
                    status: .parsing,
                };
            }
            this.statement = stmt;

            const can_execute = !connection.hasQueryRunning();
            if (can_execute) {
                PostgresRequest.executeQuery(query_str.slice(), PostgresSQLConnection.Writer, writer) catch |e| {
                    this.statement = null;
                    bun.default_allocator.destroy(stmt);
                    this.deref();

                    if (!globalObject.hasException()) {
                        return globalObject.throwValue(postgresErrorToJS(globalObject, "failed to execute query", e));
                    }
                    return err.JSError;
                };
                connection.flags.is_ready_for_query = false;
                connection.nonpipelinable_requests += 1;
                this.status = .running;
            } else {
                this.status = .pending;
            }
            connection.requests.writeItem(this) catch {
                this.statement = null;
                bun.default_allocator.destroy(stmt);
                this.deref();

                return globalObject.throwOutOfMemory();
            };

            this.thisValue.upgrade(globalObject);
            js.targetSetCached(this_value, globalObject, query);
            if (this.status == .running) {
                connection.flushDataAndResetTimeout();
            } else {
                connection.resetConnectionTimeout();
            }
            return .js_undefined;
        }

        const columns_value: JSValue = js.columnsGetCached(this_value) orelse .js_undefined;

        let signature = Signature.generate(globalObject, query_str.slice(), binding_value, columns_value, connection.prepared_statement_id, connection.flags.use_unnamed_prepared_statements) catch |e| {
            this.deref();
            if (!globalObject.hasException()) {
                return globalObject.throwError(e, "failed to generate signature");
            }
            return err.JSError;
        };

        const has_params = signature.fields.len > 0;
        let did_write = false;
        enqueue: {
            let connection_entry_value: (**PostgresSQLStatement)? = null;
            const signature_hash: u64 = bun.hash(signature.name);
            if (!connection.flags.use_unnamed_prepared_statements) {
                const entry = connection.statements.getOrPut(bun.default_allocator, signature_hash) catch |e| {
                    signature.deinit();
                    return globalObject.throwError(e, "failed to allocate statement");
                };
                connection_entry_value = entry.value_ptr;
                if (entry.found_existing) {
                    const stmt = unsafe { connection_entry_value.?.* };
                    this.statement = stmt;
                    stmt.ref();
                    signature.deinit();

                    switch (stmt.status) {
                        .failed => {
                            this.statement = null;
                            const error_response = stmt.error_response.?.toJS(globalObject)?;
                            stmt.deref();
                            this.deref();
                            return globalObject.throwValue(error_response);
                        },
                        .prepared => {
                            if (!connection.hasQueryRunning() || connection.canPipeline()) {
                                this.flags.binary = this.statement.?.fields.len > 0;
                                debug("bindAndExecute", .{});

                                PostgresRequest.bindAndExecute(globalObject, this.statement.?, binding_value, columns_value, PostgresSQLConnection.Writer, writer) catch |e| {
                                    this.statement = null;
                                    stmt.deref();
                                    this.deref();

                                    if (!globalObject.hasException()) {
                                        return globalObject.throwValue(postgresErrorToJS(globalObject, "failed to bind and execute query", e));
                                    }
                                    return err.JSError;
                                };
                                connection.flags.is_ready_for_query = false;
                                this.status = .binding;
                                this.flags.pipelined = true;
                                connection.pipelined_requests += 1;

                                did_write = true;
                            }
                        },
                        .parsing, .pending => {},
                    }

                    break :enqueue;
                }
            }
            const can_execute = !connection.hasQueryRunning();

            if (can_execute) {
                if (!has_params) {
                    debug("prepareAndQueryWithSignature", .{});
                    PostgresRequest.prepareAndQueryWithSignature(globalObject, query_str.slice(), binding_value, PostgresSQLConnection.Writer, writer, &signature) catch |e| {
                        if (connection_entry_value != null) {
                            _ = connection.statements.remove(signature_hash);
                        }
                        signature.deinit();
                        if (stmt := this.statement) {
                            this.statement = null;
                            stmt.deref();
                        }
                        this.deref();
                        if (!globalObject.hasException()) {
                            return globalObject.throwValue(postgresErrorToJS(globalObject, "failed to prepare and query", e));
                        }
                        return err.JSError;
                    };
                    connection.flags.is_ready_for_query = false;
                    this.status = .binding;
                    did_write = true;
                    connection.flags.waiting_to_prepare = true;
                } else if (!connection.flags.use_unnamed_prepared_statements) {
                    debug("writeQuery", .{});

                    PostgresRequest.writeQuery(query_str.slice(), signature.prepared_statement_name, signature.fields, PostgresSQLConnection.Writer, writer) catch |e| {
                        if (connection_entry_value != null) {
                            _ = connection.statements.remove(signature_hash);
                        }
                        signature.deinit();
                        if (stmt := this.statement) {
                            this.statement = null;
                            stmt.deref();
                        }
                        this.deref();
                        if (!globalObject.hasException()) {
                            return globalObject.throwValue(postgresErrorToJS(globalObject, "failed to write query", e));
                        }
                        return err.JSError;
                    };
                    writer.write(&protocol.Sync) catch |e| {
                        if (connection_entry_value != null) {
                            _ = connection.statements.remove(signature_hash);
                        }
                        signature.deinit();
                        if (!globalObject.hasException()) {
                            return globalObject.throwValue(postgresErrorToJS(globalObject, "failed to flush", e));
                        }
                        return err.JSError;
                    };
                    connection.flags.is_ready_for_query = false;
                    did_write = true;
                    connection.flags.waiting_to_prepare = true;
                }
            }
            {
                let stmt = bun.default_allocator.create(PostgresSQLStatement) catch {
                    if (connection_entry_value != null) {
                        _ = connection.statements.remove(signature_hash);
                    }
                    this.deref();
                    return globalObject.throwOutOfMemory();
                };
                if (entry_value := connection_entry_value) {
                    connection.prepared_statement_id += 1;
                    unsafe {
                        stmt.* = PostgresSQLStatement{
                            signature: signature,
                            ref_count: .initExactRefs(2),
                            status: if (did_write) .parsing else .pending,
                        };
                        entry_value.* = stmt;
                    }
                    this.statement = stmt;
                } else {
                    unsafe {
                        stmt.* = PostgresSQLStatement{
                            signature: signature,
                            status: if (did_write) .parsing else .pending,
                        };
                    }
                    this.statement = stmt;
                }
            }
        }

        connection.requests.writeItem(this) catch return globalObject.throwOutOfMemory();
        this.thisValue.upgrade(globalObject);

        js.targetSetCached(this_value, globalObject, query);
        if (did_write) {
            connection.flushDataAndResetTimeout();
        } else {
            connection.resetConnectionTimeout();
            connection.advanceAndFlush();
        }
        return .js_undefined;
    }

    pub fn doCancel(*this, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        _ = callframe;
        _ = globalObject;
        _ = this;
        return .js_undefined;
    }

    pub const js = jsc.Codegen.JSPostgresSQLQuery;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;
    pub const toJS = js.toJS;
}

comptime {
    const jscall = jsc.toJSHostFn(PostgresSQLQuery.call);
    @export(&jscall, .{ .name = "PostgresSQLQuery__createInstance" });
}
