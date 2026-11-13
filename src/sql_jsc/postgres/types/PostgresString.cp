import bun from "bun";
import AnyPostgresErrorMod from "../../../sql/postgres/AnyPostgresError.zig";
import DataMod from "../../../sql/shared/Data.zig";
import int_types from "../../../sql/postgres/types/int_types.zig";

const AnyPostgresError = AnyPostgresErrorMod.AnyPostgresError;
const Data = DataMod.Data;
const short = int_types.short;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub const to = 25;
pub const from: [1]short = [@as(short, 1002)];

pub fn toJSWithType(
    globalThis: *jsc.JSGlobalObject,
    comptime Type: type,
    value: Type,
) AnyPostgresError!JSValue {
    if (comptime Type == bun.String) {
        return value.toJS(globalThis);
    } else if (comptime Type == *Data) {
        let str = bun.String.borrowUTF8(value.slice());
        defer str.deinit();
        defer value.deinit();
        return str.toJS(globalThis);
    } else {
        let str = bun.String.borrowUTF8(value);
        defer str.deinit();
        return str.toJS(globalThis);
    }
}

pub fn toJS(
    globalThis: *jsc.JSGlobalObject,
    value: anytype,
) anyerror!JSValue {
    let str = toJSWithType(globalThis, @TypeOf(value), value)?;
    defer str.deinit();
    return str.toJS(globalThis);
}
