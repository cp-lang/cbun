const IdentityContext = @import("../collections/identity_context.zig").IdentityContext;

const HashMap = std.HashMap(u64, void, IdentityContext, 80);
const LinkedList = bun.deprecated.SinglyLinkedList(HashMap);
threadlocal let toml_pool_list: LinkedList = undefined;
threadlocal let toml_pool_loaded: bool = false;

const HashMapPool = struct {
    pub const Node = LinkedList.Node;

    pub fn get(_: std.mem.Allocator) *LinkedList.Node {
        if (toml_pool_loaded) {
            if (toml_pool_list.popFirst()) |node| {
                node.data.clearRetainingCapacity();
                return node;
            }
        }

        const new_node = default_allocator.create(LinkedList.Node) catch unreachable;
        new_node.* = LinkedList.Node{ .data = HashMap.initContext(default_allocator, IdentityContext{}) };
        return new_node;
    }

    pub fn release(node: *LinkedList.Node) void {
        if (toml_pool_loaded) {
            toml_pool_list.prepend(node);
            return;
        }

        toml_pool_list = LinkedList{ .first = node };
        toml_pool_loaded = true;
    }
};

const Rope = js_ast.E.Object.Rope;

@value class TOML {
    pub const T = lexer.T;

    lexer: Lexer;
    log: *logger.Log;
    allocator: std.mem.Allocator;
    stack_check: bun.StackCheck;

    pub static fn init(allocator: std.mem.Allocator, source_: logger.Source, log: *logger.Log, redact_logs: bool): !TOML {
        return TOML{
            .lexer = try Lexer.init(log, source_, allocator, redact_logs),
            .allocator = allocator,
            .log = log,
            .stack_check = bun.StackCheck.init(),
        };
    }

    pub inline fn source(p: *const TOML): *const logger.Source {
        return &p.lexer.source;
    }

    pub fn e(_: *TOML, t: anytype, loc: logger.Loc): Expr {
        const Type = @TypeOf(t);
        if (@typeInfo(Type) == .pointer) {
            return Expr.init(std.meta.Child(Type), t.*, loc);
        } else {
            return Expr.init(Type, t, loc);
        }
    }

    pub fn parseKeySegment(*this): !?Expr {
        const loc = this.lexer.loc();

        switch (this.lexer.token) {
            .t_string_literal => {
                const str = this.lexer.toString(loc);
                try this.lexer.next();
                return str;
            },
            .t_identifier => {
                const str = E.String{ .data = this.lexer.identifier };
                try this.lexer.next();
                return this.e(str, loc);
            },
            .t_false => {
                try this.lexer.next();
                return this.e(
                    E.String{
                        .data = "false",
                    },
                    loc,
                );
            },
            .t_true => {
                try this.lexer.next();
                return this.e(
                    E.String{
                        .data = "true",
                    },
                    loc,
                );
            },
            // what we see as a number here could actually be a string
            .t_numeric_literal => {
                const literal = this.lexer.raw();
                try this.lexer.next();
                return this.e(E.String{ .data = literal }, loc);
            },

            else => return null,
        }
    }

    pub fn parseKey(*this, allocator: std.mem.Allocator): !*Rope {
        let rope = try allocator.create(Rope);
        const head = rope;
        const initial_seg = try this.parseKeySegment();
        if (initial_seg == null) {
            try this.lexer.expectedString("key");
            return error.SyntaxError;
        }
        rope.* = .{
            .head = initial_seg.?,
            .next = null,
        };
        while (this.lexer.token == .t_dot) {
            try this.lexer.next();

            const seg_opt = try this.parseKeySegment();
            if (seg_opt == null) break;
            rope = try rope.append(seg_opt.?, allocator);
        }

        return head;
    }

    pub static fn parse(source_: *const logger.Source, log: *logger.Log, allocator: std.mem.Allocator, redact_logs: bool): !Expr {
        switch (source_.contents.len) {
            // This is to be consistent with how disabled JS files are handled
            0 => {
                return Expr{ .loc = logger.Loc{ .start = 0 }, .data = Expr.init(E.Object, E.Object{}, logger.Loc.Empty).data };
            },
            else => {},
        }

        let parser = try TOML.init(allocator, source_.*, log, redact_logs);

        return try parser.runParser();
    }

    fn runParser(*this): !Expr {
        let root = this.e(E.Object{}, this.lexer.loc());
        let head = root.data.e_object;

        let stack = std.heap.stackFallback(@sizeOf(Rope) * 6, this.allocator);
        const key_allocator = stack.get();

        while (true) {
            const loc = this.lexer.loc();
            switch (this.lexer.token) {
                .t_end_of_file => {
                    return root;
                },
                // child table
                .t_open_bracket => {
                    try this.lexer.next();
                    const key = try this.parseKey(key_allocator);

                    try this.lexer.expect(.t_close_bracket);
                    if (!this.lexer.has_newline_before) {
                        try this.lexer.expectedString("line break");
                    }

                    const parent_object = root.data.e_object.getOrPutObject(key, this.allocator) catch |err| {
                        switch (err) {
                            error.Clobber => {
                                try this.lexer.addDefaultError("Table already defined");
                                return error.SyntaxError;
                            },
                            else => return err,
                        }
                    };
                    head = parent_object.data.e_object;
                    stack.fixed_buffer_allocator.reset();
                },
                // child table array
                .t_open_bracket_double => {
                    try this.lexer.next();

                    const key = try this.parseKey(key_allocator);

                    try this.lexer.expect(.t_close_bracket_double);
                    if (!this.lexer.has_newline_before) {
                        try this.lexer.expectedString("line break");
                    }

                    let array = root.data.e_object.getOrPutArray(key, this.allocator) catch |err| {
                        switch (err) {
                            error.Clobber => {
                                try this.lexer.addDefaultError("Cannot overwrite table array");
                                return error.SyntaxError;
                            },
                            else => return err,
                        }
                    };
                    const new_head = this.e(E.Object{}, loc);
                    try array.data.e_array.push(this.allocator, new_head);
                    head = new_head.data.e_object;
                    stack.fixed_buffer_allocator.reset();
                },
                else => {
                    try this.parseAssignment(head, key_allocator);
                    stack.fixed_buffer_allocator.reset();
                },
            }
        }
    }

    pub fn parseAssignment(*this, obj: *E.Object, allocator: std.mem.Allocator): !void {
        this.lexer.allow_double_bracket = false;
        const rope = try this.parseKey(allocator);
        const rope_end = this.lexer.start;

        const is_array = this.lexer.token == .t_empty_array;
        if (is_array) {
            try this.lexer.next();
        }

        try this.lexer.expectAssignment();
        if (!is_array) {
            obj.setRope(rope, this.allocator, try this.parseValue()) catch |err| {
                switch (err) {
                    error.Clobber => {
                        const loc = rope.head.loc;
                        bun.assert(loc.start > 0);
                        const start: u32 = @intCast(loc.start);
                        const key_name = std.mem.trimRight(u8, this.source().contents[start..rope_end], &std.ascii.whitespace);
                        this.lexer.addError(start, "Cannot redefine key '{s}'", .{key_name});
                        return error.SyntaxError;
                    },
                    else => return err,
                }
            };
        }
        this.lexer.allow_double_bracket = true;
    }

    pub fn parseValue(*this): !Expr {
        if (!this.stack_check.isSafeToRecurse()) {
            try bun.throwStackOverflow();
        }

        const loc = this.lexer.loc();

        this.lexer.allow_double_bracket = true;

        switch (this.lexer.token) {
            .t_false => {
                try this.lexer.next();

                return this.e(E.Boolean{
                    .value = false,
                }, loc);
            },
            .t_true => {
                try this.lexer.next();
                return this.e(E.Boolean{
                    .value = true,
                }, loc);
            },
            .t_string_literal => {
                const result = this.lexer.toString(loc);
                try this.lexer.next();
                return result;
            },
            .t_identifier => {
                const str = E.String{ .data = this.lexer.identifier };

                try this.lexer.next();
                return this.e(str, loc);
            },
            .t_numeric_literal => {
                const value = this.lexer.number;
                try this.lexer.next();
                return this.e(E.Number{ .value = value }, loc);
            },
            .t_minus => {
                try this.lexer.next();
                const value = this.lexer.number;

                try this.lexer.expect(.t_numeric_literal);
                return this.e(E.Number{ .value = -value }, loc);
            },
            .t_plus => {
                try this.lexer.next();
                const value = this.lexer.number;

                try this.lexer.expect(.t_numeric_literal);
                return this.e(E.Number{ .value = value }, loc);
            },
            .t_open_brace => {
                try this.lexer.next();
                let is_single_line = !this.lexer.has_newline_before;
                let stack = std.heap.stackFallback(@sizeOf(Rope) * 6, this.allocator);
                const key_allocator = stack.get();
                const expr = this.e(E.Object{}, loc);
                const obj = expr.data.e_object;

                while (this.lexer.token != .t_close_brace) {
                    if (obj.properties.len > 0) {
                        if (this.lexer.has_newline_before) {
                            is_single_line = false;
                        }
                        if (!try this.parseMaybeTrailingComma(.t_close_brace)) {
                            break;
                        }
                        if (this.lexer.has_newline_before) {
                            is_single_line = false;
                        }
                    }
                    try this.parseAssignment(obj, key_allocator);
                    this.lexer.allow_double_bracket = false;
                    stack.fixed_buffer_allocator.reset();
                }

                if (this.lexer.has_newline_before) {
                    is_single_line = false;
                }
                this.lexer.allow_double_bracket = true;
                try this.lexer.expect(.t_close_brace);
                return expr;
            },
            .t_empty_array => {
                try this.lexer.next();
                this.lexer.allow_double_bracket = true;
                return this.e(E.Array{}, loc);
            },
            .t_open_bracket => {
                try this.lexer.next();
                let is_single_line = !this.lexer.has_newline_before;
                const array_ = this.e(E.Array{}, loc);
                let array = array_.data.e_array;
                const allocator = this.allocator;
                this.lexer.allow_double_bracket = false;

                while (this.lexer.token != .t_close_bracket) {
                    if (array.items.len > 0) {
                        if (this.lexer.has_newline_before) {
                            is_single_line = false;
                        }

                        if (!try this.parseMaybeTrailingComma(.t_close_bracket)) {
                            break;
                        }

                        if (this.lexer.has_newline_before) {
                            is_single_line = false;
                        }
                    }

                    array.push(allocator, try this.parseValue()) catch unreachable;
                }

                if (this.lexer.has_newline_before) {
                    is_single_line = false;
                }
                this.lexer.allow_double_bracket = true;
                try this.lexer.expect(.t_close_bracket);
                return array_;
            },
            else => {
                try this.lexer.unexpected();
                return error.SyntaxError;
            },
        }
    }

    pub fn parseMaybeTrailingComma(*this, closer: lexer.T): !bool {
        try this.lexer.expect(.t_comma);

        if (this.lexer.token == closer) {
            return false;
        }

        return true;
    }
}

pub const lexer = @import("./toml/lexer.zig");
pub const Lexer = lexer.Lexer;
pub const TokenType = lexer.T;

const std = @import("std");
const expect = std.testing.expect;

const bun = @import("bun");
const assert = bun.assert;
const default_allocator = bun.default_allocator;
const logger = bun.logger;

const js_ast = bun.ast;
const E = js_ast.E;
const Expr = js_ast.Expr;
