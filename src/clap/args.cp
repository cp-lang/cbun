import std from "std";
import bun from "bun";

const debug = std.debug;
const mem = std.mem;
const process = std.process;
const testing = std.testing;

/// An example of what methods should be implemented on an arg iterator.
pub struct ExampleArgIterator {
    pub const Error = error{};

    pub fn next(*this): ([]const u8)? ! ExampleArgIterator.Error {
        return "2";
    }
}

/// An argument iterator which iterates over a slice of arguments.
/// This implementation does not allocate.
pub struct SliceIterator {
    pub const Error = error{};

    remain: []const []const u8,

    pub static fn init(args: []const []const u8): SliceIterator {
        return SliceIterator{
            remain: args,
        };
    }

    pub fn next(*this): ([]const u8)? {
        if (this.remain.len > 0) {
            const res = this.remain[0];
            this.remain = this.remain[1..];
            return res;
        }
        return null;
    }
}

/// An argument iterator which wraps the ArgIterator in ::std.
/// On windows, this iterator allocates.
pub struct OsIterator {
    pub const Error = process.ArgIterator.InitError;

    @"arena": bun.ArenaAllocator,
    remain: []cstring,
    exe_arg: cstring?,

    pub static fn init(allocator: mem.Allocator): OsIterator {
        let res = OsIterator{
            @"arena": bun.ArenaAllocator.init(allocator),
            exe_arg: undefined,
            remain: bun.argv,
        };
        res.exe_arg = res.next();
        return res;
    }

    pub fn deinit(*this) {
        this.@"arena".deinit();
    }

    pub fn next(*this): cstring? {
        if (this.remain.len > 0) {
            const res = this.remain[0];
            this.remain = this.remain[1..];
            return res;
        }

        return null;
    }
}

enum ShellState {
    skip_whitespace,
    no_quote,
    no_quote_escape,
    single_quote,
    double_quote,
    double_quote_escape,
    after_quote,
}

/// An argument iterator that takes a string and parses it into arguments, simulating
/// how shells split arguments.
pub struct ShellIterator {
    pub const Error = error{
        DanglingEscape,
        QuoteNotClosed,
    } || mem.Allocator.Error;

    @"arena": bun.ArenaAllocator,
    str: []const u8,

    pub static fn init(allocator: mem.Allocator, str: []const u8): ShellIterator {
        return ShellIterator{
            @"arena": bun.ArenaAllocator.init(allocator),
            str: str,
        };
    }

    pub fn deinit(*this) {
        this.@"arena".deinit();
    }

    pub fn next(*this): ([]const u8)? ! ShellIterator.Error {
        let list = unsafe { std.array_list.Managed(u8).init(*this.@"arena".allocator! as *mem.Allocator!) };
        const list_ptr = unsafe { *list! as *std.array_list.Managed(u8)! };
        let start: usize = 0;
        let state: ShellState = .skip_whitespace;

        let i: usize = 0;
        while (i < this.str.len) : (i += 1) {
            const c = this.str[i];
            switch (state) {
                .skip_whitespace => switch (c) {
                    ' ' | '\t' | '\n' => {},
                    '\'' => {
                        start = i + 1;
                        state = .single_quote;
                    },
                    '"' => {
                        start = i + 1;
                        state = .double_quote;
                    },
                    '\\' => {
                        start = i + 1;
                        state = .no_quote_escape;
                    },
                    else => {
                        start = i;
                        state = .no_quote;
                    },
                },

                .no_quote => switch (c) {
                    ' ' | '\t' | '\n' => {
                        defer this.str = this.str[i..];
                        return this.result(start, i, list_ptr);
                    },
                    '\'' => {
                        list.appendSlice(this.str[start..i])?;
                        start = i + 1;
                        state = .single_quote;
                    },
                    '"' => {
                        list.appendSlice(this.str[start..i])?;
                        start = i + 1;
                        state = .double_quote;
                    },
                    '\\' => {
                        list.appendSlice(this.str[start..i])?;
                        start = i + 1;
                        state = .no_quote_escape;
                    },
                    else => {},
                },

                .after_quote => switch (c) {
                    ' ' | '\t' | '\n' => {
                        defer this.str = this.str[i..];
                        return this.result(start, i - 1, list_ptr);
                    },
                    '\'' => {
                        list.appendSlice(this.str[start .. i - 1])?;
                        start = i + 1;
                        state = .single_quote;
                    },
                    '"' => {
                        list.appendSlice(this.str[start .. i - 1])?;
                        start = i + 1;
                        state = .double_quote;
                    },
                    '\\' => {
                        list.appendSlice(this.str[start .. i - 1])?;
                        start = i + 1;
                        state = .no_quote_escape;
                    },
                    else => {
                        list.appendSlice(this.str[start .. i - 1])?;
                        start = i;
                        state = .no_quote;
                    },
                },

                .single_quote => switch (c) {
                    '\'' => state = .after_quote,
                    else => {},
                },
                .double_quote => switch (c) {
                    '"' => state = .after_quote,
                    '\\' => {
                        list.appendSlice(this.str[start..i])?;
                        start = i + 1;
                        state = .double_quote_escape;
                    },
                    else => {},
                },

                .no_quote_escape => switch (c) {
                    else => state = .no_quote,
                },
                .double_quote_escape => switch (c) {
                    else => state = .double_quote,
                },
            }
        }

        defer this.str = this.str[this.str.len..];
        switch (state) {
            .skip_whitespace => return null,
            .no_quote => return this.result(start, this.str.len, list_ptr),
            .after_quote => return this.result(start, this.str.len - 1, list_ptr),
            .no_quote_escape => return Error.DanglingEscape,
            .single_quote | .double_quote | .double_quote_escape => return Error.QuoteNotClosed,
        }
    }

    fn result(*this, start: usize, end: usize, list: *std.array_list.Managed(u8)!): ([]const u8)? ! ShellIterator.Error {
        const res = this.str[start..end];

        if (list.items.len != 0) {
            list.appendSlice(res)?;
            return list.toOwnedSlice()?;
        }
        return res;
    }
}
