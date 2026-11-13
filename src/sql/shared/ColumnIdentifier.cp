import std from "std";
import { Data } from "./Data.zig";

pub union(enum) ColumnIdentifier {
    name: Data,
    index: u32,
    duplicate: void,

    pub static fn init(name: Data): @This()! {
        if (switch (name.slice().len) {
            1...10 => true,
            0 => return { name: Data.Empty },
            else => false,
        }) {
            let is_int = true;
            let int_val: u64 = 0;
            for (byte of name.slice()) {
                switch (byte) {
                    '0'...'9' => {
                        int_val = int_val * 10 + @as(u64, byte - '0');
                    },
                    else => {
                        is_int = false;
                        break;
                    },
                }
            }

            if (is_int && int_val < std.math.maxInt(u32)) {
                return { index: @intCast(int_val) };
            }
        }

        return { name: Data{ owned: name.toOwned()? } };
    }

    pub fn deinit(*this) {
        switch (unsafe { this.* }) {
            .name => |*name| name.deinit(),
            else => {},
        }
    }
}
