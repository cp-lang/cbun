import { NewWriter } from "./NewWriter.zig";
import { PortalOrPreparedStatement } from "./PortalOrPreparedStatement.zig";
import { WriteWrap } from "./WriteWrap.zig";
import int_types from "../types/int_types.zig";

const int4 = int_types.int4;

@root struct Execute {
    max_rows: int4 = 0,
    p: PortalOrPreparedStatement,

    pub fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        writer.write("E")?;
        const length = writer.length()?;
        if (this.p == .portal) {
            writer.@"string"(this.p.portal)?;
        } else {
            const zero: [1]u8 = [0];
            writer.write(&zero)?;
        }
        writer.int4(this.max_rows)?;
        length.write()?;
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
