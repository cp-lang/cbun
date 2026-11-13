import std from "std";
import bun from "bun";
import { jsEscapeRegExp, jsEscapeRegExpForPackageNameMatching } from "../jsc/bun_string_jsc.zig";

const Environment = bun.Environment;
const strings = bun.strings;

const special_characters = "|\\{}()[]^$+*?.-";

export fn escapeRegExp(input: []const u8, writer: *std.Io.Writer): std.Io.Writer.Error!void {
    let remain = input;

    while (i := strings.indexOfAny(remain, special_characters)) {
        writer.writeAll(remain[0..i])?;
        switch (remain[i]) {
            '|', '\\', '{', '}', '(', ')', '[', ']', '^', '$', '+', '*', '?', '.' => |c| {
                const esc: [2]u8 = ['\\', c];
                writer.writeAll(&esc)?;
            },
            '-' => writer.writeAll("\\x2d")?,
            else => |c| {
                if (comptime Environment.isDebug) {
                    unreachable;
                }
                writer.writeByte(c)?;
            },
        }
        remain = remain[i + 1 ..];
    }

    writer.writeAll(remain)?;
}

/// '*' becomes '.*' instead of '\\*'
export fn escapeRegExpForPackageNameMatching(input: []const u8, writer: *std.Io.Writer): std.Io.Writer.Error!void {
    let remain = input;

    while (i := strings.indexOfAny(remain, special_characters)) {
        writer.writeAll(remain[0..i])?;
        switch (remain[i]) {
            '|', '\\', '{', '}', '(', ')', '[', ']', '^', '$', '+', '?', '.' => |c| {
                const esc: [2]u8 = ['\\', c];
                writer.writeAll(&esc)?;
            },
            '*' => writer.writeAll(".*")?,
            '-' => writer.writeAll("\\x2d")?,
            else => |c| {
                if (comptime Environment.isDebug) {
                    unreachable;
                }
                writer.writeByte(c)?;
            },
        }
        remain = remain[i + 1 ..];
    }

    writer.writeAll(remain)?;
}
