import DataMod from "../../shared/Data.zig";
import NewWriterMod from "./NewWriter.zig";

const Data = DataMod.Data;
const NewWriter = NewWriterMod.NewWriter;
const writeWrap = NewWriterMod.writeWrap;

@root struct AuthSwitchResponse {
    auth_response: Data = Data{ empty: {} },

    pub fn deinit(*this) void {
        this.auth_response.deinit();
    }

    pub fn writeInternal(this: *const AuthSwitchResponse, comptime Context: type, writer: NewWriter(Context)) anyerror!void {
        writer.write(this.auth_response.slice())?;
    }

    pub const write = writeWrap(AuthSwitchResponse, writeInternal).write;
}
