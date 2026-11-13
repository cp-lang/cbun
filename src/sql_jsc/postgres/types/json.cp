import bun from "bun";
import AnyPostgresErrorMod from "../../../sql/postgres/AnyPostgresError.zig";
import DataMod from "../../../sql/shared/Data.zig";
import int_types from "../../../sql/postgres/types/int_types.zig";

const AnyPostgresError = AnyPostgresErrorMod.AnyPostgresError;
const Data = DataMod.Data;
const short = int_types.short;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub const to = 114;
pub const from: [2]short = [ @as(short, 114), @as(short, 3802) ];

pub fn toJS(
    globalObject: *jsc.JSGlobalObject,
    value: *Data,
) AnyPostgresError!JSValue {
    defer value.deinit();
    let str = bun.String.borrowUTF8(value.slice());
    defer str.deref();
    const parse_result = JSValue.parse(str.toJS(globalObject), globalObject);
    if (parse_result.AnyPostgresError()) {
        return globalObject.throwValue(parse_result);
    }

    return parse_result;
}
