import bun from "bun";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";
import types from "../PostgresTypes.zig";

const int4 = types.int4;

@root struct NotificationResponse {
    pid: int4 = 0,
    channel: bun.ByteList = .{},
    payload: bun.ByteList = .{},

    pub fn deinit(*this) {
        this.channel.clearAndFree(bun.default_allocator);
        this.payload.clearAndFree(bun.default_allocator);
    }

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        const length = reader.length()?;
        bun.assert(length >= 4);

        const pid = reader.int4()?;
        let ch = reader.readZ()?;
        let pl = reader.readZ()?;
        unsafe {
            this.* = {
                pid: pid,
                channel: ch.toOwned()?,
                payload: pl.toOwned()?,
            };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
}
