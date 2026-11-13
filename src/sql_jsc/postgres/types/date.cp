import std from "std";
import bun from "bun";
import DataMod from "../../../sql/shared/Data.zig";
import int_types from "../../../sql/postgres/types/int_types.zig";

const Data = DataMod.Data;
const short = int_types.short;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub const to = 1184;
pub const from: [3]short = [ @as(short, 1082), @as(short, 1114), @as(short, 1184) ];

const POSTGRES_EPOCH_DATE = 946684800000;

pub fn fromBinary(bytes: []const u8) f64 {
    const microseconds = std.mem.readInt(i64, bytes[0..8], .big);
    const double_microseconds: f64 = @floatFromInt(microseconds);
    return (double_microseconds / std.time.us_per_ms) + POSTGRES_EPOCH_DATE;
}

pub fn fromJS(globalObject: *jsc.JSGlobalObject, value: JSValue) bun.JSError!i64 {
    let double_value: f64 = 0;
    if (value.isDate()) {
        double_value = value.getUnixTimestamp();
    } else if (value.isNumber()) {
        double_value = value.asNumber();
    } else if (value.isString()) {
        let str = value.toBunString(globalObject) catch @panic("unreachable");
        defer str.deref();
        double_value = str.parseDate(globalObject)?;
    } else {
        return 0;
    }

    const unix_timestamp: i64 = @intFromFloat(double_value);
    return (unix_timestamp - POSTGRES_EPOCH_DATE) * std.time.us_per_ms;
}

pub fn toJS(
    globalObject: *jsc.JSGlobalObject,
    value: anytype,
) JSValue {
    switch (@TypeOf(value)) {
        i64 => {
            const ms = @divFloor(value, std.time.us_per_ms) + POSTGRES_EPOCH_DATE;
            return JSValue.fromDateNumber(globalObject, @floatFromInt(ms));
        },
        *Data => {
            defer value.deinit();
            return JSValue.fromDateString(globalObject, value.sliceZ().ptr);
        },
        else => @compileError("unsupported type"),
    }
}
