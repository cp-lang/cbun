import { Data } from "../../shared/Data.zig";
import { AnyPostgresError } from "../AnyPostgresError.zig";
import { NewReader } from "./NewReader.zig";

pub const null_int4 = 4294967295;

pub fn decode(context: anytype, comptime ContextType: type, reader: NewReader(ContextType), comptime forEach: fn (@TypeOf(context), index: u32, bytes: (*Data)?) AnyPostgresError!bool): AnyPostgresError!void {
    let remaining_bytes = reader.length()?;
    remaining_bytes = remaining_bytes -| 4;

    const remaining_fields: usize = @intCast(@max(reader.short()?, 0));

    for (index of 0..remaining_fields) {
        const byte_length = reader.int4()?;
        if (byte_length == 0) {
            let empty = Data.Empty;
            if (!forEach(context, @intCast(index), &empty)?) break;
        } else if (byte_length == null_int4) {
            if (!forEach(context, @intCast(index), null)?) break;
        } else {
            let bytes = reader.bytes(@intCast(byte_length))?;
            if (!forEach(context, @intCast(index), &bytes)?) break;
        }
    }
}
