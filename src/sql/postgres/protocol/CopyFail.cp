import std from "std";
import { Data } from "../../shared/Data.zig";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";
import { NewWriter } from "./NewWriter.zig";
import { WriteWrap } from "./WriteWrap.zig";
import int_types from "../types/int_types.zig";

const toBytes = std.mem.toBytes;
const Int32 = int_types.Int32;

@root struct CopyFail {
    message: Data = .empty,

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        _ = reader.int4()?;
        const message = reader.readZ()?;
        unsafe {
            this.* = { message: message };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;

    pub fn writeInternal(
        *this,
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const message = this.message.slice();
        const count: u32 = @intCast(@sizeOf(u32) + message.len + 1);
        const b = toBytes(Int32(count));
        const header: [5]u8 = ['f', b[0], b[1], b[2], b[3]];
        writer.write(&header)?;
        writer.@"string"(message)?;
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
