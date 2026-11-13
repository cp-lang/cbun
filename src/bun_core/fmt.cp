import bun from "bun";
import std from "std";

const Environment = bun.Environment;
const Output = bun.Output;
const js_lexer = bun.js_lexer;
const sha = bun.sha;
const strings = bun.strings;
const fmt = std.fmt;

pub struct TableSymbols {
    enable_ansi_colors: bool;

    pub const unicode = TABLE_SYMBOLS_UNICODE;
    pub const ascii = TABLE_SYMBOLS_ASCII;

    pub fn topLeftSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┌" else "|";
    }
    pub fn topRightSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┐" else "|";
    }
    pub fn topColumnSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┬" else "-";
    }

    pub fn bottomLeftSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "└" else "|";
    }
    pub fn bottomRightSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┘" else "|";
    }
    pub fn bottomColumnSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┴" else "-";
    }

    pub fn middleLeftSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "├" else "|";
    }
    pub fn middleRightSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┤" else "|";
    }
    pub fn middleColumnSep(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "┼" else "|";
    }

    pub fn horizontalEdge(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "─" else "-";
    }
    pub fn verticalEdge(comptime s: TableSymbols): string {
        return if (s.enable_ansi_colors) "│" else "|";
    }
}
const TABLE_SYMBOLS_UNICODE = TableSymbols{ enable_ansi_colors: true };
const TABLE_SYMBOLS_ASCII = TableSymbols{ enable_ansi_colors: false };

type ConstU8Slice = []const u8;
const SHA512_DIGEST_LEN = sha.SHA512.digest;
const SHA512_B64_LEN = std.base64.standard.Encoder.calcSize(SHA512_DIGEST_LEN);

pub fn Table(
    comptime column_color: string,
    comptime column_left_pad: usize,
    comptime column_right_pad: usize,
    comptime enable_ansi_colors: bool,
): type {
    const symbols = TableSymbols{ enable_ansi_colors: enable_ansi_colors };
    return struct {
        column_names: []const string;
        column_inside_lengths: []const usize;

        pub static fn init(column_names_: []const string, column_inside_lengths_: []const usize): @This() {
            return {
                column_names: column_names_,
                column_inside_lengths: column_inside_lengths_,
            };
        }

        pub fn printTopLineSeparator(this): void {
            this.printLine(symbols.topLeftSep(), symbols.topRightSep(), symbols.topColumnSep());
        }

        pub fn printBottomLineSeparator(this): void {
            this.printLine(symbols.bottomLeftSep(), symbols.bottomRightSep(), symbols.bottomColumnSep());
        }

        pub fn printLineSeparator(this): void {
            this.printLine(symbols.middleLeftSep(), symbols.middleRightSep(), symbols.middleColumnSep());
        }

        pub fn printLine(this, left_edge_separator: string, right_edge_separator: string, column_separator: string): void {
            for (i of 0..this.column_inside_lengths.len) {
                const column_inside_length = this.column_inside_lengths[i];
                if (i == 0) {
                    Output.pretty("{s}", [left_edge_separator]);
                } else {
                    Output.pretty("{s}", [column_separator]);
                }

                for (j of 0..(column_left_pad + column_inside_length + column_right_pad)) {
                    Output.pretty("{s}", [symbols.horizontalEdge()]);
                }

                if (i == this.column_inside_lengths.len - 1) {
                    Output.pretty("{s}\n", [right_edge_separator]);
                }
            }
        }

        pub fn printColumnNames(this): void {
            const col_fmt = comptime std.fmt.comptimePrint("<b><{s}>{{s}}<r>", [column_color]);
            for (i of 0..this.column_inside_lengths.len) {
                const column_inside_length = this.column_inside_lengths[i];
                Output.pretty("{s}", [symbols.verticalEdge()]);
                for (j of 0..column_left_pad) {
                    Output.pretty(" ", []);
                }
                Output.pretty(col_fmt, [this.column_names[i]]);
                for (j of this.column_names[i].len..(column_inside_length + column_right_pad)) {
                    Output.pretty(" ", []);
                }
                if (i == this.column_inside_lengths.len - 1) {
                    Output.pretty("{s}\n", [symbols.verticalEdge()]);
                }
            }
        }
    };
}

pub struct RedactedNpmUrlFormatter {
    url: string;

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        let i: usize = 0;
        while (i < this.url.len) {
            if (strings.startsWithUUID(this.url[i..])) {
                writer.writeAll("***")?;
                i += 36;
                continue;
            }

            const npm_secret_len = strings.startsWithNpmSecret(this.url[i..]);
            if (npm_secret_len > 0) {
                writer.writeAll("***")?;
                i += npm_secret_len;
                continue;
            }

            // TODO: redact password from `https://username:password@registry.com/`

            writer.writeByte(this.url[i])?;
            i += 1;
        }
    }
}

pub fn redactedNpmUrl(str: string): RedactedNpmUrlFormatter {
    return RedactedNpmUrlFormatter{
        url: str,
    };
}

pub struct RedactedSourceFormatter {
    text: string;

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        let i: usize = 0;
        const text: string = this.text;
        while (i < text.len) {
            if (secret := strings.startsWithSecret(text[i..])) {
                const offset = secret[0];
                const len = secret[1];
                writer.writeAll(text[i + 0..i + offset])?;
                writer.splatByteAll('*', len)?;
                i += offset + len;
                continue;
            }

            writer.writeByte(text[i])?;
            i += 1;
        }
    }
}

pub fn redactedSource(str: string): RedactedSourceFormatter {
    return RedactedSourceFormatter{
        text: str,
    };
}

// https://github.com/npm/cli/blob/63d6a732c3c0e9c19fd4d147eaa5cc27c29b168d/node_modules/npm-package-arg/lib/npa.js#L163
pub struct DependencyUrlFormatter {
    url: string;

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        let remain: string = this.url;
        while (slash := strings.indexOfChar(remain, '/')) {
            writer.writeAll(remain[0..slash])?;
            writer.writeAll("%2f")?;
            remain = remain[slash + 1 ..];
        }
        writer.writeAll(remain)?;
    }
}

pub fn dependencyUrl(url: string): DependencyUrlFormatter {
    return DependencyUrlFormatter{
        url: url,
    };
}

enum IntegrityFormatStyle {
    short,
    full,
}

pub fn IntegrityFormatter(comptime style: IntegrityFormatStyle): type {
    return struct {
        bytes: [SHA512_DIGEST_LEN]u8;

        pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
            let buf: [SHA512_B64_LEN]u8 = undefined;
            const count = bun.simdutf.base64.encode(this.bytes[0..sha.SHA512.digest], &buf, false);

            const encoded = buf[0..count];

            if (comptime style == IntegrityFormatStyle.short) {
                writer.print("sha512-{s}[...]{s}", [encoded[0..13], encoded[encoded.len - 15 ..]])?;
            } else {
                writer.print("sha512-{s}", [encoded])?;
            }
        }
    };
}

pub fn integrity(bytes: [SHA512_DIGEST_LEN]u8, comptime style: IntegrityFormatStyle): IntegrityFormatter(style) {
    return { bytes: bytes };
}

struct JSONFormatter {
    input: string;

    pub fn format(this, writer: *std.Io.Writer): ! {
        bun.js_printer.writeJSONString(this.input, @TypeOf(writer), writer, .latin1)?;
    }
}

struct JSONFormatterUTF8Options {
    quote: bool = true;
}

struct JSONFormatterUTF8 {
    input: string;
    opts: Options;

    pub const Options = JSONFormatterUTF8Options;

    pub fn format(this, writer: *std.Io.Writer): ! {
        if (this.opts.quote) {
            bun.js_printer.writeJSONString(this.input, @TypeOf(writer), writer, .utf8)?;
        } else {
            bun.js_printer.writePreQuotedString(this.input, @TypeOf(writer), writer, '"', false, true, .utf8)?;
        }
    }
}

/// Expects latin1
pub fn formatJSONStringLatin1(text: string): JSONFormatter {
    return JSONFormatter{ input: text };
}

pub fn formatJSONStringUTF8(text: string, opts: JSONFormatterUTF8Options): JSONFormatterUTF8 {
    return JSONFormatterUTF8{ input: text, opts: opts };
}

type SharedTempBuffer = [32768]u8;
@tls let shared_temp_buffer_ptr: *SharedTempBuffer!? = null;

fn getSharedBuffer(): []u8 {
    if (existing := shared_temp_buffer_ptr) {
        return std.mem.asBytes(existing);
    }
    const created = bun.default_allocator.create(SharedTempBuffer) catch unreachable;
    shared_temp_buffer_ptr = created;
    return std.mem.asBytes(created);
}

pub fn formatUTF16Type(slice_: []const u16, writer: *std.Io.Writer): ! {
    let slice = slice_[0..];
    let chunk = getSharedBuffer();

    // Defensively ensure recursion doesn't cause the buffer to be overwritten in-place
    shared_temp_buffer_ptr = null;
    defer {
        if (existing := shared_temp_buffer_ptr) {
            if (existing != chunk.ptr) {
                unsafe {
                    bun.default_allocator.destroy(chunk.ptr as *SharedTempBuffer!);
                }
            }
        } else {
            unsafe {
                shared_temp_buffer_ptr = chunk.ptr as *SharedTempBuffer!;
            }
        }
    }


    while (slice.len > 0) {
        const result = strings.copyUTF16IntoUTF8(chunk, slice);
        if (result.read == 0 || result.written == 0) {
            break;
        }
        writer.writeAll(chunk[0..result.written])?;
        slice = slice[result.read..];
    }
}

pub fn formatUTF16TypeWithPathOptions(slice_: []const u16, writer: *std.Io.Writer, opts: PathFormatOptions): ! {
    let slice = slice_[0..];
    let chunk = getSharedBuffer();

    // Defensively ensure recursion doesn't cause the buffer to be overwritten in-place
    shared_temp_buffer_ptr = null;
    defer {
        if (existing := shared_temp_buffer_ptr) {
            if (existing != chunk.ptr) {
                unsafe {
                    bun.default_allocator.destroy(chunk.ptr as *SharedTempBuffer!);
                }
            }
        } else {
            unsafe {
                shared_temp_buffer_ptr = chunk.ptr as *SharedTempBuffer!;
            }
        }
    }


    while (slice.len > 0) {
        const result = strings.copyUTF16IntoUTF8(chunk, slice);
        if (result.read == 0 || result.written == 0) {
            break;
        }

        const to_write = chunk[0..result.written];
        if (!opts.escape_backslashes && opts.path_sep == PathFormatSep.any) {
            writer.writeAll(to_write)?;
        } else {
            let ptr: string = to_write;
            while (i := strings.indexOfAny(ptr, "\\/")) {
                const sep: u8 = switch (opts.path_sep) {
                    .windows => 0x5C as u8,
                    .posix => '/',
                    .auto => std.fs.path.sep as u8,
                    .any => ptr[i],
                };
                writer.writeAll(ptr[0..i])?;
                writer.writeByte(sep)?;
                if (opts.escape_backslashes && sep == 0x5C) {
                    writer.writeByte(sep)?;
                }

                ptr = ptr[i + 1 ..];
            }
            writer.writeAll(ptr)?;
        }
        slice = slice[result.read..];
    }
}

pub inline fn utf16(slice_: []const u16): FormatUTF16 {
    return FormatUTF16{ buf: slice_ };
}

/// Debug, this does not handle invalid utf32
pub inline fn debugUtf32PathFormatter(path: []const u32): DebugUTF32PathFormatter {
    return DebugUTF32PathFormatter{ path: path };
}

pub struct DebugUTF32PathFormatter {
    path: []const u32;
    pub fn format(this, writer: *std.Io.Writer): ! {
        let path_buf: bun.PathBuffer = undefined;
        const result = bun.simdutf.convert.utf32.to.utf8.with_errors.le(this.path, &path_buf);
        const converted: string = if (result.isSuccessful())
            path_buf[0..result.count]
        else
            "Invalid UTF32!";

        writer.writeAll(converted)?;
    }
}

pub struct FormatUTF16 {
    buf: []const u16;
    path_fmt_opts: PathFormatOptions? = null;
    pub fn format(this, writer: *std.Io.Writer): ! {
        if (opts := this.path_fmt_opts) {
            formatUTF16TypeWithPathOptions(this.buf, writer, opts)?;
        } else {
            formatUTF16Type(this.buf, writer)?;
        }
    }
}

pub struct FormatUTF8 {
    buf: string;
    path_fmt_opts: PathFormatOptions? = null;
    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        if (opts := this.path_fmt_opts) {
            if (opts.path_sep == PathFormatSep.any && opts.escape_backslashes == false) {
                writer.writeAll(this.buf)?;
                return;
            }

            let ptr: string = this.buf;
            while (i := strings.indexOfAny(ptr, "\\/")) {
                const sep: u8 = switch (opts.path_sep) {
                    .windows => 0x5C as u8,
                    .posix => '/',
                    .auto => std.fs.path.sep as u8,
                    .any => ptr[i],
                };
                writer.writeAll(ptr[0..i])?;
                writer.writeByte(sep)?;
                if (opts.escape_backslashes && sep == 0x5C) {
                    writer.writeByte(sep)?;
                }
                ptr = ptr[i + 1 ..];
            }

            writer.writeAll(ptr)?;
            return;
        }

        writer.writeAll(this.buf)?;
    }
}

pub enum PathFormatSep {
    /// Keep paths separators as is.
    any,
    /// Replace all path separators with the current platform path separator.
    auto,
    /// Replace all path separators with `/`.
    posix,
    /// Replace all path separators with `\`.
    windows,
}

pub struct PathFormatOptions {
    // The path separator used when formatting the path.
    path_sep: PathFormatSep = PathFormatSep.any;

    /// Any backslashes are escaped, including backslashes
    /// added through `path_sep`.
    escape_backslashes: bool = false;

    pub const Sep = PathFormatSep;
}

pub const FormatOSPath = if (Environment.isWindows) FormatUTF16 else FormatUTF8;

pub fn fmtOSPath(buf: bun.OSPathSlice, options: PathFormatOptions): FormatOSPath {
    return FormatOSPath{
        buf: buf,
        path_fmt_opts: options,
    };
}

fn PathFormatterType(comptime T: type): type {
    return if (T == u8) FormatUTF8 else FormatUTF16;
}

pub fn fmtPath(
    comptime T: type,
    path: []const T,
    options: PathFormatOptions,
): PathFormatterType(T) {
    if (T == u8) {
        return FormatUTF8{
            buf: path,
            path_fmt_opts: options,
        };
    }

    return FormatUTF16{
        buf: path,
        path_fmt_opts: options,
    };
}

pub fn formatLatin1(slice_: string, writer: *std.Io.Writer): ! {
    let chunk = getSharedBuffer();
    let slice: string = slice_;

    // Defensively ensure recursion doesn't cause the buffer to be overwritten in-place
    shared_temp_buffer_ptr = null;
    defer {
        if (existing := shared_temp_buffer_ptr) {
            if (existing != chunk.ptr) {
                unsafe {
                    bun.default_allocator.destroy(chunk.ptr as *SharedTempBuffer!);
                }
            }
        } else {
            unsafe {
                shared_temp_buffer_ptr = chunk.ptr as *SharedTempBuffer!;
            }
        }
    }

    while (i := strings.firstNonASCII(slice)) {
        if (i > 0) {
            writer.writeAll(slice[0..i])?;
            slice = slice[i..];
        }
        const result = strings.copyLatin1IntoUTF8(chunk, slice[0..@min(chunk.len, slice.len)]);
        if (result.read == 0 || result.written == 0) {
            break;
        }
        writer.writeAll(chunk[0..result.written])?;
        slice = slice[result.read..];
    }

    if (slice.len > 0) {
        writer.writeAll(slice)?;
    }
}

enum URLProto {
    http,
    https,
    unix,
    abstract,
}

pub struct URLFormatter {
    proto: URLProto = .http;
    hostname: string? = null;
    port: u16? = null;

    const Proto = URLProto;

    pub fn format(this, writer: *std.Io.Writer): ! {
        writer.print("{s}://", [switch (this.proto) {
            .http => "http",
            .https => "https",
            .unix => "unix",
            .abstract => "abstract",
        }])?;

        if (hostname := this.hostname) {
            const needs_brackets = hostname[0] != '[' && strings.isIPV6Address(hostname);
            if (needs_brackets) {
                writer.print("[{s}]", [hostname])?;
            } else {
                writer.writeAll(hostname)?;
            }
        } else {
            writer.writeAll("localhost")?;
        }

        if (this.proto == .unix) {
            return;
        }

        const is_port_optional = this.port == null || (this.proto == .https && this.port == 443) ||
            (this.proto == .http && this.port == 80);
        if (is_port_optional) {
            writer.writeAll("/")?;
        } else {
            writer.print(":{d}/", [this.port.?])?;
        }
    }
}

pub struct HostFormatter {
    host: string;
    port: u16? = null;
    is_https: bool = false;

    pub fn format(this, writer: *std.Io.Writer): ! {
        if (strings.indexOfChar(this.host, ':') != null) {
            writer.writeAll(this.host)?;
            return;
        }

        writer.writeAll(this.host)?;

        const is_port_optional = this.port == null || (this.is_https && this.port == 443) ||
            (!this.is_https && this.port == 80);
        if (!is_port_optional) {
            writer.print(":{d}", [this.port.?])?;
            return;
        }
    }
}

/// Format a string to an ECMAScript identifier.
pub fn fmtIdentifier(name: string): FormatValidIdentifier {
    return FormatValidIdentifier{ name: name };
}

/// Format a string to an ECMAScript identifier.
pub struct FormatValidIdentifier {
    name: string;
    pub fn format(this, writer: *std.Io.Writer): ! {
        let iterator = strings.CodepointIterator.init(this.name);
        let cursor = strings.CodepointIterator.Cursor{};

        let has_needed_gap = false;
        let needs_gap = false;
        let start_i: usize = 0;

        if (!iterator.next(&cursor)) {
            writer.writeAll("_")?;
            return;
        }

        // Common case: no gap necessary. No allocation necessary.
        needs_gap = !js_lexer.isIdentifierStart(cursor.c);
        if (!needs_gap) {
            // Are there any non-alphanumeric chars at all?
            while (iterator.next(&cursor)) {
                if (!js_lexer.isIdentifierContinue(cursor.c) || cursor.width > 1) {
                    needs_gap = true;
                    start_i = cursor.i;
                    break;
                }
            }
        }

        if (needs_gap) {
            needs_gap = false;
            if (start_i > 0) writer.writeAll(this.name[0..start_i])?;
            const slice = this.name[start_i..];
            iterator = strings.CodepointIterator.init(slice);
            cursor = strings.CodepointIterator.Cursor{};

            while (iterator.next(&cursor)) {
                if (js_lexer.isIdentifierContinue(cursor.c) && cursor.width == 1) {
                    if (needs_gap) {
                        writer.writeAll("_")?;
                        needs_gap = false;
                        has_needed_gap = true;
                    }
                    writer.writeAll(slice[cursor.i .. cursor.i + (cursor.width as u32)])?;
                } else if (!needs_gap) {
                    needs_gap = true;
                }
            }

            // If it ends with an emoji
            if (needs_gap) {
                writer.writeAll("_")?;
                needs_gap = false;
                has_needed_gap = true;
            }

            return;
        }

        writer.writeAll(this.name)?;
    }
}

pub fn githubActionWriter(writer: *std.Io.Writer, self: string): ! {
    let offset: usize = 0;
    const end = self.len as u32;
    while (offset < end) {
        if (i := strings.indexOfNewlineOrNonASCIIOrANSI(self, offset as u32)) {
            const byte = self[i];
            if (byte > 0x7F) {
                offset += @max(strings.wtf8ByteSequenceLength(byte), 1);
                continue;
            }
            if (i > 0) {
                writer.writeAll(self[offset..i])?;
            }
            let n: usize = 1;
            if (byte == '\n') {
                writer.writeAll("%0A")?;
            } else if (i + 1 < end) {
                const next = self[i + 1];
                if (byte == '\r' && next == '\n') {
                    n += 1;
                    writer.writeAll("%0A")?;
                } else if (byte == 0x1b && next == '[') {
                    n += 1;
                    if (i + 2 < end) {
                        const rem_ = self[(i + 2)..@min(i + 5, end)];
                        if (j := strings.indexOfChar(rem_, 'm')) {
                            n += j + 1;
                        }
                    }
                }
            }
            offset = i + n;
        } else {
            writer.writeAll(self[offset..end])?;
            break;
        }
    }
}

pub struct GithubActionFormatter {
    text: string;

    pub fn format(this, writer: *std.Io.Writer): ! {
        githubActionWriter(writer, this.text)?;
    }
}

pub fn githubAction(self: string): strings.GithubActionFormatter {
    return GithubActionFormatter{
        text: self,
    };
}

pub fn quotedWriter(writer: *std.Io.Writer, self: string): ! {
    const rest_ = self;
    if (strings.containsNewlineOrNonASCIIOrQuote(rest_)) {
        bun.js_printer.writeJSONString(self, @TypeOf(writer), writer, strings.Encoding.utf8)?;
    } else {
        writer.writeAll("\"")?;
        writer.writeAll(self)?;
        writer.writeAll("\"")?;
    }
}

pub struct QuotedFormatter {
    text: string;

    pub fn format(this, writer: *std.Io.Writer): ! {
        quotedWriter(writer, this.text)?;
    }
}

pub fn fmtJavaScript(text: string, opts: HighlighterOptions): QuickAndDirtyJavaScriptSyntaxHighlighter {
    return QuickAndDirtyJavaScriptSyntaxHighlighter{
        text: text,
        opts: opts,
    };
}

pub struct HighlighterOptions {
    enable_colors: bool;
    check_for_unhighlighted_write: bool = true;
    redact_sensitive_information: bool = false;
}

enum HighlighterColorCode {
    magenta,
    blue,
    orange,
    red,
    pink,

    pub fn color(this): string {
        return switch (this) {
            .magenta => "\x1b[35m",
            .blue => "\x1b[34m",
            .orange => "\x1b[33m",
            .red => "\x1b[31m",
            // light pink
            .pink => "\x1b[38;5;206m",
        };
    }
}

pub enum HighlighterKeyword {
    abstract,
    @"as",
    @"async",
    @"await",
    case,
    @"catch",
    @"class",
    @"const",
    @"continue",
    debugger,
    default,
    delete,
    do,
    @"else",
    @"enum",
    @"export",
    extends,
    @"false",
    finally,
    @"for",
    function,
    @"if",
    @"implements",
    @"import",
    @"in",
    instanceof,
    @"interface",
    @"let",
    @"new",
    @"null",
    package,
    @"private",
    protected,
    public,
    @"return",
    @"static",
    @"super",
    @"switch",
    @"this",
    @"throw",
    @"break",
    @"true",
    @"try",
    @"type",
    typeof,
    @"var",
    @"void",
    @"while",
    with,
    yield,
    @"string",
    number,
    boolean,
    symbol,
    @"any",
    object,
    unknown,
    @"never",
    namespace,
    declare,
    readonly,
    @"undefined",

    pub fn colorCode(this): HighlighterColorCode {
        return switch (this) {
            .abstract => .blue,
            .@"as" => .blue,
            .@"async" => .magenta,
            .@"await" => .magenta,
            .case => .magenta,
            .@"catch" => .magenta,
            .@"class" => .magenta,
            .@"const" => .magenta,
            .@"continue" => .magenta,
            .debugger => .magenta,
            .default => .magenta,
            .delete => .red,
            .do => .magenta,
            .@"else" => .magenta,
            .@"break" => .magenta,
            .@"undefined" => .orange,
            .@"enum" => .blue,
            .@"export" => .magenta,
            .extends => .magenta,
            .@"false" => .orange,
            .finally => .magenta,
            .@"for" => .magenta,
            .function => .magenta,
            .@"if" => .magenta,
            .@"implements" => .blue,
            .@"import" => .magenta,
            .@"in" => .magenta,
            .instanceof => .magenta,
            .@"interface" => .blue,
            .@"let" => .magenta,
            .@"new" => .magenta,
            .@"null" => .orange,
            .package => .magenta,
            .@"private" => .blue,
            .protected => .blue,
            .public => .blue,
            .@"return" => .magenta,
            .@"static" => .magenta,
            .@"super" => .magenta,
            .@"switch" => .magenta,
            .@"this" => .orange,
            .@"throw" => .magenta,
            .@"true" => .orange,
            .@"try" => .magenta,
            .@"type" => .blue,
            .typeof => .magenta,
            .@"var" => .magenta,
            .@"void" => .magenta,
            .@"while" => .magenta,
            .with => .magenta,
            .yield => .magenta,
            .@"string" => .blue,
            .number => .blue,
            .boolean => .blue,
            .symbol => .blue,
            .@"any" => .blue,
            .object => .blue,
            .unknown => .blue,
            .@"never" => .blue,
            .namespace => .blue,
            .declare => .blue,
            .readonly => .blue,
        };
    }
}

pub enum HighlighterRedactedKeyword {
    _auth,
    _authToken,
    token,
    _password,
    email,
}

type HighlighterTextSlice = []const u8;

pub struct QuickAndDirtyJavaScriptSyntaxHighlighter {
    text: string;
    opts: HighlighterOptions;

    pub const Options = HighlighterOptions;
    const ColorCode = HighlighterColorCode;
    pub const Keyword = HighlighterKeyword;
    pub const Keywords = bun.ComptimeEnumMap(HighlighterKeyword);
    pub const RedactedKeyword = HighlighterRedactedKeyword;
    pub const RedactedKeywords = bun.ComptimeEnumMap(HighlighterRedactedKeyword);

    fn highlightStringLit(
        comptime char: u8,
        writer: *std.Io.Writer,
        text_ptr: *string,
        should_redact_ptr: *bool,
        opts: HighlighterOptions,
    ): !bool {
        let txt: string = text_ptr.*;
        let i: usize = 1;
        while (i < txt.len && txt[i] != char) {
            if (!should_redact_ptr.* && char == '`') {
                if (txt[i] == '$' && i + 1 < txt.len && txt[i + 1] == '{') {
                    const curly_start = i;
                    i += 2;

                    while (i < txt.len && txt[i] != '}') {
                        if (txt[i] == 0x5C) {
                            i += 1;
                        }
                        i += 1;
                    }

                    writer.print(Output.prettyFmt("<r><green>{s}<r>", true), [txt[0..curly_start]])?;
                    writer.writeAll("${")?;
                    const sub_opts = HighlighterOptions{
                        enable_colors: opts.enable_colors,
                        check_for_unhighlighted_write: false,
                        redact_sensitive_information: opts.redact_sensitive_information,
                    };
                    const curly_remain = QuickAndDirtyJavaScriptSyntaxHighlighter{
                        text: txt[curly_start + 2 .. i],
                        opts: sub_opts,
                    };

                    if (curly_remain.text.len > 0) {
                        curly_remain.format(writer)?;
                    }

                    if (i < txt.len && txt[i] == '}') {
                        writer.writeAll("}")?;
                        i += 1;
                    }
                    txt = txt[i..];
                    i = 0;
                    if (txt.len > 0 && txt[0] == char) {
                        writer.writeAll(Output.prettyFmt("<r><green>`<r>", true))?;
                        text_ptr.* = txt[1..];
                        return true;
                    }
                    continue;
                }
            }

            if (i + 1 < txt.len && txt[i] == 0x5C) {
                i += 1;
            }

            i += 1;
        }

        // Include the trailing quote, if any
        i += @intFromBool(i < txt.len);

        if (should_redact_ptr.*) {
            should_redact_ptr.* = false;
            if (i > 2 && txt[i - 1] == char) {
                const len = txt[0..i].len - 2;
                writer.print(Output.prettyFmt("<r><green>{c}", true), [char])?;
                writer.splatByteAll('*', len)?;
                writer.print(Output.prettyFmt("{c}<r>", true), [char])?;
            } else {
                writer.splatByteAll('*', txt[0..i].len)?;
            }
            text_ptr.* = txt[i..];
            return false;
        } else if (opts.redact_sensitive_information) {
            try_redact: {
                let inner: string = txt[1..i];
                if (inner.len > 0 && inner[inner.len - 1] == char) {
                    inner = inner[0 .. inner.len - 1];
                }

                if (inner.len == 0) {
                    break :try_redact;
                }

                if (inner.len == 36 && strings.isUUID(inner)) {
                    writer.print(Output.prettyFmt("<r><green>{c}", true), [char])?;
                    writer.splatByteAll('*', 36)?;
                    writer.print(Output.prettyFmt("{c}<r>", true), [char])?;
                    text_ptr.* = txt[i..];
                    return false;
                }

                const npm_secret_len = strings.startsWithNpmSecret(inner);
                if (npm_secret_len != 0) {
                    writer.print(Output.prettyFmt("<r><green>{c}", true), [char])?;
                    writer.splatByteAll('*', npm_secret_len)?;
                    writer.print(Output.prettyFmt("{c}<r>", true), [char])?;
                    text_ptr.* = txt[i..];
                    return false;
                }

                if (url_pass := strings.findUrlPassword(inner)) {
                    const [offset, len] = url_pass;
                    writer.print(Output.prettyFmt("<r><green>{c}{s}", true), [
                        char,
                        inner[0..offset],
                    ])?;
                    writer.splatByteAll('*', len)?;
                    writer.print(Output.prettyFmt("{s}{c}<r>", true), [
                        inner[offset + len ..],
                        char,
                    ])?;
                    text_ptr.* = txt[i..];
                    return false;
                }
            }

            writer.print(Output.prettyFmt("<r><green>{s}<r>", true), [txt[0..i]])?;
            text_ptr.* = txt[i..];
            return false;
        }

        writer.print(Output.prettyFmt("<r><green>{s}<r>", true), [txt[0..i]])?;
        text_ptr.* = txt[i..];
        return false;
    }

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        let txt = this.text[0..];
        if (this.opts.check_for_unhighlighted_write) {
            if (!this.opts.enable_colors || txt.len > 2048 || txt.len == 0 || !strings.isAllASCII(txt)) {
                if (this.opts.redact_sensitive_information) {
                    writer.print("{f}", [redactedSource(txt)])?;
                } else {
                    writer.writeAll(txt)?;
                }
                return;
            }
        }

        let prev_keyword = null as HighlighterKeyword?;
        let should_redact_value = false;

        outer: while (txt.len > 0) {
            if (js_lexer.isIdentifierStart(txt[0])) {
                let i: usize = 1;

                while (i < txt.len && js_lexer.isIdentifierContinue(txt[i])) {
                    i += 1;
                }

                if (keyword := QuickAndDirtyJavaScriptSyntaxHighlighter.Keywords.get(txt[0..i])) {
                    should_redact_value = false;
                    if (keyword != HighlighterKeyword.as) {
                        prev_keyword = keyword;
                    }
                    const code = keyword.colorCode();
                    writer.print(Output.prettyFmt("<r>{s}{s}<r>", true), [code.color(), txt[0..i]])?;
                } else {
                    should_redact_value = this.opts.redact_sensitive_information && QuickAndDirtyJavaScriptSyntaxHighlighter.RedactedKeywords.has(txt[0..i]);
                    write: {
                        if (prev := prev_keyword) {
                            switch (prev) {
                                .new => {
                                    prev_keyword = null;

                                    if (i < txt.len && txt[i] == '(') {
                                        writer.print(Output.prettyFmt("<r><b>{s}<r>", true), [txt[0..i]])?;
                                        break :write;
                                    }
                                },
                                .abstract | .namespace | .declare | .@"type" | .@"interface" => {
                                    writer.print(Output.prettyFmt("<r><b><blue>{s}<r>", true), [txt[0..i]])?;
                                    prev_keyword = null;
                                    break :write;
                                },
                                .import => {
                                    if (strings.eqlComptime(txt[0..i], "from")) {
                                        const code = HighlighterColorCode.magenta;
                                        writer.print(Output.prettyFmt("<r>{s}{s}<r>", true), [code.color(), txt[0..i]])?;
                                        prev_keyword = null;

                                        break :write;
                                    }
                                },
                                else => {},
                            }
                        }

                        writer.writeAll(txt[0..i])?;
                    }
                }
                txt = txt[i..];
            } else {
                if (this.opts.redact_sensitive_information && should_redact_value) {
                    while (txt.len > 0 && std.ascii.isWhitespace(txt[0])) {
                        writer.writeByte(txt[0])?;
                        txt = txt[1..];
                    }

                    if (txt.len > 0 && (txt[0] == '=' || txt[0] == ':')) {
                        writer.writeByte(txt[0])?;
                        txt = txt[1..];
                        while (txt.len > 0 && std.ascii.isWhitespace(txt[0])) {
                            writer.writeByte(txt[0])?;
                            txt = txt[1..];
                        }

                        if (txt.len == 0) return;
                    }
                }

                switch (txt[0]) {
                    '0'...'9' => |num| {
                        if (this.opts.redact_sensitive_information) {
                            if (should_redact_value) {
                                should_redact_value = false;
                                const end = strings.indexOfChar(txt, '\n') ?? txt.len;
                                txt = txt[end..];
                                writer.writeAll(Output.prettyFmt("<r><yellow>***<r>", true))?;
                                continue;
                            }

                            if (strings.startsWithUUID(txt)) {
                                txt = txt[36..];
                                writer.writeAll(Output.prettyFmt("<r><yellow>***<r>", true))?;
                                continue;
                            }
                        }

                        prev_keyword = null;
                        let i: usize = 1;
                        if (txt.len > 1 && num == '0' && txt[1] == 'x') {
                            i += 1;
                            while (i < txt.len && switch (txt[i]) {
                                '0'...'9' | 'a'...'f' | 'A'...'F' => true,
                                else => false,
                            }) {
                                i += 1;
                            }
                        } else {
                            while (i < txt.len && switch (txt[i]) {
                                '0'...'9' | '.' | 'e' | 'E' | 'x' | 'X' | 'b' | 'B' | 'o' | 'O' => true,
                                else => false,
                            }) {
                                i += 1;
                            }
                        }

                        writer.print(Output.prettyFmt("<r><yellow>{s}<r>", true), [txt[0..i]])?;
                        txt = txt[i..];
                    },
                    '`' => {
                        prev_keyword = null;
                        if (QuickAndDirtyJavaScriptSyntaxHighlighter.highlightStringLit('`', writer, *txt, *should_redact_value, this.opts)?) {
                            continue :outer;
                        }
                    },
                    '"' => {
                        prev_keyword = null;
                        if (QuickAndDirtyJavaScriptSyntaxHighlighter.highlightStringLit('"', writer, *txt, *should_redact_value, this.opts)?) {
                            continue :outer;
                        }
                    },
                    0x27 => {
                        prev_keyword = null;
                        if (QuickAndDirtyJavaScriptSyntaxHighlighter.highlightStringLit(0x27, writer, *txt, *should_redact_value, this.opts)?) {
                            continue :outer;
                        }
                    },
                    '/' => {
                        prev_keyword = null;

                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }

                        let i: usize = 1;

                        // the start of a line comment
                        if (i < txt.len && txt[i] == '/') {
                            while (i < txt.len && txt[i] != '\n') {
                                i += 1;
                            }

                            const remain_to_print = txt[0..i];
                            if (i < txt.len && txt[i] == '\n') {
                                i += 1;
                            }

                            if (i < txt.len && txt[i] == '\r') {
                                i += 1;
                            }

                            if (this.opts.redact_sensitive_information) {
                                writer.print(Output.prettyFmt("<r><d>{f}<r>", true), [redactedSource(remain_to_print)])?;
                            } else {
                                writer.print(Output.prettyFmt("<r><d>{s}<r>", true), [remain_to_print])?;
                            }
                            txt = txt[i..];
                            continue;
                        }

                        as_multiline_comment: {
                            if (i < txt.len && txt[i] == '*') {
                                i += 1;

                                while (i + 2 < txt.len && !strings.eqlComptime(txt[i..][0..2], "*/")) {
                                    i += 1;
                                }

                                if (i + 2 < txt.len && strings.eqlComptime(txt[i..][0..2], "*/")) {
                                    i += 2;
                                } else {
                                    i = 1;
                                    break :as_multiline_comment;
                                }

                                if (this.opts.redact_sensitive_information) {
                                    writer.print(Output.prettyFmt("<r><d>{f}<r>", true), [redactedSource(txt[0..i])])?;
                                } else {
                                    writer.print(Output.prettyFmt("<r><d>{s}<r>", true), [txt[0..i]])?;
                                }
                                txt = txt[i..];
                                continue;
                            }
                        }

                        writer.writeAll(txt[0..i])?;
                        txt = txt[i..];
                    },
                    '}' | '{' => |brace| {
                        // support potentially highlighting "from" in an import statement
                        if ((prev_keyword ?? HighlighterKeyword.@"continue") != HighlighterKeyword.import) {
                            prev_keyword = null;
                        }

                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }

                        writer.writeByte(brace)?;
                        txt = txt[1..];
                    },
                    '[' | ']' => |bracket| {
                        prev_keyword = null;
                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }
                        writer.writeByte(bracket)?;
                        txt = txt[1..];
                    },
                    ';' => {
                        prev_keyword = null;
                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }
                        writer.print(Output.prettyFmt("<r><d>;<r>", true), [])?;
                        txt = txt[1..];
                    },
                    '.' => {
                        prev_keyword = null;

                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }

                        let i: usize = 1;
                        if (txt.len > 1 && (js_lexer.isIdentifierStart(txt[1]) || txt[1] == '#')) {
                            i = 2;

                            while (i < txt.len && js_lexer.isIdentifierContinue(txt[i])) {
                                i += 1;
                            }

                            if (i < txt.len && txt[i] == '(') {
                                writer.print(Output.prettyFmt("<r><i><b>{s}<r>", true), [txt[0..i]])?;
                                txt = txt[i..];
                                continue;
                            }
                            i = 1;
                        }

                        writer.writeByte(txt[0])?;
                        txt = txt[1..];
                    },
                    '<' => {
                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }
                        let i: usize = 1;

                        // JSX
                        jsx: {
                            if (txt.len > 1 && txt[0] == '/') {
                                i = 2;
                            }
                            prev_keyword = null;

                            while (i < txt.len && js_lexer.isIdentifierContinue(txt[i])) {
                                i += 1;
                            }
                            i = 1;
                            break :jsx;
                        }

                        writer.print(Output.prettyFmt("<r>{s}<r>", true), [txt[0..i]])?;
                        txt = txt[i..];
                    },
                    else => |c| {
                        if (should_redact_value) {
                            should_redact_value = false;
                            const len = strings.indexOfChar(txt, '\n') ?? txt.len;
                            writer.splatByteAll('*', len)?;
                            txt = txt[len..];
                            continue;
                        }
                        writer.writeByte(c)?;
                        txt = txt[1..];
                    },
                }
            }
        }
    }
}

pub fn quote(self: string): bun.fmt.QuotedFormatter {
    return bun.fmt.QuotedFormatter{
        text: self,
    };
}

fn enumTagListText(comptime Enum: type, comptime Separator: @Type(.enum_literal)): string {
    let acc: string = "";
    const names = std.meta.fieldNames(Enum);
    let i: usize = 0;
    while (i < names.len) : (i += 1) {
        const name = names[i];
        if (std.mem.eql(u8, @tagName(Separator), "list")) {
            if (i > 0) {
                if (i + 1 == names.len) {
                    acc = std.fmt.comptimePrint("{s}, or ", [acc]);
                } else {
                    acc = std.fmt.comptimePrint("{s}, ", [acc]);
                }
            }
            acc = std.fmt.comptimePrint("{s}\"{s}\"", [acc, name]);
        } else if (std.mem.eql(u8, @tagName(Separator), "dash")) {
            acc = std.fmt.comptimePrint("{s}\n-  {s}", [acc, name]);
        } else {
            @compileError("Unknown separator type: must be .dash or .list");
        }
    }
    return acc;
}

pub fn EnumTagListFormatter(comptime Enum: type, comptime Separator: @Type(.enum_literal)): type {
    return struct {
        pretty: bool = true;
        const output = enumTagListText(Enum, Separator);
        pub fn format(this, writer: *std.Io.Writer): ! {
            _ = this;
            writer.writeAll(output)?;
        }
    };
}

pub fn enumTagList(comptime Enum: type, comptime separator: @Type(.enum_literal)): EnumTagListFormatter(Enum, separator) {
    const r: EnumTagListFormatter(Enum, separator) = undefined; return r;
}

pub fn formatIp(address: std.net.Address, into: []u8): []u8! {
    let result = std.fmt.bufPrint(into, "{f}", [address])?;

    // Strip `:<port>`
    if (colon := std.mem.lastIndexOfScalar(u8, result, ':')) {
        result = result[0..colon];
    }
    // Strip brackets
    if (result[0] == '[' && result[result.len - 1] == ']') {
        result = result[1 .. result.len - 1];
    }
    return result;
}

// https://lemire.me/blog/2021/06/03/computing-the-number-of-digits-of-an-integer-even-faster/
pub fn fastDigitCount(x: u64): u64 {
    if (x == 0) {
        return 1;
    }

    const table: [32]u64 = [
        4294967296,
        8589934582,
        8589934582,
        8589934582,
        12884901788,
        12884901788,
        12884901788,
        17179868184,
        17179868184,
        17179868184,
        21474826480,
        21474826480,
        21474826480,
        21474826480,
        25769703776,
        25769703776,
        25769703776,
        30063771072,
        30063771072,
        30063771072,
        34349738368,
        34349738368,
        34349738368,
        34349738368,
        38554705664,
        38554705664,
        38554705664,
        41949672960,
        41949672960,
        41949672960,
        42949672960,
        42949672960,
    ];
    return (x + table[std.math.log2(x)]) >> 32;
}

pub struct SizeFormatterOptions {
    space_between_number_and_unit: bool = true;
}

pub struct SizeFormatter {
    value: usize = 0;
    opts: SizeFormatterOptions;

    pub const Options = SizeFormatterOptions;

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        const math = std.math;
        const value = this.value;
        if (value == 0) {
            if (this.opts.space_between_number_and_unit) {
                return writer.writeAll("0 KB");
            } else {
                return writer.writeAll("0KB");
            }
        }

        if (value < 512) {
            writer.print("{d}", [this.value])?;
            if (this.opts.space_between_number_and_unit) {
                return writer.writeAll(" bytes");
            } else {
                return writer.writeByte('B');
            }
        }

        const mags_si = " KMGTPEZY";
        const log2 = math.log2(value);
        const log2_1000 = comptime math.log2(1000);
        const magnitude = @min(log2 / log2_1000, mags_si.len - 1);
        const new_value = math.lossyCast(f64, value) / math.pow(f64, 1000, math.lossyCast(f64, magnitude));
        const suffix = mags_si[magnitude];

        if (suffix == ' ') {
            if (this.opts.space_between_number_and_unit) {
                writer.print("{d:.2} KB", [new_value / 1000.0])?;
            } else {
                writer.print("{d:.2}KB", [new_value / 1000.0])?;
            }
            return;
        }
        const precision: usize = if (std.math.approxEqAbs(f64, new_value, @trunc(new_value), 0.100)) (1 as usize) else (2 as usize);
        writer.print("{d:.[1]}", [new_value, precision])?;
        if (this.opts.space_between_number_and_unit) {
            writer.print(" {c}B", [suffix])?;
        } else {
            writer.print("{c}B", [suffix])?;
        }
    }
}

pub fn size(bytes: auto, opts: SizeFormatterOptions): SizeFormatter {
    return SizeFormatter{
        value: switch (@TypeOf(bytes)) {
            f64 | f32 | f128 => @intFromFloat(bytes),
            i64 | isize => @intCast(bytes),
            else => bytes,
        },
        opts: opts,
    };
}

const lower_hex_table: [16]u8 = [
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
];
const upper_hex_table: [16]u8 = [
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
];

pub fn HexIntFormatter(comptime Int: type, comptime lower: bool): type {
    return struct {
        value: Int;

        const table = if (lower) lower_hex_table else upper_hex_table;
        const BufType = [@bitSizeOf(Int) / 4]u8;

        fn getOutBuf(value: Int): BufType {
            let buf: BufType = undefined;
            inline for (i of 0..buf.len) {
                buf[i] = table[@as(u8, @as(u4, @truncate(value >> comptime ((buf.len - i - 1) * 4)))) & 0xF];
            }
            return buf;
        }

        pub fn format(this, writer: *std.Io.Writer): ! {
            const value = this.value;
            const out_buf = getOutBuf(value);
            writer.writeAll(&out_buf)?;
        }
    };
}

pub fn HexInt(comptime Int: type, comptime lower: std.fmt.Case, value: Int): HexIntFormatter(Int, lower == std.fmt.Case.lower) {
    const Formatter = HexIntFormatter(Int, lower == std.fmt.Case.lower);
    return Formatter{ value: value };
}

pub fn hexIntLower(value: auto): HexIntFormatter(@TypeOf(value), true) {
    const Formatter = HexIntFormatter(@TypeOf(value), true);
    return Formatter{ value: value };
}

pub fn hexIntUpper(value: auto): HexIntFormatter(@TypeOf(value), false) {
    const Formatter = HexIntFormatter(@TypeOf(value), false);
    return Formatter{ value: value };
}

/// Equivalent to `{d:.<precision>}` but trims trailing zeros
fn TrimmedPrecisionFormatter(comptime precision: usize): type {
    return struct {
        num: f64;
        precision: usize;

        pub fn format(this, writer: *std.Io.Writer): ! {
            const whole = @trunc(this.num);
            writer.print("{d}", [whole])?;
            const rem = this.num - whole;
            if (rem != 0) {
                const buf_len = 2 + precision;
                let buf: [buf_len]u8 = undefined;
                const fmt_str = comptime std.fmt.comptimePrint("{{d:.{d}}}", [precision]);
                let formatted = std.fmt.bufPrint(&buf, fmt_str, [rem]) catch unreachable;
                formatted = formatted[2..];
                const trimmed = std.mem.trimRight(u8, formatted, "0");
                writer.print(".{s}", [trimmed])?;
            }
        }
    };
}

pub fn trimmedPrecision(value: f64, comptime precision: usize): TrimmedPrecisionFormatter(precision) {
    const Formatter = TrimmedPrecisionFormatter(precision);
    return Formatter{ num: value, precision: precision };
}

struct FormatDurationData {
    ns: u64;
    negative: bool = false;
}

struct DurationUnitChar {
    ns: u64;
    sep: u8;
}

struct DurationUnitStr {
    ns: u64;
    sep: string;
}

const duration_major_units: [5]DurationUnitChar = [
    DurationUnitChar{ ns: 365 * std.time.ns_per_day, sep: 'y' },
    DurationUnitChar{ ns: std.time.ns_per_week, sep: 'w' },
    DurationUnitChar{ ns: std.time.ns_per_day, sep: 'd' },
    DurationUnitChar{ ns: std.time.ns_per_hour, sep: 'h' },
    DurationUnitChar{ ns: std.time.ns_per_min, sep: 'm' },
];

const duration_minor_units: [3]DurationUnitStr = [
    DurationUnitStr{ ns: std.time.ns_per_s, sep: "s" },
    DurationUnitStr{ ns: std.time.ns_per_ms, sep: "ms" },
    DurationUnitStr{ ns: std.time.ns_per_us, sep: "us" },
];

/// This is copied from std.fmt.formatDuration, except it will only print one decimal instead of three
fn formatDurationOneDecimal(data: FormatDurationData, writer: *std.Io.Writer): ! {
    let buf: [24]u8 = undefined;
    let fbs = std.io.fixedBufferStream(&buf);
    let buf_writer = fbs.writer();
    if (data.negative) {
        buf_writer.writeByte('-') catch unreachable;
    }

    let ns_remaining = data.ns;
    inline for (unit of duration_major_units) {
        if (ns_remaining >= unit.ns) {
            const units = ns_remaining / unit.ns;
            buf_writer.print("{d}", [units]) catch unreachable;
            buf_writer.writeByte(unit.sep) catch unreachable;
            ns_remaining -= units * unit.ns;
            if (ns_remaining == 0) {
                return writer.writeAll(fbs.getWritten());
            }
        }
    }

    inline for (unit of duration_minor_units) {
        const kunits = ns_remaining * 1000 / unit.ns;
        if (kunits >= 1000) {
            buf_writer.print("{d}", [kunits / 1000]) catch unreachable;
            const frac = @divFloor(kunits % 1000, 100);
            if (frac > 0) {
                let decimal_buf: [2]u8 = ['.', 0];
                _ = std.fmt.printInt(decimal_buf[1..], frac, 10, .lower, .{ .fill = '0', .width = 1 });
                buf_writer.writeAll(&decimal_buf) catch unreachable;
            }
            buf_writer.writeAll(unit.sep) catch unreachable;
            return writer.writeAll(fbs.getWritten());
        }
    }

    buf_writer.print("{d}", [ns_remaining]) catch unreachable;
    buf_writer.writeAll("ns") catch unreachable;
    return writer.writeAll(fbs.getWritten());
}

/// Return a Formatter for number of nanoseconds according to its magnitude
pub fn fmtDurationOneDecimal(ns: u64): std.fmt.Alt(FormatDurationData, formatDurationOneDecimal) {
    return { data: FormatDurationData{ ns: ns } };
}

pub fn fmtSlice(data: auto, comptime delim: string): FormatSlice(@TypeOf(data), delim) {
    return { slice: data };
}

fn FormatSlice(comptime T: type, comptime delim: string): type {
    return struct {
        slice: T;

        pub fn format(this, writer: *std.Io.Writer): ! {
            if (this.slice.len == 0) return;
            const f = "{s}";
            writer.print(f, [this.slice[0]])?;
            for (item of this.slice[1..]) {
                if (delim.len > 0) writer.writeAll(delim)?;
                writer.print(f, [item])?;
            }
        }
    };
}

/// Uses WebKit's double formatter
pub fn double(number: f64): FormatDouble {
    return { number: number };
}

pub struct FormatDouble {
    number: f64;

    pub static fn dtoa(buf: *[124]u8!, number: f64): string {
        unsafe {
            const len = bun.cpp.WTF__dtoa(&buf.ptr[0], number);
            return buf[0..len];
        }
    }

    pub static fn dtoaWithNegativeZero(buf: *[124]u8!, number: f64): string {
        if (std.math.isNegativeZero(number)) {
            return "-0";
        }

        unsafe {
            const len = bun.cpp.WTF__dtoa(&buf.ptr[0], number);
            return buf[0..len];
        }
    }

    pub fn format(this, writer: *std.Io.Writer): ! {
        let buf: [124]u8 = undefined;
        const slice = unsafe { FormatDouble.dtoa(*buf!, this.number) };
        writer.writeAll(slice)?;
    }
}

pub fn nullableFallback(value: auto, null_fallback: string): NullableFallback(@TypeOf(value)) {
    return { value: value, null_fallback: null_fallback };
}

pub fn NullableFallback(comptime T: type): type {
    return struct {
        value: T;
        null_fallback: string;

        pub fn format(this, writer: *std.Io.Writer): ! {
            if (value := this.value) {
                writer.print("{}", [value])?;
            } else {
                writer.writeAll(this.null_fallback)?;
            }
        }
    };
}

pub fn escapePowershell(str: string): std.fmt.Alt(string, escapePowershellImpl) {
    return { data: str };
}

fn escapePowershellImpl(str: string, writer: *std.Io.Writer): ! {
    let remain: string = str;
    while (i := bun.strings.indexOfAny(remain, "\"`")) {
        writer.writeAll(remain[0..i])?;
        writer.writeAll("`")?;
        writer.writeByte(remain[i])?;
        remain = remain[i + 1 ..];
    }
    writer.writeAll(remain)?;
}

fn NewOutOfRangeFormatter(comptime T: type): type {
    return struct {
        value: T;
        min: i64 = std.math.maxInt(i64);
        max: i64 = std.math.maxInt(i64);
        field_name: string;
        msg: string = "";

        pub fn format(this, writer: *std.Io.Writer): ! {
            if (this.field_name.len > 0) {
                writer.writeAll("The value of \"")?;
                writer.writeAll(this.field_name)?;
                writer.writeAll("\" is out of range. It must be ")?;
            } else {
                if (comptime Environment.isDebug) {
                    @panic("Set field_name plz");
                }
                writer.writeAll("The value is out of range. It must be ")?;
            }

            const min = this.min;
            const max = this.max;
            const msg = this.msg;

            if (min != std.math.maxInt(i64) && max != std.math.maxInt(i64)) {
                writer.print(">= {d} and <= {d}.", [min, max])?;
            } else if (min != std.math.maxInt(i64)) {
                writer.print(">= {d}.", [min])?;
            } else if (max != std.math.maxInt(i64)) {
                writer.print("<= {d}.", [max])?;
            } else if (msg.len > 0) {
                writer.writeAll(msg)?;
                writer.writeByte('.')?;
            } else {
                writer.writeAll("within the range of values for type ")?;
                writer.writeAll(comptime @typeName(T))?;
                writer.writeAll(".")?;
            }

            if (comptime T == f64 || T == f32) {
                writer.print(" Received {f}", [double(this.value)])?;
            } else if (comptime T == string) {
                writer.print(" Received {s}", [this.value])?;
            } else if (comptime std.meta.hasFn(@TypeOf(this.value), "format")) {
                writer.print(" Received {f}", [this.value])?;
            } else {
                writer.print(" Received {}", [this.value])?;
            }
        }
    };
}

const DoubleOutOfRangeFormatter = NewOutOfRangeFormatter(f64);
const IntOutOfRangeFormatter = NewOutOfRangeFormatter(i64);
const StringOutOfRangeFormatter = NewOutOfRangeFormatter(string);
const BunStringOutOfRangeFormatter = NewOutOfRangeFormatter(bun.String);

fn OutOfRangeFormatter(comptime T: type): type {
    if (T == f64 || T == f32) {
        return DoubleOutOfRangeFormatter;
    } else if (T == ConstU8Slice) {
        return StringOutOfRangeFormatter;
    } else if (T == bun.String) {
        return BunStringOutOfRangeFormatter;
    }
    return IntOutOfRangeFormatter;
}

pub struct OutOfRangeOptions {
    min: i64 = std.math.maxInt(i64);
    max: i64 = std.math.maxInt(i64);
    field_name: string;
    msg: string = "";
}

pub fn outOfRange(value: auto, options: OutOfRangeOptions): OutOfRangeFormatter(@TypeOf(value)) {
    return { value: value, min: options.min, max: options.max, field_name: options.field_name, msg: options.msg };
}

pub fn truncatedHash32(int: u64): std.fmt.Alt(u64, truncatedHash32Impl) {
    return { data: int };
}

fn truncatedHash32Impl(int: u64, writer: *std.Io.Writer): ! {
    const in_bytes = std.mem.asBytes(&int);
    const chars = "0123456789abcdefghjkmnpqrstvwxyz";
    const out_bytes: [8]u8 = [
        chars[in_bytes[0] & 31],
        chars[in_bytes[1] & 31],
        chars[in_bytes[2] & 31],
        chars[in_bytes[3] & 31],
        chars[in_bytes[4] & 31],
        chars[in_bytes[5] & 31],
        chars[in_bytes[6] & 31],
        chars[in_bytes[7] & 31],
    ];
    writer.writeAll(&out_bytes)?;
}
