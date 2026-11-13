import std from "std";
import bun from "bun";
import AnyMySQLError from "./AnyMySQLError.zig";
import CommandTypeMod from "./CommandType.zig";
import ParamMod from "../MySQLParam.zig";
import TypesMod from "../MySQLTypes.zig";
import NewReaderMod from "./NewReader.zig";
import NewWriterMod from "./NewWriter.zig";

const CommandType = CommandTypeMod.CommandType;
const Param = ParamMod.Param;
const Value = TypesMod.Value;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;
const NewWriter = NewWriterMod.NewWriter;
const writeWrap = NewWriterMod.writeWrap;
const debug = bun.Output.scoped(.PreparedStatement, .hidden);
const empty_values: [0]Value = [];

export struct PrepareOK {
    status: u8 = 0,
    statement_id: u32,
    num_columns: u16,
    num_params: u16,
    warning_count: u16,

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.status = reader.int(u8)?;
        if (this.status != 0) {
            return error.InvalidPrepareOKPacket;
        }

        this.statement_id = reader.int(u32)?;
        this.num_columns = reader.int(u16)?;
        this.num_params = reader.int(u16)?;
        _ = reader.int(u8)?;
        this.warning_count = reader.int(u16)?;
    }

    pub const decode = decoderWrap(PrepareOK, decodeInternal).decode;
}

export struct Execute {
    statement_id: u32,
    flags: u8 = 0,
    iteration_count: u32 = 1,
    params: []Value = empty_values[0..0],
    param_types: []const Param,
    new_params_bind_flag: bool,

    pub fn deinit(*this) void {
        let i: usize = 0;
        while (i < this.params.len) {
            this.params[i].deinit(bun.default_allocator);
            i += 1;
        }
        if (this.params.len > 0) {
            bun.default_allocator.free(this.params);
        }
    }

    fn writeNullBitmap(this: *const Execute, comptime Context: type, writer: NewWriter(Context)) AnyMySQLError.Error!void {
        const MYSQL_MAX_PARAMS = (std.math.maxInt(u16) / 8) + 1;

        let null_bitmap_buf: [MYSQL_MAX_PARAMS]u8 = undefined;
        const bitmap_bytes = (this.params.len + 7) / 8;
        let null_bitmap = null_bitmap_buf[0..bitmap_bytes];
        @memset(null_bitmap, 0);

        let i: usize = 0;
        while (i < this.params.len) {
            if (this.params[i] == .@"null") {
                null_bitmap[i >> 3] |= @as(u8, 1) << @as(u3, @truncate(i & 7));
            }
            i += 1;
        }

        writer.write(null_bitmap)?;
    }

    pub fn writeInternal(this: *const Execute, comptime Context: type, writer: NewWriter(Context)) AnyMySQLError.Error!void {
        writer.int1(@intFromEnum(CommandType.COM_STMT_EXECUTE))?;
        writer.int4(this.statement_id)?;
        writer.int1(this.flags)?;
        writer.int4(this.iteration_count)?;

        if (this.params.len > 0) {
            this.writeNullBitmap(Context, writer)?;

            writer.int1(@intFromBool(this.new_params_bind_flag))?;

            if (this.new_params_bind_flag) {
                const param_types = this.param_types;
                for (param_types) |param_type| {
                    debug("New params bind flag {s} unsigned? {}", .{ @tagName(param_type.type), param_type.flags.UNSIGNED });
                    writer.int1(@intFromEnum(param_type.type))?;
                    writer.int1(if (param_type.flags.UNSIGNED) 0x80 else 0)?;
                }
            }

            let idx: usize = 0;
            while (idx < this.params.len) {
                const param = &this.params[idx];
                const param_type = this.param_types[idx];
                idx += 1;
                if (unsafe { param.* } == .@"null" || param_type.type == .MYSQL_TYPE_NULL) continue;

                let value = param.toData(param_type.type)?;
                defer value.deinit();
                if (param_type.type.isBinaryFormatSupported()) {
                    writer.write(value.slice())?;
                } else {
                    writer.writeLengthEncodedString(value.slice())?;
                }
            }
        }
    }

    pub const write = writeWrap(Execute, writeInternal).write;
}

