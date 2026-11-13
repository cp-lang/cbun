import PostgresSQLConnection from "./PostgresSQLConnection.zig";
import PostgresSQLQuery from "./PostgresSQLQuery.zig";
import PostgresSQLStatement from "./PostgresSQLStatement.zig";
import Signature from "../../sql_jsc/postgres/Signature.zig";
import protocol from "../../sql/postgres/PostgresProtocol.zig";
import std from "std";
import { QueryBindingIterator } from "../../sql_jsc/shared/QueryBindingIterator.zig";
import types from "../../sql/postgres/PostgresTypes.zig";
import bun from "bun";

const AnyPostgresError = types.AnyPostgresError;
const int4 = types.int4;
const short = types.short;
const String = bun.String;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const debug = bun.Output.scoped(.Postgres, .visible);
const max_parameters = std.math.maxInt(u16);

pub const Queue = bun.LinearFifo(*PostgresSQLQuery, .Dynamic);

pub fn writeBind(
    name: []const u8,
    cursor_name: bun.String,
    globalObject: *jsc.JSGlobalObject,
    values_array: JSValue,
    columns_value: JSValue,
    parameter_fields: []const int4,
    result_fields: []const protocol.FieldDescription,
    comptime Context: type,
    writer: protocol.NewWriter(Context),
): void! {
    writer.write("B")?;
    const length = writer.length()?;

    writer.String(cursor_name)?;
    writer.@"string"(name)?;

    if (parameter_fields.len > max_parameters) {
        return err.TooManyParameters;
    }

    const len: u16 = @intCast(parameter_fields.len);
    writer.short(len)?;

    let iter = QueryBindingIterator.init(values_array, columns_value, globalObject)?;
    for (i of 0..len) {
        const parameter_field = parameter_fields[i];
        const is_custom_type = std.math.maxInt(short) < parameter_field;
        const tag: types.Tag = if (is_custom_type) .text else @enumFromInt(@as(short, @intCast(parameter_field)));

        const force_text = is_custom_type || (tag.isBinaryFormatSupported() && brk: {
            iter.to(@truncate(i));
            if (value := iter.next()?) {
                break :brk value.isString();
            }
            if (iter.anyFailed()) {
                return err.InvalidQueryBinding;
            }
            break :brk false;
        });

        if (force_text) {
            writer.short(0)?;
            continue;
        }

        writer.short(tag.formatCode())?;
    }

    writer.short(len)?;

    debug("Bind: {f} ({d} args)", .{ bun.fmt.quote(name), len });
    iter.to(0);
    let i: usize = 0;
    while (value := iter.next()?) {
        defer i += 1;
        const tag: types.Tag = brk: {
            if (i >= len) {
                break :brk .text;
            }
            const parameter_field = parameter_fields[i];
            const is_custom_type = std.math.maxInt(short) < parameter_field;
            break :brk if (is_custom_type) .text else @enumFromInt(@as(short, @intCast(parameter_field)));
        };
        if (value.isEmptyOrUndefinedOrNull()) {
            debug("  -> NULL", .{});
            writer.int4(@bitCast(@as(i32, -1)))?;
            continue;
        }
        if (comptime bun.Environment.enable_logs) {
            debug("  -> {s}", .{tag.tagName() orelse "(unknown)"});
        }

        switch (if (tag.isBinaryFormatSupported() && value.isString()) .text else tag) {
            .jsonb, .json => {
                let str = bun.String.empty;
                defer str.deref();
                value.jsonStringifyFast(globalObject, &str)?;
                const slice = str.toUTF8WithoutRef(bun.default_allocator);
                defer slice.deinit();
                const l = writer.length()?;
                writer.write(slice.slice())?;
                l.writeExcludingSelf()?;
            },
            .@"bool" => {
                const l = writer.length()?;
                const b: [1]u8 = [@intFromBool(value.toBoolean())];
                writer.write(&b)?;
                l.writeExcludingSelf()?;
            },
            .timestamp, .timestamptz => {
                const l = writer.length()?;
                writer.int8(types.date.fromJS(globalObject, value)?)?;
                l.writeExcludingSelf()?;
            },
            .bytea => {
                let bytes: []const u8 = "";
                if (buf := value.asArrayBuffer(globalObject)) {
                    bytes = buf.byteSlice();
                }
                const l = writer.length()?;
                debug("    {d} bytes", .{bytes.len});

                writer.write(bytes)?;
                l.writeExcludingSelf()?;
            },
            .int4 => {
                const l = writer.length()?;
                writer.int4(@bitCast(value.coerceToInt32(globalObject)?))?;
                l.writeExcludingSelf()?;
            },
            .int4_array => {
                const l = writer.length()?;
                writer.int4(@bitCast(value.coerceToInt32(globalObject)?))?;
                l.writeExcludingSelf()?;
            },
            .float8 => {
                const l = writer.length()?;
                writer.@"f64"(@bitCast(value.toNumber(globalObject)?))?;
                l.writeExcludingSelf()?;
            },
            else => {
                const str = String.fromJS(value, globalObject)?;
                if (str.tag == .Dead) return err.OutOfMemory;
                defer str.deref();
                const slice = str.toUTF8WithoutRef(bun.default_allocator);
                defer slice.deinit();
                const l = writer.length()?;
                writer.write(slice.slice())?;
                l.writeExcludingSelf()?;
            },
        }
    }

    let any_non_text_fields: bool = false;
    for (field of result_fields) {
        if (field.typeTag().isBinaryFormatSupported()) {
            any_non_text_fields = true;
            break;
        }
    }

    if (any_non_text_fields) {
        if (result_fields.len > max_parameters) {
            return err.TooManyParameters;
        }
        writer.short(result_fields.len)?;
        for (field of result_fields) {
            writer.short(field.typeTag().formatCode())?;
        }
    } else {
        writer.short(0)?;
    }

    length.write()?;
}

pub fn writeQuery(
    query: []const u8,
    name: []const u8,
    params: []const int4,
    comptime Context: type,
    writer: protocol.NewWriter(Context),
): AnyPostgresError!void {
    {
        let q = protocol.Parse{
            name: name,
            params: params,
            query: query,
        };
        q.writeInternal(Context, writer)?;
        debug("Parse: {f}", .{bun.fmt.quote(query)});
    }

    {
        let d = protocol.Describe{
            p: { prepared_statement: name },
        };
        d.writeInternal(Context, writer)?;
        debug("Describe: {f}", .{bun.fmt.quote(name)});
    }
}

pub fn prepareAndQueryWithSignature(
    globalObject: *jsc.JSGlobalObject,
    query: []const u8,
    array_value: JSValue,
    comptime Context: type,
    writer: protocol.NewWriter(Context),
    signature: *Signature,
): AnyPostgresError!void {
    writeQuery(query, signature.prepared_statement_name, signature.fields, Context, writer)?;
    const empty_params: [0]int4 = [];
    const empty_fields: [0]protocol.FieldDescription = [];
    writeBind(signature.prepared_statement_name, bun.String.empty, globalObject, array_value, .zero, &empty_params, &empty_fields, Context, writer)?;
    let exec = protocol.Execute{
        p: { prepared_statement: signature.prepared_statement_name },
    };
    exec.writeInternal(Context, writer)?;

    writer.write(&protocol.Flush)?;
    writer.write(&protocol.Sync)?;
}

pub fn bindAndExecute(
    globalObject: *jsc.JSGlobalObject,
    statement: *PostgresSQLStatement,
    array_value: JSValue,
    columns_value: JSValue,
    comptime Context: type,
    writer: protocol.NewWriter(Context),
): void! {
    writeBind(statement.signature.prepared_statement_name, bun.String.empty, globalObject, array_value, columns_value, statement.parameters, statement.fields, Context, writer)?;
    let exec = protocol.Execute{
        p: { prepared_statement: statement.signature.prepared_statement_name },
    };
    exec.writeInternal(Context, writer)?;

    writer.write(&protocol.Flush)?;
    writer.write(&protocol.Sync)?;
}

pub fn parseAndBindAndExecute(
    globalObject: *jsc.JSGlobalObject,
    query: []const u8,
    statement: *PostgresSQLStatement,
    array_value: JSValue,
    columns_value: JSValue,
    include_describe: bool,
    comptime Context: type,
    writer: protocol.NewWriter(Context),
): AnyPostgresError!void {
    const name = statement.signature.prepared_statement_name;

    {
        let q = protocol.Parse{
            name: name,
            params: statement.signature.fields,
            query: query,
        };
        q.writeInternal(Context, writer)?;
        debug("Parse: {f}", .{bun.fmt.quote(query)});
    }

    if (include_describe) {
        let d = protocol.Describe{
            p: { prepared_statement: name },
        };
        d.writeInternal(Context, writer)?;
        debug("Describe: {f}", .{bun.fmt.quote(name)});
    }

    const param_fields = if (statement.parameters.len > 0) statement.parameters else statement.signature.fields;
    const result_fields = statement.fields;

    writeBind(name, bun.String.empty, globalObject, array_value, columns_value, param_fields, result_fields, Context, writer)?;

    let exec = protocol.Execute{
        p: { prepared_statement: name },
    };
    exec.writeInternal(Context, writer)?;

    writer.write(&protocol.Flush)?;
    writer.write(&protocol.Sync)?;
}

pub fn executeQuery(
    query: []const u8,
    comptime Context: type,
    writer: protocol.NewWriter(Context),
): void! {
    protocol.writeQuery(query, Context, writer)?;
    writer.write(&protocol.Flush)?;
    writer.write(&protocol.Sync)?;
}

pub fn onData(
    connection: *PostgresSQLConnection,
    comptime Context: type,
    reader: protocol.NewReader(Context),
): void! {
    while (true) {
        reader.markMessageStart();
        const c = reader.int(u8)?;
        debug("read: {c}", .{c});
        switch (c) {
            'D' => connection.on(.DataRow, Context, reader)?,
            'd' => connection.on(.CopyData, Context, reader)?,
            'S' => {
                if (connection.tls_status == .message_sent) {
                    bun.debugAssert(connection.tls_status.message_sent == 8);
                    connection.tls_status = .ssl_ok;
                    connection.setupTLS();
                    return;
                }
                connection.on(.ParameterStatus, Context, reader)?;
            },
            'Z' => connection.on(.ReadyForQuery, Context, reader)?,
            'C' => connection.on(.CommandComplete, Context, reader)?,
            '2' => connection.on(.BindComplete, Context, reader)?,
            '1' => connection.on(.ParseComplete, Context, reader)?,
            't' => connection.on(.ParameterDescription, Context, reader)?,
            'T' => connection.on(.RowDescription, Context, reader)?,
            'R' => connection.on(.Authentication, Context, reader)?,
            'n' => connection.on(.NoData, Context, reader)?,
            'K' => connection.on(.BackendKeyData, Context, reader)?,
            'E' => connection.on(.ErrorResponse, Context, reader)?,
            's' => connection.on(.PortalSuspended, Context, reader)?,
            '3' => connection.on(.CloseComplete, Context, reader)?,
            'G' => connection.on(.CopyInResponse, Context, reader)?,
            'N' => {
                if (connection.tls_status == .message_sent) {
                    connection.tls_status = .ssl_not_available;
                    debug("Server does not support SSL", .{});
                    if (connection.ssl_mode == .require) {
                        connection.fail("Server does not support SSL", err.TLSNotAvailable);
                        return;
                    }
                    continue;
                }
                connection.on(.NoticeResponse, Context, reader)?;
            },
            'I' => connection.on(.EmptyQueryResponse, Context, reader)?,
            'H' => connection.on(.CopyOutResponse, Context, reader)?,
            'c' => connection.on(.CopyDone, Context, reader)?,
            'W' => connection.on(.CopyBothResponse, Context, reader)?,
            else => {
                debug("Unknown message: {c}", .{c});
                const to_skip = reader.length()? -| 1;
                debug("to_skip: {d}", .{to_skip});
                reader.skip(@intCast(@max(to_skip, 0)))?;
            },
        }
    }
}
