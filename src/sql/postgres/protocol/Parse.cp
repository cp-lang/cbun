import std from "std";
import { NewWriter } from "./NewWriter.zig";
import { WriteWrap } from "./WriteWrap.zig";
import types from "../PostgresTypes.zig";
import zHelpers from "./zHelpers.zig";

const toBytes = std.mem.toBytes;
const Int32 = types.Int32;
const int4 = types.int4;
const zCount = zHelpers.zCount;

@root struct Parse {
    name: []const u8 = "",
    query: []const u8 = "",
    params: []const int4 = &[],

    pub fn deinit(*this) {
        _ = this;
    }

    pub fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const parameters = this.params;
        if (parameters.len > std.math.maxInt(u16)) {
            return err.TooManyParameters;
        }
        const count: usize = @sizeOf(u32) + @sizeOf(u16) + (parameters.len * @sizeOf(u32)) + @max(zCount(this.name), 1) + @max(zCount(this.query), 1);
        const b = toBytes(Int32(count));
        const header: [5]u8 = ['P', b[0], b[1], b[2], b[3]];
        writer.write(&header)?;
        writer.@"string"(this.name)?;
        writer.@"string"(this.query)?;
        writer.short(parameters.len)?;
        for (parameter of parameters) {
            writer.int4(parameter)?;
        }
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
