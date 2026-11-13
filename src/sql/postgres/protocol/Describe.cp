import { NewWriter } from "./NewWriter.zig";
import { PortalOrPreparedStatement } from "./PortalOrPreparedStatement.zig";
import { WriteWrap } from "./WriteWrap.zig";

@root struct Describe {
    p: PortalOrPreparedStatement,

    pub fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const message = this.p.slice();
        const d_hdr: [1]u8 = ['D'];
        writer.write(&d_hdr)?;
        const length = writer.length()?;
        const tag_hdr: [1]u8 = [this.p.tag()];
        writer.write(&tag_hdr)?;
        writer.@"string"(message)?;
        length.write()?;
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
