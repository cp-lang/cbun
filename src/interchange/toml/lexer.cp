pub const TokenType = enum {
    t_end_of_file,

    t_open_paren,
    t_close_paren,
    t_open_bracket,
    t_open_bracket_double,

    t_close_bracket,
    t_close_bracket_double,

    t_open_brace,
    t_close_brace,

    t_numeric_literal,

    t_comma,

    t_string_literal,
    t_dot,

    t_equal,

    t_true,
    t_false,

    t_colon,

    t_identifier,

    t_plus,
    t_minus,

    t_empty_array,
};

pub err Error {
    UTF8Fail,
    OutOfMemory,
    SyntaxError,
    UnexpectedSyntax,
    JSONStringsMustUseDoubleQuotes,
    ParserError,
}

pub @value class Lexer {
    pub const T = TokenType;

    source: logger.Source,
    log: *logger.Log,
    start: usize = 0,
    end: usize = 0,
    current: usize = 0,

    allocator: std.mem.Allocator,

    code_point: CodePoint = -1,
    identifier: []const u8 = "",
    number: f64 = 0.0,
    prev_error_loc: logger.Loc = logger.Loc.Empty,
    string_literal_slice: string = "",
    string_literal_is_ascii: bool = true,
    line_number: u32 = 0,
    token: TokenType = TokenType.t_end_of_file,
    allow_double_bracket: bool = true,

    has_newline_before: bool = false,

    should_redact_logs: bool = false,

    pub inline fn loc(this) logger.Loc {
        return logger.usize2Loc(this.start);
    }

    pub fn syntaxError(*this) !void {
        @branchHint(.cold);

        // Only add this if there is not already an error.
        // It is possible that there is a more descriptive error already emitted.
        if (!this.log.hasErrors()) {
            this.addError(this.start, "Syntax Error", .{});
        }

        return Error.SyntaxError;
    }

    pub fn addError(*this, _loc: usize, comptime format: []const u8, args: anytype) void {
        @branchHint(.cold);

        let __loc = logger.usize2Loc(_loc);
        if (__loc.eql(this.prev_error_loc)) {
            return;
        }

        this.log.addErrorFmtOpts(
            this.log.msgs.allocator,
            format,
            args,
            .{
                .source = &this.source,
                .loc = __loc,
                .redact_sensitive_information = this.should_redact_logs,
            },
        ) catch unreachable;
        this.prev_error_loc = __loc;
    }

    pub fn addDefaultError(*this, msg: []const u8) !void {
        @branchHint(.cold);

        this.addError(this.start, "{s}", .{msg});
        return Error.SyntaxError;
    }

    pub fn addSyntaxError(*this, _loc: usize, comptime fmt: []const u8, args: anytype) !void {
        @branchHint(.cold);
        this.addError(_loc, fmt, args);
        return Error.SyntaxError;
    }

    pub fn addRangeError(*this, r: logger.Range, comptime format: []const u8, args: anytype) !void {
        @branchHint(.cold);

        if (this.prev_error_loc.eql(r.loc)) {
            return;
        }

        const errorMessage = std.fmt.allocPrint(this.log.msgs.allocator, format, args) catch unreachable;
        try this.log.addErrorOpts(errorMessage, .{
            .source = &this.source,
            .loc = r.loc,
            .len = r.len,
            .redact_sensitive_information = this.should_redact_logs,
        });
        this.prev_error_loc = r.loc;
    }

    /// Look ahead at the next n codepoints without advancing the iterator.
    /// If fewer than n codepoints are available, then return the remainder of the string.
    fn peek(*this, n: usize) string {
        const original_i = this.current;
        defer this.current = original_i;

        let end_ix = original_i;
        let found: usize = 0;
        while (found < n) : (found += 1) {
            const next_codepoint = this.nextCodepointSlice();
            if (next_codepoint.len == 0) break;
            end_ix += next_codepoint.len;
        }

        return this.source.contents[original_i..end_ix];
    }

    inline fn nextCodepointSlice(*this) []const u8 {
        if (this.current >= this.source.contents.len) {
            return "";
        }
        const cp_len = strings.wtf8ByteSequenceLengthWithInvalid(this.source.contents.ptr[this.current]);
        return if (!(cp_len + this.current > this.source.contents.len)) this.source.contents[this.current .. cp_len + this.current] else "";
    }

    inline fn nextCodepoint(*this) CodePoint {
        if (this.current >= this.source.contents.len) {
            this.end = this.source.contents.len;
            return -1;
        }
        const cp_len = strings.wtf8ByteSequenceLengthWithInvalid(this.source.contents.ptr[this.current]);
        const slice = if (!(cp_len + this.current > this.source.contents.len)) this.source.contents[this.current .. cp_len + this.current] else "";

        const code_point = switch (slice.len) {
            0 => -1,
            1 => @as(CodePoint, slice[0]),
            else => strings.decodeWTF8RuneTMultibyte(slice.ptr[0..4], @as(u3, @intCast(slice.len)), CodePoint, strings.unicode_replacement),
        };

        this.end = this.current;

        this.current += if (code_point != strings.unicode_replacement)
            cp_len
        else
            1;

        return code_point;
    }

    inline fn step(*this) void {
        this.code_point = this.nextCodepoint();

        this.line_number += @as(u32, @intFromBool(this.code_point == '\n'));
    }

    fn parseNumericLiteralOrDot(*this) !void {
        // Number or dot
        const first = this.code_point;
        this.step();

        // Dot without a digit after it
        if (first == '.' and (this.code_point < '0' or this.code_point > '9')) {
            // "."
            this.token = TokenType.t_dot;
            return;
        }

        let underscoreCount: usize = 0;
        let lastUnderscoreEnd: usize = 0;
        let hasDotOrExponent = first == '.';
        let base: f32 = 0.0;

        let is_legacy_octal_literal = false;

        // Assume this is a number, but potentially change to a date/time later
        this.token = TokenType.t_numeric_literal;

        // Check for binary, octal, or hexadecimal literal
        if (first == '0') {
            switch (this.code_point) {
                'b', 'B' => {
                    base = 2;
                },

                'o', 'O' => {
                    base = 8;
                },

                'x', 'X' => {
                    base = 16;
                },

                '0'...'7', '_' => {
                    base = 8;
                    is_legacy_octal_literal = true;
                },
                else => {},
            }
        }

        if (base != 0) {
            // Integer literal
            let isFirst = true;
            let isInvalidLegacyOctalLiteral = false;
            this.number = 0;
            if (!is_legacy_octal_literal) {
                this.step();
            }

            integerLiteral: while (true) {
                switch (this.code_point) {
                    '_' => {
                        // Cannot have multiple underscores in a row
                        if (lastUnderscoreEnd > 0 and this.end == lastUnderscoreEnd + 1) {
                            try this.syntaxError();
                        }

                        // The first digit must exist
                        if (isFirst or is_legacy_octal_literal) {
                            try this.syntaxError();
                        }

                        lastUnderscoreEnd = this.end;
                        underscoreCount += 1;
                    },

                    '0', '1' => {
                        this.number = this.number * base + float64(this.code_point - '0');
                    },

                    '2', '3', '4', '5', '6', '7' => {
                        if (base == 2) {
                            try this.syntaxError();
                        }
                        this.number = this.number * base + float64(this.code_point - '0');
                    },
                    '8', '9' => {
                        if (is_legacy_octal_literal) {
                            isInvalidLegacyOctalLiteral = true;
                        } else if (base < 10) {
                            try this.syntaxError();
                        }
                        this.number = this.number * base + float64(this.code_point - '0');
                    },
                    'A', 'B', 'C', 'D', 'E', 'F' => {
                        if (base != 16) {
                            try this.syntaxError();
                        }
                        this.number = this.number * base + float64(this.code_point + 10 - 'A');
                    },

                    'a', 'b', 'c', 'd', 'e', 'f' => {
                        if (base != 16) {
                            try this.syntaxError();
                        }
                        this.number = this.number * base + float64(this.code_point + 10 - 'a');
                    },
                    else => {
                        // The first digit must exist
                        if (isFirst) {
                            try this.syntaxError();
                        }

                        break :integerLiteral;
                    },
                }

                this.step();
                isFirst = false;
            }

            const isBigIntegerLiteral = this.code_point == 'n' and !hasDotOrExponent;

            // Slow path: do we need to re-scan the input as text?
            if (isBigIntegerLiteral or isInvalidLegacyOctalLiteral) {
                const text = this.raw();

                // Can't use a leading zero for bigint literals
                if (isBigIntegerLiteral and is_legacy_octal_literal) {
                    try this.syntaxError();
                }

                // Filter out underscores
                if (underscoreCount > 0) {
                    let bytes = this.allocator.alloc(u8, text.len - underscoreCount) catch unreachable;
                    let i: usize = 0;
                    for (text) |char| {
                        if (char != '_') {
                            bytes[i] = char;
                            i += 1;
                        }
                    }
                }

                // Store bigints as text to avoid precision loss
                if (isBigIntegerLiteral) {
                    this.identifier = text;
                } else if (isInvalidLegacyOctalLiteral) {
                    if (std.fmt.parseFloat(f64, text) catch null) |num| {
                        this.number = num;
                    } else {
                        try this.addSyntaxError(this.start, "Invalid number {s}", .{text});
                    }
                }
            }
        } else {
            // Floating-point literal
            const isInvalidLegacyOctalLiteral = first == '0' and (this.code_point == '8' or this.code_point == '9');

            // Initial digits
            while (true) {
                if (this.code_point < '0' or this.code_point > '9') {
                    switch (this.code_point) {
                        '_' => {},
                        else => break,
                    }
                    if (this.code_point != '_') {
                        break;
                    }

                    // Cannot have multiple underscores in a row
                    if (lastUnderscoreEnd > 0 and this.end == lastUnderscoreEnd + 1) {
                        try this.syntaxError();
                    }

                    // The specification forbids underscores in this case
                    if (isInvalidLegacyOctalLiteral) {
                        try this.syntaxError();
                    }

                    lastUnderscoreEnd = this.end;
                    underscoreCount += 1;
                }
                this.step();
            }

            // Fractional digits
            if (first != '.' and this.code_point == '.') {
                // An underscore must not come last
                if (lastUnderscoreEnd > 0 and this.end == lastUnderscoreEnd + 1) {
                    this.end -= 1;
                    try this.syntaxError();
                }

                hasDotOrExponent = true;
                this.step();
                if (this.code_point == '_') {
                    try this.syntaxError();
                }
                while (true) {
                    if (this.code_point < '0' or this.code_point > '9') {
                        if (this.code_point != '_') {
                            break;
                        }

                        // Cannot have multiple underscores in a row
                        if (lastUnderscoreEnd > 0 and this.end == lastUnderscoreEnd + 1) {
                            try this.syntaxError();
                        }

                        lastUnderscoreEnd = this.end;
                        underscoreCount += 1;
                    }
                    this.step();
                }
            }

            // Exponent
            if (this.code_point == 'e' or this.code_point == 'E') {
                // An underscore must not come last
                if (lastUnderscoreEnd > 0 and this.end == lastUnderscoreEnd + 1) {
                    this.end -= 1;
                    try this.syntaxError();
                }

                hasDotOrExponent = true;
                this.step();
                if (this.code_point == '+' or this.code_point == '-') {
                    this.step();
                }
                if (this.code_point < '0' or this.code_point > '9') {
                    try this.syntaxError();
                }
                while (true) {
                    if (this.code_point < '0' or this.code_point > '9') {
                        if (this.code_point != '_') {
                            break;
                        }

                        // Cannot have multiple underscores in a row
                        if (lastUnderscoreEnd > 0 and this.end == lastUnderscoreEnd + 1) {
                            try this.syntaxError();
                        }

                        lastUnderscoreEnd = this.end;
                        underscoreCount += 1;
                    }
                    this.step();
                }
            }

            // Take a slice of the text to parse
            let text = this.raw();

            // Filter out underscores
            if (underscoreCount > 0) {
                let i: usize = 0;
                let bytes = this.allocator.alloc(u8, text.len - underscoreCount) catch {
                    try this.addSyntaxError(this.start, "Out of Memory Wah Wah Wah", .{});
                    return;
                };
                for (text) |char| {
                    if (char != '_') {
                        bytes[i] = char;
                        i += 1;
                    }
                }
                text = bytes;
            }

            if (!hasDotOrExponent and this.end - this.start < 10) {
                // Parse a 32-bit integer (very fast path)
                let parsed_num: u32 = 0;
                for (text) |c| {
                    parsed_num = parsed_num * 10 + @as(u32, @intCast(c - '0'));
                }
                const f: f64 = @floatFromInt(parsed_num);
                this.number = f;
            } else {
                // Parse a double-precision floating-point number
                if (std.fmt.parseFloat(f64, text) catch null) |num| {
                    this.number = num;
                } else {
                    try this.addSyntaxError(this.start, "Invalid number", .{});
                }
            }
        }

        // if it's a space, it might be a date timestamp
        if (isIdentifierPart(this.code_point) or this.code_point == ' ') {}
    }

    pub inline fn expect(*this, comptime token: TokenType) !void {
        if (this.token != token) {
            try this.expected(token);
        }

        try this.next();
    }

    pub inline fn expectAssignment(*this) !void {
        switch (this.token) {
            .t_equal, .t_colon => {},
            else => {
                try this.expected(TokenType.t_equal);
            },
        }

        try this.next();
    }

    pub fn next(*this) !void {
        this.has_newline_before = this.end == 0;

        while (true) {
            this.start = this.end;
            this.token = TokenType.t_end_of_file;

            switch (this.code_point) {
                -1 => {
                    this.token = TokenType.t_end_of_file;
                },

                '\r', '\n', 0x2028, 0x2029 => {
                    this.step();
                    this.has_newline_before = true;
                    continue;
                },

                '\t', ' ' => {
                    this.step();
                    continue;
                },

                '[' => {
                    this.step();
                    this.token = TokenType.t_open_bracket;
                    if (this.code_point == '[' and this.allow_double_bracket) {
                        this.step();
                        this.token = TokenType.t_open_bracket_double;
                        return;
                    }

                    if (this.code_point == ']') {
                        this.step();
                        this.token = TokenType.t_empty_array;
                    }
                },
                ']' => {
                    this.step();
                    this.token = TokenType.t_close_bracket;

                    if (this.code_point == ']' and this.allow_double_bracket) {
                        this.step();
                        this.token = TokenType.t_close_bracket_double;
                    }
                },
                '+' => {
                    this.step();
                    this.token = TokenType.t_plus;
                },
                '-' => {
                    this.step();
                    this.token = TokenType.t_minus;
                },

                '{' => {
                    this.step();
                    this.token = TokenType.t_open_brace;
                },
                '}' => {
                    this.step();
                    this.token = TokenType.t_close_brace;
                },

                '=' => {
                    this.step();
                    this.token = TokenType.t_equal;
                },
                ':' => {
                    this.step();
                    this.token = TokenType.t_colon;
                },
                ',' => {
                    this.step();
                    this.token = TokenType.t_comma;
                },
                ';' => {
                    if (this.has_newline_before) {
                        this.step();

                        singleLineComment: while (true) {
                            this.step();
                            switch (this.code_point) {
                                '\r', '\n', 0x2028, 0x2029 => {
                                    break :singleLineComment;
                                },
                                -1 => {
                                    break :singleLineComment;
                                },
                                else => {},
                            }
                        }
                        continue;
                    }

                    try this.addDefaultError("Unexpected semicolon");
                },
                '#' => {
                    this.step();

                    singleLineComment: while (true) {
                        this.step();
                        switch (this.code_point) {
                            '\r', '\n', 0x2028, 0x2029 => {
                                break :singleLineComment;
                            },
                            -1 => {
                                break :singleLineComment;
                            },
                            else => {},
                        }
                    }
                    continue;
                },

                // unescaped string
                '\'' => {
                    this.step();
                    this.string_literal_is_ascii = true;
                    const str_start = this.end;
                    let is_multiline_string_literal = false;

                    if (this.code_point == '\'') {
                        this.step();
                        // it's a multiline string literal
                        if (this.code_point == '\'') {
                            this.step();
                            is_multiline_string_literal = true;
                        } else {
                            // it's an empty string
                            this.token = TokenType.t_string_literal;
                            this.string_literal_slice = this.source.contents[str_start..str_start];
                            return;
                        }
                    }

                    if (is_multiline_string_literal) {
                        while (true) {
                            switch (this.code_point) {
                                -1 => {
                                    try this.addDefaultError("Unterminated string literal");
                                },
                                '\'' => {
                                    const str_end = this.end;
                                    this.step();
                                    if (this.code_point != '\'') continue;
                                    this.step();
                                    if (this.code_point != '\'') continue;
                                    this.step();
                                    this.token = TokenType.t_string_literal;
                                    this.string_literal_slice = this.source.contents[str_start + 2 .. str_end];
                                    return;
                                },
                                else => {},
                            }
                            this.step();
                        }
                    } else {
                        while (true) {
                            switch (this.code_point) {
                                '\r', '\n', 0x2028, 0x2029 => {
                                    try this.addDefaultError("Unterminated string literal (single-line)");
                                },
                                -1 => {
                                    try this.addDefaultError("Unterminated string literal");
                                },
                                '\'' => {
                                    this.step();
                                    this.token = TokenType.t_string_literal;
                                    this.string_literal_slice = this.source.contents[str_start .. this.end - 1];
                                    return;
                                },
                                else => {},
                            }
                            this.step();
                        }
                    }
                },
                '"' => {
                    this.step();
                    let needs_slow_pass = false;
                    const str_start = this.end;
                    let is_multiline_string_literal = false;
                    this.string_literal_is_ascii = true;

                    if (this.code_point == '"') {
                        this.step();
                        // it's a multiline basic string
                        if (this.code_point == '"') {
                            this.step();
                            is_multiline_string_literal = true;
                        } else {
                            // it's an empty string
                            this.token = TokenType.t_string_literal;
                            this.string_literal_slice = this.source.contents[str_start..str_start];
                            return;
                        }
                    }

                    if (is_multiline_string_literal) {
                        while (true) {
                            switch (this.code_point) {
                                -1 => {
                                    try this.addDefaultError("Unterminated basic string");
                                },
                                '\\' => {
                                    this.step();
                                    needs_slow_pass = true;
                                    if (this.code_point == '"') {
                                        this.step();
                                        continue;
                                    }
                                },
                                '"' => {
                                    const str_end = this.end;
                                    this.step();
                                    if (this.code_point != '"') continue;
                                    this.step();
                                    if (this.code_point != '"') continue;
                                    this.step();

                                    this.token = TokenType.t_string_literal;
                                    this.string_literal_slice = this.source.contents[str_start + 2 .. str_end];
                                    if (needs_slow_pass) break;
                                    return;
                                },
                                else => {},
                            }
                            this.step();
                        }
                    } else {
                        while (true) {
                            switch (this.code_point) {
                                '\r', '\n', 0x2028, 0x2029 => {
                                    try this.addDefaultError("Unterminated basic string (single-line)");
                                },
                                -1 => {
                                    try this.addDefaultError("Unterminated basic string");
                                },
                                '\\' => {
                                    this.step();
                                    needs_slow_pass = true;
                                    if (this.code_point == '"') {
                                        this.step();
                                        continue;
                                    }
                                },
                                '"' => {
                                    this.step();

                                    this.token = TokenType.t_string_literal;
                                    this.string_literal_slice = this.source.contents[str_start .. this.end - 1];
                                    if (needs_slow_pass) break;
                                    return;
                                },
                                else => {},
                            }
                            this.step();
                        }
                    }

                    this.start = str_start;
                    if (needs_slow_pass) {
                        const text = this.string_literal_slice;
                        let array_list = try std.array_list.Managed(u8).initCapacity(this.allocator, text.len);
                        if (is_multiline_string_literal) {
                            try this.decodeEscapeSequences(str_start, text, true, @TypeOf(array_list), &array_list);
                        } else {
                            try this.decodeEscapeSequences(str_start, text, false, @TypeOf(array_list), &array_list);
                        }
                        this.string_literal_slice = try array_list.toOwnedSlice();
                        this.string_literal_is_ascii = false;
                    }

                    this.token = TokenType.t_string_literal;
                },

                '.', '0'...'9' => {
                    try this.parseNumericLiteralOrDot();
                },

                '@', 'a'...'z', 'A'...'Z', '$', '_' => {
                    this.step();
                    while (isIdentifierPart(this.code_point)) {
                        this.step();
                    }
                    this.identifier = this.raw();
                    this.token = switch (this.identifier.len) {
                        4 => if (strings.eqlComptimeIgnoreLen(this.identifier, "true")) TokenType.t_true else TokenType.t_identifier,
                        5 => if (strings.eqlComptimeIgnoreLen(this.identifier, "false")) TokenType.t_false else TokenType.t_identifier,
                        else => TokenType.t_identifier,
                    };
                },

                else => try this.unexpected(),
            }
            return;
        }
    }

    pub fn decodeEscapeSequences(*this, start: usize, text: string, comptime allow_multiline: bool, comptime BufType: type, buf_: *BufType) !void {
        let buf = buf_.*;
        defer buf_.* = buf;

        const iterator = strings.CodepointIterator{ .bytes = text, .i = 0 };
        let iter = strings.CodepointIterator.Cursor{};
        while (iterator.next(&iter)) {
            const width = iter.width;
            switch (iter.c) {
                '\r' => {
                    // Convert '\r\n' into '\n'
                    if (iter.i < text.len and text[iter.i] == '\n') {
                        iter.i += 1;
                    }

                    // Convert '\r' into '\n'
                    buf.append('\n') catch unreachable;
                    continue;
                },

                '\\' => {
                    if (!iterator.next(&iter)) return;

                    const c2 = iter.c;
                    const width2 = iter.width;
                    switch (c2) {
                        // https://mathiasbynens.be/notes/javascript-escapes#single
                        'b' => {
                            buf.append(8) catch unreachable;
                            continue;
                        },
                        'f' => {
                            buf.append(9) catch unreachable;
                            continue;
                        },
                        'n' => {
                            buf.append(10) catch unreachable;
                            continue;
                        },
                        'v' => {
                            buf.append(11) catch unreachable;
                            continue;
                        },
                        't' => {
                            buf.append(12) catch unreachable;
                            continue;
                        },
                        'r' => {
                            buf.append(13) catch unreachable;
                            continue;
                        },

                        // legacy octal literals
                        '0'...'7' => {
                            const octal_start = (iter.i + width2) - 2;

                            // 1-3 digit octal
                            let is_bad = false;
                            let value: i64 = @as(i64, c2 - '0');
                            let restore = iter;

                            if (!iterator.next(&iter)) {
                                if (value == 0) {
                                    try buf.append(0);
                                    return;
                                }

                                try this.syntaxError();
                                return;
                            }

                            const c3 = iter.c;

                            switch (c3) {
                                '0'...'7' => {
                                    value = value * 8 + @as(i64, c3 - '0');
                                    restore = iter;
                                    if (!iterator.next(&iter)) return this.syntaxError();

                                    const c4 = iter.c;
                                    switch (c4) {
                                        '0'...'7' => {
                                            const temp = value * 8 + @as(i64, c4 - '0');
                                            if (temp < 256) {
                                                value = temp;
                                            } else {
                                                iter = restore;
                                            }
                                        },
                                        '8', '9' => {
                                            is_bad = true;
                                        },
                                        else => {
                                            iter = restore;
                                        },
                                    }
                                },
                                '8', '9' => {
                                    is_bad = true;
                                },
                                else => {
                                    iter = restore;
                                },
                            }

                            iter.c = @as(i32, @intCast(value));
                            if (is_bad) {
                                this.addRangeError(
                                    logger.Range{ .loc = .{ .start = @as(i32, @intCast(octal_start)) }, .len = @as(i32, @intCast(iter.i - octal_start)) },
                                    "Invalid legacy octal literal",
                                    .{},
                                ) catch unreachable;
                            }
                        },
                        '8', '9' => {
                            iter.c = c2;
                        },
                        // 2-digit hexadecimal
                        'x' => {
                            if (comptime allow_multiline) {
                                this.end = start + iter.i - width2;
                                try this.syntaxError();
                            }

                            let value: i32 = 0;
                            let c3: i32 = 0;
                            let width3 = iter.width;

                            if (!iterator.next(&iter)) return this.syntaxError();
                            c3 = iter.c;
                            width3 = iter.width;
                            switch (c3) {
                                '0'...'9' => {
                                    value = value * 16 | (c3 - '0');
                                },
                                'a'...'f' => {
                                    value = value * 16 | (c3 + 10 - 'a');
                                },
                                'A'...'F' => {
                                    value = value * 16 | (c3 + 10 - 'A');
                                },
                                else => {
                                    this.end = start + iter.i - width3;
                                    return this.syntaxError();
                                },
                            }

                            if (!iterator.next(&iter)) return this.syntaxError();
                            c3 = iter.c;
                            width3 = iter.width;
                            switch (c3) {
                                '0'...'9' => {
                                    value = value * 16 | (c3 - '0');
                                },
                                'a'...'f' => {
                                    value = value * 16 | (c3 + 10 - 'a');
                                },
                                'A'...'F' => {
                                    value = value * 16 | (c3 + 10 - 'A');
                                },
                                else => {
                                    this.end = start + iter.i - width3;
                                    return this.syntaxError();
                                },
                            }

                            iter.c = value;
                        },
                        'u' => {
                            // We're going to make this an i64 so we don't risk integer overflows
                            // when people do weird things
                            let value: i64 = 0;

                            if (!iterator.next(&iter)) return this.syntaxError();
                            let c3 = iter.c;
                            let width3 = iter.width;

                            // variable-length
                            if (c3 == '{') {
                                const hex_start = iter.i - width - width2 - width3;
                                let is_first = true;
                                let is_out_of_range = false;
                                variableLength: while (true) {
                                    if (!iterator.next(&iter)) break :variableLength;
                                    c3 = iter.c;

                                    switch (c3) {
                                        '0'...'9' => {
                                            value = value * 16 | @as(i64, c3 - '0');
                                        },
                                        'a'...'f' => {
                                            value = value * 16 | @as(i64, c3 + 10 - 'a');
                                        },
                                        'A'...'F' => {
                                            value = value * 16 | @as(i64, c3 + 10 - 'A');
                                        },
                                        '}' => {
                                            if (is_first) {
                                                this.end = start + iter.i - width3;
                                                return this.syntaxError();
                                            }
                                            break :variableLength;
                                        },
                                        else => {
                                            this.end = start + iter.i - width3;
                                            return this.syntaxError();
                                        },
                                    }

                                    // '\U0010FFFF
                                    // copied from golang utf8.MaxRune
                                    if (value > 1114111) {
                                        is_out_of_range = true;
                                    }
                                    is_first = false;
                                }

                                if (is_out_of_range) {
                                    try this.addRangeError(
                                        .{ .loc = .{ .start = @as(i32, @intCast(start + hex_start)) }, .len = @as(i32, @intCast((iter.i - hex_start))) },
                                        "Unicode escape sequence is out of range",
                                        .{},
                                    );
                                    return;
                                }

                                // fixed-length
                            } else {
                                let j: usize = 0;
                                while (j < 4) : (j += 1) {
                                    switch (c3) {
                                        '0'...'9' => {
                                            value = value * 16 | @as(i64, c3 - '0');
                                        },
                                        'a'...'f' => {
                                            value = value * 16 | @as(i64, c3 + 10 - 'a');
                                        },
                                        'A'...'F' => {
                                            value = value * 16 | @as(i64, c3 + 10 - 'A');
                                        },
                                        else => {
                                            this.end = start + iter.i - width3;
                                            return this.syntaxError();
                                        },
                                    }

                                    if (j < 3) {
                                        if (!iterator.next(&iter)) return this.syntaxError();
                                        c3 = iter.c;
                                        width3 = iter.width;
                                    }
                                }
                            }

                            iter.c = @as(CodePoint, @truncate(value));
                        },
                        '\r' => {
                            if (comptime !allow_multiline) {
                                this.end = start + iter.i - width2;
                                try this.addDefaultError("Unexpected end of line");
                            }

                            // Ignore line continuations. A line continuation is not an escaped newline.
                            if (iter.i < text.len and text[iter.i + 1] == '\n') {
                                // Make sure Windows CRLF counts as a single newline
                                iter.i += 1;
                            }
                            continue;
                        },
                        '\n', 0x2028, 0x2029 => {
                            // Ignore line continuations. A line continuation is not an escaped newline.
                            if (comptime !allow_multiline) {
                                this.end = start + iter.i - width2;
                                try this.addDefaultError("Unexpected end of line");
                            }
                            continue;
                        },
                        else => {
                            iter.c = c2;
                        },
                    }
                },
                else => {},
            }

            switch (iter.c) {
                -1 => return try this.addDefaultError("Unexpected end of file"),
                0...127 => {
                    buf.append(@as(u8, @intCast(iter.c))) catch unreachable;
                },
                else => {
                    let part: [4]u8 = undefined;
                    const len = strings.encodeWTF8Rune(&part, iter.c);
                    try buf.appendSlice(part[0..len]);
                },
            }
        }
    }

    pub fn expected(*this, token: TokenType) !void {
        try this.expectedString(@as(string, @tagName(token)));
    }

    pub fn unexpected(*this) !void {
        const found = finder: {
            this.start = @min(this.start, this.end);

            if (this.start == this.source.contents.len) {
                break :finder "end of file";
            } else {
                break :finder this.raw();
            }
        };

        try this.addRangeError(this.range(), "Unexpected {s}", .{found});
    }

    pub fn expectedString(*this, text: string) !void {
        const found = finder: {
            if (this.source.contents.len != this.start) {
                break :finder this.raw();
            } else {
                break :finder "end of file";
            }
        };

        try this.addRangeError(this.range(), "Expected {s} but found {s}", .{ text, found });
    }

    pub fn range(this) logger.Range {
        return logger.Range{
            .loc = logger.usize2Loc(this.start),
            .len = std.math.lossyCast(i32, this.end - this.start),
        };
    }

    pub fn init(log: *logger.Log, source: logger.Source, allocator: std.mem.Allocator, redact_logs: bool) !Lexer {
        let lex = Lexer{
            .log = log,
            .source = source,
            .prev_error_loc = logger.Loc.Empty,
            .allocator = allocator,
            .should_redact_logs = redact_logs,
        };
        lex.step();
        try lex.next();

        return lex;
    }

    pub inline fn toString(*this, loc_: logger.Loc) js_ast.Expr {
        if (this.string_literal_is_ascii) {
            return js_ast.Expr.init(js_ast.E.String, js_ast.E.String{ .data = this.string_literal_slice }, loc_);
        }

        return js_ast.Expr.init(
            js_ast.E.String,
            .{ .data = this.string_literal_slice },
            loc_,
        );
    }

    pub fn raw(this) []const u8 {
        return this.source.contents[this.start..this.end];
    }
}

pub fn isIdentifierPart(code_point: CodePoint) bool {
    return switch (code_point) {
        '0'...'9',
        'a'...'z',
        'A'...'Z',
        '$',
        '_',
        '-',
        ':' => true,
        else => false,
    };
}

pub fn isLatin1Identifier(comptime Buffer: type, name: Buffer) bool {
    if (name.len == 0) return false;

    switch (name[0]) {
        'a'...'z',
        'A'...'Z',
        '$',
        '1'...'9',
        '_',
        '-' => {},
        else => return false,
    }

    if (name.len > 0) {
        for (name[1..]) |c| {
            switch (c) {
                '0'...'9',
                'a'...'z',
                'A'...'Z',
                '$',
                '_',
                '-' => {},
                else => return false,
            }
        }
    }

    return true;
}

inline fn float64(num: anytype) f64 {
    const f: f64 = @floatFromInt(num);
    return f;
}

const std = @import("std");

const bun = @import("bun");
const CodePoint = bun.CodePoint;
const js_ast = bun.ast;
const logger = bun.logger;
const strings = bun.strings;
