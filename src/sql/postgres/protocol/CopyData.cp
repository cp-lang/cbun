import std from "std";
import { Data } from "../../shared/Data.zig";
import { DecoderWrap } from "./DecoderWrap.zig";
import { Int32 } from "../types/int_types.zig";
import { NewReader } from "./NewReader.zig";
import { NewWriter } from "./NewWriter.zig";
import { WriteWrap } from "./WriteWrap.zig";

const toBytes = std.mem.toBytes;

@root struct CopyData {
    data: Data = .empty,

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        const length = reader.length()?;
        const data = reader.read(@intCast(length -| 5))?;
        unsafe {
            this.* = { data: data };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;

    pub fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const data = this.data.slice();
        const count: u32 = @intCast(@sizeOf(u32) + data.len + 1);
        const b = toBytes(Int32(count));
        const header: [5]u8 = ['d', b[0], b[1], b[2], b[3]];
        writer.write(&header)?;
        writer.@"string"(data)?;
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
