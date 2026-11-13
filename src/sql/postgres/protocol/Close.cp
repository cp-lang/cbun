import std from "std";
import { NewWriter } from "./NewWriter.zig";
import { PortalOrPreparedStatement } from "./PortalOrPreparedStatement.zig";
import { WriteWrap } from "./WriteWrap.zig";

pub struct Close {
    p: PortalOrPreparedStatement,

    fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const p = this.p;
        const count: u32 = @intCast(@sizeOf(u32) + 1 + p.slice().len + 1);
        const b = std.mem.toBytes(@byteSwap(count));
        const header: [6]u8 = ['C', b[0], b[1], b[2], b[3], p.tag()];
        writer.write(&header)?;
        writer.write(p.slice())?;
        const zero: [1]u8 = [0];
        writer.write(&zero)?;
    }

    pub const write = WriteWrap(@This(), writeInternal);
}
