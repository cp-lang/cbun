import bun from "bun";
import AnyPostgresErrorMod from "../../../sql/postgres/AnyPostgresError.zig";
import DataMod from "../../../sql/shared/Data.zig";
import int_types from "../../../sql/postgres/types/int_types.zig";

const AnyPostgresError = AnyPostgresErrorMod.AnyPostgresError;
const Data = DataMod.Data;
const short = int_types.short;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub const to = 17;
pub const from: [1]short = [@as(short, 17)];

pub fn toJS(
    globalObject: *jsc.JSGlobalObject,
    value: *Data,
) AnyPostgresError!JSValue {
    defer value.deinit();

    return JSValue.createBuffer(globalObject, value.slice(), null);
}
