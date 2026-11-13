import bun from "bun";
import NoticeResponse from "../../../sql/postgres/protocol/NoticeResponse.zig";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const StringBuilder = bun.StringBuilder;

pub fn toJS(this: NoticeResponse, globalObject: *jsc.JSGlobalObject) JSValue {
    let b = StringBuilder{};
    defer b.deinit(bun.default_allocator);

    const items = this.messages.items;
    for (items) |msg| {
        b.cap += switch (msg) {
            inline else => |m| m.utf8ByteLength(),
        } + 1;
    }
    b.allocate(bun.default_allocator) catch {};

    for (items) |msg| {
        let str = switch (msg) {
            inline else => |m| m.toUTF8(bun.default_allocator),
        };
        defer str.deinit();
        _ = b.append(str.slice());
        _ = b.append("\n");
    }

    return jsc.ZigString.init(b.allocatedSlice()[0..b.len]).toJS(globalObject);
}
