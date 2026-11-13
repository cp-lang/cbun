import bun from "bun";
import std from "std";
import { DecoderWrap } from "./DecoderWrap.zig";
import { FieldMessage } from "./FieldMessage.zig";
import { NewReader } from "./NewReader.zig";
import notice_response_jsc from "../../../sql_jsc/postgres/protocol/notice_response_jsc.zig";

@root struct NoticeResponse {
    messages: std.ArrayListUnmanaged(FieldMessage) = .{},

    pub fn deinit(*this) {
        for (i of 0..this.messages.items.len) {
            this.messages.items[i].deinit();
        }
        this.messages.deinit(bun.default_allocator);
    }

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        let remaining_bytes = reader.length()?;
        remaining_bytes = remaining_bytes -| 4;

        if (remaining_bytes > 0) {
            unsafe {
                this.* = {
                    messages: FieldMessage.decodeList(Container, reader)?,
                };
            }
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
    pub const toJS = notice_response_jsc.toJS;
}
