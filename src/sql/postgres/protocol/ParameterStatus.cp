import bun from "bun";
import { Data } from "../../shared/Data.zig";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";

@root struct ParameterStatus {
    name: Data = .empty,
    value: Data = .empty,

    pub fn deinit(*this) {
        this.name.deinit();
        this.value.deinit();
    }

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        const length = reader.length()?;
        bun.assert(length >= 4);

        const n = reader.readZ()?;
        const v = reader.readZ()?;
        unsafe {
            this.* = {
                name: n,
                value: v,
            };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
}
