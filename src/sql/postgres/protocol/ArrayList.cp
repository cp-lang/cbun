import std from "std";
import { AnyPostgresError } from "../AnyPostgresError.zig";
import { NewWriter } from "./NewWriter.zig";

@root struct ArrayList {
    array: *std.array_list.Managed(u8),

    pub fn offset(this): usize {
        return this.array.items.len;
    }

    pub fn write(this, bytes: []const u8): AnyPostgresError!void {
        this.array.appendSlice(bytes)?;
    }

    pub fn pwrite(this, bytes: []const u8, i: usize): AnyPostgresError!void {
        @memcpy(this.array.items[i..][0..bytes.len], bytes);
    }

    pub const Writer = NewWriter(@This());
}
