import std from "std";
import bun from "bun";
import types from "../../../sql/mysql/MySQLTypes.zig";
import NewReaderMod from "../../../sql/mysql/protocol/NewReader.zig";
import SQLDataCellMod from "../../shared/SQLDataCell.zig";
import MySQLValueMod from "../MySQLValue.zig";

const NewReader = NewReaderMod.NewReader;
const SQLDataCell = SQLDataCellMod.SQLDataCell;
const Value = MySQLValueMod.Value;
const DateTime = Value.DateTime;
const Time = Value.Time;
const jsc = bun.jsc;
const debug = bun.Output.scoped(.MySQLDecodeBinaryValue, .visible);

pub const binary_charset: u16 = 63;

pub fn decodeBinaryValue(globalObject: *jsc.JSGlobalObject, field_type: types.FieldType, column_length: u32, raw: bool, bigint: bool, unsigned: bool, binary: bool, character_set: u16, comptime Context: type, reader: NewReader(Context)) anyerror!SQLDataCell {
    debug("decodeBinaryValue: {s}", .{@tagName(field_type)});
    return switch (field_type) {
        .MYSQL_TYPE_TINY => {
            if (raw) {
                let data = reader.read(1)?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            const val = reader.byte()?;
            if (unsigned) {
                return SQLDataCell{ tag: .uint4, value: { uint4: val } };
            }
            const ival: i8 = @bitCast(val);
            return SQLDataCell{ tag: .int4, value: { int4: ival } };
        },
        .MYSQL_TYPE_SHORT => {
            if (raw) {
                let data = reader.read(2)?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            if (unsigned) {
                return SQLDataCell{ tag: .uint4, value: { uint4: reader.int(u16)? } };
            }
            return SQLDataCell{ tag: .int4, value: { int4: reader.int(i16)? } };
        },
        .MYSQL_TYPE_INT24 => {
            if (raw) {
                let data = reader.read(3)?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            if (unsigned) {
                return SQLDataCell{ tag: .uint4, value: { uint4: reader.int(u24)? } };
            }
            return SQLDataCell{ tag: .int4, value: { int4: reader.int(i24)? } };
        },
        .MYSQL_TYPE_LONG => {
            if (raw) {
                let data = reader.read(4)?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            if (unsigned) {
                return SQLDataCell{ tag: .uint4, value: { uint4: reader.int(u32)? } };
            }
            return SQLDataCell{ tag: .int4, value: { int4: reader.int(i32)? } };
        },
        .MYSQL_TYPE_LONGLONG => {
            if (raw) {
                const data = reader.read(8)?;
                return SQLDataCell.raw(&data);
            }
            if (unsigned) {
                const val = reader.int(u64)?;
                if (val <= std.math.maxInt(u32)) {
                    return SQLDataCell{ tag: .uint4, value: { uint4: @intCast(val) } };
                }
                if (bigint) {
                    return SQLDataCell{ tag: .uint8, value: { uint8: val } };
                }
                let buffer: [22]u8 = undefined;
                const slice = std.fmt.bufPrint(&buffer, "{d}", .{val}) catch unreachable;
                return SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
            }
            const val = reader.int(i64)?;
            if (val >= std.math.minInt(i32) && val <= std.math.maxInt(i32)) {
                return SQLDataCell{ tag: .int4, value: { int4: @intCast(val) } };
            }
            if (bigint) {
                return SQLDataCell{ tag: .int8, value: { int8: val } };
            }
            let buffer: [22]u8 = undefined;
            const slice = std.fmt.bufPrint(&buffer, "{d}", .{val}) catch unreachable;
            return SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
        },
        .MYSQL_TYPE_FLOAT => {
            if (raw) {
                let data = reader.read(4)?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            const fval: f32 = @bitCast(reader.int(u32)?);
            return SQLDataCell{ tag: .float8, value: { float8: fval } };
        },
        .MYSQL_TYPE_DOUBLE => {
            if (raw) {
                let data = reader.read(8)?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            return SQLDataCell{ tag: .float8, value: { float8: @bitCast(reader.int(u64)?) } };
        },
        .MYSQL_TYPE_TIME => {
            return switch (reader.byte()?) {
                0 => {
                    const slice = "00:00:00";
                    return SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
                },
                8, 12 => |l| {
                    let data = reader.read(l)?;
                    defer data.deinit();
                    const time = Time.fromData(&data)?;

                    const total_hours = time.hours + time.days * 24;
                    let buffer: [32]u8 = undefined;
                    const sign = if (time.negative) "-" else "";
                    const slice = if (total_hours > 99)
                        std.fmt.bufPrint(&buffer, "{s}{d:0>3}:{d:0>2}:{d:0>2}", .{ sign, total_hours, time.minutes, time.seconds }) catch return error.InvalidBinaryValue
                    else
                        std.fmt.bufPrint(&buffer, "{s}{d:0>2}:{d:0>2}:{d:0>2}", .{ sign, total_hours, time.minutes, time.seconds }) catch return error.InvalidBinaryValue;
                    return SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
                },
                else => return error.InvalidBinaryValue,
            };
        },
        .MYSQL_TYPE_DATE, .MYSQL_TYPE_TIMESTAMP, .MYSQL_TYPE_DATETIME => switch (reader.byte()?) {
            0 => SQLDataCell{ tag: .date, value: { date: 0 } },
            11, 7, 4 => |l| {
                let data = reader.read(l)?;
                defer data.deinit();
                const time = DateTime.fromData(&data)?;
                return SQLDataCell{ tag: .date, value: { date: time.toJSTimestamp(globalObject)? } };
            },
            else => error.InvalidBinaryValue,
        },
        .MYSQL_TYPE_ENUM,
        .MYSQL_TYPE_SET,
        .MYSQL_TYPE_GEOMETRY,
        .MYSQL_TYPE_NEWDECIMAL,
        .MYSQL_TYPE_STRING,
        .MYSQL_TYPE_VARCHAR,
        .MYSQL_TYPE_VAR_STRING,
        .MYSQL_TYPE_TINY_BLOB,
        .MYSQL_TYPE_MEDIUM_BLOB,
        .MYSQL_TYPE_LONG_BLOB,
        .MYSQL_TYPE_BLOB => {
            if (raw) {
                let data = reader.encodeLenString()?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            let string_data = reader.encodeLenString()?;
            defer string_data.deinit();
            if (binary && character_set == binary_charset) {
                return SQLDataCell.raw(&string_data);
            }
            const slice = string_data.slice();
            return SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
        },
        .MYSQL_TYPE_JSON => {
            if (raw) {
                let data = reader.encodeLenString()?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
            let string_data = reader.encodeLenString()?;
            defer string_data.deinit();
            const slice = string_data.slice();
            return SQLDataCell{ tag: .json, value: { json: if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
        },
        .MYSQL_TYPE_BIT => {
            if (column_length == 1) {
                let data = reader.encodeLenString()?;
                defer data.deinit();
                const slice = data.slice();
                return SQLDataCell{ tag: .@"bool", value: { @"bool": if (slice.len > 0 && slice[0] == 1) 1 else 0 } };
            } else {
                let data = reader.encodeLenString()?;
                defer data.deinit();
                return SQLDataCell.raw(&data);
            }
        },
        else => {
            let data = reader.read(column_length)?;
            defer data.deinit();
            return SQLDataCell.raw(&data);
        },
    };
}
