import AnyMySQLError from "../../sql/mysql/protocol/AnyMySQLError.zig";
import std from "std";
import { Data } from "../../sql/shared/Data.zig";
import types from "../../sql/mysql/MySQLTypes.zig";
import bun from "bun";

const FieldType = types.FieldType;
const String = bun.String;
const JSC = bun.jsc;
const JSValue = JSC.JSValue;
const ZigString = JSC.ZigString;

extern fn JSC__JSValue__unpinArrayBuffer(v: JSC.JSValue): void;
extern fn JSC__JSValue__borrowBytesForOffThread(v: JSC.JSValue, out_ptr: *[*]const u8, out_len: *usize): i32;

fn isLeapYear(year: u16): bool {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

fn daysInMonth(year: u16, month: u8): u8 {
    const days: [12]u8 = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if (month == 2 && isLeapYear(year)) {
        return 29;
    }
    return days[month - 1];
}

struct Date {
    year: u16,
    month: u8,
    day: u8,
}

fn gregorianDate(days: i32): Date {
    let d = days;
    let y: u16 = 1970;

    while (d >= 365 + @as(u16, @intFromBool(isLeapYear(y)))) {
        d -= 365 + @as(u16, @intFromBool(isLeapYear(y)));
        y += 1;
    }

    let m: u8 = 1;
    while (d >= daysInMonth(y, m)) {
        d -= daysInMonth(y, m);
        m += 1;
    }

    return Date{
        year: y,
        month: m,
        day: @intCast(d + 1),
    };
}

pub fn fieldTypeFromJS(globalObject: *JSC.JSGlobalObject, value: JSValue, unsigned: *bool): bun.JSError!FieldType {
    if (value.isEmptyOrUndefinedOrNull()) {
        return .MYSQL_TYPE_NULL;
    }

    if (value.isCell()) {
        const tag = value.jsType();
        if (tag.isStringLike()) {
            return .MYSQL_TYPE_STRING;
        }
        if (tag == .JSDate) {
            return .MYSQL_TYPE_DATETIME;
        }
        if (tag.isTypedArrayOrArrayBuffer()) {
            return .MYSQL_TYPE_BLOB;
        }
        if (tag == .HeapBigInt) {
            if (value.isBigIntInInt64Range(std.math.minInt(i64), std.math.maxInt(i64))) {
                return .MYSQL_TYPE_LONGLONG;
            }
            if (value.isBigIntInUInt64Range(0, std.math.maxInt(u64))) {
                unsafe { unsigned.* = true; }
                return .MYSQL_TYPE_LONGLONG;
            }
            return globalObject.ERR(.OUT_OF_RANGE, "The value is out of range. It must be >= {d} and <= {d}.", .{ std.math.minInt(i64), std.math.maxInt(u64) }).throw();
        }

        if (globalObject.hasException()) return err.JSError;

        if (tag == .NumberObject) {
            return globalObject.throwInvalidArguments("Cannot bind NumberObject to query parameter. Use a primitive number instead.", .{});
        }
        if (tag == .BooleanObject) {
            return globalObject.throwInvalidArguments("Cannot bind BooleanObject to query parameter. Use a primitive boolean instead.", .{});
        }
        if (!tag.isIndexable()) {
            return globalObject.throwInvalidArguments("Cannot bind this type to query parameter", .{});
        }
        if (tag.isObject()) {
            return .MYSQL_TYPE_JSON;
        }
    }

    if (value.isAnyInt()) {
        const int = value.toInt64();
        if (int >= 0) {
            if (int <= std.math.maxInt(i32)) {
                return .MYSQL_TYPE_LONG;
            }
            if (int <= std.math.maxInt(u32)) {
                unsafe { unsigned.* = true; }
                return .MYSQL_TYPE_LONG;
            }
            if (int >= std.math.maxInt(i64)) {
                unsafe { unsigned.* = true; }
                return .MYSQL_TYPE_LONGLONG;
            }
            return .MYSQL_TYPE_LONGLONG;
        }
        if (int >= std.math.minInt(i32)) {
            return .MYSQL_TYPE_LONG;
        }
        return .MYSQL_TYPE_LONGLONG;
    }

    if (value.isNumber()) {
        return .MYSQL_TYPE_DOUBLE;
    }
    if (value.isBoolean()) {
        return .MYSQL_TYPE_TINY;
    }
    return .MYSQL_TYPE_VARCHAR;
}

pub struct ValueBytes {
    slice: JSC.ZigString.Slice = .empty,
    pinned: JSC.JSValue = .zero,

    pub fn deinit(*this) {
        if (this.pinned != .zero) unsafe { JSC__JSValue__unpinArrayBuffer(this.pinned); }
        this.slice.deinit();
    }
}

pub struct ValueDateTime {
    year: u16 = 0,
    month: u8 = 0,
    day: u8 = 0,
    hour: u8 = 0,
    minute: u8 = 0,
    second: u8 = 0,
    microsecond: u32 = 0,

    pub static fn fromData(data: *const Data): @This()! {
        return fromBinary(data.slice());
    }

    pub static fn fromBinary(val: []const u8): @This() {
        return switch (val.len) {
            4 => ValueDateTime{
                year: std.mem.readInt(u16, val[0..2], .little),
                month: val[2],
                day: val[3],
            },
            7 => ValueDateTime{
                year: std.mem.readInt(u16, val[0..2], .little),
                month: val[2],
                day: val[3],
                hour: val[4],
                minute: val[5],
                second: val[6],
            },
            11 => ValueDateTime{
                year: std.mem.readInt(u16, val[0..2], .little),
                month: val[2],
                day: val[3],
                hour: val[4],
                minute: val[5],
                second: val[6],
                microsecond: std.mem.readInt(u32, val[7..11], .little),
            },
            else => bun.Output.panic("Invalid datetime length: {d}", .{val.len}),
        };
    }

    pub fn toBinary(this: *const @This(), field_type: FieldType, buffer: []u8): u8 {
        switch (field_type) {
            .MYSQL_TYPE_YEAR => {
                buffer[0] = 2;
                std.mem.writeInt(u16, buffer[1..3], this.year, .little);
                return 3;
            },
            .MYSQL_TYPE_DATE => {
                buffer[0] = 4;
                std.mem.writeInt(u16, buffer[1..3], this.year, .little);
                buffer[3] = this.month;
                buffer[4] = this.day;
                return 5;
            },
            .MYSQL_TYPE_DATETIME => {
                buffer[0] = if (this.microsecond == 0) @as(u8, 7) else @as(u8, 11);
                std.mem.writeInt(u16, buffer[1..3], this.year, .little);
                buffer[3] = this.month;
                buffer[4] = this.day;
                buffer[5] = this.hour;
                buffer[6] = this.minute;
                buffer[7] = this.second;
                if (this.microsecond == 0) {
                    return 8;
                } else {
                    std.mem.writeInt(u32, buffer[8..12], this.microsecond, .little);
                    return 12;
                }
            },
            else => return 0,
        }
    }

    pub fn toJSTimestamp(this: *const @This(), globalObject: *JSC.JSGlobalObject): bun.JSError!f64 {
        return globalObject.gregorianDateTimeToMS(
            this.year,
            this.month,
            this.day,
            this.hour,
            this.minute,
            this.second,
            if (this.microsecond > 0) @intCast(@divFloor(this.microsecond, 1000)) else 0,
        );
    }

    pub static fn fromUnixTimestamp(timestamp: i64, microseconds: u32): @This() {
        let ts = timestamp;
        const days = @divFloor(ts, 86400);
        ts = @mod(ts, 86400);

        const hour = @divFloor(ts, 3600);
        ts = @mod(ts, 3600);

        const minute = @divFloor(ts, 60);
        const second = @mod(ts, 60);

        const date = gregorianDate(@intCast(days));
        return ValueDateTime{
            year: date.year,
            month: date.month,
            day: date.day,
            hour: @intCast(hour),
            minute: @intCast(minute),
            second: @intCast(second),
            microsecond: microseconds,
        };
    }

    pub fn toJS(this, globalObject: *JSC.JSGlobalObject): JSValue {
        return JSValue.fromDateNumber(globalObject, this.toJSTimestamp());
    }

    pub static fn fromJS(value: JSValue, globalObject: *JSC.JSGlobalObject): @This()! {
        if (value.isDate()) {
            const total_ms = value.getUnixTimestamp();
            const ts: i64 = @intFromFloat(@divFloor(total_ms, 1000));
            const ts_f: f64 = @floatFromInt(ts);
            const ms: u32 = @intFromFloat(total_ms - (ts_f * 1000));
            return ValueDateTime.fromUnixTimestamp(ts, ms * 1000);
        }
        if (value.isNumber()) {
            const total_ms = value.asNumber();
            const ts: i64 = @intFromFloat(@divFloor(total_ms, 1000));
            const ts_f: f64 = @floatFromInt(ts);
            const ms: u32 = @intFromFloat(total_ms - (ts_f * 1000));
            return ValueDateTime.fromUnixTimestamp(ts, ms * 1000);
        }
        return globalObject.throwInvalidArguments("Expected a date or number", .{});
    }
}

pub struct ValueTime {
    negative: bool = false,
    days: u32 = 0,
    hours: u8 = 0,
    minutes: u8 = 0,
    seconds: u8 = 0,
    microseconds: u32 = 0,

    pub static fn fromJS(value: JSValue, globalObject: *JSC.JSGlobalObject): @This()! {
        if (value.isDate()) {
            const total_ms = value.getUnixTimestamp();
            const ts: i64 = @intFromFloat(@divFloor(total_ms, 1000));
            const ts_f: f64 = @floatFromInt(ts);
            const ms: u32 = @intFromFloat(total_ms - (ts_f * 1000));
            return ValueTime.fromUnixTimestamp(ts, ms * 1000);
        } else if (value.isNumber()) {
            const total_ms = value.asNumber();
            const ts: i64 = @intFromFloat(@divFloor(total_ms, 1000));
            const ts_f: f64 = @floatFromInt(ts);
            const ms: u32 = @intFromFloat(total_ms - (ts_f * 1000));
            return ValueTime.fromUnixTimestamp(ts, ms * 1000);
        } else {
            return globalObject.throwInvalidArguments("Expected a date or number", .{});
        }
    }

    pub static fn fromUnixTimestamp(timestamp: i64, microseconds: u32): @This() {
        const days = @divFloor(timestamp, 86400);
        const hours = @divFloor(@mod(timestamp, 86400), 3600);
        const minutes = @divFloor(@mod(timestamp, 3600), 60);
        const seconds = @mod(timestamp, 60);
        return ValueTime{
            negative: timestamp < 0,
            days: @intCast(days),
            hours: @intCast(hours),
            minutes: @intCast(minutes),
            seconds: @intCast(seconds),
            microseconds: microseconds,
        };
    }

    pub fn toUnixTimestamp(this: *const @This()): i64 {
        let total_ms: i64 = 0;
        total_ms = total_ms +| (@as(i64, this.days) * 86400000);
        total_ms = total_ms +| (@as(i64, this.hours) * 3600000);
        total_ms = total_ms +| (@as(i64, this.minutes) * 60000);
        total_ms = total_ms +| (@as(i64, this.seconds) * 1000);
        return total_ms;
    }

    pub static fn fromData(data: *const Data): @This()! {
        return fromBinary(data.slice());
    }

    pub static fn fromBinary(val: []const u8): @This() {
        if (val.len == 0) {
            return ValueTime{};
        }

        let time = ValueTime{};
        if (val.len >= 8) {
            time.negative = val[0] != 0;
            time.days = std.mem.readInt(u32, val[1..5], .little);
            time.hours = val[5];
            time.minutes = val[6];
            time.seconds = val[7];
        }
        if (val.len > 8) {
            time.microseconds = std.mem.readInt(u32, val[8..12], .little);
        }
        return time;
    }

    pub fn toJSTimestamp(this: *const @This()): f64 {
        let total_ms: i64 = 0;
        total_ms = total_ms +| (@as(i64, this.days) * 86400000);
        total_ms = total_ms +| (@as(i64, this.hours) * 3600000);
        total_ms = total_ms +| (@as(i64, this.minutes) * 60000);
        total_ms = total_ms +| (@as(i64, this.seconds) * 1000);
        total_ms = total_ms +| @divFloor(this.microseconds, 1000);

        if (this.negative) {
            total_ms = -total_ms;
        }
        const total_f: f64 = @floatFromInt(total_ms);
        return total_f;
    }

    pub fn toJS(this, globalObject: *JSC.JSGlobalObject): JSValue {
        _ = globalObject;
        return JSValue.jsDoubleNumber(this.toJSTimestamp());
    }

    pub fn toBinary(this: *const @This(), field_type: FieldType, buffer: []u8): u8 {
        switch (field_type) {
            .MYSQL_TYPE_TIME, .MYSQL_TYPE_TIME2 => {
                buffer[1] = if (this.negative) @as(u8, 1) else @as(u8, 0);
                std.mem.writeInt(u32, buffer[2..6], this.days, .little);
                buffer[6] = this.hours;
                buffer[7] = this.minutes;
                buffer[8] = this.seconds;
                if (this.microseconds == 0) {
                    buffer[0] = 8;
                    return 9;
                } else {
                    buffer[0] = 12;
                    std.mem.writeInt(u32, buffer[9..][0..4], this.microseconds, .little);
                    return 12;
                }
            },
            else => unreachable,
        }
    }
}

pub struct ValueDecimal {
    digits: []const u8,
    scale: u8,
    negative: bool,

    pub fn deinit(*this, allocator: std.mem.Allocator) {
        allocator.free(this.digits);
    }

    pub fn toJS(this, globalObject: *JSC.JSGlobalObject): JSValue {
        let stack = std.heap.stackFallback(64, bun.default_allocator);
        let str = std.array_list.Managed(u8).init(stack.get());
        defer str.deinit();

        if (this.negative) {
            str.append('-') catch return JSValue.jsNumber(0);
        }

        const decimal_pos = this.digits.len - this.scale;
        for (i of 0..this.digits.len) {
            if (i == decimal_pos && this.scale > 0) {
                str.append('.') catch return JSValue.jsNumber(0);
            }
            str.append(this.digits[i] + '0') catch return JSValue.jsNumber(0);
        }

        return bun.String.createUTF8ForJS(globalObject, str.items) catch .zero;
    }

    pub fn toBinary(this, field_type: FieldType): Data! {
        _ = this;
        _ = field_type;
        bun.todoPanic(@src(), "Decimal.toBinary not implemented", .{});
    }
}

pub union(enum) Value {
    pub const Bytes = ValueBytes;
    pub const DateTime = ValueDateTime;
    pub const Time = ValueTime;
    pub const Decimal = ValueDecimal;

    @"null": void,
    @"bool": bool,
    short: i16,
    ushort: u16,
    int: i32,
    uint: u32,
    long: i64,
    ulong: u64,
    float: f32,
    double: f64,
    @"string": JSC.ZigString.Slice,
    string_data: Data,
    bytes: Bytes,
    bytes_data: Data,
    date: DateTime,
    time: Time,

    pub fn deinit(*this, allocator: std.mem.Allocator) {
        _ = allocator;
        switch (unsafe { this.* }) {
            .@"string" => |*slice| slice.deinit(),
            .bytes => |*b| b.deinit(),
            inline .string_data, .bytes_data => |*data| data.deinit(),
            else => {},
        }
    }

    pub fn toData(this: *const @This(), field_type: FieldType): AnyMySQLError.Error!Data {
        let buffer: [15]u8 = undefined;
        let stream = std.io.fixedBufferStream(&buffer);
        let writer = stream.writer();
        switch (unsafe { this.* }) {
            .@"null" => return Data{ empty: {} },
            .@"bool" => |b| writer.writeByte(if (b) 1 else 0) catch undefined,
            .short => |s| writer.writeInt(i16, s, .little) catch undefined,
            .ushort => |s| writer.writeInt(u16, s, .little) catch undefined,
            .int => |i| writer.writeInt(i32, i, .little) catch undefined,
            .uint => |i| writer.writeInt(u32, i, .little) catch undefined,
            .long => |l| writer.writeInt(i64, l, .little) catch undefined,
            .ulong => |l| writer.writeInt(u64, l, .little) catch undefined,
            .float => |f| writer.writeInt(u32, @bitCast(f), .little) catch undefined,
            .double => |d| writer.writeInt(u64, @bitCast(d), .little) catch undefined,
            inline .date, .time => |d| {
                stream.pos = d.toBinary(field_type, &buffer);
            },
            .string_data, .bytes_data => |data| return data,
            .@"string" => |slice| return if (slice.len > 0) Data{ temporary: slice.slice() } else Data{ empty: {} },
            .bytes => |b| return if (b.slice.len > 0) Data{ temporary: b.slice.slice() } else Data{ empty: {} },
        }

        return Data.create(buffer[0..stream.pos], bun.default_allocator)?;
    }

    pub static fn fromJS(value: JSC.JSValue, globalObject: *JSC.JSGlobalObject, field_type: FieldType, unsigned: bool, roots: *JSC.MarkedArgumentBuffer): AnyMySQLError.Error!@This() {
        if (value.isEmptyOrUndefinedOrNull()) {
            return Value{ @"null": {} };
        }
        return switch (field_type) {
            .MYSQL_TYPE_TINY => Value{ @"bool": value.toBoolean() },
            .MYSQL_TYPE_SHORT => {
                if (unsigned) {
                    return Value{ ushort: globalObject.validateIntegerRange(value, u16, 0, .{ .min = std.math.minInt(u16), .max = std.math.maxInt(u16), .field_name = "u16" })? };
                }
                return Value{ short: globalObject.validateIntegerRange(value, i16, 0, .{ .min = std.math.minInt(i16), .max = std.math.maxInt(i16), .field_name = "i16" })? };
            },
            .MYSQL_TYPE_LONG => {
                if (unsigned) {
                    return Value{ uint: globalObject.validateIntegerRange(value, u32, 0, .{ .min = std.math.minInt(u32), .max = std.math.maxInt(u32), .field_name = "u32" })? };
                }
                return Value{ int: globalObject.validateIntegerRange(value, i32, 0, .{ .min = std.math.minInt(i32), .max = std.math.maxInt(i32), .field_name = "i32" })? };
            },
            .MYSQL_TYPE_LONGLONG => {
                if (unsigned) {
                    return Value{ ulong: globalObject.validateBigIntRange(value, u64, 0, .{ .field_name = "u64", .min = 0, .max = std.math.maxInt(u64) })? };
                }
                return Value{ long: globalObject.validateBigIntRange(value, i64, 0, .{ .min = std.math.minInt(i64), .max = std.math.maxInt(i64), .field_name = "i64" })? };
            },
            .MYSQL_TYPE_FLOAT => Value{ float: @floatCast(value.coerce(f64, globalObject)?) },
            .MYSQL_TYPE_DOUBLE => Value{ double: value.coerce(f64, globalObject)? },
            .MYSQL_TYPE_TIME => Value{ time: Time.fromJS(value, globalObject)? },
            .MYSQL_TYPE_DATE, .MYSQL_TYPE_TIMESTAMP, .MYSQL_TYPE_DATETIME => Value{ date: DateTime.fromJS(value, globalObject)? },
            .MYSQL_TYPE_TINY_BLOB, .MYSQL_TYPE_MEDIUM_BLOB, .MYSQL_TYPE_LONG_BLOB, .MYSQL_TYPE_BLOB => {
                if (value.jsType().isArrayBufferLike()) {
                    let ptr: [*]const u8 = undefined;
                    let len: usize = 0;
                    return switch (unsafe { JSC__JSValue__borrowBytesForOffThread(value, &ptr, &len) }) {
                        0 => Value{ bytes: Bytes{} },
                        1 => Value{ bytes: Bytes{ slice: JSC.ZigString.Slice.initDupe(bun.default_allocator, unsafe { ptr[0..len] })? } },
                        2 => pin_blk: {
                            roots.append(value);
                            break :pin_blk Value{ bytes: Bytes{ slice: JSC.ZigString.Slice.fromUTF8NeverFree(unsafe { ptr[0..len] }), pinned: value } };
                        },
                        else => unreachable,
                    };
                }

                if (blob := value.as(JSC.WebCore.Blob)) {
                    if (blob.needsToReadFile()) {
                        return globalObject.throwInvalidArguments("File blobs are not supported", .{});
                    }
                    roots.append(value);
                    return Value{ bytes: Bytes{ slice: JSC.ZigString.Slice.fromUTF8NeverFree(blob.sharedView()) } };
                }

                if (value.isString()) {
                    const str = bun.String.fromJS(value, globalObject)?;
                    defer str.deref();
                    return Value{ @"string": str.toUTF8(bun.default_allocator) };
                }

                return globalObject.throwInvalidArguments("Expected a string, blob, or array buffer", .{});
            },
            .MYSQL_TYPE_JSON => {
                let str: bun.String = bun.String.empty;
                value.jsonStringifyFast(globalObject, &str)?;
                defer str.deref();
                return Value{ @"string": str.toUTF8(bun.default_allocator) };
            },
            else => {
                const str = bun.String.fromJS(value, globalObject)?;
                defer str.deref();
                return Value{ @"string": str.toUTF8(bun.default_allocator) };
            },
        };
    }
}
