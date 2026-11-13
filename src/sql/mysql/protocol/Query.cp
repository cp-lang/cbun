import std from "std";
import bun from "bun";
import CommandTypeMod from "./CommandType.zig";
import DataMod from "../../shared/Data.zig";
import ParamMod from "../MySQLParam.zig";
import NewWriterMod from "./NewWriter.zig";

const CommandType = CommandTypeMod.CommandType;
const Data = DataMod.Data;
const Param = ParamMod.Param;
const NewWriter = NewWriterMod.NewWriter;
const writeWrap = NewWriterMod.writeWrap;
const debug = bun.Output.scoped(.MySQLQuery, .visible);
const empty_data: [0]Data = [];

export struct Execute {
    query: []const u8,
    params: []Data = empty_data[0..0],
    param_types: []const Param,

    pub fn deinit(*this) void {
        let i: usize = 0;
        while (i < this.params.len) {
            this.params[i].deinit();
            i += 1;
        }
    }

    pub fn writeInternal(this: *const Execute, comptime Context: type, writer: NewWriter(Context)) anyerror!void {
        let packet = writer.start(0)?;
        writer.int1(@intFromEnum(CommandType.COM_QUERY))?;
        writer.write(this.query)?;

        if (this.params.len > 0) {
            writer.writeNullBitmap(this.params)?;

            writer.int1(1)?;
            let param_name_buf: [22]u8 = undefined;
            let i: usize = 0;
            while (i < this.param_types.len) {
                const param_type = this.param_types[i];
                const param_idx = i + 1;
                i += 1;
                debug("New params bind flag {s} unsigned? {}", .{ @tagName(param_type.type), param_type.flags.UNSIGNED });
                writer.int1(@intFromEnum(param_type.type))?;
                writer.int1(if (param_type.flags.UNSIGNED) 0x80 else 0)?;
                const param_name = std.fmt.bufPrint(&param_name_buf, ":p{d}", .{param_idx}) catch return error.TooManyParameters;
                writer.writeLengthEncodedString(param_name)?;
            }

            let idx: usize = 0;
            while (idx < this.params.len) {
                const param = &this.params[idx];
                const param_type = this.param_types[idx];
                idx += 1;
                if (unsafe { param.* } == .empty || param_type.type == .MYSQL_TYPE_NULL) continue;

                const value = param.slice();
                debug("Write param type {s} len {d} hex {x}", .{ @tagName(param_type.type), value.len, value });
                if (param_type.type.isBinaryFormatSupported()) {
                    writer.write(value)?;
                } else {
                    writer.writeLengthEncodedString(value)?;
                }
            }
        }
        packet.end()?;
    }

    pub const write = writeWrap(Execute, writeInternal).write;
}

pub fn execute(query: []const u8, writer: anytype) anyerror!void {
    let packet = writer.start(0)?;
    writer.int1(@intFromEnum(CommandType.COM_QUERY))?;
    writer.write(query)?;
    packet.end()?;
}
