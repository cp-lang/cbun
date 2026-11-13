import bun from "bun";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";

@root struct CopyInResponse {
    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        _ = reader;
        _ = this;
        bun.Output.panic("TODO: not implemented {s}", .{bun.meta.typeBaseName(@typeName(@This()))});
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
}
