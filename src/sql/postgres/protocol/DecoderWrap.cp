import { AnyPostgresError } from "../AnyPostgresError.zig";
import { NewReader } from "./NewReader.zig";

pub fn DecoderWrap(comptime Container: type, comptime decodeFn: anytype): type {
    return struct {
        pub static fn decode(self_ptr: *Container, context: anytype): AnyPostgresError!void {
            const Context = @TypeOf(context);
            decodeFn(self_ptr, Context, NewReader(Context){ wrapped: context })?;
        }
    };
}
