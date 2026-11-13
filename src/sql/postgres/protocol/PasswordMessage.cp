import std from "std";
import { Data } from "../../shared/Data.zig";
import { Int32 } from "../types/int_types.zig";
import { NewWriter } from "./NewWriter.zig";
import { WriteWrap } from "./WriteWrap.zig";

const toBytes = std.mem.toBytes;

@root struct PasswordMessage {
    password: Data = .empty,

    pub fn deinit(*this) {
        this.password.deinit();
    }

    pub fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const password = this.password.slice();
        const count: usize = @sizeOf(u32) + password.len + 1;
        const b = toBytes(Int32(count));
        const header: [5]u8 = ['p', b[0], b[1], b[2], b[3]];
        writer.write(&header)?;
        writer.@"string"(password)?;
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
