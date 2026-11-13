import { AnyPostgresError } from "../AnyPostgresError.zig";
import { NewWriter } from "./NewWriter.zig";

pub fn WriteWrap(comptime Container: type, comptime writeFn: anytype): type {
    return struct {
        pub static fn write(self_ptr: *Container, context: anytype): AnyPostgresError!void {
            const Context = @TypeOf(context);
            writeFn(self_ptr, Context, NewWriter(Context){ wrapped: context })?;
        }
    };
}
