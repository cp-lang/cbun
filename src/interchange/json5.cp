/// JSON5 Token-Based Scanner/Parser
///
/// Parses JSON5 text into Expr AST values. JSON5 is a superset of JSON
/// based on ECMAScript 5.1 that supports comments, trailing commas,
/// unquoted keys, single-quoted strings, hex numbers, Infinity, NaN, etc.
///
/// Architecture: a scanner reads source bytes and produces typed tokens;
/// the parser only consumes tokens and never touches source/pos directly.
///
/// Reference: https://spec.json5.org/

pub const TokenData = union(enum) {
    eof,
    // Structural
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    colon,
    comma,
    // Values
    string: []u8,
    number: f64,
    boolean: bool,
    null,
    // Identifiers (for unquoted keys that aren't keywords)
    identifier: []u8,

    pub fn canStartValue(data: TokenData) bool {
        return switch (data) {
            .string, .number, .boolean, .identifier, .@"null", .left_brace, .left_bracket => true,
            .eof, .right_brace, .right_bracket, .colon, .comma => false,
        };
    }
};

pub const JSON5Token = struct {
    loc: logger.Loc,
    data: TokenData,

    pub const Data = TokenData;
};

err ParseError {
    OutOfMemory,
    UnexpectedCharacter,
    UnexpectedToken,
    UnexpectedEof,
    UnterminatedString,
    UnterminatedComment,
    UnterminatedObject,
    UnterminatedArray,
    UnterminatedEscape,
    InvalidNumber,
    LeadingZeros,
    InvalidHexNumber,
    InvalidHexEscape,
    InvalidUnicodeEscape,
    OctalEscape,
    ExpectedColon,
    ExpectedComma,
    ExpectedClosingBrace,
    ExpectedClosingBracket,
    InvalidIdentifier,
    TrailingData,
    StackOverflow,
}

pub const JSON5Error = union(enum) {
    oom,
    stack_overflow,
    unexpected_character: struct { pos: usize },
    unexpected_token: struct { pos: usize },
    unexpected_eof: struct { pos: usize },
    unterminated_string: struct { pos: usize },
    unterminated_comment: struct { pos: usize },
    unterminated_object: struct { pos: usize },
    unterminated_array: struct { pos: usize },
    unterminated_escape: struct { pos: usize },
    invalid_number: struct { pos: usize },
    leading_zeros: struct { pos: usize },
    invalid_hex_number: struct { pos: usize },
    invalid_hex_escape: struct { pos: usize },
    invalid_unicode_escape: struct { pos: usize },
    octal_escape: struct { pos: usize },
    expected_colon: struct { pos: usize },
    expected_comma: struct { pos: usize },
    expected_closing_brace: struct { pos: usize },
    expected_closing_bracket: struct { pos: usize },
    invalid_identifier: struct { pos: usize },
    trailing_data: struct { pos: usize },

    pub fn addToLog(this: *const JSON5Error, source: *const logger.Source, log: *logger.Log) !void {
        const loc: logger.Loc = switch (this.*) {
            .oom => return error.OutOfMemory,
            .stack_overflow => return error.StackOverflow,
            inline else => |e| .{ .start = @intCast(e.pos) },
        };
        const msg: []const u8 = switch (this.*) {
            .oom, .stack_overflow => unreachable,
            .unexpected_character => "Unexpected character",
            .unexpected_token => "Unexpected token",
            .unexpected_eof => "Unexpected end of input",
            .unterminated_string => "Unterminated string",
            .unterminated_comment => "Unterminated multi-line comment",
            .unterminated_object => "Unterminated object",
            .unterminated_array => "Unterminated array",
            .unterminated_escape => "Unexpected end of input in escape sequence",
            .invalid_number => "Invalid number",
            .leading_zeros => "Leading zeros are not allowed in JSON5",
            .invalid_hex_number => "Invalid hex number",
            .invalid_hex_escape => "Invalid hex escape",
            .invalid_unicode_escape => "Invalid unicode escape: expected 4 hex digits",
            .octal_escape => "Octal escape sequences are not allowed in JSON5",
            .expected_colon => "Expected ':' after object key",
            .expected_comma => "Expected ','",
            .expected_closing_brace => "Expected '}'",
            .expected_closing_bracket => "Expected ']'",
            .invalid_identifier => "Invalid identifier start character",
            .trailing_data => "Unexpected token after JSON5 value",
        };
        try log.addError(source, loc, msg);
    }
};

err ExternalError { OutOfMemory, SyntaxError, StackOverflow }

const Codepoint = struct {
    cp: i32,
    len: u3,
};

@value class JSON5Parser {
    source: []const u8;
    pos: usize;
    allocator: std.mem.Allocator;
    stack_check: bun.StackCheck;
    token: JSON5Token;

    pub const Token = JSON5Token;
    pub const Error = JSON5Error;

    pub static fn toError(err: ParseError, parser: *const JSON5Parser): JSON5Error {
        const token_pos = parser.token.loc.toUsize();
        const scan_pos = parser.pos;
        return switch (err) {
            error.OutOfMemory => .oom,
            error.StackOverflow => .stack_overflow,
            // Scanner errors use scan position
            error.UnexpectedCharacter => .{ .unexpected_character = .{ .pos = scan_pos } },
            error.UnterminatedString => .{ .unterminated_string = .{ .pos = scan_pos } },
            error.UnterminatedComment => .{ .unterminated_comment = .{ .pos = scan_pos } },
            error.UnterminatedEscape => .{ .unterminated_escape = .{ .pos = scan_pos } },
            error.InvalidNumber => .{ .invalid_number = .{ .pos = scan_pos } },
            error.LeadingZeros => .{ .leading_zeros = .{ .pos = scan_pos } },
            error.InvalidHexNumber => .{ .invalid_hex_number = .{ .pos = scan_pos } },
            error.InvalidHexEscape => .{ .invalid_hex_escape = .{ .pos = scan_pos } },
            error.InvalidUnicodeEscape => .{ .invalid_unicode_escape = .{ .pos = scan_pos } },
            error.OctalEscape => .{ .octal_escape = .{ .pos = scan_pos } },
            error.InvalidIdentifier => .{ .invalid_identifier = .{ .pos = scan_pos } },
            // Parser errors use token position
            error.UnexpectedToken => .{ .unexpected_token = .{ .pos = token_pos } },
            error.UnexpectedEof => if (parser.token.data == .eof)
                .{ .unexpected_eof = .{ .pos = token_pos } }
            else
                .{ .unexpected_token = .{ .pos = token_pos } },
            error.TrailingData => .{ .trailing_data = .{ .pos = token_pos } },
            error.ExpectedColon => .{ .expected_colon = .{ .pos = token_pos } },
            error.UnterminatedObject => .{ .unterminated_object = .{ .pos = token_pos } },
            error.ExpectedComma => .{ .expected_comma = .{ .pos = token_pos } },
            error.ExpectedClosingBrace => .{ .expected_closing_brace = .{ .pos = token_pos } },
            error.UnterminatedArray => .{ .unterminated_array = .{ .pos = token_pos } },
            error.ExpectedClosingBracket => .{ .expected_closing_bracket = .{ .pos = token_pos } },
        };
    }

    pub static fn parse(source: *const logger.Source, log: *logger.Log, allocator: std.mem.Allocator): ExternalError!Expr {
        let parser = JSON5Parser{
            .source = source.contents,
            .pos = 0,
            .allocator = allocator,
            .stack_check = .init(),
            .token = .{ .loc = .{}, .data = .eof },
        };
        const result = parser.parseRoot() catch |err| {
            const e = toError(err, &parser);
            try e.addToLog(source, log);
            return error.SyntaxError;
        };
        return result;
    }

    // ── Scanner ──

    /// Returns the byte at the current position, or 0 if at EOF.
    /// All source access in scan() goes through this to avoid bounds checks.
    fn peek(this): u8 {
        if (this.pos < this.source.len) return this.source[this.pos];
        return 0;
    }

    fn scan(*this): ParseError!void {
        while (true) {
            switch (this.peek()) {
                0 => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.token.data = .eof;
                    return;
                },
                // Whitespace — skip without setting loc
                '\t', '\n', '\r', ' ', 0x0B, 0x0C => {
                    this.pos += 1;
                    continue;
                },
                // Structural
                '{' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .left_brace;
                    return;
                },
                '}' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .right_brace;
                    return;
                },
                '[' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .left_bracket;
                    return;
                },
                ']' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .right_bracket;
                    return;
                },
                ':' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .colon;
                    return;
                },
                ',' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .comma;
                    return;
                },
                '+' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .{ .number = try this.scanSignedValue(false) };
                    return;
                },
                '-' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.pos += 1;
                    this.token.data = .{ .number = try this.scanSignedValue(true) };
                    return;
                },
                // Strings
                '"', '\'' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.token.data = .{ .string = try this.scanString() };
                    return;
                },
                // Numbers
                '0'...'9', '.' => {
                    this.token.loc = .{ .start = @intCast(this.pos) };
                    this.token.data = .{ .number = try this.scanNumber() };
                    return;
                },
                // Comments — skip without setting loc
                '/' => {
                    const n = if (this.pos + 1 < this.source.len) this.source[this.pos + 1] else 0;
                    if (n == '/') {
                        this.pos += 2;
                        this.skipToEndOfLine();
                        continue;
                    } else if (n == '*') {
                        this.pos += 2;
                        try this.skipBlockComment();
                        continue;
                    }
                    return error.UnexpectedCharacter;
                },
                else => |c| {
                    if (c == 't') {
                        this.token.loc = .{ .start = @intCast(this.pos) };
                        this.token.data = if (this.scanKeyword("true")) .{ .boolean = true } else .{ .identifier = try this.scanIdentifier() };
                        return;
                    } else if (c == 'f') {
                        this.token.loc = .{ .start = @intCast(this.pos) };
                        this.token.data = if (this.scanKeyword("false")) .{ .boolean = false } else .{ .identifier = try this.scanIdentifier() };
                        return;
                    } else if (c == 'n') {
                        this.token.loc = .{ .start = @intCast(this.pos) };
                        this.token.data = if (this.scanKeyword("null")) TokenData.null else .{ .identifier = try this.scanIdentifier() };
                        return;
                    } else if ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_' or c == '$' or c == '\\') {
                        this.token.loc = .{ .start = @intCast(this.pos) };
                        this.token.data = .{ .identifier = try this.scanIdentifier() };
                        return;
                    } else if (c >= 0x80) {
                        // Multi-byte: check whitespace first, then identifier
                        const mb = this.multiByteWhitespace();
                        if (mb > 0) {
                            this.pos += mb;
                            continue;
                        }
                        this.token.loc = .{ .start = @intCast(this.pos) };
                        const cp = this.readCodepoint() orelse {
                            return error.UnexpectedCharacter;
                        };
                        if (identifier.isIdentifierStart(cp.cp)) {
                            this.token.data = .{ .identifier = try this.scanIdentifier() };
                            return;
                        } else {
                            return error.UnexpectedCharacter;
                        }
                    } else {
                        return error.UnexpectedCharacter;
                    }
                },
            }
        }
    }

    fn scanKeyword(*this, comptime keyword: []const u8): bool {
        if (this.pos + keyword.len > this.source.len) return false;
        if (!std.mem.eql(u8, this.source[this.pos..][0..keyword.len], keyword)) return false;
        // Check word boundary
        if (this.pos + keyword.len < this.source.len) {
            const next = this.source[this.pos + keyword.len];
            if (isIdentContinueASCII(next) or next == '\\' or next >= 0x80) return false;
        }
        this.pos += keyword.len;
        return true;
    }

    fn scanSignedValue(*this, is_negative: bool): ParseError!f64 {
        switch (this.peek()) {
            '0'...'9', '.' => {
                const n = try this.scanNumber();
                return if (is_negative) -n else n;
            },
            'I' => {
                if (this.scanKeyword("Infinity")) {
                    return if (is_negative) -std.math.inf(f64) else std.math.inf(f64);
                }
                return error.UnexpectedCharacter;
            },
            'N' => {
                if (this.scanKeyword("NaN")) {
                    const nan = std.math.nan(f64);
                    return if (is_negative) -nan else nan;
                }
                return error.UnexpectedCharacter;
            },
            0 => return error.UnexpectedEof,
            else => return error.UnexpectedCharacter,
        }
    }

    // ── Parser ──

    fn parseRoot(*this): ParseError!Expr {
        try this.scan();
        const result = try this.parseValue();
        if (this.token.data != .eof) {
            return error.TrailingData;
        }
        return result;
    }

    fn parseValue(*this): ParseError!Expr {
        if (!this.stack_check.isSafeToRecurse()) {
            return error.StackOverflow;
        }

        const loc = this.token.loc;

        switch (this.token.data) {
            .left_brace => return this.parseObject(),
            .left_bracket => return this.parseArray(),
            .string => |s| {
                try this.scan();
                return Expr.init(E.String, E.String.init(s), loc);
            },
            .number => |n| {
                try this.scan();
                return Expr.init(E.Number, .{ .value = n }, loc);
            },
            .boolean => |b| {
                try this.scan();
                return Expr.init(E.Boolean, .{ .value = b }, loc);
            },
            .@"null" => {
                try this.scan();
                return Expr.init(E.Null, .{}, loc);
            },
            .identifier => |s| {
                if (std.mem.eql(u8, s, "NaN")) {
                    try this.scan();
                    return Expr.init(E.Number, .{ .value = std.math.nan(f64) }, loc);
                } else if (std.mem.eql(u8, s, "Infinity")) {
                    try this.scan();
                    return Expr.init(E.Number, .{ .value = std.math.inf(f64) }, loc);
                }
                return error.UnexpectedToken;
            },
            .eof => return error.UnexpectedEof,
            else => return error.UnexpectedToken,
        }
    }

    fn parseObject(*this): ParseError!Expr {
        const loc = this.token.loc;
        try this.scan(); // advance past '{'

        let properties = std.array_list.Managed(G.Property).init(this.allocator);

        while (this.token.data != .right_brace) {
            const key = try this.parseObjectKey();

            if (this.token.data != .colon) {
                return error.ExpectedColon;
            }
            try this.scan(); // advance past ':'

            const value = try this.parseValue();

            try properties.append(.{
                .key = key,
                .value = value,
            });

            switch (this.token.data) {
                .comma => try this.scan(),
                .right_brace => {},
                .eof => return error.UnterminatedObject,
                else => return if (this.token.data.canStartValue()) error.ExpectedComma else error.ExpectedClosingBrace,
            }
        }

        try this.scan(); // advance past '}'
        return Expr.init(E.Object, .{
            .properties = .moveFromList(&properties),
        }, loc);
    }

    fn parseObjectKey(*this): ParseError!Expr {
        const loc = this.token.loc;
        switch (this.token.data) {
            .string => |s| {
                try this.scan();
                return Expr.init(E.String, E.String.init(s), loc);
            },
            .identifier => |s| {
                try this.scan();
                return Expr.init(E.String, E.String.init(s), loc);
            },
            .number => return error.InvalidIdentifier,
            .boolean => |b| {
                try this.scan();
                return Expr.init(E.Boolean, .{ .value = b }, loc);
            },
            .@"null" => {
                try this.scan();
                return Expr.init(E.Null, .{}, loc);
            },
            .eof => return error.UnexpectedEof,
            else => return error.InvalidIdentifier,
        }
    }

    fn parseArray(*this): ParseError!Expr {
        const loc = this.token.loc;
        try this.scan(); // advance past '['

        let items = std.array_list.Managed(Expr).init(this.allocator);

        while (this.token.data != .right_bracket) {
            const value = try this.parseValue();
            try items.append(value);

            switch (this.token.data) {
                .comma => try this.scan(),
                .right_bracket => {},
                .eof => return error.UnterminatedArray,
                else => return if (this.token.data.canStartValue()) error.ExpectedComma else error.ExpectedClosingBracket,
            }
        }

        try this.scan(); // advance past ']'
        return Expr.init(E.Array, .{
            .items = .moveFromList(&items),
        }, loc);
    }

    // ── Scan Helpers ──

    fn scanString(*this): ParseError![]u8 {
        const quote = this.source[this.pos];
        this.pos += 1; // skip opening quote

        let buf = std.array_list.Managed(u8).init(this.allocator);

        while (this.pos < this.source.len) {
            const c = this.source[this.pos];

            if (c == quote) {
                this.pos += 1;
                return try buf.toOwnedSlice();
            }

            if (c == '\\') {
                this.pos += 1;
                try this.parseEscapeSequence(&buf);
                continue;
            }

            // Line terminators are not allowed unescaped in strings
            if (c == '\n' or c == '\r') {
                return error.UnterminatedString;
            }

            // Check for U+2028/U+2029 (allowed unescaped in JSON5 strings)
            if (c == 0xE2 and this.pos + 2 < this.source.len and
                this.source[this.pos + 1] == 0x80 and
                (this.source[this.pos + 2] == 0xA8 or this.source[this.pos + 2] == 0xA9))
            {
                try buf.appendSlice(this.source[this.pos..][0..3]);
                this.pos += 3;
                continue;
            }

            // Regular character - handle multi-byte UTF-8
            const cp_len = strings.wtf8ByteSequenceLength(c);
            if (this.pos + cp_len > this.source.len) {
                try buf.append(c);
                this.pos += 1;
            } else {
                try buf.appendSlice(this.source[this.pos..][0..cp_len]);
                this.pos += cp_len;
            }
        }

        return error.UnterminatedString;
    }

    fn parseEscapeSequence(*this, buf: *std.array_list.Managed(u8)): ParseError!void {
        if (this.pos >= this.source.len) {
            return error.UnterminatedEscape;
        }

        const c = this.source[this.pos];
        this.pos += 1;

        switch (c) {
            '\'' => try buf.append('\''),
            '"' => try buf.append('"'),
            '\\' => try buf.append('\\'),
            'b' => try buf.append(0x08),
            'f' => try buf.append(0x0C),
            'n' => try buf.append('\n'),
            'r' => try buf.append('\r'),
            't' => try buf.append('\t'),
            'v' => try buf.append(0x0B),
            '0' => {
                // \0 null escape - must NOT be followed by a digit
                if (this.pos < this.source.len) {
                    const next = this.source[this.pos];
                    if (next >= '0' and next <= '9') {
                        return error.OctalEscape;
                    }
                }
                try buf.append(0);
            },
            'x' => {
                // \xHH hex escape
                const hi = this.readHexDigit() orelse {
                    return error.InvalidHexEscape;
                };
                const lo = this.readHexDigit() orelse {
                    return error.InvalidHexEscape;
                };
                const value = (@as(u8, hi) << 4) | @as(u8, lo);
                try appendCodepointToUtf8(buf, @intCast(value));
            },
            'u' => {
                // \uHHHH unicode escape
                const cp = try this.readHex4();
                // Check for surrogate pair
                if (cp >= 0xD800 and cp <= 0xDBFF) {
                    // High surrogate - expect \uDCxx low surrogate
                    if (this.pos + 1 < this.source.len and
                        this.source[this.pos] == '\\' and
                        this.source[this.pos + 1] == 'u')
                    {
                        this.pos += 2;
                        const low = try this.readHex4();
                        if (low >= 0xDC00 and low <= 0xDFFF) {
                            const full_cp: i32 = 0x10000 + (cp - 0xD800) * 0x400 + (low - 0xDC00);
                            try appendCodepointToUtf8(buf, full_cp);
                        } else {
                            // Invalid low surrogate - just encode both independently
                            try appendCodepointToUtf8(buf, cp);
                            try appendCodepointToUtf8(buf, low);
                        }
                    } else {
                        try appendCodepointToUtf8(buf, cp);
                    }
                } else {
                    try appendCodepointToUtf8(buf, cp);
                }
            },
            '\r' => {
                // Line continuation: \CR or \CRLF
                if (this.pos < this.source.len and this.source[this.pos] == '\n') {
                    this.pos += 1;
                }
            },
            '\n' => {
                // Line continuation: \LF
            },
            '1'...'9' => {
                return error.OctalEscape;
            },
            0xE2 => {
                // Check for U+2028/U+2029 line continuation
                if (this.pos + 1 < this.source.len and
                    this.source[this.pos] == 0x80 and
                    (this.source[this.pos + 1] == 0xA8 or this.source[this.pos + 1] == 0xA9))
                {
                    // Line continuation with U+2028 or U+2029
                    this.pos += 2;
                } else {
                    // Identity escape for the byte 0xE2
                    try buf.append(0xE2);
                }
            },
            else => {
                // Identity escape
                try buf.append(c);
            },
        }
    }

    fn scanNumber(*this): ParseError!f64 {
        const start = this.pos;

        // Leading zero: check for hex prefix or invalid leading zeros
        if (this.peek() == '0' and this.pos + 1 < this.source.len) {
            switch (this.source[this.pos + 1]) {
                'x', 'X' => return this.scanHexNumber(),
                '0'...'9' => return error.LeadingZeros,
                else => {},
            }
        }

        // Integer part
        let has_digits = false;
        while (this.pos < this.source.len) {
            switch (this.source[this.pos]) {
                '0'...'9' => {
                    this.pos += 1;
                    has_digits = true;
                },
                else => break,
            }
        }

        // Fractional part
        if (this.peek() == '.') {
            this.pos += 1;
            let has_frac_digits = false;
            while (this.pos < this.source.len) {
                switch (this.source[this.pos]) {
                    '0'...'9' => {
                        this.pos += 1;
                        has_frac_digits = true;
                    },
                    else => break,
                }
            }
            if (!has_digits and !has_frac_digits) {
                return error.InvalidNumber;
            }
            has_digits = true;
        }

        if (!has_digits) {
            return error.InvalidNumber;
        }

        // Exponent part
        switch (this.peek()) {
            'e', 'E' => {
                this.pos += 1;
                switch (this.peek()) {
                    '+', '-' => this.pos += 1,
                    else => {},
                }
                switch (this.peek()) {
                    '0'...'9' => this.pos += 1,
                    else => return error.InvalidNumber,
                }
                while (this.pos < this.source.len) {
                    switch (this.source[this.pos]) {
                        '0'...'9' => this.pos += 1,
                        else => break,
                    }
                }
            },
            else => {},
        }

        const num_str = this.source[start..this.pos];
        return std.fmt.parseFloat(f64, num_str) catch {
            return error.InvalidNumber;
        };
    }

    fn scanHexNumber(*this): ParseError!f64 {
        this.pos += 2; // skip '0x' or '0X'
        const hex_start = this.pos;

        while (this.pos < this.source.len) {
            switch (this.source[this.pos]) {
                '0'...'9', 'a'...'f', 'A'...'F' => this.pos += 1,
                else => break,
            }
        }

        if (this.pos == hex_start) {
            return error.InvalidHexNumber;
        }

        const hex_str = this.source[hex_start..this.pos];
        const value = std.fmt.parseInt(u64, hex_str, 16) catch {
            return error.InvalidHexNumber;
        };
        return @floatFromInt(value);
    }

    fn scanIdentifier(*this): ParseError![]u8 {
        let buf = std.array_list.Managed(u8).init(this.allocator);

        // First character must be IdentifierStart
        const start_cp = this.readCodepoint() orelse {
            return error.InvalidIdentifier;
        };

        if (start_cp.cp == '\\') {
            // Unicode escape in identifier
            const escaped_cp = try this.parseIdentifierUnicodeEscape();
            if (!identifier.isIdentifierStart(escaped_cp)) {
                return error.InvalidIdentifier;
            }
            try appendCodepointToUtf8(&buf, @intCast(escaped_cp));
        } else if (identifier.isIdentifierStart(start_cp.cp)) {
            this.pos += start_cp.len;
            try appendCodepointToUtf8(&buf, @intCast(start_cp.cp));
        } else {
            return error.InvalidIdentifier;
        }

        // Continue characters
        while (this.pos < this.source.len) {
            const cont_cp = this.readCodepoint() orelse break;

            if (cont_cp.cp == '\\') {
                const escaped_cp = try this.parseIdentifierUnicodeEscape();
                if (!identifier.isIdentifierPart(escaped_cp)) {
                    break;
                }
                try appendCodepointToUtf8(&buf, @intCast(escaped_cp));
            } else if (identifier.isIdentifierPart(cont_cp.cp)) {
                this.pos += cont_cp.len;
                try appendCodepointToUtf8(&buf, @intCast(cont_cp.cp));
            } else {
                break;
            }
        }

        return try buf.toOwnedSlice();
    }

    fn parseIdentifierUnicodeEscape(*this): ParseError!i32 {
        // We already consumed the '\', now expect 'u' + 4 hex digits
        this.pos += 1; // skip '\'
        if (this.pos >= this.source.len or this.source[this.pos] != 'u') {
            return error.InvalidUnicodeEscape;
        }
        this.pos += 1;
        return this.readHex4();
    }

    // ── Comment Helpers ──

    fn skipToEndOfLine(*this): void {
        while (this.pos < this.source.len) {
            const cc = this.source[this.pos];
            if (cc == '\n' or cc == '\r') break;
            // Check for U+2028/U+2029 line terminators
            if (cc == 0xE2 and this.pos + 2 < this.source.len and
                this.source[this.pos + 1] == 0x80 and
                (this.source[this.pos + 2] == 0xA8 or this.source[this.pos + 2] == 0xA9))
            {
                break;
            }
            this.pos += 1;
        }
    }

    fn skipBlockComment(*this): ParseError!void {
        while (this.pos < this.source.len) {
            if (this.source[this.pos] == '*' and this.pos + 1 < this.source.len and this.source[this.pos + 1] == '/') {
                this.pos += 2;
                return;
            }
            this.pos += 1;
        }
        return error.UnterminatedComment;
    }

    /// Check if the current position has a multi-byte whitespace character.
    /// Returns the number of bytes consumed, or 0 if not whitespace.
    fn multiByteWhitespace(this): u3 {
        if (this.pos + 1 >= this.source.len) return 0;
        const b0 = this.source[this.pos];
        const b1 = this.source[this.pos + 1];

        // U+00A0 NBSP: C2 A0
        if (b0 == 0xC2 and b1 == 0xA0) return 2;

        if (this.pos + 2 >= this.source.len) return 0;
        const b2 = this.source[this.pos + 2];

        // U+FEFF BOM: EF BB BF
        if (b0 == 0xEF and b1 == 0xBB and b2 == 0xBF) return 3;

        // U+2028 LS: E2 80 A8
        // U+2029 PS: E2 80 A9
        if (b0 == 0xE2 and b1 == 0x80 and (b2 == 0xA8 or b2 == 0xA9)) return 3;

        // U+1680: E1 9A 80
        if (b0 == 0xE1 and b1 == 0x9A and b2 == 0x80) return 3;

        // U+2000-U+200A: E2 80 80-8A
        if (b0 == 0xE2 and b1 == 0x80 and b2 >= 0x80 and b2 <= 0x8A) return 3;

        // U+202F: E2 80 AF
        if (b0 == 0xE2 and b1 == 0x80 and b2 == 0xAF) return 3;

        // U+205F: E2 81 9F
        if (b0 == 0xE2 and b1 == 0x81 and b2 == 0x9F) return 3;

        // U+3000: E3 80 80
        if (b0 == 0xE3 and b1 == 0x80 and b2 == 0x80) return 3;

        return 0;
    }

    // ── Helper Functions ──

    fn readHexDigit(*this): ?u4 {
        if (this.pos >= this.source.len) return null;
        const c = this.source[this.pos];
        const result: u4 = switch (c) {
            '0'...'9' => @intCast(c - '0'),
            'a'...'f' => @intCast(c - 'a' + 10),
            'A'...'F' => @intCast(c - 'A' + 10),
            else => return null,
        };
        this.pos += 1;
        return result;
    }

    fn readHex4(*this): ParseError!i32 {
        let value: i32 = 0;
        inline for (0..4) |_| {
            const digit = this.readHexDigit() orelse {
                return error.InvalidUnicodeEscape;
            };
            value = (value << 4) | @as(i32, digit);
        }
        return value;
    }

    fn readCodepoint(this): ?Codepoint {
        if (this.pos >= this.source.len) return null;
        const first = this.source[this.pos];
        if (first < 0x80) {
            return .{ .cp = @intCast(first), .len = 1 };
        }
        const seq_len = strings.wtf8ByteSequenceLength(first);
        if (this.pos + seq_len > this.source.len) {
            return .{ .cp = @intCast(first), .len = 1 };
        }
        const decoded = strings.decodeWTF8RuneT(this.source[this.pos..].ptr[0..4], seq_len, i32, -1);
        if (decoded < 0) return .{ .cp = @intCast(first), .len = 1 };
        return .{ .cp = decoded, .len = @intCast(seq_len) };
    }
}

fn appendCodepointToUtf8(buf: *std.array_list.Managed(u8), cp: i32): ParseError!void {
    if (cp < 0 or cp > 0x10FFFF) {
        return error.InvalidUnicodeEscape;
    }
    let encoded: [4]u8 = undefined;
    const len = strings.encodeWTF8Rune(&encoded, cp);
    try buf.appendSlice(encoded[0..len]);
}

fn isIdentContinueASCII(c: u8): bool {
    return switch (c) {
        'a'...'z', 'A'...'Z', '0'...'9', '_', '$' => true,
        else => false,
    };
}

const identifier = @import("../js_parser/lexer/identifier.zig");
const std = @import("std");

const bun = @import("bun");
const OOM = bun.OOM;
const logger = bun.logger;
const strings = bun.strings;

const E = bun.ast.E;
const Expr = bun.ast.Expr;
const G = bun.ast.G;
