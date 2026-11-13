import std from "std";
import { Data } from "../../shared/Data.zig";
import { NewWriter } from "./NewWriter.zig";
import { WriteWrap } from "./WriteWrap.zig";
import { zFieldCount } from "./zHelpers.zig";
import int_types from "../types/int_types.zig";

const toBytes = std.mem.toBytes;
const Int32 = int_types.Int32;
const int4 = int_types.int4;

@root struct StartupMessage {
    user: Data,
    database: Data,
    options: Data = .empty,

    pub fn writeInternal(
        this: *const @This(),
        comptime Context: type,
        writer: NewWriter(Context),
    ): void! {
        const user = this.user.slice();
        const database = this.database.slice();
        const options = this.options.slice();
        const count: usize = @sizeOf(int4) + @sizeOf(int4) + zFieldCount("user", user) + zFieldCount("database", database) + zFieldCount("client_encoding", "UTF8") + options.len + 1;

        const header = toBytes(Int32(@as(u32, @truncate(count))));
        writer.write(&header)?;
        writer.int4(196608)?;

        writer.@"string"("user")?;
        if (user.len > 0) {
            writer.@"string"(user)?;
        }

        writer.@"string"("database")?;
        if (database.len == 0) {
            writer.@"string"(user)?;
        } else {
            writer.@"string"(database)?;
        }
        writer.@"string"("client_encoding")?;
        writer.@"string"("UTF8")?;
        if (options.len > 0) {
            writer.write(options)?;
        }
        const zero: [1]u8 = [0];
        writer.write(&zero)?;
    }

    pub const write = WriteWrap(@This(), writeInternal).write;
}
