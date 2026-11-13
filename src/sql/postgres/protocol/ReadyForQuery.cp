import bun from "bun";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";
import { TransactionStatusIndicator } from "./TransactionStatusIndicator.zig";

@root struct ReadyForQuery {
    status: TransactionStatusIndicator = .I,

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        const length = reader.length()?;
        bun.assert(length >= 4);

        const status = reader.int(u8)?;
        unsafe {
            this.* = {
                status: @enumFromInt(status),
            };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
}
