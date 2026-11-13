import FieldDescription from "./FieldDescription.zig";
import bun from "bun";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";

@root struct RowDescription {
    fields: []FieldDescription = &[],

    pub fn deinit(*this) {
        for (i of 0..this.fields.len) {
            this.fields[i].deinit();
        }
        bun.default_allocator.free(this.fields);
    }

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        let remaining_bytes = reader.length()?;
        remaining_bytes = remaining_bytes -| 4;

        const field_count: usize = @intCast(@max(reader.short()?, 0));
        let fields = bun.default_allocator.alloc(FieldDescription, field_count)?;
        let remaining = fields;
        errdefer {
            for (i of 0 .. field_count - remaining.len) {
                fields[i].deinit();
            }
            bun.default_allocator.free(fields);
        }
        while (remaining.len > 0) {
            remaining[0].decodeInternal(Container, reader)?;
            remaining = remaining[1..];
        }
        unsafe {
            this.* = { fields: fields };
        }
    }

    pub const decode = DecoderWrap(@This(), decodeInternal).decode;
}
