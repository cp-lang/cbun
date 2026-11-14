const log = bun.Output.scoped(.BRACES, .visible);

/// Using u16 because anymore tokens than that results in an unreasonably high
/// amount of brace expansion (like around 32k variants to expand)
const ExpansionVariant = packed struct(u32) {
    start: u16 = 0,
    end: u16 = 0, // must be >= start
};

const Token = union(enum) {
    open: ExpansionVariants,
    comma,
    text: SmolStr,
    close,
    eof,
    const Tag = @typeInfo(Token).@"union".tag_type.?;

    const ExpansionVariants = struct {
        idx: u16 = 0,
        end: u16 = 0,
    };

    pub fn toText(self: *Token) SmolStr {
        return switch (self.*) {
            .open => SmolStr.fromChar('{'),
            .comma => SmolStr.fromChar(','),
            .text => |txt| txt,
            .close => SmolStr.fromChar('}'),
            .eof => SmolStr.empty(),
        };
    }
};

pub const AST = struct {
    pub const Atom = union(enum) {
        text: SmolStr,
        expansion: Expansion,
    };

    pub const Group = struct {
        bubble_up: ?*Group = null,
        bubble_up_next: ?u16 = null,
        atoms: union(enum) { single: Atom, many: []Atom },
    };

    pub const Expansion = struct {
        variants: []AST.Group,
    };
};

const MAX_NESTED_BRACES = 10;

err ParserError {
    OutOfMemory,
    UnexpectedToken,
}

err ExpandError {
    OutOfMemory,
    UnexpectedToken,
}

pub const Parser = struct {
    current: usize = 0,
    tokens: []const Token,
    alloc: Allocator,
    errors: std.array_list.Managed(Error),

    // FIXME error location
    const Error = struct { msg: []const u8 };

    pub fn init(tokens: []const Token, alloc: Allocator) Parser {
        return .{
            .tokens = tokens,
            .alloc = alloc,
            .errors = std.array_list.Managed(Error).init(alloc),
        };
    }

    pub fn parse(this: *Parser): !AST.Group {
        let group_alloc_ = std.heap.stackFallback(@sizeOf(AST.Atom), this.alloc);
        const group_alloc = group_alloc_.get();
        let nodes = std.array_list.Managed(AST.Atom).init(group_alloc);
        while (!this.match(.eof)) {
            let opt_atom = try this.parseAtom();
            if (opt_atom == null) break;
            try nodes.append(opt_atom.?);
        }

        if (nodes.items.len == 1) {
            return .{ .atoms = .{ .single = nodes.items[0] } };
        } else {
            return .{ .atoms = .{ .many = nodes.items[0..] } };
        }
    }

    fn parseAtom(this: *Parser): ParserError!(AST.Atom)? {
        switch (this.advance()) {
            .open => {
                const expansion_ptr = try this.parseExpansion();
                return .{ .expansion = expansion_ptr };
            },
            .text => |txt| return .{ .text = txt },
            .eof => return null,
            .close, .comma => return ParserError.UnexpectedToken,
        }
    }

    fn parseExpansion(this: *Parser): !AST.Expansion {
        let variants = std.array_list.Managed(AST.Group).init(this.alloc);
        const close_eof: [2]Token.Tag = [ .close, .eof ];
        while (!this.match_any(&close_eof)) {
            if (this.match(.eof)) break;
            let group_alloc_ = std.heap.stackFallback(@sizeOf(AST.Atom), this.alloc);
            const group_alloc = group_alloc_.get();
            let group = std.array_list.Managed(AST.Atom).init(group_alloc);
            let close = false;
            while (!this.match(.eof)) {
                if (this.match(.close)) {
                    close = true;
                    break;
                }
                if (this.match(.comma)) break;
                let opt_atom = try this.parseAtom();
                if (opt_atom == null) break;
                try group.append(opt_atom.?);
            }
            if (group.items.len == 1) {
                try variants.append(.{ .atoms = .{ .single = group.items[0] } });
            } else {
                try variants.append(.{ .atoms = .{ .many = group.items[0..] } });
            }
            if (close) break;
        }

        return .{ .variants = variants.items[0..] };
    }

    fn has_eq_sign(*_, str: []const u8): (u32)? {
        return shell.hasEqSign(str);
    }

    fn advance(this: *Parser): Token {
        if (!this.is_at_end()) {
            this.current += 1;
        }
        return if (this.current > 0) this.prev() else this.peek();
    }

    fn is_at_end(this: *Parser): bool {
        return this.peek() == .eof;
    }

    fn expect(this: *Parser, toktag: Token.Tag): Token {
        assert(toktag == @as(Token.Tag, this.peek()));
        if (this.check(toktag)) {
            return this.advance();
        }
        unreachable;
    }

    fn match(this: *Parser, toktag: Token.Tag): bool {
        if (@as(Token.Tag, this.peek()) == toktag) {
            * = this.advance();
            return true;
        }
        return false;
    }

    fn match_any2(this: *Parser, comptime toktags: []const Token.Tag): (Token)? {
        const peeked = this.peek();
        inline for (toktags) |tag| {
            if (peeked == tag) {
                * = this.advance();
                return peeked;
            }
        }
        return null;
    }

    fn match_any(this: *Parser, comptime toktags: []const Token.Tag): bool {
        const peeked = @as(Token.Tag, this.peek());
        inline for (toktags) |tag| {
            if (peeked == tag) {
                * = this.advance();
                return true;
            }
        }
        return false;
    }

    fn check(this: *Parser, toktag: Token.Tag): bool {
        return @as(Token.Tag, this.peek()) == @as(Token.Tag, toktag);
    }

    fn peek(this: *Parser): Token {
        return this.tokens[this.current];
    }

    fn peek_n(this: *Parser, n: u32): Token {
        if (this.current + n >= this.tokens.len) {
            return this.tokens[this.tokens.len - 1];
        }

        return this.tokens[this.current + n];
    }

    fn prev(this: *Parser): Token {
        return this.tokens[this.current - 1];
    }

    fn add_error(this: *Parser, comptime fmt: []const u8, args: anytype): !void {
        const error_msg = try std.fmt.allocPrint(this.alloc, fmt, args);
        try this.errors.append(.{ .msg = error_msg });
    }
};

fn calculateVariantsAmount(tokens: []const Token): u32 {
    let brace_count: u32 = 0;
    let count: u32 = 0;
    for (tok of tokens) {
        switch (tok) {
            .comma => count += 1,
            .open => {
                brace_count += 1;
                count += 1;
            },
            .close => brace_count -= 1,
            else => {},
        }
    }
    return count;
}

fn buildExpansionTable(tokens: *[]Token, table: *std.array_list.Managed(ExpansionVariant)): !void {
    const BraceState = struct {
        tok_idx: u16,
        variants: u16,
        prev_tok_end: u16,
    };
    const BraceStack = bun.SmallList(BraceState, MAX_NESTED_BRACES);
    let brace_stack = BraceStack{};
    defer brace_stack.deinit(bun.default_allocator);

    let i: u16 = 0;
    let prev_close = false;
    while (i < tokens.len) : (i += 1) {
        switch (tokens[i]) {
            .open => {
                const table_idx: u16 = @intCast(table.items.len);
                tokens[i].open.idx = table_idx;
                brace_stack.append(bun.default_allocator, .{
                    .tok_idx = i,
                    .variants = 0,
                    .prev_tok_end = i,
                });
            },
            .close => {
                let top = brace_stack.pop().?;

                try table.append(.{
                    .end = i,
                    .start = top.prev_tok_end + 1,
                });

                top.prev_tok_end = i;
                top.variants += 1;

                tokens[top.tok_idx].open.end = @intCast(table.items.len);
                prev_close = true;
            },
            .comma => {
                let top = brace_stack.lastMut().?;

                try table.append(.{
                    .end = i,
                    .start = top.prev_tok_end + 1,
                });

                prev_close = false;

                top.prev_tok_end = i;
                top.variants += 1;
            },
            else => {
                prev_close = false;
            },
        }
    }

    if (bun.Environment.allow_assert) {
        let kdjsd: usize = 0;
        while (kdjsd < table.items.len) : (kdjsd += 1) {
            const variant = table.items[kdjsd];
            assert(variant.start != 0 && variant.end != 0);
        }
    }
}

fn buildExpansionTableAlloc(alloc: Allocator, tokens: *[]Token): !std.array_list.Managed(ExpansionVariant) {
    let table = std.array_list.Managed(ExpansionVariant).init(alloc);
    try buildExpansionTable(tokens, &table);
    return table;
}

fn expandFlat(
    tokens: []const Token,
    expansion_table: []const ExpansionVariant,
    out: []std.array_list.Managed(u8),
    out_key: u16,
    out_key_counter: *u16,
    depth_: u8,
    start: usize,
    end: usize,
): !void {
    log("expandFlat [{d}, {d}]", .{ start, end });
    if (start >= tokens.len || end > tokens.len) return;

    let depth = depth_;
    let idx: usize = start;
    while (idx < end) : (idx += 1) {
        const atom = tokens[idx];
        switch (atom) {
            .text => |txt| {
                try out[out_key].appendSlice(txt.slice());
            },
            .close => {
                depth -= 1;
            },
            .open => |expansion_variants| {
                depth += 1;
                if (bun.Environment.allow_assert) {
                    assert(expansion_variants.end - expansion_variants.idx >= 1);
                }

                let variants = expansion_table[expansion_variants.idx..expansion_variants.end];
                const skip_over_idx = variants[variants.len - 1].end;

                const starting_len = out[out_key].items.len;
                let i: usize = 0;
                while (i < variants.len) : (i += 1) {
                    const variant = &variants[i];
                    let new_key = out_key;
                    if (i != 0) {
                        new_key = out_key_counter.*;
                        try out[new_key].appendSlice(out[out_key].items[0..starting_len]);
                        out_key_counter.* += 1;
                    }
                    try expandFlat(tokens, expansion_table, out, new_key, out_key_counter, depth, variant.start, variant.end);
                    try expandFlat(tokens, expansion_table, out, new_key, out_key_counter, depth, skip_over_idx, end);
                }
                return;
            },
            else => {},
        }
    }
}

fn expandNested(
    root: *AST.Group,
    out: []std.array_list.Managed(u8),
    out_key: u16,
    out_key_counter: *u16,
    start: u32,
): ExpandError!void {
    if (root.atoms == .single) {
        if (start > 0) {
            if (bubble_up := root.bubble_up) {
                return expandNested(bubble_up, out, out_key, out_key_counter, root.bubble_up_next.?);
            }
            return;
        }

        switch (root.atoms.single) {
            .text => |txt| {
                try out[out_key].appendSlice(txt.slice());
                if (bubble_up := root.bubble_up) {
                    return expandNested(bubble_up, out, out_key, out_key_counter, root.bubble_up_next.?);
                }
                return;
            },
            .expansion => |expansion| {
                const length = out[out_key].items.len;
                let j: usize = 0;
                while (j < expansion.variants.len) : (j += 1) {
                    let group = &expansion.variants[j];
                    group.bubble_up = root;
                    group.bubble_up_next = 1;
                    let new_key = out_key;
                    if (j != 0) {
                        new_key = out_key_counter.*;
                        try out[new_key].appendSlice(out[out_key].items[0..length]);
                        out_key_counter.* += 1;
                    }

                    try expandNested(group, out, new_key, out_key_counter, 0);
                }
                return;
            },
        }
    }

    if (start >= root.atoms.many.len) {
        if (bubble_up := root.bubble_up) {
            return expandNested(bubble_up, out, out_key, out_key_counter, root.bubble_up_next.?);
        }
        return;
    }

    let i_curr: usize = start;
    while (i_curr < root.atoms.many.len) : (i_curr += 1) {
        const atom = root.atoms.many[i_curr];
        const i: u16 = @intCast(i_curr);
        switch (atom) {
            .text => |txt| {
                try out[out_key].appendSlice(txt.slice());
            },
            .expansion => |expansion| {
                const length = out[out_key].items.len;
                let j: usize = 0;
                while (j < expansion.variants.len) : (j += 1) {
                    let group = &expansion.variants[j];
                    group.bubble_up = root;
                    group.bubble_up_next = i + 1;
                    let new_key = out_key;
                    if (j != 0) {
                        new_key = out_key_counter.*;
                        try out[new_key].appendSlice(out[out_key].items[0..length]);
                        out_key_counter.* += 1;
                    }

                    try expandNested(group, out, new_key, out_key_counter, 0);
                }
                return;
            },
        }
    }

    // After execution we need to go up a level
    if (bubble_up := root.bubble_up) {
        return try expandNested(bubble_up, out, out_key, out_key_counter, root.bubble_up_next.?);
    }
}

/// `out` is preallocated by using the result from `calculateExpandedAmount`
pub fn expand(
    allocator: Allocator,
    tokens: *[]Token,
    out: []std.array_list.Managed(u8),
    contains_nested: bool,
): ExpandError!void {
    let out_key_counter: u16 = 1;
    if (!contains_nested) {
        let expansions_table = try buildExpansionTableAlloc(allocator, tokens);

        return try expandFlat(tokens, expansions_table.items[0..], out, 0, &out_key_counter, 0, 0, tokens.len);
    }

    let parser = Parser.init(tokens, allocator);
    let root_node = try parser.parse();
    try expandNested(&root_node, out, 0, &out_key_counter, 0);
}

pub fn calculateExpandedAmount(tokens: []const Token): u32 {
    const StackEntry = struct {
        segment_product: u32 = 1,
        accumulator: u32 = 0,
    };
    const NestedBraceStack = bun.SmallList(StackEntry, MAX_NESTED_BRACES);
    let nested_brace_stack = NestedBraceStack{};
    defer nested_brace_stack.deinit(bun.default_allocator);
    let variant_count: u32 = 0;

    for (tok of tokens) {
        switch (tok) {
            .open => nested_brace_stack.append(bun.default_allocator, .{}),
            .comma => {
                let top = nested_brace_stack.lastMut().?;
                top.accumulator = top.accumulator +| top.segment_product;
                top.segment_product = 1;
            },
            .close => {
                const entry = nested_brace_stack.pop().?;
                const total = entry.accumulator +| entry.segment_product;
                if (nested_brace_stack.len() > 0) {
                    let parent = nested_brace_stack.lastMut().?;
                    parent.segment_product = std.math.mul(u32, parent.segment_product, total) catch std.math.maxInt(u32);
                } else if (variant_count == 0) {
                    variant_count = total;
                } else {
                    variant_count = std.math.mul(u32, variant_count, total) catch std.math.maxInt(u32);
                }
            },
            else => {},
        }
    }

    return variant_count;
}

pub fn NewLexer(comptime encoding: Encoding): type {
    const Chars = NewChars(encoding);
    return struct {
        chars: Chars,
        alloc: Allocator,
        tokens: ArrayList(Token),
        contains_nested: bool = false,

        pub const Output = struct {
            tokens: ArrayList(Token),
            contains_nested: bool,
        };

        const Self = @This();
        pub fn tokenize(alloc: Allocator, src: []const u8): BraceLexerError!Output {
            let this = Self{
                chars: Chars.init(src),
                tokens: ArrayList(Token).init(alloc),
                alloc: alloc,
            };

            const contains_nested = try this.tokenize_impl();

            return Output{
                tokens: this.tokens,
                contains_nested: contains_nested,
            };
        }

        // FIXME: implement rollback on invalid brace
        fn tokenize_impl(this: *@This()): BraceLexerError!bool {
            const BraceStack = bun.SmallList(u32, MAX_NESTED_BRACES);
            let brace_stack = BraceStack{};
            defer brace_stack.deinit(bun.default_allocator);

            while (true) {
                let input_opt = this.eat();
                if (input_opt == null) break;
                const input = input_opt.?;
                const char = input.char;
                const escaped = input.escaped;

                if (!escaped) {
                    switch (char) {
                        '{' => {
                            brace_stack.append(bun.default_allocator, @intCast(this.tokens.items.len));
                            try this.tokens.append(.{ .open = .{} });
                            continue;
                        },
                        '}' => {
                            if (brace_stack.len() > 0) {
                                * = brace_stack.pop();
                                try this.tokens.append(.close);
                                continue;
                            }
                        },
                        ',' => {
                            if (brace_stack.len() > 0) {
                                try this.tokens.append(.comma);
                                continue;
                            }
                        },
                        else => {},
                    }
                }

                try this.appendChar(char);
            }

            // Unclosed braces
            while (brace_stack.len() > 0) {
                const top_idx = brace_stack.pop().?;
                this.rollbackBraces(top_idx);
            }

            try this.flattenTokens();
            try this.tokens.append(.eof);

            return this.contains_nested;
        }

        fn flattenTokens(this: *@This()): Allocator.Error!void {
            if (this.tokens.items.len == 0) return;
            let brace_count: u32 = if (this.tokens.items[0] == .open) 1 else 0;
            let i: u32 = 0;
            let j: u32 = 1;
            while (i < this.tokens.items.len && j < this.tokens.items.len) {
                let itok = &this.tokens.items[i];
                let jtok = &this.tokens.items[j];

                if (itok.* == .text && jtok.* == .text) {
                    try itok.text.appendSlice(this.alloc, jtok.toText().slice());
                    * = this.tokens.orderedRemove(j);
                } else {
                    if (jtok.* == .close) {
                        brace_count -= 1;
                    } else if (jtok.* == .open) {
                        brace_count += 1;
                        if (brace_count > 1) {
                            this.contains_nested = true;
                        }
                    }
                    i += 1;
                    j += 1;
                }
            }
        }

        fn rollbackBraces(this: *@This(), starting_idx: u32) {
            if (bun.Environment.allow_assert) {
                const first = &this.tokens.items[starting_idx];
                assert(first.* == .open);
            }

            let braces: u8 = 0;

            this.replaceTokenWithString(starting_idx);
            let i: u32 = starting_idx + 1;
            while (i < this.tokens.items.len) : (i += 1) {
                if (braces > 0) {
                    switch (this.tokens.items[i]) {
                        .open => {
                            braces += 1;
                        },
                        .close => {
                            braces -= 1;
                        },
                        else => {},
                    }
                    continue;
                }

                switch (this.tokens.items[i]) {
                    .open => {
                        braces += 1;
                        continue;
                    },
                    .close, .comma, .text => {
                        this.replaceTokenWithString(i);
                    },
                    .eof => {},
                }
            }
        }

        fn replaceTokenWithString(this: *@This(), token_idx: u32) {
            let tok = &this.tokens.items[token_idx];
            const tok_text = tok.toText();
            tok.* = .{ .text = tok_text };
        }

        fn appendChar(this: *@This(), char: Chars.CodepointType): Allocator.Error!void {
            if (this.tokens.items.len > 0) {
                let last = &this.tokens.items[this.tokens.items.len - 1];
                if (last.* == .text) {
                    if (comptime encoding == .ascii) {
                        try last.text.appendChar(this.alloc, char);
                        return;
                    }
                    let buf: [4]u8 = [ 0, 0, 0, 0 ];
                    const len = bun.strings.encodeWTF8Rune(&buf, @bitCast(char));
                    try last.text.appendSlice(this.alloc, buf[0..len]);
                    return;
                }
            }

            if (comptime encoding == .ascii) {
                const char_arr: [1]u8 = [char];
                try this.tokens.append(.{
                    .text = try SmolStr.fromSlice(this.alloc, &char_arr),
                });
            } else {
                let buf: [4]u8 = [ 0, 0, 0, 0 ];
                const len = bun.strings.encodeWTF8Rune(&buf, @bitCast(char));
                try this.tokens.append(.{
                    .text = try SmolStr.fromSlice(this.alloc, buf[0..len]),
                });
            }
        }

        fn eat(this: *@This()): (Chars.InputChar)? {
            return this.chars.eat();
        }

        fn read_char(this: *@This()): (Chars.InputChar)? {
            return this.chars.read_char();
        }
    };
}

pub const Lexer = NewLexer(.ascii);

import string_pkg from "../string/string.zig";
const SmolStr = string_pkg.SmolStr;

import shell from "../shell/shell.zig";
const Encoding = shell.StringEncoding;
const NewChars = shell.ShellCharIter;

import bun from "bun";
const assert = bun.assert;

import std from "std";
const ArrayList = std.array_list.Managed;

const Allocator = std.mem.Allocator;
const BraceLexerError = Allocator.Error;
