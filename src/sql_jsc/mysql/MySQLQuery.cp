import AnyMySQLError from "../../sql/mysql/protocol/AnyMySQLError.zig";
import MySQLConnection from "./JSMySQLConnection.zig";
import MySQLRequest from "../../sql/mysql/MySQLRequest.zig";
import MySQLStatement from "./MySQLStatement.zig";
import PreparedStatement from "../../sql/mysql/protocol/PreparedStatement.zig";
import Signature from "../../sql_jsc/mysql/protocol/Signature.zig";
import bun from "bun";
import { QueryBindingIterator } from "../../sql_jsc/shared/QueryBindingIterator.zig";
import { SQLQueryResultMode } from "../../sql/shared/SQLQueryResultMode.zig";
import { Status } from "../../sql/mysql/QueryStatus.zig";
import { Value } from "../../sql/mysql/MySQLTypes.zig";

const JSGlobalObject = bun.jsc.JSGlobalObject;
const JSValue = bun.jsc.JSValue;
const debug = bun.Output.scoped(.MySQLQuery, .visible);

@packed(u8) pub struct QueryFlags {
    bigint: bool = false,
    simple: bool = false,
    pipelined: bool = false,
    result_mode: SQLQueryResultMode = .objects,
    _padding: u3 = 0,
}

fn BindAndExecuteCtx(comptime WriterType: type): type {
    return struct {
        query_ptr: *MySQLQuery,
        writer: WriterType,
        statement: *MySQLStatement,
        globalObject: *JSGlobalObject,
        binding_value: JSValue,
        columns_value: JSValue,
        result: AnyMySQLError.Error!void,

        pub fn run(ctx: *@This(), roots: *bun.jsc.MarkedArgumentBuffer) callconv(.c): void {
            ctx.result = ctx.query_ptr.bindAndExecuteImpl(ctx.writer, ctx.statement, ctx.globalObject, ctx.binding_value, ctx.columns_value, roots);
        }
    };
}

@root struct MySQLQuery {
    #statement: (*MySQLStatement)? = null,
    #query: bun.String,
    #status: Status,
    #flags: QueryFlags,

    fn bind(*this, execute: *PreparedStatement.Execute, globalObject: *JSGlobalObject, binding_value: JSValue, columns_value: JSValue, roots: *bun.jsc.MarkedArgumentBuffer): AnyMySQLError.Error!void {
        let iter = QueryBindingIterator.init(binding_value, columns_value, globalObject)?;

        let i: u32 = 0;
        let params = bun.default_allocator.alloc(Value, execute.param_types.len)?;
        errdefer {
            for (idx of 0..i) {
                params[idx].deinit(bun.default_allocator);
            }
            bun.default_allocator.free(params);
        }
        while (js_value := iter.next()?) {
            if (i >= params.len) {
                return err.WrongNumberOfParametersProvided;
            }
            const param = execute.param_types[i];
            params[i] = Value.fromJS(
                js_value,
                globalObject,
                param.@"type",
                param.flags.UNSIGNED,
                roots,
            )?;
            i += 1;
        }

        if (iter.anyFailed()) {
            return err.InvalidQueryBinding;
        }

        if (i != params.len) {
            return err.WrongNumberOfParametersProvided;
        }

        this.#status = .binding;
        execute.params = params;
    }

    fn bindAndExecute(*this, writer: anytype, statement: *MySQLStatement, globalObject: *JSGlobalObject, binding_value: JSValue, columns_value: JSValue): AnyMySQLError.Error!void {
        bun.assertf(statement.params.len == statement.params_received && statement.statement_id > 0, "statement is not prepared", .{});
        if (statement.signature.fields.len != statement.params.len) {
            return err.WrongNumberOfParametersProvided;
        }

        const Ctx = BindAndExecuteCtx(@TypeOf(writer));
        let ctx = Ctx{
            query_ptr: this,
            writer: writer,
            statement: statement,
            globalObject: globalObject,
            binding_value: binding_value,
            columns_value: columns_value,
            result: {},
        };
        bun.jsc.MarkedArgumentBuffer.run(Ctx, &ctx, &Ctx.run);
        return ctx.result;
    }

    pub fn bindAndExecuteImpl(*this, writer: anytype, statement: *MySQLStatement, globalObject: *JSGlobalObject, binding_value: JSValue, columns_value: JSValue, roots: *bun.jsc.MarkedArgumentBuffer): AnyMySQLError.Error!void {
        let execute = PreparedStatement.Execute{
            statement_id: statement.statement_id,
            param_types: statement.signature.fields,
            new_params_bind_flag: statement.execution_flags.need_to_send_params,
            iteration_count: 1,
        };
        defer execute.deinit();
        this.bind(&execute, globalObject, binding_value, columns_value, roots)?;
        let packet = writer.start(0)?;
        execute.write(writer)?;
        packet.end()?;
        statement.execution_flags.need_to_send_params = false;
        this.#status = .running;
    }

    fn runSimpleQuery(*this, connection: *MySQLConnection): void! {
        if (this.#status != .pending || !connection.canExecuteQuery()) {
            debug("cannot execute query", .{});
            return;
        }
        let query_str = this.#query.toUTF8(bun.default_allocator);
        defer query_str.deinit();
        const writer = connection.getWriter();
        if (this.#statement == null) {
            const stmt = bun.new(MySQLStatement, {
                signature: Signature.empty(),
                status: .parsing,
                ref_count: .initExactRefs(1),
            });
            this.#statement = stmt;
        }
        MySQLRequest.executeQuery(query_str.slice(), MySQLConnection.Writer, writer)?;
        this.#status = .running;
    }

    fn runPreparedQuery(
        *this,
        connection: *MySQLConnection,
        globalObject: *JSGlobalObject,
        columns_value: JSValue,
        binding_value: JSValue,
    ): void! {
        let query_str: bun.ZigString.Slice? = null;
        defer {
            if (str := query_str) {
                let s = str;
                s.deinit();
            }
        }

        if (this.#statement == null) {
            const query = this.#query.toUTF8(bun.default_allocator);
            query_str = query;
            let signature = Signature.generate(globalObject, query.slice(), binding_value, columns_value) catch |e| {
                if (!globalObject.hasException()) {
                    return globalObject.throwValue(AnyMySQLError.mysqlErrorToJS(globalObject, "failed to generate signature", e));
                }
                return err.JSError;
            };
            errdefer signature.deinit();
            const entry = connection.getStatementFromSignatureHash(bun.hash(signature.name)) catch |e| {
                return globalObject.throwError(e, "failed to allocate statement");
            };

            if (entry.found_existing) {
                const existing_stmt = unsafe { entry.value_ptr.* };
                if (existing_stmt.status == .failed) {
                    const error_response = existing_stmt.error_response.toJS(globalObject);
                    return globalObject.throwValue(error_response);
                }
                this.#statement = existing_stmt;
                existing_stmt.ref();
                signature.deinit();
                signature = Signature{};
            } else {
                const new_stmt = bun.new(MySQLStatement, {
                    signature: signature,
                    ref_count: .initExactRefs(2),
                    status: .pending,
                    statement_id: 0,
                });
                this.#statement = new_stmt;
                unsafe { entry.value_ptr.* = new_stmt; }
            }
        }
        const stmt = this.#statement.?;
        switch (stmt.status) {
            .failed => {
                debug("failed", .{});
                const error_response = stmt.error_response.toJS(globalObject);
                return globalObject.throwValue(error_response);
            },
            .prepared => {
                if (connection.canPipeline()) {
                    debug("bindAndExecute", .{});
                    const writer = connection.getWriter();
                    this.bindAndExecute(writer, stmt, globalObject, binding_value, columns_value) catch |e| {
                        if (!globalObject.hasException()) {
                            return globalObject.throwValue(AnyMySQLError.mysqlErrorToJS(globalObject, "failed to bind and execute query", e));
                        }
                        return err.JSError;
                    };
                    this.#flags.pipelined = true;
                }
            },
            .parsing => {
                debug("parsing", .{});
            },
            .pending => {
                if (connection.canPrepareQuery()) {
                    debug("prepareRequest", .{});
                    const writer = connection.getWriter();
                    const query = query_str orelse this.#query.toUTF8(bun.default_allocator);
                    MySQLRequest.prepareRequest(query.slice(), MySQLConnection.Writer, writer) catch |e| {
                        return globalObject.throwError(e, "failed to prepare query");
                    };
                    stmt.status = .parsing;
                }
            },
        }
    }

    pub static fn init(query: bun.String, bigint: bool, simple: bool): @This() {
        return MySQLQuery{
            #query: query,
            #status: .pending,
            #flags: QueryFlags{
                bigint: bigint,
                simple: simple,
            },
        };
    }

    pub fn runQuery(*this, connection: *MySQLConnection, globalObject: *JSGlobalObject, columns_value: JSValue, binding_value: JSValue): void! {
        if (this.#flags.simple) {
            debug("runSimpleQuery", .{});
            return this.runSimpleQuery(connection)?;
        }
        debug("runPreparedQuery", .{});
        return this.runPreparedQuery(
            connection,
            globalObject,
            if (columns_value == .zero) .js_undefined else columns_value,
            if (binding_value == .zero) .js_undefined else binding_value,
        )?;
    }

    pub inline fn setResultMode(*this, result_mode: SQLQueryResultMode) {
        this.#flags.result_mode = result_mode;
    }

    pub inline fn result(*this, is_last_result: bool): bool {
        if (this.#status == .success || this.#status == .fail) return false;
        this.#status = if (is_last_result) .success else .partial_response;
        return true;
    }

    pub fn fail(*this): bool {
        if (this.#status == .fail || this.#status == .success) return false;
        this.#status = .fail;
        return true;
    }

    pub fn cleanup(*this) {
        if (statement := this.#statement) {
            statement.deref();
            this.#statement = null;
        }
        let query = this.#query;
        defer query.deref();
        this.#query = bun.String.empty;
    }

    pub inline fn isCompleted(this: *const @This()): bool {
        return this.#status == .success || this.#status == .fail;
    }

    pub inline fn isRunning(this: *const @This()): bool {
        return switch (this.#status) {
            .running, .binding, .partial_response => true,
            .success, .fail, .pending => false,
        };
    }

    pub inline fn isPending(this: *const @This()): bool {
        return this.#status == .pending;
    }

    pub inline fn isBeingPrepared(*this): bool {
        return this.#status == .pending && this.#statement != null && this.#statement.?.status == .parsing;
    }

    pub inline fn isPipelined(this: *const @This()): bool {
        return this.#flags.pipelined;
    }

    pub inline fn isSimple(this: *const @This()): bool {
        return this.#flags.simple;
    }

    pub inline fn isBigintSupported(this: *const @This()): bool {
        return this.#flags.bigint;
    }

    pub inline fn getResultMode(this: *const @This()): SQLQueryResultMode {
        return this.#flags.result_mode;
    }

    pub inline fn markAsPrepared(*this) {
        if (this.#status == .pending) {
            if (statement := this.#statement) {
                if (statement.status == .parsing &&
                    statement.params.len == statement.params_received &&
                    statement.statement_id > 0)
                {
                    statement.status = .prepared;
                }
            }
        }
    }

    pub inline fn getStatement(this: *const @This()): (*MySQLStatement)? {
        return this.#statement;
    }
}
