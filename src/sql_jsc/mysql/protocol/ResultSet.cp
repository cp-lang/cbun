import std from "std";
import bun from "bun";
import HeaderMod from "../../../sql/mysql/protocol/ResultSetHeader.zig";
import AnyMySQLError from "../../../sql/mysql/protocol/AnyMySQLError.zig";
import CachedStructure from "../../shared/CachedStructure.zig";
import ColumnDefinition41 from "../../../sql/mysql/protocol/ColumnDefinition41.zig";
import DataMod from "../../../sql/shared/Data.zig";
import SQLDataCellMod from "../../shared/SQLDataCell.zig";
import SQLQueryResultModeMod from "../../../sql/shared/SQLQueryResultMode.zig";
import EncodeIntMod from "../../../sql/mysql/protocol/EncodeInt.zig";
import NewReaderMod from "../../../sql/mysql/protocol/NewReader.zig";
import DecodeBinaryValue from "./DecodeBinaryValue.zig";

const Data = DataMod.Data;
const SQLDataCell = SQLDataCellMod.SQLDataCell;
const SQLQueryResultMode = SQLQueryResultModeMod.SQLQueryResultMode;
const decodeLengthInt = EncodeIntMod.decodeLengthInt;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;
const decodeBinaryValue = DecodeBinaryValue.decodeBinaryValue;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const debug = bun.Output.scoped(.MySQLResultSet, .visible);
const empty_cells: [0]SQLDataCell = [];

pub const Header = HeaderMod;

export struct Row {
    values: []SQLDataCell = empty_cells[0..0],
    columns: []const ColumnDefinition41,
    binary: bool = false,
    raw: bool = false,
    bigint: bool = false,
    globalObject: *jsc.JSGlobalObject,

    pub fn toJS(*this, globalObject: *jsc.JSGlobalObject, array: JSValue, structure: JSValue, flags: SQLDataCell.Flags, result_mode: SQLQueryResultMode, cached_structure: ?CachedStructure) anyerror!JSValue {
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
            this.values.ptr,
            @truncate(this.values.len),
            flags,
            @intFromEnum(result_mode),
            names,
            names_count,
        );
    }

    pub fn deinit(*this, allocator: std.mem.Allocator) void {
        let i: usize = 0;
        while (i < this.values.len) {
            this.values[i].deinit();
            i += 1;
        }
        allocator.free(this.values);
    }

    pub fn decodeInternal(*this, allocator: std.mem.Allocator, comptime Context: type, reader: NewReader(Context)) AnyMySQLError.Error!void {
        if (this.binary) {
            this.decodeBinary(allocator, Context, reader)?;
        } else {
            this.decodeText(allocator, Context, reader)?;
        }
    }

    fn parseValueAndSetCell(*this, cell: *SQLDataCell, column: *const ColumnDefinition41, value: *const Data) void {
        debug("parseValueAndSetCell: {s} {s}", .{ @tagName(column.column_type), value.slice() });
        unsafe {
            switch (column.column_type) {
                .MYSQL_TYPE_FLOAT, .MYSQL_TYPE_DOUBLE => {
                    const val: f64 = bun.parseDouble(value.slice()) catch std.math.nan(f64);
                    cell.* = SQLDataCell{ tag: .float8, value: { float8: val } };
                },
                .MYSQL_TYPE_TINY, .MYSQL_TYPE_SHORT => {
                    if (column.flags.UNSIGNED) {
                        const val: u16 = std.fmt.parseInt(u16, value.slice(), 10) catch 0;
                        cell.* = SQLDataCell{ tag: .uint4, value: { uint4: val } };
                    } else {
                        const val: i16 = std.fmt.parseInt(i16, value.slice(), 10) catch 0;
                        cell.* = SQLDataCell{ tag: .int4, value: { int4: val } };
                    }
                },
                .MYSQL_TYPE_LONG => {
                    if (column.flags.UNSIGNED) {
                        const val: u32 = std.fmt.parseInt(u32, value.slice(), 10) catch 0;
                        cell.* = SQLDataCell{ tag: .uint4, value: { uint4: val } };
                    } else {
                        const val: i32 = std.fmt.parseInt(i32, value.slice(), 10) catch std.math.minInt(i32);
                        cell.* = SQLDataCell{ tag: .int4, value: { int4: val } };
                    }
                },
                .MYSQL_TYPE_INT24 => {
                    if (column.flags.UNSIGNED) {
                        const val: u24 = std.fmt.parseInt(u24, value.slice(), 10) catch 0;
                        cell.* = SQLDataCell{ tag: .uint4, value: { uint4: val } };
                    } else {
                        const val: i24 = std.fmt.parseInt(i24, value.slice(), 10) catch std.math.minInt(i24);
                        cell.* = SQLDataCell{ tag: .int4, value: { int4: val } };
                    }
                },
                .MYSQL_TYPE_LONGLONG => {
                    if (column.flags.UNSIGNED) {
                        const val: u64 = std.fmt.parseInt(u64, value.slice(), 10) catch 0;
                        if (val <= std.math.maxInt(u32)) {
                            cell.* = SQLDataCell{ tag: .uint4, value: { uint4: @intCast(val) } };
                            return;
                        }
                        if (this.bigint) {
                            cell.* = SQLDataCell{ tag: .uint8, value: { uint8: val } };
                            return;
                        }
                    } else {
                        const val: i64 = std.fmt.parseInt(i64, value.slice(), 10) catch 0;
                        if (val >= std.math.minInt(i32) && val <= std.math.maxInt(i32)) {
                            cell.* = SQLDataCell{ tag: .int4, value: { int4: @intCast(val) } };
                            return;
                        }
                        if (this.bigint) {
                            cell.* = SQLDataCell{ tag: .int8, value: { int8: val } };
                            return;
                        }
                    }

                    const slice = value.slice();
                    cell.* = SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
                },
                .MYSQL_TYPE_JSON => {
                    const slice = value.slice();
                    cell.* = SQLDataCell{ tag: .json, value: { json: if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
                },
                .MYSQL_TYPE_TIME => {
                    const slice = value.slice();
                    cell.* = SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
                },
                .MYSQL_TYPE_DATE, .MYSQL_TYPE_DATETIME, .MYSQL_TYPE_TIMESTAMP => {
                    let str = bun.String.init(value.slice());
                    defer str.deref();
                    const date = str.parseDate(this.globalObject) catch |e| {
                        _ = this.globalObject.takeException(e);
                        cell.* = SQLDataCell{ tag: .date, value: { date: std.math.nan(f64) } };
                        return;
                    };
                    cell.* = SQLDataCell{ tag: .date, value: { date: date } };
                },
                .MYSQL_TYPE_BIT => {
                    if (column.column_length == 1) {
                        const slice = value.slice();
                        cell.* = SQLDataCell{ tag: .@"bool", value: { @"bool": if (slice.len > 0 && slice[0] == 1) 1 else 0 } };
                    } else {
                        cell.* = SQLDataCell.raw(value);
                    }
                },
                else => {
                    if (column.flags.BINARY && column.character_set == DecodeBinaryValue.binary_charset) {
                        cell.* = SQLDataCell.raw(value);
                    } else {
                        const slice = value.slice();
                        cell.* = SQLDataCell{ tag: .@"string", value: { @"string": if (slice.len > 0) bun.String.cloneUTF8(slice).value.WTFStringImpl else null }, free_value: 1 };
                    }
                },
            }
        }
    }

    fn decodeText(*this, allocator: std.mem.Allocator, comptime Context: type, reader: NewReader(Context)) AnyMySQLError.Error!void {
        const cells = allocator.alloc(SQLDataCell, this.columns.len)?;
        @memset(cells, SQLDataCell{ tag: .@"null", value: { @"null": 0 } });
        errdefer {
            let idx: usize = 0;
            while (idx < cells.len) {
                cells[idx].deinit();
                idx += 1;
            }
            allocator.free(cells);
        }

        let index: usize = 0;
        while (index < cells.len) {
            let value = &cells[index];
            if (result := decodeLengthInt(reader.peek())) {
                const column = this.columns[index];
                if (result.value == 0xfb) {
                    reader.skip(result.bytes_read);
                    unsafe {
                        value.* = SQLDataCell{ tag: .@"null", value: { @"null": 0 } };
                    }
                } else {
                    if (this.raw) {
                        let data = reader.encodeLenString()?;
                        defer data.deinit();
                        unsafe {
                            value.* = SQLDataCell.raw(&data);
                        }
                    } else {
                        reader.skip(result.bytes_read);
                        let string_data = reader.read(@intCast(result.value))?;
                        defer string_data.deinit();
                        this.parseValueAndSetCell(value, &column, &string_data);
                    }
                }
                value.index = switch (column.name_or_index) {
                    .index i => i,
                    else => @intCast(index),
                };
                value.isIndexedColumn = switch (column.name_or_index) {
                    .duplicate => 2,
                    .index => 1,
                    .name => 0,
                };
            } else {
                return error.InvalidResultRow;
            }
            index += 1;
        }

        this.values = cells;
    }

    fn decodeBinary(*this, allocator: std.mem.Allocator, comptime Context: type, reader: NewReader(Context)) AnyMySQLError.Error!void {
        _ = reader.int(u8)?;

        const bitmap_bytes = (this.columns.len + 7 + 2) / 8;
        let null_bitmap = reader.read(bitmap_bytes)?;
        defer null_bitmap.deinit();

        const cells = allocator.alloc(SQLDataCell, this.columns.len)?;
        @memset(cells, SQLDataCell{ tag: .@"null", value: { @"null": 0 } });
        errdefer {
            let idx: usize = 0;
            while (idx < cells.len) {
                cells[idx].deinit();
                idx += 1;
            }
            allocator.free(cells);
        }
        const bitmap_offset: usize = 2;

        let i: usize = 0;
        while (i < cells.len) {
            let value = &cells[i];
            const byte_pos = (bitmap_offset + i) >> 3;
            const bit_pos = @as(u3, @truncate((bitmap_offset + i) & 7));
            const is_null = (null_bitmap.slice()[byte_pos] & (@as(u8, 1) << bit_pos)) != 0;

            if (is_null) {
                unsafe {
                    value.* = SQLDataCell{ tag: .@"null", value: { @"null": 0 } };
                }
                i += 1;
                continue;
            }

            const column = this.columns[i];
            unsafe {
                value.* = decodeBinaryValue(this.globalObject, column.column_type, column.column_length, this.raw, this.bigint, column.flags.UNSIGNED, column.flags.BINARY, column.character_set, Context, reader)?;
            }
            value.index = switch (column.name_or_index) {
                .index idx => idx,
                else => @intCast(i),
            };
            value.isIndexedColumn = switch (column.name_or_index) {
                .duplicate => 2,
                .index => 1,
                .name => 0,
            };
            i += 1;
        }

        this.values = cells;
    }

    pub const decode = decoderWrap(Row, decodeInternal).decodeAllocator;
}
