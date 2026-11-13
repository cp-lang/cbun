import std from "std";
import bun from "bun";
import SQLDataCellMod from "../shared/SQLDataCell.zig";
import PostgresCachedStructure from "../shared/CachedStructure.zig";
import protocol from "../../sql/postgres/PostgresProtocol.zig";
import DataMod from "../../sql/shared/Data.zig";
import SQLQueryResultModeMod from "../../sql/shared/SQLQueryResultMode.zig";
import pg_types from "../../sql/postgres/PostgresTypes.zig";

pub const SQLDataCell = SQLDataCellMod.SQLDataCell;
const Data = DataMod.Data;
const PostgresSQLQueryResultMode = SQLQueryResultModeMod.SQLQueryResultMode;
const AnyPostgresError = pg_types.AnyPostgresError;
const int4 = pg_types.int4;
const short = pg_types.short;
const PgTag = pg_types.Tag;
const String = bun.String;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const CellList = std.ArrayListUnmanaged(SQLDataCell);
const ByteList = std.array_list.Managed(u8);
const debug = bun.Output.scoped(.Postgres, .visible);
const log = bun.Output.scoped(.PostgresDataCell, .visible);

extern fn Postgres__formatTime(microseconds: i64, buffer: [*]u8, bufferSize: usize) usize;
extern fn Postgres__formatTimeTz(microseconds: i64, tzOffsetSeconds: i32, buffer: [*]u8, bufferSize: usize) usize;

fn parseBytea(hex: []const u8) anyerror!SQLDataCell {
    const len = hex.len / 2;
    const buf = bun.default_allocator.alloc(u8, len)?;
    errdefer bun.default_allocator.free(buf);

    return SQLDataCell{
        tag: .bytea,
        value: {
            bytea: [
                @intFromPtr(buf.ptr),
                bun.strings.decodeHexToBytes(buf, u8, hex)?,
            ],
        },
        free_value: 1,
    };
}

fn unescapePostgresString(input: []const u8, buffer: []u8) anyerror![]u8 {
    let buf = buffer[0..];
    let out_index: usize = 0;
    let i: usize = 0;

    while (i < input.len) {
        if (out_index >= buf.len) return error.BufferTooSmall;

        if (input[i] == '\\' && i + 1 < input.len) {
            i += 1;
            switch (input[i]) {
                'b' => buf[out_index] = 0x08,
                'f' => buf[out_index] = 0x0C,
                'n' => buf[out_index] = '\n',
                'r' => buf[out_index] = '\r',
                't' => buf[out_index] = '\t',
                '"' => buf[out_index] = '"',
                '\\' => buf[out_index] = '\\',
                '\'' => buf[out_index] = '\'',
                '/' => buf[out_index] = '/',
                'x' => {
                    if (i + 2 >= input.len) return error.InvalidEscapeSequence;
                    const hex_value = std.fmt.parseInt(u8, input[i + 1 .. i + 3], 16)?;
                    buf[out_index] = hex_value;
                    i += 2;
                },
                else => return error.UnknownEscapeSequence,
            }
        } else {
            buf[out_index] = input[i];
        }
        out_index += 1;
        i += 1;
    }

    return buf[0..out_index];
}

fn parseArray(bytes: []const u8, bigint: bool, arrayType: PgTag, globalObject: *jsc.JSGlobalObject, offset: (*usize)?, comptime is_json_sub_array: bool) anyerror!SQLDataCell {
    const closing_brace = if (is_json_sub_array) ']' else '}';
    const opening_brace = if (is_json_sub_array) '[' else '{';
    if (bytes.len < 2 || bytes[0] != opening_brace) {
        return error.UnsupportedArrayFormat;
    }
    if (bytes.len == 2 && bytes[1] == closing_brace) {
        if (offset_ptr := offset) {
            unsafe {
                offset_ptr.* = 2;
            }
        }
        return SQLDataCell{ tag: .array, value: { array: { ptr: null, len: 0, cap: 0 } } };
    }

    let array = CellList{};
    let stack_buffer: [16384]u8 = undefined;

    errdefer {
        let idx: usize = 0;
        while (idx < array.items.len) {
            array.items[idx].deinit();
            idx += 1;
        }
        if (array.capacity > 0) array.deinit(bun.default_allocator);
    }
    let pos: usize = 1;
    let reached_end = false;
    const separator = switch (arrayType) {
        .box_array => ';',
        else => ',',
    };
    while (pos < bytes.len) {
        const slice = bytes[pos..];
        if (slice[0] == closing_brace) {
            if (reached_end) {
                return error.UnsupportedArrayFormat;
            }
            reached_end = true;
            pos += 1;
            break;
        }
        if (slice[0] == opening_brace) {
            let sub_array_offset: usize = 0;
            let sub_array = parseArray(slice, bigint, arrayType, globalObject, &sub_array_offset, is_json_sub_array)?;
            errdefer sub_array.deinit();
            array.append(bun.default_allocator, sub_array)?;
            pos += sub_array_offset;
            continue;
        }
        if (slice[0] == separator) {
            pos += 1;
            continue;
        }
        switch (slice[0]) {
            '"' => {
                let current_idx: usize = 0;
                const source = slice[1..];
                let is_escaped = false;
                let index: usize = 0;
                while (index < source.len) {
                    const byte = source[index];
                    if (byte == '"' && !is_escaped) {
                        current_idx = index + 1;
                        break;
                    }
                    is_escaped = !is_escaped && byte == '\\';
                    index += 1;
                }
                if (current_idx == 0) return error.UnsupportedArrayFormat;
                switch (arrayType) {
                    .bytea_array => {
                        const bytea_bytes = slice[1..current_idx];
                        if (bun.strings.startsWith(bytea_bytes, "\\\\x")) {
                            array.append(bun.default_allocator, parseBytea(bytea_bytes[3..][0 .. bytea_bytes.len - 3])?)?;
                            pos += current_idx + 1;
                            continue;
                        }
                        return error.UnsupportedByteaFormat;
                    },
                    .timestamptz_array,
                    .timestamp_array,
                    .date_array => {
                        const date_str = slice[1..current_idx];
                        let str = bun.String.init(date_str);
                        defer str.deref();
                        array.append(bun.default_allocator, SQLDataCell{ tag: .date, value: { date: str.parseDate(globalObject)? } })?;
                        pos += current_idx + 1;
                        continue;
                    },
                    .json_array,
                    .jsonb_array => {
                        const str_bytes = slice[1..current_idx];
                        const needs_dynamic_buffer = str_bytes.len > stack_buffer.len;
                        const buffer = if (needs_dynamic_buffer) bun.default_allocator.alloc(u8, str_bytes.len)? else stack_buffer[0..str_bytes.len];
                        defer if (needs_dynamic_buffer) bun.default_allocator.free(buffer);
                        const unescaped = unescapePostgresString(str_bytes, buffer) catch return error.InvalidByteSequence;
                        array.append(bun.default_allocator, SQLDataCell{ tag: .json, value: { json: if (unescaped.len > 0) String.cloneUTF8(unescaped).value.WTFStringImpl else null }, free_value: 1 })?;
                        pos += current_idx + 1;
                        continue;
                    },
                    else => {},
                }
                const str_bytes = slice[1..current_idx];
                if (str_bytes.len == 0) {
                    array.append(bun.default_allocator, SQLDataCell{ tag: .@"string", value: { @"string": null }, free_value: 1 })?;
                    pos += current_idx + 1;
                    continue;
                }
                const needs_dynamic_buffer = str_bytes.len > stack_buffer.len;
                const buffer = if (needs_dynamic_buffer) bun.default_allocator.alloc(u8, str_bytes.len)? else stack_buffer[0..str_bytes.len];
                defer if (needs_dynamic_buffer) bun.default_allocator.free(buffer);
                const string_bytes = unescapePostgresString(str_bytes, buffer) catch return error.InvalidByteSequence;
                array.append(bun.default_allocator, SQLDataCell{ tag: .@"string", value: { @"string": if (string_bytes.len > 0) String.cloneUTF8(string_bytes).value.WTFStringImpl else null }, free_value: 1 })?;

                pos += current_idx + 1;
                continue;
            },
            else => {
                switch (arrayType) {
                    .timetz_array,
                    .date_array,
                    .time_array,
                    .interval_array,
                    .bpchar_array,
                    .varchar_array,
                    .char_array,
                    .text_array,
                    .name_array,
                    .numeric_array,
                    .money_array,
                    .varbit_array,
                    .int2vector_array,
                    .bit_array,
                    .path_array,
                    .xml_array,
                    .point_array,
                    .lseg_array,
                    .box_array,
                    .polygon_array,
                    .line_array,
                    .cidr_array,
                    .circle_array,
                    .macaddr8_array,
                    .macaddr_array,
                    .inet_array,
                    .aclitem_array,
                    .pg_database_array,
                    .pg_database_array2 => {
                        let current_idx: usize = 0;
                        let index: usize = 0;
                        while (index < slice.len) {
                            const byte = slice[index];
                            if (byte == '}' || byte == separator) {
                                current_idx = index;
                                break;
                            }
                            index += 1;
                        }
                        if (current_idx == 0) return error.UnsupportedArrayFormat;
                        const element = slice[0..current_idx];
                        if (bun.strings.eqlComptime(element, "NULL")) {
                            array.append(bun.default_allocator, SQLDataCell{ tag: .@"null", value: { @"null": 0 } })?;
                            pos += current_idx;
                            continue;
                        }
                        if (arrayType == .date_array) {
                            let str = bun.String.init(element);
                            defer str.deref();
                            array.append(bun.default_allocator, SQLDataCell{ tag: .date, value: { date: str.parseDate(globalObject)? } })?;
                        } else {
                            if (bun.strings.eqlComptime(element, "\\b")) {
                                array.append(bun.default_allocator, SQLDataCell{ tag: .@"string", value: { @"string": bun.String.cloneUTF8("\x08").value.WTFStringImpl }, free_value: 1 })?;
                            } else {
                                array.append(bun.default_allocator, SQLDataCell{ tag: .@"string", value: { @"string": if (element.len > 0) bun.String.cloneUTF8(element).value.WTFStringImpl else null }, free_value: 1 })?;
                            }
                        }
                        pos += current_idx;
                        continue;
                    },
                    else => {
                        switch (slice[0]) {
                            'N' => {
                                if (slice.len < 3) return error.UnsupportedArrayFormat;
                                if (slice.len >= 4) {
                                    if (bun.strings.eqlComptime(slice[0..4], "NULL")) {
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .@"null", value: { @"null": 0 } })?;
                                        pos += 4;
                                        continue;
                                    }
                                }
                                if (bun.strings.eqlComptime(slice[0..3], "NaN")) {
                                    array.append(bun.default_allocator, SQLDataCell{ tag: .float8, value: { float8: std.math.nan(f64) } })?;
                                    pos += 3;
                                    continue;
                                }
                                return error.UnsupportedArrayFormat;
                            },
                            'f' => {
                                if (arrayType == .json_array || arrayType == .jsonb_array) {
                                    if (slice.len < 5) return error.UnsupportedArrayFormat;
                                    if (bun.strings.eqlComptime(slice[0..5], "false")) {
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .@"bool", value: { @"bool": 0 } })?;
                                        pos += 5;
                                        continue;
                                    }
                                } else {
                                    array.append(bun.default_allocator, SQLDataCell{ tag: .@"bool", value: { @"bool": 0 } })?;
                                    pos += 1;
                                    continue;
                                }
                            },
                            't' => {
                                if (arrayType == .json_array || arrayType == .jsonb_array) {
                                    if (slice.len < 4) return error.UnsupportedArrayFormat;
                                    if (bun.strings.eqlComptime(slice[0..4], "true")) {
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .@"bool", value: { @"bool": 1 } })?;
                                        pos += 4;
                                        continue;
                                    }
                                } else {
                                    array.append(bun.default_allocator, SQLDataCell{ tag: .@"bool", value: { @"bool": 1 } })?;
                                    pos += 1;
                                    continue;
                                }
                            },
                            'I', 'i' => {
                                if (slice.len < 8) return error.UnsupportedArrayFormat;
                                if (bun.strings.eqlCaseInsensitiveASCII(slice[0..8], "Infinity", false)) {
                                    if (arrayType == .date_array || arrayType == .timestamp_array || arrayType == .timestamptz_array) {
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .date, value: { date: std.math.inf(f64) } })?;
                                    } else {
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .float8, value: { float8: std.math.inf(f64) } })?;
                                    }
                                    pos += 8;
                                    continue;
                                }
                                return error.UnsupportedArrayFormat;
                            },
                            '+' => {
                                pos += 1;
                                continue;
                            },
                            '-', '0'...'9' => {
                                let is_negative = false;
                                let is_float = false;
                                let current_idx: usize = 0;
                                let is_infinity = false;
                                let has_exponent = false;
                                let has_negative_sign = false;
                                let has_positive_sign = false;
                                let index: usize = 0;
                                while (index < slice.len) {
                                    const byte = slice[index];
                                    if (byte == closing_brace || byte == separator) {
                                        current_idx = index;
                                        break;
                                    }
                                    switch (byte) {
                                        '0'...'9' => {},
                                        'e' => {
                                            if (!is_float || has_exponent) return error.UnsupportedArrayFormat;
                                            has_exponent = true;
                                        },
                                        '+' => {
                                            if (!has_exponent || has_positive_sign) return error.UnsupportedArrayFormat;
                                            has_positive_sign = true;
                                        },
                                        '-' => {
                                            if (index == 0) {
                                                is_negative = true;
                                            } else {
                                                if (!has_exponent || has_negative_sign) return error.UnsupportedArrayFormat;
                                                has_negative_sign = true;
                                            }
                                        },
                                        '.' => {
                                            if (is_float) return error.UnsupportedArrayFormat;
                                            is_float = true;
                                        },
                                        'I', 'i' => {
                                            is_infinity = true;
                                            const element = if (is_negative) slice[1..] else slice;
                                            if (element.len < 8) return error.UnsupportedArrayFormat;
                                            if (bun.strings.eqlCaseInsensitiveASCII(element[0..8], "Infinity", false)) {
                                                if (arrayType == .date_array || arrayType == .timestamp_array || arrayType == .timestamptz_array) {
                                                    array.append(bun.default_allocator, SQLDataCell{ tag: .date, value: { date: if (is_negative) -std.math.inf(f64) else std.math.inf(f64) } })?;
                                                } else {
                                                    array.append(bun.default_allocator, SQLDataCell{ tag: .float8, value: { float8: if (is_negative) -std.math.inf(f64) else std.math.inf(f64) } })?;
                                                }
                                                pos += 8 + @as(usize, @intFromBool(is_negative));
                                                break;
                                            }
                                            return error.UnsupportedArrayFormat;
                                        },
                                        else => return error.UnsupportedArrayFormat,
                                    }
                                    index += 1;
                                }
                                if (is_infinity) {
                                    continue;
                                }
                                if (current_idx == 0) return error.UnsupportedArrayFormat;
                                const element = slice[0..current_idx];
                                if (is_float || arrayType == .float8_array) {
                                    array.append(bun.default_allocator, SQLDataCell{ tag: .float8, value: { float8: bun.parseDouble(element) catch std.math.nan(f64) } })?;
                                    pos += current_idx;
                                    continue;
                                }
                                switch (arrayType) {
                                    .int8_array => {
                                        if (bigint) {
                                            array.append(bun.default_allocator, SQLDataCell{ tag: .int8, value: { int8: std.fmt.parseInt(i64, element, 0) catch return error.UnsupportedArrayFormat } })?;
                                        } else {
                                            array.append(bun.default_allocator, SQLDataCell{ tag: .@"string", value: { @"string": if (element.len > 0) bun.String.cloneUTF8(element).value.WTFStringImpl else null }, free_value: 1 })?;
                                        }
                                        pos += current_idx;
                                        continue;
                                    },
                                    .cid_array, .xid_array, .oid_array => {
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .uint4, value: { uint4: std.fmt.parseInt(u32, element, 0) catch 0 } })?;
                                        pos += current_idx;
                                        continue;
                                    },
                                    else => {
                                        const value = std.fmt.parseInt(i32, element, 0) catch return error.UnsupportedArrayFormat;
                                        array.append(bun.default_allocator, SQLDataCell{ tag: .int4, value: { int4: @intCast(value) } })?;
                                        pos += current_idx;
                                        continue;
                                    },
                                }
                            },
                            else => {
                                if (arrayType == .json_array || arrayType == .jsonb_array) {
                                    if (slice[0] == '[') {
                                        let sub_array_offset: usize = 0;
                                        let sub_array = parseArray(slice, bigint, arrayType, globalObject, &sub_array_offset, true)?;
                                        errdefer sub_array.deinit();
                                        array.append(bun.default_allocator, sub_array)?;
                                        pos += sub_array_offset;
                                        continue;
                                    }
                                }
                                return error.UnsupportedArrayFormat;
                            },
                        }
                    },
                }
            },
        }
    }

    if (offset_ptr := offset) {
        unsafe {
            offset_ptr.* = @min(bytes.len, pos);
        }
    }

    if (!reached_end || array.items.len > std.math.maxInt(u32)) {
        @branchHint(.unlikely);
        return error.UnsupportedArrayFormat;
    }
    return SQLDataCell{ tag: .array, value: { array: { ptr: array.items.ptr, len: @truncate(array.items.len), cap: @truncate(array.capacity) } }, free_value: 1 };
}

fn fromTypedArrayBytes(comptime tag: PgTag, binary: bool, bigint: bool, bytes: []const u8, globalObject: *jsc.JSGlobalObject) anyerror!SQLDataCell {
    if (binary) {
        if (bytes.len < 12) {
            return error.InvalidBinaryData;
        }
        const dimensions_raw: int4 = @bitCast(unsafe { bytes[0..4].* });
        const contains_nulls: int4 = @bitCast(unsafe { bytes[4..8].* });

        const dimensions = @byteSwap(dimensions_raw);
        if (dimensions > 1) {
            return error.MultidimensionalArrayNotSupportedYet;
        }

        if (contains_nulls != 0) {
            return error.NullsInArrayNotSupportedYet;
        }

        if (dimensions == 0) {
            return SQLDataCell{
                tag: .typed_array,
                value: {
                    typed_array: {
                        ptr: null,
                        len: 0,
                        byte_len: 0,
                        type: tag.toJSTypedArrayType()?,
                    },
                },
            };
        }

        if (bytes.len < 20) {
            return error.InvalidBinaryData;
        }
        const array_len: i32 = @byteSwap(@as(i32, @bitCast(unsafe { bytes[12..16].* })));
        if (array_len < 0) {
            return error.InvalidBinaryData;
        }
        const ElemType = tag.byteArrayType()?;
        const element_stride: usize = @sizeOf(ElemType) * 2;
        const max_elements = (bytes.len - 20) / element_stride;
        if (@as(usize, @intCast(array_len)) > max_elements) {
            return error.InvalidBinaryData;
        }

        const PgArrType = tag.pgArrayType()?;
        const elements = PgArrType.init(bytes).slice();

        return SQLDataCell{
            tag: .typed_array,
            value: {
                typed_array: {
                    head_ptr: if (bytes.len > 0) @constCast(bytes.ptr) else null,
                    ptr: if (elements.len > 0) unsafe { @ptrCast(elements.ptr) } else null,
                    len: @truncate(elements.len),
                    byte_len: @truncate(bytes.len),
                    type: tag.toJSTypedArrayType()?,
                },
            },
        };
    } else {
        return parseArray(bytes, bigint, tag, globalObject, null, false);
    }
}

pub fn fromBytes(binary: bool, bigint: bool, oid: PgTag, bytes: []const u8, globalObject: *jsc.JSGlobalObject) anyerror!SQLDataCell {
    switch (oid) {
        .int4_array => return fromTypedArrayBytes(.int4_array, binary, bigint, bytes, globalObject),
        .float4_array => return fromTypedArrayBytes(.float4_array, binary, bigint, bytes, globalObject),
        .int2 => {
            if (binary) {
                return SQLDataCell{ tag: .int4, value: { int4: parseBinary(.int2, i16, bytes)? } };
            } else {
                return SQLDataCell{ tag: .int4, value: { int4: std.fmt.parseInt(i32, bytes, 0) catch 0 } };
            }
        },
        .cid, .xid, .oid => {
            if (binary) {
                return SQLDataCell{ tag: .uint4, value: { uint4: parseBinary(.oid, u32, bytes)? } };
            } else {
                return SQLDataCell{ tag: .uint4, value: { uint4: std.fmt.parseInt(u32, bytes, 0) catch 0 } };
            }
        },
        .int4 => {
            if (binary) {
                return SQLDataCell{ tag: .int4, value: { int4: parseBinary(.int4, i32, bytes)? } };
            } else {
                return SQLDataCell{ tag: .int4, value: { int4: std.fmt.parseInt(i32, bytes, 0) catch 0 } };
            }
        },
        .int8 => {
            if (bigint) {
                return SQLDataCell{ tag: .int8, value: { int8: std.fmt.parseInt(i64, bytes, 0) catch 0 } };
            } else {
                return SQLDataCell{ tag: .@"string", value: { @"string": if (bytes.len > 0) bun.String.cloneUTF8(bytes).value.WTFStringImpl else null }, free_value: 1 };
            }
        },
        .float8 => {
            if (binary && bytes.len == 8) {
                return SQLDataCell{ tag: .float8, value: { float8: parseBinary(.float8, f64, bytes)? } };
            } else {
                const float8: f64 = bun.parseDouble(bytes) catch std.math.nan(f64);
                return SQLDataCell{ tag: .float8, value: { float8: float8 } };
            }
        },
        .float4 => {
            if (binary && bytes.len == 4) {
                return SQLDataCell{ tag: .float8, value: { float8: parseBinary(.float4, f32, bytes)? } };
            } else {
                const float4: f64 = bun.parseDouble(bytes) catch std.math.nan(f64);
                return SQLDataCell{ tag: .float8, value: { float8: float4 } };
            }
        },
        .numeric => {
            if (binary) {
                let stack_buffer = std.heap.stackFallback(1024, bun.default_allocator);
                const allocator = stack_buffer.get();
                let numeric_buffer = ByteList.fromOwnedSlice(allocator, &stack_buffer.buffer);
                numeric_buffer.items.len = 0;
                defer numeric_buffer.deinit();

                const result = parseBinaryNumeric(bytes, &numeric_buffer) catch return error.UnsupportedNumericFormat;
                return SQLDataCell{ tag: .@"string", value: { @"string": bun.String.cloneUTF8(result.slice()).value.WTFStringImpl }, free_value: 1 };
            } else {
                return SQLDataCell{ tag: .@"string", value: { @"string": if (bytes.len > 0) String.cloneUTF8(bytes).value.WTFStringImpl else null }, free_value: 1 };
            }
        },
        .jsonb, .json => {
            return SQLDataCell{ tag: .json, value: { json: if (bytes.len > 0) String.cloneUTF8(bytes).value.WTFStringImpl else null }, free_value: 1 };
        },
        .@"bool" => {
            if (binary) {
                return SQLDataCell{ tag: .@"bool", value: { @"bool": @intFromBool(bytes.len > 0 && bytes[0] == 1) } };
            } else {
                return SQLDataCell{ tag: .@"bool", value: { @"bool": @intFromBool(bytes.len > 0 && bytes[0] == 't') } };
            }
        },
        .date, .timestamp, .timestamptz => |tag| {
            if (bytes.len == 0) {
                return SQLDataCell{ tag: .@"null", value: { @"null": 0 } };
            }
            if (binary && bytes.len == 8) {
                switch (tag) {
                    .timestamptz => return SQLDataCell{ tag: .date_with_time_zone, value: { date_with_time_zone: pg_types.date.fromBinary(bytes) } },
                    .timestamp => return SQLDataCell{ tag: .date, value: { date: pg_types.date.fromBinary(bytes) } },
                    else => unreachable,
                }
            } else {
                if (bun.strings.eqlCaseInsensitiveASCII(bytes, "NULL", true)) {
                    return SQLDataCell{ tag: .@"null", value: { @"null": 0 } };
                }
                let str = bun.String.init(bytes);
                defer str.deref();
                return SQLDataCell{ tag: .date, value: { date: str.parseDate(globalObject)? } };
            }
        },
        .time, .timetz => |tag| {
            if (bytes.len == 0) {
                return SQLDataCell{ tag: .@"null", value: { @"null": 0 } };
            }
            if (binary) {
                if (tag == .time && bytes.len == 8) {
                    const microseconds = @byteSwap(@as(i64, @bitCast(unsafe { bytes[0..8].* })));
                    let buffer: [32]u8 = undefined;
                    const len = unsafe { Postgres__formatTime(microseconds, &buffer, buffer.len) };
                    return SQLDataCell{ tag: .@"string", value: { @"string": bun.String.cloneUTF8(buffer[0..len]).value.WTFStringImpl }, free_value: 1 };
                } else if (tag == .timetz && bytes.len == 12) {
                    const microseconds = @byteSwap(@as(i64, @bitCast(unsafe { bytes[0..8].* })));
                    const tz_offset_seconds = @byteSwap(@as(i32, @bitCast(unsafe { bytes[8..12].* })));
                    let buffer: [48]u8 = undefined;
                    const len = unsafe { Postgres__formatTimeTz(microseconds, tz_offset_seconds, &buffer, buffer.len) };
                    return SQLDataCell{ tag: .@"string", value: { @"string": bun.String.cloneUTF8(buffer[0..len]).value.WTFStringImpl }, free_value: 1 };
                } else {
                    return error.InvalidBinaryData;
                }
            } else {
                return SQLDataCell{ tag: .@"string", value: { @"string": if (bytes.len > 0) bun.String.cloneUTF8(bytes).value.WTFStringImpl else null }, free_value: 1 };
            }
        },
        .bytea => {
            if (binary) {
                return SQLDataCell{ tag: .bytea, value: { bytea: [ @intFromPtr(bytes.ptr), bytes.len ] } };
            } else {
                if (bun.strings.hasPrefixComptime(bytes, "\\x")) {
                    return parseBytea(bytes[2..]);
                }
                return error.UnsupportedByteaFormat;
            }
        },
        .bpchar_array,
        .varchar_array,
        .char_array,
        .text_array,
        .name_array,
        .json_array,
        .jsonb_array,
        .path_array,
        .xml_array,
        .point_array,
        .lseg_array,
        .box_array,
        .polygon_array,
        .line_array,
        .cidr_array,
        .numeric_array,
        .money_array,
        .varbit_array,
        .bit_array,
        .int2vector_array,
        .circle_array,
        .macaddr8_array,
        .macaddr_array,
        .inet_array,
        .aclitem_array,
        .tid_array,
        .pg_database_array,
        .pg_database_array2,
        .int8_array,
        .int2_array,
        .float8_array,
        .oid_array,
        .xid_array,
        .cid_array,
        .bool_array,
        .bytea_array,
        .time_array,
        .date_array,
        .timetz_array,
        .timestamp_array,
        .timestamptz_array,
        .interval_array => |tag| {
            return parseArray(bytes, bigint, tag, globalObject, null, false);
        },
        else => {
            return SQLDataCell{ tag: .@"string", value: { @"string": if (bytes.len > 0) bun.String.cloneUTF8(bytes).value.WTFStringImpl else null }, free_value: 1 };
        },
    }
}

fn pg_ntoh16(x: u16) u16 {
    return @byteSwap(x);
}

fn pg_ntoh32(x: u32) u32 {
    return @byteSwap(x);
}

union PGNummericString {
    static: []const u8,
    dynamic: []const u8,

    pub fn slice(this) []const u8 {
        return switch (this) {
            .static value => value,
            .dynamic value => value,
        };
    }
}

fn parseBinaryNumeric(input: []const u8, result: *ByteList) anyerror!PGNummericString {
    if (input.len < 8) return error.InvalidBuffer;
    let fixed_buffer = std.io.fixedBufferStream(input);
    let reader = fixed_buffer.reader();

    const ndigits = reader.readInt(i16, .big)?;
    const weight = reader.readInt(i16, .big)?;
    const sign = reader.readInt(u16, .big)?;
    const dscale = reader.readInt(i16, .big)?;
    log("ndigits: {d}, weight: {d}, sign: {d}, dscale: {d}", .{ ndigits, weight, sign, dscale });

    switch (sign) {
        0xC000 => return PGNummericString{ static: "NaN" },
        0xD000 => return PGNummericString{ static: "Infinity" },
        0xF000 => return PGNummericString{ static: "-Infinity" },
        0x4000, 0x0000 => {},
        else => return error.InvalidSign,
    }

    if (ndigits == 0) {
        return PGNummericString{ static: "0" };
    }

    if (sign == 0x4000) {
        result.append('-')?;
    }

    let decimal_pos: i32 = @as(i32, weight + 1) * 4;
    if (decimal_pos <= 0) {
        decimal_pos = 1;
    }

    let scale_start: i32 = 0;
    if (weight < 0) {
        result.append('0')?;
        scale_start = @as(i32, @intCast(weight)) + 1;
    } else {
        let idx: usize = 0;
        let first_non_zero = false;
        const uweight: usize = @intCast(weight);
        const undigits: usize = if (ndigits > 0) @intCast(ndigits) else 0;

        while (idx <= uweight) {
            const digit = if (idx < undigits) reader.readInt(u16, .big)? else 0;
            log("digit: {d}", .{digit});
            let digit_str: [4]u8 = undefined;
            const digit_len = std.fmt.printInt(&digit_str, digit, 10, .lower, { width: 4, fill: '0' });
            if (!first_non_zero) {
                let start_idx: usize = 0;
                while (start_idx < digit_len && digit_str[start_idx] == '0') {
                    start_idx += 1;
                }
                if (start_idx == digit_len) {
                    idx += 1;
                    continue;
                }
                const digit_slice = digit_str[start_idx..digit_len];
                result.appendSlice(digit_slice)?;
                first_non_zero = true;
            } else {
                result.appendSlice(digit_str[0..digit_len])?;
            }
            idx += 1;
        }
    }
    if (dscale > 0) {
        result.append('.')?;
        let idx: isize = scale_start;
        const end: usize = result.items.len + @as(usize, @intCast(dscale));
        while (idx < dscale) {
            if (idx >= 0 && idx < dscale) {
                const digit = reader.readInt(u16, .big) catch 0;
                log("dscale digit: {d}", .{digit});
                let digit_str: [4]u8 = undefined;
                const digit_len = std.fmt.printInt(&digit_str, digit, 10, .lower, { width: 4, fill: '0' });
                result.appendSlice(digit_str[0..digit_len])?;
            } else {
                log("dscale digit: 0000", .{});
                result.appendSlice("0000")?;
            }
            idx += 4;
        }
        if (result.items.len > end) {
            result.items.len = end;
        }
    }
    return PGNummericString{ dynamic: result.items };
}

pub fn parseBinary(comptime tag: PgTag, comptime ReturnType: type, bytes: []const u8) AnyPostgresError!ReturnType {
    switch (comptime tag) {
        .float8 => {
            const v: f64 = @bitCast(parseBinary(.int8, i64, bytes)?);
            return v;
        },
        .int8 => {
            if (bytes.len != 8) return error.InvalidBinaryData;
            return @byteSwap(@as(i64, @bitCast(unsafe { bytes[0..8].* })));
        },
        .int4 => {
            switch (bytes.len) {
                1 => return bytes[0],
                2 => return pg_ntoh16(@as(u16, @bitCast(unsafe { bytes[0..2].* }))),
                4 => return @bitCast(pg_ntoh32(@as(u32, @bitCast(unsafe { bytes[0..4].* })))),
                else => return error.UnsupportedIntegerSize,
            }
        },
        .oid => {
            switch (bytes.len) {
                1 => return bytes[0],
                2 => return pg_ntoh16(@as(u16, @bitCast(unsafe { bytes[0..2].* }))),
                4 => return pg_ntoh32(@as(u32, @bitCast(unsafe { bytes[0..4].* }))),
                else => return error.UnsupportedIntegerSize,
            }
        },
        .int2 => {
            switch (bytes.len) {
                1 => return bytes[0],
                2 => {
                    const value: u16 = @bitCast(unsafe { bytes[0..2].* });
                    return @bitCast(@byteSwap(value));
                },
                else => return error.UnsupportedIntegerSize,
            }
        },
        .float4 => {
            const v: f32 = @bitCast(parseBinary(.int4, i32, bytes)?);
            return v;
        },
        else => @compileError("TODO"),
    }
}

export struct Putter {
    list: []SQLDataCell,
    fields: []const protocol.FieldDescription,
    binary: bool = false,
    bigint: bool = false,
    count: usize = 0,
    globalObject: *jsc.JSGlobalObject,

    pub fn toJS(*this, globalObject: *jsc.JSGlobalObject, array: JSValue, structure: JSValue, flags: SQLDataCell.Flags, result_mode: PostgresSQLQueryResultMode, cached_structure: ?PostgresCachedStructure) anyerror!JSValue {
        let names: ?[*]jsc.JSObject.ExternColumnIdentifier = null;
        let names_count: u32 = 0;
        if (c := cached_structure) {
            if (f := c.fields) {
                names = f.ptr;
                names_count = @truncate(f.len);
            }
        }

        return SQLDataCell.constructObjectFromDataCell(
            globalObject,
            array,
            structure,
            this.list.ptr,
            @truncate(this.fields.len),
            flags,
            @intFromEnum(result_mode),
            names,
            names_count,
        );
    }

    fn putImpl(*this, index: u32, optional_bytes: (*Data)?, comptime is_raw: bool) anyerror!bool {
        if (index >= this.fields.len) {
            debug("putImpl: index {d} >= fields.len {d}, ignoring extra field", .{ index, this.fields.len });
            return false;
        }
        if (index >= this.list.len) {
            debug("putImpl: index {d} >= list.len {d}, ignoring extra field", .{ index, this.list.len });
            return false;
        }

        const field = &this.fields[index];
        const oid = field.type_oid;
        debug("index: {d}, oid: {d}", .{ index, oid });
        let cell = &this.list[index];
        unsafe {
            if (is_raw) {
                cell.* = SQLDataCell.raw(optional_bytes);
            } else {
                const tag: PgTag = if (std.math.maxInt(short) < oid) .text else @enumFromInt(@as(short, @intCast(oid)));
                if (data := optional_bytes) {
                    cell.* = fromBytes((field.binary || this.binary) && tag.isBinaryFormatSupported(), this.bigint, tag, data.slice(), this.globalObject)?;
                } else {
                    cell.* = SQLDataCell{
                        tag: .@"null",
                        value: {
                            @"null": 0,
                        },
                    };
                }
            }
        }
        this.count += 1;
        cell.index = switch (field.name_or_index) {
            .index i => i,
            else => @intCast(index),
        };

        cell.isIndexedColumn = switch (field.name_or_index) {
            .duplicate => 2,
            .index => 1,
            .name => 0,
        };
        return true;
    }

    pub fn putRaw(*this, index: u32, optional_bytes: (*Data)?) anyerror!bool {
        return this.putImpl(index, optional_bytes, true);
    }

    pub fn put(*this, index: u32, optional_bytes: (*Data)?) anyerror!bool {
        return this.putImpl(index, optional_bytes, false);
    }
}
