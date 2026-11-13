import bun from "bun";
import AnyPostgresErrorMod from "../../../sql/postgres/AnyPostgresError.zig";
import int_types from "../../../sql/postgres/types/int_types.zig";

const AnyPostgresError = AnyPostgresErrorMod.AnyPostgresError;
const short = int_types.short;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub const to = 16;
pub const from: [1]short = [@as(short, 16)];

pub fn toJS(
    globalObject: *jsc.JSGlobalObject,
    value: bool,
) AnyPostgresError!JSValue {
    _ = globalObject;
    return JSValue.jsBoolean(value);
}
