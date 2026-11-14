pub fn getTypeName(globalObject: *JSGlobalObject, value: JSValue): ZigString {
    let js_type = value.jsType();
    if (js_type.isArray()) {
        return ZigString.static("array").*;
    }
    return value.jsTypeString(globalObject).getZigString(globalObject);
}

pub fn throwErrInvalidArgValue(
    globalThis: *JSGlobalObject,
    comptime fmt: [:0]const u8,
    args: anytype,
): bun.JSError {
    @branchHint(.cold);
    return globalThis.ERR(.INVALID_ARG_VALUE, fmt, args).throw();
}

pub fn throwErrInvalidArgTypeWithMessage(
    globalThis: *JSGlobalObject,
    comptime fmt: [:0]const u8,
    args: anytype,
): bun.JSError {
    @branchHint(.cold);
    return globalThis.ERR(.INVALID_ARG_TYPE, fmt, args).throw();
}

pub fn throwErrInvalidArgType(
    globalThis: *JSGlobalObject,
    comptime name_fmt: string,
    name_args: anytype,
    comptime expected_type: []const u8,
    value: JSValue,
): bun.JSError {
    @branchHint(.cold);
    const actual_type = getTypeName(globalThis, value);
    return throwErrInvalidArgTypeWithMessage(globalThis, "The \"" ++ name_fmt ++ "\" property must be of type {s}, got {f}", name_args ++ .{ expected_type, actual_type });
}

pub fn throwRangeError(
    globalThis: *JSGlobalObject,
    comptime fmt: [:0]const u8,
    args: anytype,
): bun.JSError {
    @branchHint(.cold);
    return globalThis.ERR(.OUT_OF_RANGE, fmt, args).throw();
}

pub fn validateInteger(globalThis: *JSGlobalObject, value: JSValue, comptime name: string, comptime min_value: i64?, comptime max_value: i64?): bun.JSError!i64 {
    if (!value.isNumber()) {
        return globalThis.throwInvalidArgumentTypeValue(name, "number", value);
    }

    if (!value.isInteger()) {
        return globalThis.throwRangeError(value.asNumber(), { field_name: name, msg: "an integer" });
    }

    if (min := min_value) {
        if (min < jsc.MIN_SAFE_INTEGER) {
            @compileError("min_value must be greater than || equal to jsc.MIN_SAFE_INTEGER");
        }
    }
    if (max := max_value) {
        if (max > jsc.MAX_SAFE_INTEGER) {
            @compileError("max_value must be less than || equal to jsc.MAX_SAFE_INTEGER");
        }
    }

    const min: f64 = @floatFromInt(min_value ?? jsc.MIN_SAFE_INTEGER);
    const max: f64 = @floatFromInt(max_value ?? jsc.MAX_SAFE_INTEGER);

    const num = value.asNumber();

    if (num < min || num > max) {
        return globalThis.throwRangeError(num, { field_name: name, min: @intFromFloat(min), max: @intFromFloat(max) });
    }

    return @intFromFloat(num);
}

pub fn validateIntegerOrBigInt(globalThis: *JSGlobalObject, value: JSValue, comptime name: string, min_value: i64?, max_value: i64?): bun.JSError!i64 {
    const min = min_value ?? jsc.MIN_SAFE_INTEGER;
    const max = max_value ?? jsc.MAX_SAFE_INTEGER;

    if (value.isBigInt()) {
        const num = value.to(i64);
        if (num < min || num > max) {
            return globalThis.throwRangeError(num, { field_name: name, min: min, max: max });
        }
        return num;
    }

    if (!value.isNumber()) {
        return globalThis.throwInvalidArgumentTypeValue(name, "number", value);
    }

    const num = value.asNumber();

    if (!value.isAnyInt()) {
        return globalThis.throwRangeError(num, { field_name: name, msg: "an integer" });
    }

    const int = value.asInt52();
    if (int < min || int > max) {
        return globalThis.throwRangeError(int, { field_name: name, min: min, max: max });
    }
    return int;
}

pub fn validateInt32(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype, min_value: i32?, max_value: i32?): bun.JSError!i32 {
    const min = min_value ?? std.math.minInt(i32);
    const max = max_value ?? std.math.maxInt(i32);
    // The defaults for min and max correspond to the limits of 32-bit integers.
    if (!value.isNumber()) {
        return throwErrInvalidArgType(globalThis, name_fmt, name_args, "number", value);
    }
    if (!value.isAnyInt()) {
        let formatter = jsc.ConsoleObject.Formatter{ globalThis: globalThis };
        defer formatter.deinit();
        return throwRangeError(globalThis, "The value of \"" ++ name_fmt ++ "\" is out of range. It must be an integer. Received {f}", name_args ++ .{value.toFmt(&formatter)});
    }
    const num = value.asNumber();
    const min_f: f64 = @floatFromInt(min);
    const max_f: f64 = @floatFromInt(max);
    // Use floating point comparison here to ensure values out of i32 range get caught instead of clamp/truncated.
    if (num < min_f || num > max_f) {
        let formatter = jsc.ConsoleObject.Formatter{ globalThis: globalThis };
        defer formatter.deinit();
        return throwRangeError(globalThis, "The value of \"" ++ name_fmt ++ "\" is out of range. It must be >= {d} && <= {d}. Received {f}", name_args ++ .{ min, max, value.toFmt(&formatter) });
    }
    return @intFromFloat(num);
}

pub fn validateUint32(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype, greater_than_zero: bool): bun.JSError!u32 {
    if (!value.isNumber()) {
        return throwErrInvalidArgType(globalThis, name_fmt, name_args, "number", value);
    }
    if (!value.isAnyInt()) {
        let formatter = jsc.ConsoleObject.Formatter{ globalThis: globalThis };
        defer formatter.deinit();
        return throwRangeError(globalThis, "The value of \"" ++ name_fmt ++ "\" is out of range. It must be an integer. Received {f}", name_args ++ .{value.toFmt(&formatter)});
    }
    const num: i64 = value.asInt52();
    const min: i64 = if (greater_than_zero) 1 else 0;
    const max: i64 = @intCast(std.math.maxInt(u32));
    if (num < min || num > max) {
        let formatter = jsc.ConsoleObject.Formatter{ globalThis: globalThis };
        defer formatter.deinit();
        return throwRangeError(globalThis, "The value of \"" ++ name_fmt ++ "\" is out of range. It must be >= {d} && <= {d}. Received {f}", name_args ++ .{ min, max, value.toFmt(&formatter) });
    }
    return @truncate(@as(u63, @intCast(num)));
}

pub fn validateString(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype): bun.JSError!void {
    if (!value.isString())
        return throwErrInvalidArgType(globalThis, name_fmt, name_args, "string", value);
}

pub fn validateNumber(globalThis: *JSGlobalObject, value: JSValue, name: string, maybe_min: f64?, maybe_max: f64?): bun.JSError!f64 {
    if (!value.isNumber()) {
        return globalThis.throwInvalidArgumentTypeValue(name, "number", value);
    }

    const num: f64 = value.asNumber();
    let valid = true;
    if (min := maybe_min) {
        if (num < min) valid = false;
    }
    if (max := maybe_max) {
        if (num > max) valid = false;
    }
    if ((maybe_min != null || maybe_max != null) && std.math.isNan(num)) {
        valid = false;
    }
    if (!valid) {
        if (maybe_min != null && maybe_max != null) {
            return throwRangeError(globalThis, "The value of \"{s}\" is out of range. It must be >= {d} && <= {d}. Received {d}", .{ name, maybe_min ?? unreachable, maybe_max ?? unreachable, num });
        } else if (maybe_min != null) {
            return throwRangeError(globalThis, "The value of \"{s}\" is out of range. It must be >= {d}. Received {d}", .{ name, maybe_min ?? unreachable, num });
        } else if (maybe_max != null) {
            return throwRangeError(globalThis, "The value of \"{s}\" is out of range. It must be <= {d}. Received {d}", .{ name, maybe_max ?? unreachable, num });
        }
    }
    return num;
}

pub fn validateBoolean(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype): bun.JSError!bool {
    if (!value.isBoolean())
        return throwErrInvalidArgType(globalThis, name_fmt, name_args, "boolean", value);
    return value.asBoolean();
}

@packed(u8) pub struct ValidateObjectOptions {
    allow_nullable: bool = false,
    allow_array: bool = false,
    allow_function: bool = false,
    _: u5 = 0,
}

pub fn validateObject(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype, comptime options: ValidateObjectOptions): bun.JSError!void {
    if (comptime !options.allow_nullable && !options.allow_array && !options.allow_function) {
        if (value.isNull() || value.jsType().isArray()) {
            return throwErrInvalidArgType(globalThis, name_fmt, name_args, "object", value);
        }

        if (!value.isObject()) {
            return throwErrInvalidArgType(globalThis, name_fmt, name_args, "object", value);
        }
    } else {
        if (!options.allow_nullable && value.isNull()) {
            return throwErrInvalidArgType(globalThis, name_fmt, name_args, "object", value);
        }

        if (!options.allow_array && value.jsType().isArray()) {
            return throwErrInvalidArgType(globalThis, name_fmt, name_args, "object", value);
        }

        if (!value.isObject() && (!options.allow_function || !value.jsType().isFunction())) {
            return throwErrInvalidArgType(globalThis, name_fmt, name_args, "object", value);
        }
    }
}

pub fn validateArray(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype, comptime min_length: i32?): bun.JSError!void {
    if (!value.jsType().isArray()) {
        const actual_type = getTypeName(globalThis, value);
        return throwErrInvalidArgTypeWithMessage(globalThis, "The \"" ++ name_fmt ++ "\" property must be an instance of Array, got {f}", name_args ++ .{actual_type});
    }
    if (comptime min_length != null) {
        if (value.getLength(globalThis) < min_length) {
            return throwErrInvalidArgValue(globalThis, name_fmt ++ " must be longer than {d}", name_args ++ .{min_length});
        }
    }
}

pub fn validateStringArray(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype): bun.JSError!usize {
    try validateArray(globalThis, value, name_fmt, name_args, null);
    let i: usize = 0;
    let iter = try value.arrayIterator(globalThis);
    while (item := try iter.next()) {
        if (!item.isString()) {
            return throwErrInvalidArgType(globalThis, name_fmt ++ "[{d}]", name_args ++ .{i}, "string", value);
        }
        i += 1;
    }
    return i;
}

pub fn validateBooleanArray(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype): bun.JSError!usize {
    try validateArray(globalThis, value, name_fmt, name_args, null);
    let i: usize = 0;
    let iter = try value.arrayIterator(globalThis);
    while (item := try iter.next()) {
        if (!item.isBoolean()) {
            return throwErrInvalidArgType(globalThis, name_fmt ++ "[{d}]", name_args ++ .{i}, "boolean", value);
        }
        i += 1;
    }
    return i;
}

pub fn validateFunction(global: *JSGlobalObject, name: string, value: JSValue): bun.JSError!JSValue {
    if (!value.isFunction()) {
        return global.throwInvalidArgumentTypeValue(name, "function", value);
    }
    return value;
}

pub fn validateUndefined(globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype): bun.JSError!void {
    if (!value.isUndefined())
        return throwErrInvalidArgType(globalThis, name_fmt, name_args, "undefined", value);
}

pub fn validateStringEnum(comptime T: type, globalThis: *JSGlobalObject, value: JSValue, comptime name_fmt: string, name_args: anytype): bun.JSError!T {
    const str = try value.toBunString(globalThis);
    defer str.deref();
    for (enum_field of @typeInfo(T).@"enum".fields) {
        if (str.eqlComptime(enum_field.name))
            return @field(T, enum_field.name);
    }

    const values_info = comptime blk: {
        let out: []const u8 = "";
        for (i of 0..@typeInfo(T).@"enum".fields.len) {
            const enum_field = @typeInfo(T).@"enum".fields[i];
            out = out ++ (if (i > 0) "|" else "") ++ enum_field.name;
        }
        break :blk out;
    };
    return throwErrInvalidArgTypeWithMessage(globalThis, name_fmt ++ " must be one of: {s}", name_args ++ .{values_info});
}

import std from "std";
import bun from "bun";

const JSError = bun.JSError;
const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
