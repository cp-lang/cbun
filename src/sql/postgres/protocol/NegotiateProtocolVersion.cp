import std from "std";
import bun from "bun";
import { NewReader } from "./NewReader.zig";
import int_types from "../types/int_types.zig";

const int4 = int_types.int4;
const String = bun.String;

@root struct NegotiateProtocolVersion {
    version: int4 = 0,
    unrecognized_options: std.ArrayListUnmanaged(String) = .{},

    pub fn decodeInternal(
        *this,
        comptime Container: type,
        reader: NewReader(Container),
    ): void! {
        const length = reader.length()?;
        bun.assert(length >= 4);

        const version = reader.int4()?;
        unsafe {
            this.* = {
                version: version,
            };
        }

        const unrecognized_options_count: u32 = @intCast(@max(reader.int4()?, 0));
        this.unrecognized_options.ensureTotalCapacity(bun.default_allocator, unrecognized_options_count)?;
        errdefer {
            for (i of 0..this.unrecognized_options.items.len) {
                this.unrecognized_options.items[i].deinit();
            }
            this.unrecognized_options.deinit(bun.default_allocator);
        }
        for (_i of 0..unrecognized_options_count) {
            let option = reader.readZ()?;
            if (option.slice().len == 0) break;
            defer option.deinit();
            this.unrecognized_options.appendAssumeCapacity(
                String.borrowUTF8(option),
            );
        }
    }
}
