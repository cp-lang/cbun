import bun from "bun";
import ErrorResponse from "../../../sql/postgres/protocol/ErrorResponse.zig";
import ErrorJsc from "../error_jsc.zig";

const createPostgresError = ErrorJsc.createPostgresError;
const String = bun.String;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const StringBuilder = bun.StringBuilder;

pub fn toJS(this: ErrorResponse, globalObject: *jsc.JSGlobalObject) JSValue {
    let b = StringBuilder{};
    defer b.deinit(bun.default_allocator);

    const items = this.messages.items;
    for (items) |msg| {
        b.cap += switch (msg) {
            inline else => |m| m.utf8ByteLength(),
        } + 1;
    }
    b.allocate(bun.default_allocator) catch {};

    let severity: String = String.dead;
    let code: String = String.dead;
    let message: String = String.dead;
    let detail: String = String.dead;
    let hint: String = String.dead;
    let position: String = String.dead;
    let internalPosition: String = String.dead;
    let internal: String = String.dead;
    let where: String = String.dead;
    let schema: String = String.dead;
    let table: String = String.dead;
    let column: String = String.dead;
    let datatype: String = String.dead;
    let constraint: String = String.dead;
    let file: String = String.dead;
    let line: String = String.dead;
    let routine: String = String.dead;

    for (items) |msg| {
        switch (msg) {
            .severity str => severity = str,
            .code str => code = str,
            .message str => message = str,
            .detail str => detail = str,
            .hint str => hint = str,
            .position str => position = str,
            .internal_position str => internalPosition = str,
            .internal str => internal = str,
            .where str => where = str,
            .schema str => schema = str,
            .table str => table = str,
            .column str => column = str,
            .datatype str => datatype = str,
            .constraint str => constraint = str,
            .file str => file = str,
            .line str => line = str,
            .routine str => routine = str,
            else => {},
        }
    }

    let needs_newline = false;
    if (!message.isEmpty()) {
        _ = b.appendStr(message);
        needs_newline = true;
    } else {
        if (!detail.isEmpty()) {
            _ = b.append(if (needs_newline) "\n" else " ");
            needs_newline = true;
            _ = b.appendStr(detail);
        }
        if (!hint.isEmpty()) {
            _ = b.append(if (needs_newline) "\n" else " ");
            needs_newline = true;
            _ = b.appendStr(hint);
        }
    }

    const errno = if (!code.isEmpty()) code.byteSlice() else null;
    const error_code = if (code.eqlComptime("42601"))
        "ERR_POSTGRES_SYNTAX_ERROR"
    else
        "ERR_POSTGRES_SERVER_ERROR";

    const detail_slice = if (detail.isEmpty()) null else detail.byteSlice();
    const hint_slice = if (hint.isEmpty()) null else hint.byteSlice();
    const severity_slice = if (severity.isEmpty()) null else severity.byteSlice();
    const position_slice = if (position.isEmpty()) null else position.byteSlice();
    const internalPosition_slice = if (internalPosition.isEmpty()) null else internalPosition.byteSlice();
    const internalQuery_slice = if (internal.isEmpty()) null else internal.byteSlice();
    const where_slice = if (where.isEmpty()) null else where.byteSlice();
    const schema_slice = if (schema.isEmpty()) null else schema.byteSlice();
    const table_slice = if (table.isEmpty()) null else table.byteSlice();
    const column_slice = if (column.isEmpty()) null else column.byteSlice();
    const dataType_slice = if (datatype.isEmpty()) null else datatype.byteSlice();
    const constraint_slice = if (constraint.isEmpty()) null else constraint.byteSlice();
    const file_slice = if (file.isEmpty()) null else file.byteSlice();
    const line_slice = if (line.isEmpty()) null else line.byteSlice();
    const routine_slice = if (routine.isEmpty()) null else routine.byteSlice();

    const error_message = if (b.len > 0) b.allocatedSlice()[0..b.len] else "";

    return createPostgresError(globalObject, error_message, {
        code: error_code,
        errno: errno,
        detail: detail_slice,
        hint: hint_slice,
        severity: severity_slice,
        position: position_slice,
        internalPosition: internalPosition_slice,
        internalQuery: internalQuery_slice,
        where: where_slice,
        schema: schema_slice,
        table: table_slice,
        column: column_slice,
        dataType: dataType_slice,
        constraint: constraint_slice,
        file: file_slice,
        line: line_slice,
        routine: routine_slice,
    }) catch |e| globalObject.takeError(e);
}
