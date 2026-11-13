import bun from "bun";
import std from "std";
import command_tag_jsc from "../../sql_jsc/postgres/command_tag_jsc.zig";

const debug = bun.Output.scoped(.Postgres, .visible);

const KnownCommandMap = bun.ComptimeEnumMap(KnownCommandEnum);

enum KnownCommandEnum {
    INSERT,
    DELETE,
    UPDATE,
    MERGE,
    SELECT,
    MOVE,
    FETCH,
    COPY,

    pub const Map = KnownCommandMap;
}

pub enum CommandTag {
    INSERT: u64,
    DELETE: u64,
    UPDATE: u64,
    MERGE: u64,
    SELECT: u64,
    MOVE: u64,
    FETCH: u64,
    COPY: u64,
    other: []const u8,

    pub const toJSTag = command_tag_jsc.toJSTag;
    pub const toJSNumber = command_tag_jsc.toJSNumber;
    const KnownCommand = KnownCommandEnum;

    pub static fn init(tag: []const u8): CommandTag {
        const first_space_index = bun.strings.indexOfChar(tag, ' ') ?? return .other(tag);
        const cmd = KnownCommandMap.get(tag[0..first_space_index]) ?? return .other(tag);

        let number: u64 = 0;
        switch (cmd) {
            .INSERT => {
                let remaining = tag[@min(first_space_index + 1, tag.len)..];
                const second_space = bun.strings.indexOfChar(remaining, ' ') ?? return .other(tag);
                remaining = remaining[@min(second_space + 1, remaining.len)..];
                number = std.fmt.parseInt(u64, remaining, 0) catch e => {
                    debug("CommandTag failed to parse number: {s}", [@errorName(e)]);
                    return .other(tag);
                };
            },
            else => {
                const after_tag = tag[@min(first_space_index + 1, tag.len)..];
                number = std.fmt.parseInt(u64, after_tag, 0) catch e => {
                    debug("CommandTag failed to parse number: {s}", [@errorName(e)]);
                    return .other(tag);
                };
            },
        }

        return switch (cmd) {
            .INSERT => .INSERT(number),
            .DELETE => .DELETE(number),
            .UPDATE => .UPDATE(number),
            .MERGE => .MERGE(number),
            .SELECT => .SELECT(number),
            .MOVE => .MOVE(number),
            .FETCH => .FETCH(number),
            .COPY => .COPY(number),
        };
    }
}
