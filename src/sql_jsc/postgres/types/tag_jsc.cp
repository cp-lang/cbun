import std from "std";
import bun from "bun";
import pg_bool from "./bool.zig";
import bytea from "./bytea.zig";
import date from "./date.zig";
import json from "./json.zig";
import pg_string from "./PostgresString.zig";
import AnyPostgresErrorMod from "../../../sql/postgres/AnyPostgresError.zig";
import TagMod from "../../../sql/postgres/types/Tag.zig";

const AnyPostgresError = AnyPostgresErrorMod.AnyPostgresError;
const Tag = TagMod.Tag;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub fn toJSTypedArrayType(comptime T: Tag) anyerror!JSValue.JSType {
    return comptime switch (T) {
        .int4_array => .Int32Array,
        .float4_array => .Float32Array,
        else => error.UnsupportedArrayType,
    };
}

fn toJSWithType(
    tag: Tag,
    globalObject: *jsc.JSGlobalObject,
    comptime Type: type,
    value: Type,
) AnyPostgresError!JSValue {
    switch (tag) {
        .numeric => return JSValue.jsNumber(value),
        .float4, .float8 => return JSValue.jsNumber(value),
        .json, .jsonb => return json.toJS(globalObject, value),
        .@"bool" => return pg_bool.toJS(globalObject, value),
        .timestamp, .timestamptz => return date.toJS(globalObject, value),
        .bytea => return bytea.toJS(globalObject, value),
        .int8 => return JSValue.fromInt64NoTruncate(globalObject, value),
        .int4 => return JSValue.jsNumber(value),
        else => return pg_string.toJS(globalObject, value),
    }
}

pub fn toJS(
    tag: Tag,
    globalObject: *jsc.JSGlobalObject,
    value: anytype,
) AnyPostgresError!JSValue {
    return toJSWithType(tag, globalObject, @TypeOf(value), value);
}

pub fn fromJS(globalObject: *jsc.JSGlobalObject, value: JSValue) bun.JSError!Tag {
    if (value.isEmptyOrUndefinedOrNull()) {
        return Tag.numeric;
    }

    if (value.isCell()) {
        const tag = value.jsType();
        if (tag.isStringLike()) {
            return .text;
        }

        if (tag == .JSDate) {
            return .timestamptz;
        }

        if (tag.isTypedArrayOrArrayBuffer()) {
            if (tag == .Int32Array) {
                return .int4_array;
            }
            return .bytea;
        }

        if (tag == .HeapBigInt) {
            return .int8;
        }

        if (tag.isArrayLike()) {
            return .json;
        }

        if (tag == .NumberObject) {
            return globalObject.ERR(.INVALID_ARG_TYPE, "Number object is ambiguous and cannot be used as a PostgreSQL type", .{}).throw();
        }

        if (tag == .BooleanObject) {
            return globalObject.ERR(.INVALID_ARG_TYPE, "Boolean object is ambiguous and cannot be used as a PostgreSQL type", .{}).throw();
        }

        if (!tag.isIndexable()) {
            return globalObject.ERR(.INVALID_ARG_TYPE, "Unknown object is not a valid PostgreSQL type", .{}).throw();
        }

        if (tag.isObject()) {
            return .json;
        }
    }

    if (value.isInt32()) {
        return .int4;
    }

    if (value.isAnyInt()) {
        const int = value.toInt64();
        if (int >= std.math.minInt(i32) && int <= std.math.maxInt(i32)) {
            return .int4;
        }
        return .int8;
    }

    if (value.isNumber()) {
        return .float8;
    }

    if (value.isBoolean()) {
        return .@"bool";
    }

    return .numeric;
}
