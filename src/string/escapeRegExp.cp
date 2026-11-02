// src/string/escapeRegExp.cp
// Bun escapeRegExp module ported to CP (Concurrent Plus) language

zig {
    const bun = @import("bun");
    const Environment = bun.Environment;
    const strings = bun.strings;

    pub const jsEscapeRegExp = @import("../jsc/bun_string_jsc.zig").jsEscapeRegExp;
    pub const jsEscapeRegExpForPackageNameMatching = @import("../jsc/bun_string_jsc.zig").jsEscapeRegExpForPackageNameMatching;
}

const special_characters: string = "|\\{}()[]^$+*?.-";

pub fn escapeRegExp(input: string, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
    let mut remain = input;

    while (true) {
        let i_opt = strings.indexOfAny(remain, special_characters);
        if (i_opt == null) {
            break;
        }
        let i = (i_opt ?? 0) as usize;
        writer.writeAll(remain[0..i])?;
        let c = remain[i];
        switch (c) {
            '|' | '\\' | '{' | '}' | '(' | ')' | '[' | ']' | '^' | '$' | '+' | '*' | '?' | '.' => {
                writer.writeByte('\\')?;
                writer.writeByte(c)?;
            },
            '-' => {
                writer.writeAll("\\x2d")?;
            },
            * => {
                writer.writeByte(c)?;
            }
        }
        remain = remain[(i + 1)..];
    }

    writer.writeAll(remain)?;
}

/// '*' becomes '.*' instead of '\\*'
pub fn escapeRegExpForPackageNameMatching(input: string, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
    let mut remain = input;

    while (true) {
        let i_opt = strings.indexOfAny(remain, special_characters);
        if (i_opt == null) {
            break;
        }
        let i = (i_opt ?? 0) as usize;
        writer.writeAll(remain[0..i])?;
        let c = remain[i];
        switch (c) {
            '|' | '\\' | '{' | '}' | '(' | ')' | '[' | ']' | '^' | '$' | '+' | '?' | '.' => {
                writer.writeByte('\\')?;
                writer.writeByte(c)?;
            },
            '*' => {
                writer.writeAll(".*")?;
            },
            '-' => {
                writer.writeAll("\\x2d")?;
            },
            * => {
                writer.writeByte(c)?;
            }
        }
        remain = remain[(i + 1)..];
    }

    writer.writeAll(remain)?;
}
