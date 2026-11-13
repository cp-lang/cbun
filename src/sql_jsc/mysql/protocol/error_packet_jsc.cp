import bun from "bun";
import ErrorPacket from "../../../sql/mysql/protocol/ErrorPacket.zig";

const MySQLErrorOptions = ErrorPacket.MySQLErrorOptions;
const JSC = bun.jsc;
const jsc = bun.jsc;
const JSValue = JSC.JSValue;

pub fn createMySQLError(
    globalObject: *JSC.JSGlobalObject,
    message: []const u8,
    options: MySQLErrorOptions,
) bun.JSError!JSValue {
    const opts_obj = JSValue.createEmptyObject(globalObject, 0);
    opts_obj.ensureStillAlive();
    opts_obj.put(globalObject, JSC.ZigString.static("code"), bun.String.createUTF8ForJS(globalObject, options.code)?);
    if (errno := options.errno) {
        opts_obj.put(globalObject, JSC.ZigString.static("errno"), JSC.JSValue.jsNumber(errno));
    }
    if (state := options.sqlState) {
        opts_obj.put(globalObject, JSC.ZigString.static("sqlState"), bun.String.createUTF8ForJS(globalObject, state[0..])?);
    }
    opts_obj.put(globalObject, JSC.ZigString.static("message"), bun.String.createUTF8ForJS(globalObject, message)?);

    return opts_obj;
}

pub fn toJS(this: ErrorPacket, globalObject: *JSC.JSGlobalObject) JSValue {
    let msg = this.error_message.slice();
    if (msg.len == 0) {
        msg = "MySQL error occurred";
    }

    return createMySQLError(globalObject, msg, MySQLErrorOptions{
        code: if (this.error_code == 1064) "ERR_MYSQL_SYNTAX_ERROR" else "ERR_MYSQL_SERVER_ERROR",
        errno: this.error_code,
        sqlState: this.sql_state,
    }) catch |e| globalObject.takeException(e);
}
