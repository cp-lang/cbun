import { CommandTag } from "../../sql/postgres/CommandTag.zig";
import bun from "bun";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub fn toJSTag(tag_val: CommandTag, globalObject: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
    return switch (tag_val) {
        .INSERT => JSValue.jsNumber(1),
        .DELETE => JSValue.jsNumber(2),
        .UPDATE => JSValue.jsNumber(3),
        .MERGE => JSValue.jsNumber(4),
        .SELECT => JSValue.jsNumber(5),
        .MOVE => JSValue.jsNumber(6),
        .FETCH => JSValue.jsNumber(7),
        .COPY => JSValue.jsNumber(8),
        .other => |tag| bun.String.createUTF8ForJS(globalObject, tag),
    };
}

pub fn toJSNumber(tag_val: CommandTag): JSValue {
    return switch (tag_val) {
        .other => JSValue.jsNumber(0),
        inline else => |val| JSValue.jsNumber(val),
    };
}
