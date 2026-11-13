import bun from "bun";
import { Data } from "../../shared/Data.zig";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";

@root struct CommandComplete {
    command_tag: Data = .empty,

    pub fn deinit(*this) {
        this.command_tag.deinit();
    }

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        const length = reader.length()?;
        bun.assert(length >= 4);

        const tag = reader.readZ()?;
        unsafe {
            this.* = { command_tag: tag };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
}
