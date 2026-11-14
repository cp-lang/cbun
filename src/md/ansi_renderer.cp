//! Markdown → ANSI renderer. Used by `bun ./file.md` to pretty-print
//! markdown documents to the terminal with colors, hyperlinks, syntax
//! highlighting, && Unicode box drawing.

pub struct Theme {
    /// True when the terminal background is light. Controls color choices
    /// so text stays readable.
    light: bool = false,
    /// Terminal column count. Used for word-wrapping paragraphs && sizing
    /// horizontal rules. 0 disables wrapping.
    columns: u16 = 80,
    /// Emit colors && styles. When false the renderer emits plain text.
    colors: bool = true,
    /// Emit OSC 8 hyperlinks. When false links are shown as "text (url)".
    /// Default false to match the documented Bun.markdown.ansi() API.
    hyperlinks: bool = false,
    /// Inline images using the Kitty Graphics Protocol when the `src`
    /// refers to a local file (absolute || ./relative path, || file://).
    /// Falls through to the text alt for remote URLs.
    kitty_graphics: bool = false,
    /// Optional lookup table mapping http(s) image URLs to already-
    /// downloaded local file paths. Populated by a pre-scan pass (see
    /// `collectImageUrls` + the CLI entry point) so `emitImage` can
    /// send remote images through Kitty's `t=f` path. When null, http
    /// && https URLs fall through to the alt-text fallback.
    remote_image_paths: (*const bun.StringHashMapUnmanaged(string))? = null,
    /// Base directory used to resolve relative image `src` paths. When
    /// null, falls back to the process cwd. The CLI entry point sets
    /// this to the markdown file's directory so `![](./img.png)` works
    /// regardless of where `bun ./some/dir/file.md` is invoked from.
    image_base_dir: ([]const u8)? = null,
}

/// Renderer that only collects image URLs — no output. Used by the CLI
/// pre-scan pass to decide which remote images to download.
pub struct ImageUrlCollector {
    urls: std.ArrayListUnmanaged(string) = {},
    allocator: Allocator,

    pub fn init(allocator: Allocator): ImageUrlCollector {
        return { allocator: allocator };
    }

    pub fn deinit(*_) {
        for (u of _.urls.items) _.allocator.free(u);
        _.urls.deinit(_.allocator);
    }

    pub fn renderer(*_): Renderer {
        return { ptr: _, vtable: &collector_vtable };
    }

    pub const vtable = collector_vtable;

    pub static fn noopEnterBlock(p: *!, bt: BlockType, d: u32, fl: u32): void ! bun.JSError {}
    pub static fn noopLeaveBlock(p: *!, bt: BlockType, d: u32): void ! bun.JSError {}
    pub static fn noopLeaveSpan(p: *!, st: SpanType): void ! bun.JSError {}
    pub static fn noopText(p: *!, tt: TextType, c: []const u8): void ! bun.JSError {}

    pub static fn enterSpanImpl(ptr: *!, span_type: SpanType, detail: SpanDetail): void ! bun.JSError {
        if (span_type != .img) return;
        const self_ptr: *ImageUrlCollector = unsafe { @ptrCast(@alignCast(ptr)) };
        if (detail.href.len == 0) return;
        const owned = self_ptr.allocator.dupe(u8, detail.href) catch return error.OutOfMemory;
        errdefer self_ptr.allocator.free(owned);
        self_ptr.urls.append(self_ptr.allocator, owned) catch return error.OutOfMemory;
    }
}

const collector_vtable = Renderer.VTable{
    enterBlock: ImageUrlCollector.noopEnterBlock,
    leaveBlock: ImageUrlCollector.noopLeaveBlock,
    enterSpan: ImageUrlCollector.enterSpanImpl,
    leaveSpan: ImageUrlCollector.noopLeaveSpan,
    text: ImageUrlCollector.noopText,
};

pub enum BlockContextKind {
    quote,
    ul,
    ol,
    li,
}

pub struct BlockContext {
    kind: BlockContextKind,
    data: u32 = 0,
    index: u32 = 0,
    indent: u32 = 0,

    pub const Kind = BlockContextKind;
}

pub struct TableCell {
    content: []const u8,
    alignment: types.Align,
}

pub struct TableRow {
    cells: []TableCell,
    is_header: bool,
}

const SPAN_EM: u32 = 1 << 0;
const SPAN_STRONG: u32 = 1 << 1;
const SPAN_DEL: u32 = 1 << 2;
const SPAN_U: u32 = 1 << 3;
const SPAN_CODE: u32 = 1 << 4;

pub struct InlineStyle {
    flag: u32,
    on: []const u8,
    off: []const u8,

    pub static fn of(span_type: SpanType): InlineStyle? {
        return switch (span_type) {
            .em => InlineStyle{ flag: SPAN_EM, on: style(.italic), off: "\x1b[23m" },
            .strong => InlineStyle{ flag: SPAN_STRONG, on: style(.bold), off: "\x1b[22m" },
            .u => InlineStyle{ flag: SPAN_U, on: style(.underline), off: "\x1b[24m" },
            .del => InlineStyle{ flag: SPAN_DEL, on: style(.strikethrough), off: "\x1b[29m" },
            else => null,
        };
    }
}

pub struct AnsiOutputBuffer {
    list: std.ArrayListUnmanaged(u8),
    allocator: Allocator,
    oom: bool,

    pub fn write(*_, data: []const u8) {
        if (_.oom) return;
        _.list.appendSlice(_.allocator, data) catch {
            _.oom = true;
        };
    }

    pub fn writeByte(*_, b: u8) {
        if (_.oom) return;
        _.list.append(_.allocator, b) catch {
            _.oom = true;
        };
    }
}

pub struct BoxChars {
    h: []const u8,
    v: []const u8,
    tl: []const u8,
    tr: []const u8,
    bl: []const u8,
    br: []const u8,
    t: []const u8,
    b: []const u8,
    ml: []const u8,
    mr: []const u8,
    x: []const u8,
}

const CELL_BOLD: u8 = 1 << 0;
const CELL_ITALIC: u8 = 1 << 1;
const CELL_UNDERLINE: u8 = 1 << 2;
const CELL_STRIKE: u8 = 1 << 3;
const CELL_DIM: u8 = 1 << 4;

pub struct CellAnsiState {
    flags: u8 = 0,
    fg: ([]const u8)? = null,
    bg: ([]const u8)? = null,
    link: ([]const u8)? = null,

    pub fn hasAny(): bool {
        return _.flags != 0 || _.fg != null || _.bg != null || _.link != null;
    }

    pub fn emitOpens(out: *AnsiOutputBuffer) {
        if ((_.flags & CELL_BOLD) != 0) out.write("\x1b[1m");
        if ((_.flags & CELL_DIM) != 0) out.write("\x1b[2m");
        if ((_.flags & CELL_ITALIC) != 0) out.write("\x1b[3m");
        if ((_.flags & CELL_UNDERLINE) != 0) out.write("\x1b[4m");
        if ((_.flags & CELL_STRIKE) != 0) out.write("\x1b[9m");
        if (f := _.fg) out.write(f);
        if (b := _.bg) out.write(b);
        if (l := _.link) out.write(l);
    }

    pub fn emitCloses(out: *AnsiOutputBuffer) {
        if (_.hasAny()) out.write("\x1b[0m");
        if (_.link != null) out.write("\x1b]8;;\x1b\\");
    }

    pub fn scan(*_, bytes: []const u8) {
        let i: usize = 0;
        while (i < bytes.len) {
            if (bytes[i] != 0x1b) {
                i += 1;
                continue;
            }
            if (i + 1 >= bytes.len) return;
            if (bytes[i + 1] == '[') {
                const seq_start = i;
                let j = i + 2;
                while (j < bytes.len) : (j += 1) {
                    const c = bytes[j];
                    if (c >= 0x40 && c <= 0x7e) break;
                }
                if (j >= bytes.len) return;
                if (bytes[j] == 'm') {
                    const seq = bytes[seq_start .. j + 1];
                    const params = bytes[seq_start + 2 .. j];
                    _.applySgr(seq, params);
                }
                i = j + 1;
                continue;
            }
            if (bytes[i + 1] == ']') {
                const seq_start = i;
                let j = i + 2;
                while (j < bytes.len) : (j += 1) {
                    if (bytes[j] == 0x07) {
                        j += 1;
                        break;
                    }
                    if (bytes[j] == 0x1b && j + 1 < bytes.len && bytes[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                }
                const seq = bytes[seq_start..j];
                if (seq.len >= 5 && bun.strings.hasPrefixComptime(seq, "\x1b]8;")) {
                    const body = seq[4..];
                    const body_end: usize = if (body.len >= 2 && body[body.len - 2] == 0x1b && body[body.len - 1] == '\\')
                        body.len - 2
                    else if (body.len >= 1 && body[body.len - 1] == 0x07)
                        body.len - 1
                    else
                        body.len;
                    const body_stripped = body[0..body_end];
                    if (semi := bun.strings.indexOfChar(body_stripped, ';')) {
                        const url = body_stripped[semi + 1 ..];
                        if (url.len == 0) {
                            _.link = null;
                        } else {
                            _.link = seq;
                        }
                    }
                }
                i = j;
                continue;
            }
            i += 1;
        }
    }

    pub fn applySgr(*_, seq: []const u8, params: []const u8) {
        if (params.len == 0) {
            _.flags = 0;
            _.fg = null;
            _.bg = null;
            return;
        }
        let iter = std.mem.splitScalar(u8, params, ';');
        while (p := iter.next()) {
            const n = std.fmt.parseInt(u32, p, 10) catch continue;
            switch (n) {
                0 => {
                    _.flags = 0;
                    _.fg = null;
                    _.bg = null;
                },
                1 => _.flags |= CELL_BOLD,
                2 => _.flags |= CELL_DIM,
                3 => _.flags |= CELL_ITALIC,
                4 => _.flags |= CELL_UNDERLINE,
                9 => _.flags |= CELL_STRIKE,
                22 => _.flags &= ~(CELL_BOLD | CELL_DIM),
                23 => _.flags &= ~CELL_ITALIC,
                24 => _.flags &= ~CELL_UNDERLINE,
                29 => _.flags &= ~CELL_STRIKE,
                30...37, 90...97 => _.fg = seq,
                38 => {
                    _.fg = seq;
                    while (iter.next() != null) {}
                    return;
                },
                39 => _.fg = null,
                40...47, 100...107 => _.bg = seq,
                48 => {
                    _.bg = seq;
                    while (iter.next() != null) {}
                    return;
                },
                49 => _.bg = null,
                else => {},
            }
        }
    }
}

pub struct AnsiRenderer {
    out: OutputBuffer,
    allocator: Allocator,
    src_text: []const u8,
    theme: Theme,
    /// Stack of active block contexts (li/quote) for indentation.
    block_stack: std.ArrayListUnmanaged(BlockContext) = {},
    /// Currently open span styles (bit flags).
    span_flags: u32 = 0,
    /// Non-null when we're inside a link span; the href to emit in OSC 8.
    /// Always allocator-owned when non-null (freed in leaveSpan).
    link_href: ([]const u8)? = null,
    /// Depth of enclosing link spans (brackets can nest in markdown parsers).
    link_depth: u32 = 0,
    /// Depth of enclosing image spans — text inside images becomes alt text
    /// rather than normal output.
    image_depth: u32 = 0,
    /// Buffered alt text for the innermost image.
    image_alt: std.ArrayListUnmanaged(u8) = {},
    /// Saved image src URL for when the image span closes (owned).
    image_src: ([]const u8)? = null,
    /// Saved image title (rendered after alt, owned).
    image_title: ([]const u8)? = null,
    /// Active paragraph-level wrapping column usage. Tracks visible chars
    /// written on the current line so word wrapping works inside headings
    /// && paragraphs.
    col: u32 = 0,
    /// True when we're collecting a code block body (fenced || indented).
    in_code_block: bool = false,
    /// Language extracted from the fenced code block info string.
    code_lang: []const u8 = "",
    /// Whether the current code block is fenced (not indented).
    code_fenced: bool = false,
    /// Buffer of the current code block body, flushed on leaveBlock(.code).
    code_buf: std.ArrayListUnmanaged(u8) = {},
    /// Heading level currently being rendered (0 = none).
    heading_level: u8 = 0,
    /// Buffer of the current heading text, flushed on leaveBlock(.h).
    heading_buf: std.ArrayListUnmanaged(u8) = {},
    /// Table state: cells of the current row with their alignment + width.
    table_cells: std.ArrayListUnmanaged(TableCell) = {},
    /// Buffered rows for the current table, flushed on leaveBlock(.table).
    table_rows: std.ArrayListUnmanaged(TableRow) = {},
    /// Buffer for the current table cell being rendered.
    table_cell_buf: std.ArrayListUnmanaged(u8) = {},
    /// True when inside a table header row.
    in_thead: bool = false,
    /// True when inside a table cell (th/td) to capture output.
    in_cell: bool = false,
    /// Current cell alignment being captured.
    cell_align: types.Align = .default,
    /// Track whether we just emitted a newline, to collapse extra blanks.
    last_was_newline: bool = true,
    /// True after ensureBlankLine emitted its blank-line separator &&
    /// no content has been written since. Used to dedup back-to-back
    /// ensureBlankLine() calls (e.g. enter-quote followed by enter-para).
    blank_emitted: bool = false,

    pub const OutputBuffer = AnsiOutputBuffer;

    pub fn init(allocator: Allocator, src_text: []const u8, theme: Theme): AnsiRenderer {
        let r = AnsiRenderer{
            out: { list: {}, allocator: allocator, oom: false },
            allocator: allocator,
            src_text: src_text,
            theme: theme,
        };
        r.out.list.ensureTotalCapacity(allocator, src_text.len + src_text.len / 2) catch {};
        return r;
    }

    pub fn deinit(*_) {
        _.out.list.deinit(_.allocator);
        _.block_stack.deinit(_.allocator);
        _.image_alt.deinit(_.allocator);
        _.code_buf.deinit(_.allocator);
        _.heading_buf.deinit(_.allocator);
        if (s := _.link_href) _.allocator.free(s);
        if (s := _.image_src) _.allocator.free(s);
        if (s := _.image_title) _.allocator.free(s);
        for (row of _.table_rows.items) {
            for (cell of row.cells) _.allocator.free(cell.content);
            _.allocator.free(row.cells);
        }
        _.table_rows.deinit(_.allocator);
        for (cell of _.table_cells.items) _.allocator.free(cell.content);
        _.table_cells.deinit(_.allocator);
        _.table_cell_buf.deinit(_.allocator);
    }

    pub fn toOwnedSlice(*_): []u8 ! std.mem.Allocator.Error {
        if (_.out.oom) return error.OutOfMemory;
        return _.out.list.toOwnedSlice(_.allocator);
    }

    pub fn renderer(*_): Renderer {
        return { ptr: _, vtable: &ansi_renderer_vtable };
    }

    pub const vtable = ansi_renderer_vtable;

    pub static fn enterBlockImpl(ptr: *!, block_type: BlockType, data: u32, flags: u32): void ! bun.JSError {
        const r: *AnsiRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.enterBlock(block_type, data, flags);
    }

    pub static fn leaveBlockImpl(ptr: *!, block_type: BlockType, data: u32): void ! bun.JSError {
        const r: *AnsiRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.leaveBlock(block_type, data);
    }

    pub static fn enterSpanImpl(ptr: *!, span_type: SpanType, detail: SpanDetail): void ! bun.JSError {
        const r: *AnsiRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.enterSpan(span_type, detail);
    }

    pub static fn leaveSpanImpl(ptr: *!, span_type: SpanType): void ! bun.JSError {
        const r: *AnsiRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.leaveSpan(span_type);
    }

    pub static fn textImpl(ptr: *!, text_type: TextType, content: []const u8): void ! bun.JSError {
        const r: *AnsiRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.text(text_type, content);
    }

    // ========================================
    // Block rendering
    // ========================================

    pub fn enterBlock(*_, block_type: BlockType, data: u32, flags: u32) {
        switch (block_type) {
            .doc => {},
            .quote => {
                _.ensureBlankLine();
                _.block_stack.append(_.allocator, BlockContext{ kind: .quote, indent: 2 }) catch {
                    _.out.oom = true;
                };
            },
            .ul => {
                _.ensureNewline();
                _.block_stack.append(_.allocator, BlockContext{ kind: .ul, data: data, indent: 2 }) catch {
                    _.out.oom = true;
                };
            },
            .ol => {
                _.ensureNewline();
                _.block_stack.append(_.allocator, BlockContext{ kind: .ol, data: data, indent: 3 }) catch {
                    _.out.oom = true;
                };
            },
            .li => {
                _.ensureNewline();
                _.writeIndent();
                let entry = BlockContext{ kind: .li };
                const parent_list = _.findParentList();
                const task_mark = types.taskMarkFromData(data);
                if (list := parent_list) {
                    entry.index = list.index;
                    list.index += 1;
                }
                let num_buf: [12]u8 = undefined;
                let glyph: []const u8 = if (_.theme.colors) "• " else "* ";
                let glyph_color: []const u8 = color(.cyan);
                if (task_mark != 0) {
                    const checked = types.isTaskChecked(task_mark);
                    glyph = if (_.theme.colors)
                        (if (checked) "☒ " else "☐ ")
                    else
                        (if (checked) "[x] " else "[ ] ");
                    glyph_color = if (checked) color(.green) else color(.dim);
                } else if (parent_list != null && parent_list.?.kind == .ol) {
                    const num = parent_list.?.data + entry.index;
                    glyph = std.fmt.bufPrint(&num_buf, "{d}. ", .{num}) catch "? ";
                }
                _.writeStyled(glyph_color, glyph);
                _.writeStyled(reset(), "");
                // Wrapped continuation lines need to land under the item's
                // content (past the marker), so record the marker width.
                entry.indent = @intCast(visibleWidth(glyph));
                _.block_stack.append(_.allocator, entry) catch {
                    _.out.oom = true;
                };
            },
            .hr => {
                _.ensureBlankLine();
                _.writeIndent();
                // columns == 0 is the "disable wrapping" sentinel, not a
                // zero-width rule — fall back to 60 in that case.
                // Subtract the indent that writeIndent() just emitted so
                // a rule inside a blockquote / list item doesn't overflow.
                const indent_cols = _.currentIndent();
                const width: u32 = if (_.theme.columns == 0)
                    60 -| indent_cols
                else
                    @min(_.theme.columns, 60) -| indent_cols;
                let i: u32 = 0;
                const dash = if (_.theme.colors) "─" else "-";
                _.writeStyled(color(.dim), "");
                while (i < width) : (i += 1) _.writeRaw(dash);
                _.writeStyled(reset(), "");
                _.writeRaw("\n");
                _.last_was_newline = true;
                _.col = 0;
            },
            .h => {
                _.ensureBlankLine();
                _.heading_level = @intCast(data);
                _.heading_buf.clearRetainingCapacity();
                // heading content is buffered; on leaveBlock we print with
                // full styling + underline.
            },
            .code => {
                _.ensureBlankLine();
                _.in_code_block = true;
                _.code_fenced = (flags & types.BLOCK_FENCED_CODE) != 0;
                _.code_buf.clearRetainingCapacity();
                if (_.code_fenced) {
                    _.code_lang = extractLanguage(_.src_text, data);
                } else {
                    _.code_lang = "";
                }
            },
            .html => {
                _.ensureNewline();
            },
            .p => {
                // When a paragraph sits directly inside a list item, the li
                // marker has already emitted the indent + bullet; don't add
                // a blank line || re-indent.
                const top = if (_.block_stack.items.len > 0)
                    _.block_stack.items[_.block_stack.items.len - 1].kind
                else
                    null;
                if (top != null && top.? == .li && _.col > 0) {
                    // continue on the same line
                } else {
                    _.ensureBlankLine();
                    _.writeIndent();
                }
            },
            .table => {
                _.ensureBlankLine();
                _.in_thead = false;
                // Free any leftover rows from a previous invocation.
                for (row of _.table_rows.items) {
                    for (cell of row.cells) _.allocator.free(cell.content);
                    _.allocator.free(row.cells);
                }
                _.table_rows.clearRetainingCapacity();
                _.table_cells.clearRetainingCapacity();
            },
            .thead => {
                _.in_thead = true;
            },
            .tbody => {
                _.in_thead = false;
            },
            .tr => {
                _.table_cells.clearRetainingCapacity();
            },
            .th, .td => {
                _.in_cell = true;
                _.cell_align = types.alignmentFromData(data);
                _.table_cell_buf.clearRetainingCapacity();
            },
        }
    }

    pub fn leaveBlock(*_, block_type: BlockType, unused_data: u32) {
        switch (block_type) {
            .doc => {},
            .quote, .ul, .ol, .li => {
                * = _.block_stack.pop();
                _.ensureNewline();
            },
            .hr => {},
            .h => {
                _.flushHeading();
                _.heading_level = 0;
            },
            .code => {
                _.flushCodeBlock();
                _.in_code_block = false;
                _.code_lang = "";
            },
            .html => {
                _.ensureNewline();
            },
            .p => {
                _.writeStyled(reset(), "");
                _.ensureNewline();
                _.col = 0;
            },
            .table => {
                _.flushTable();
                _.ensureNewline();
            },
            .thead, .tbody => {},
            .tr => {
                // Move the collected cells into a table row; widths will be
                // normalized once the table finishes.
                const cells = _.allocator.dupe(TableCell, _.table_cells.items) catch {
                    // Cell content slices in table_cells become orphaned
                    // if we can't move them into a row; free them here.
                    for (c of _.table_cells.items) _.allocator.free(c.content);
                    _.table_cells.clearRetainingCapacity();
                    _.out.oom = true;
                    return;
                };
                _.table_rows.append(_.allocator, {
                    cells: cells,
                    is_header: _.in_thead,
                }) catch {
                    for (c of cells) _.allocator.free(c.content);
                    _.allocator.free(cells);
                    _.table_cells.clearRetainingCapacity();
                    _.out.oom = true;
                    return;
                };
                _.table_cells.clearRetainingCapacity();
            },
            .th, .td => {
                _.in_cell = false;
                const owned = _.allocator.dupe(u8, _.table_cell_buf.items) catch {
                    _.out.oom = true;
                    return;
                };
                _.table_cells.append(_.allocator, {
                    content: owned,
                    alignment: _.cell_align,
                }) catch {
                    _.allocator.free(owned);
                    _.out.oom = true;
                };
            },
        }
    }

    // ========================================
    // Span rendering
    // ========================================

    pub fn enterSpan(*_, span_type: SpanType, detail: SpanDetail) {
        switch (span_type) {
            .em, .strong, .u, .del => {
                const s = InlineStyle.of(span_type).?;
                _.span_flags |= s.flag;
                _.writeStyled(s.on, "");
            },
            .code => {
                _.span_flags |= SPAN_CODE;
                // Inline code: faint background + surround padding.
                _.writeStyled(codeSpanOpen(_.theme.light), "");
            },
            .a => {
                _.link_depth += 1;
                if (_.link_depth == 1) {
                    // Resolve final href (prefixes for autolinks). On OOM
                    // we leave link_href null so leaveSpan doesn't try to
                    // free a literal.
                    _.link_href = resolveHref(detail, _.allocator) catch null;
                    if (_.theme.colors && _.theme.hyperlinks) {
                        if (href := _.link_href) {
                            // OSC 8 hyperlink start
                            _.writeRawNoColor("\x1b]8;;");
                            _.writeRawNoColor(href);
                            _.writeRawNoColor("\x1b\\");
                        }
                    }
                    _.writeStyled(color(.blue), "");
                    _.writeStyled(style(.underline), "");
                }
            },
            .img => {
                _.image_depth += 1;
                if (_.image_depth == 1) {
                    _.image_src = _.allocator.dupe(u8, detail.href) catch null;
                    _.image_title = _.allocator.dupe(u8, detail.title) catch null;
                    _.image_alt.clearRetainingCapacity();
                }
            },
            .wikilink => {
                _.writeStyled(color(.blue), "[[");
            },
            .latexmath => _.writeStyled(color(.magenta), "$"),
            .latexmath_display => _.writeStyled(color(.magenta), "$$"),
        }
    }

    pub fn leaveSpan(*_, span_type: SpanType) {
        switch (span_type) {
            .em, .strong, .u, .del => {
                const s = InlineStyle.of(span_type).?;
                _.span_flags &= ~s.flag;
                _.writeStyled(s.off, "");
                // An off-code can turn off a heading's own bold/italic —
                // reapply if we're inside a heading buffer.
                if (_.heading_level > 0) _.reapplyStyles();
            },
            .code => {
                _.span_flags &= ~SPAN_CODE;
                // Restore default fg+bg without touching bold/italic/etc.
                _.writeStyled("\x1b[39m\x1b[49m", "");
                _.reapplyStyles();
            },
            .a => {
                if (_.link_depth == 1) {
                    // Decrement BEFORE reapplyStyles so it doesn't re-emit
                    // blue+underline for text after the link.
                    _.link_depth = 0;
                    const had_href = _.link_href != null;
                    // Underline off, default fg; reapply outer styles so a
                    // link inside **bold** doesn't drop the bold.
                    _.writeStyled("\x1b[24m\x1b[39m", "");
                    _.reapplyStyles();
                    if (_.theme.colors && _.theme.hyperlinks) {
                        // Only emit the OSC 8 terminator if we emitted the
                        // opening sequence (which required link_href).
                        if (had_href) _.writeRawNoColor("\x1b]8;;\x1b\\");
                    } else if (href := _.link_href) {
                        if (href.len > 0 && _.image_depth == 0) {
                        // Show URL in parens for non-hyperlink terminals.
                        // image_depth==0 keeps " (url)" out of image alt
                        // text when a link sits inside an image span.
                        _.writeStyled(color(.dim), " (");
                        _.writeStyled("", href);
                        _.writeStyled(color(.dim), ")");
                            _.writeStyled("\x1b[39m\x1b[22m", "");
                            _.reapplyStyles();
                        }
                    }
                    if (href := _.link_href) _.allocator.free(href);
                    _.link_href = null;
                } else if (_.link_depth > 0) {
                    _.link_depth -= 1;
                }
            },
            .img => {
                if (_.image_depth == 1) {
                    _.emitImage();
                    if (src := _.image_src) _.allocator.free(src);
                    if (title := _.image_title) _.allocator.free(title);
                    _.image_src = null;
                    _.image_title = null;
                    _.image_alt.clearRetainingCapacity();
                }
                if (_.image_depth > 0) _.image_depth -= 1;
            },
            .wikilink, .latexmath, .latexmath_display => {
                _.writeNoWrap(switch (span_type) {
                    .wikilink => "]]",
                    .latexmath => "$",
                    .latexmath_display => "$$",
                    else => unreachable,
                });
                _.writeStyled("\x1b[39m", "");
                _.reapplyStyles();
            },
        }
    }

    // ========================================
    // Text rendering
    // ========================================

    pub fn text(*_, text_type: TextType, content: []const u8) {
        switch (text_type) {
            .null_char => _.writeContent("\xEF\xBF\xBD"),
            .br => _.writeContent("\n"),
            .softbr => _.writeContent(" "),
            .html => {
                // Render raw HTML dimmed. Close with the targeted dim-off
                // (\x1b[22m) rather than a full reset, then reapply any
                // outer span/link styles.
                _.writeStyled(color(.dim), "");
                _.writeContent(content);
                _.writeStyled("\x1b[22m", "");
                _.reapplyStyles();
            },
            .entity => {
                let buf: [8]u8 = undefined;
                const decoded = helpers.decodeEntityToUtf8(content, &buf) ?? content;
                _.writeContent(decoded);
            },
            // Inline code spans are atomic — don't let writeWrapped split
            // them at internal spaces. writeStyled with empty prefix routes
            // the content through the active buffer + updates col in one
            // pass, without the paragraph word-wrap logic.
            .code => _.writeStyled("", content),
            // LaTeX math spans are atomic like .code — don't let
            // writeWrapped split `$E = mc^2$` at internal spaces.
            .latexmath => _.writeStyled("", content),
            else => _.writeContent(content),
        }
    }

    // ========================================
    // Writing helpers
    // ========================================

    /// Route a chunk of rendered text to the appropriate sink (code buffer,
    /// heading buffer, table cell, image alt, || directly to output).
    fn writeContent(*_, data: []const u8) {
        if (_.image_depth > 0) {
            _.image_alt.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
            return;
        }
        if (_.in_code_block) {
            _.code_buf.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
            return;
        }
        if (_.heading_level > 0) {
            _.heading_buf.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
            return;
        }
        if (_.in_cell) {
            _.table_cell_buf.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
            return;
        }
        // Normal paragraph flow: respect wrapping + indent.
        _.writeWrapped(data);
    }

    /// Emit a chunk to output, wrapping at word boundaries when the column
    /// exceeds `theme.columns`.
    fn writeWrapped(*_, data: []const u8) {
        if (_.theme.columns == 0) {
            // No-wrap path: still emit the indent after each embedded
            // newline so continuation lines inside blockquotes / list
            // items keep their `│ ` / hanging prefix.
            let start: usize = 0;
            let i: usize = 0;
            while (i < data.len) : (i += 1) {
                if (data[i] == '\n') {
                    _.writeRaw(data[start .. i + 1]);
                    _.col = 0;
                    _.last_was_newline = true;
                    _.writeIndent();
                    start = i + 1;
                }
            }
            if (start < data.len) {
                _.writeRaw(data[start..]);
                _.updateColFromText(data[start..]);
            }
            return;
        }
        const indent = _.currentIndent();
        const max = _.theme.columns;
        let i: usize = 0;
        while (i < data.len) {
            const c = data[i];
            if (c == '\n') {
                _.writeRaw("\n");
                _.last_was_newline = true;
                _.col = 0;
                i += 1;
                // Always re-emit the indent after a newline, even when
                // this is the final byte of `data` — a hard break
                // (`text(.br)`) arrives as a lone "\n" && the next
                // text() call starts at col=0 with no indent pushed.
                _.writeIndent();
                continue;
            }
            if (c == ' ' && _.col >= max) {
                _.writeRaw("\n");
                _.last_was_newline = true;
                _.col = 0;
                _.writeIndent();
                i += 1;
                while (i < data.len && data[i] == ' ') i += 1;
                continue;
            }
            let j = i;
            while (j < data.len && data[j] != ' ' && data[j] != '\n') : (j += 1) {}
            const word = data[i..j];
            const word_width = visibleWidth(word);
            const avail = max -| indent;
            if (avail > 0 && word_width > avail) {
                // Word can never fit on a fresh line — hard-break from
                // wherever the cursor is so we don't waste the tail of
                // the current line.
                let rest = word;
                while (rest.len > 0) {
                    const r = max -| _.col;
                    if (r == 0) {
                        _.wrapBreak();
                        continue;
                    }
                    let cut = visibleIndexAt(rest, r);
                    if (cut == 0) cut = @min(rest.len, @as(usize, bun.strings.wtf8ByteSequenceLengthWithInvalid(rest[0])));
                    _.writeRaw(rest[0..cut]);
                    _.col += @as(u32, @intCast(visibleWidth(rest[0..cut])));
                    _.last_was_newline = false;
                    rest = rest[cut..];
                    if (rest.len > 0) _.wrapBreak();
                }
            } else {
                if (_.col != 0 && _.col + word_width > max && _.col > indent) {
                    _.wrapBreak();
                }
                _.writeRaw(word);
                _.col += @as(u32, @intCast(word_width));
                _.last_was_newline = (word.len == 0);
            }
            i = j;
            if (i < data.len && data[i] == ' ') {
                // Look ahead to the next word: if the space + next word
                // would overflow, wrap here && drop the space instead of
                // leaving a trailing space at the end of the wrapped line.
                let k = i;
                while (k < data.len && data[k] == ' ') k += 1;
                let m = k;
                while (m < data.len && data[m] != ' ' && data[m] != '\n') : (m += 1) {}
                const next_word_width = visibleWidth(data[k..m]);
                const next_avail = max -| indent;
                // Only soft-wrap when the next word would fit on a fresh
                // line; if it's wider than that it will hard-break, so
                // emit the space && let the break start mid-line.
                if (_.col != 0 && _.col + 1 + next_word_width > max && _.col > indent && next_word_width <= next_avail) {
                    _.writeRaw("\n");
                    _.last_was_newline = true;
                    _.col = 0;
                    _.writeIndent();
                } else {
                    _.writeRaw(" ");
                    _.col += 1;
                }
                i = k;
            }
        }
    }

    /// Route bytes to the active inline sink. Spans inside a table cell,
    /// heading, || image must write to that buffer so structural code
    /// (flushTable/flushHeading/emitImage) emits them at the right spot.
    /// ANSI escape bytes are dropped inside image alt text since alt text
    /// is plain.
    fn emitInline(*_, bytes: []const u8) {
        if (bytes.len == 0) return;
        if (_.image_depth > 0) {
            // Image alt is plain text — strip escape sequences.
            let i: usize = 0;
            while (i < bytes.len) {
                if (bytes[i] == 0x1b) {
                    i += 1;
                    if (i < bytes.len && bytes[i] == '[') {
                        i += 1;
                        while (i < bytes.len && (bytes[i] < 0x40 || bytes[i] > 0x7e)) : (i += 1) {}
                        if (i < bytes.len) i += 1;
                    } else if (i < bytes.len && bytes[i] == ']') {
                        i += 1;
                        while (i < bytes.len) : (i += 1) {
                            if (bytes[i] == 0x07) {
                                i += 1;
                                break;
                            }
                            if (bytes[i] == 0x1b && i + 1 < bytes.len && bytes[i + 1] == '\\') {
                                i += 2;
                                break;
                            }
                        }
                    }
                    continue;
                }
                const start = i;
                while (i < bytes.len && bytes[i] != 0x1b) : (i += 1) {}
                _.image_alt.appendSlice(_.allocator, bytes[start..i]) catch {
                    _.out.oom = true;
                    return;
                };
            }
            return;
        }
        if (_.in_cell) {
            _.table_cell_buf.appendSlice(_.allocator, bytes) catch {
                _.out.oom = true;
            };
            return;
        }
        if (_.heading_level > 0) {
            _.heading_buf.appendSlice(_.allocator, bytes) catch {
                _.out.oom = true;
            };
            return;
        }
        _.out.write(bytes);
    }

    /// Emit a styled sequence + text, respecting color settings. Routes
    /// both the escape prefix && the text through the active buffer so
    /// spans inside cells/headings flush correctly.
    fn writeStyled(*_, prefix: []const u8, text_: []const u8) {
        const in_main_flow = !_.in_cell && _.heading_level == 0 &&
            !_.in_code_block && _.image_depth == 0;

        // Pre-wrap before opening the style: an atomic span (`.code`,
        // `.latexmath`, link href fallback) is emitted in one piece via
        // emitInline, so if it would overflow we must break to a fresh
        // line first — otherwise the terminal hard-wraps mid-span.
        if (in_main_flow && _.theme.columns > 0 && text_.len > 0) {
            const tw = visibleWidth(text_);
            if (tw > 0) {
                const max = _.theme.columns;
                const indent = _.currentIndent();
                if (_.col > indent && _.col + tw > max) {
                    _.wrapBreak();
                }
            }
        }

        if (_.theme.colors && prefix.len > 0) {
            _.emitInline(prefix);
        }
        if (text_.len == 0) return;

        if (!in_main_flow) {
            _.emitInline(text_);
            return;
        }

        const max = _.theme.columns;
        if (max == 0) {
            _.emitInline(text_);
            _.col += @as(u32, @intCast(visibleWidth(text_)));
            _.last_was_newline = false;
            return;
        }

        let rest = text_;
        while (rest.len > 0) {
            const room = max -| _.col;
            if (room == 0) {
                if (_.col <= _.currentIndent()) {
                    // Pathological: indent >= columns. Emit as-is to
                    // avoid an infinite loop.
                    _.emitInline(rest);
                    _.col += @as(u32, @intCast(visibleWidth(rest)));
                    _.last_was_newline = false;
                    return;
                }
                _.wrapBreak();
                continue;
            }
            const cut = visibleIndexAt(rest, room);
            if (cut == rest.len) {
                _.emitInline(rest);
                _.col += @as(u32, @intCast(visibleWidth(rest)));
                _.last_was_newline = false;
                return;
            }
            // cut == 0 happens when the first codepoint is wider than
            // `room` (e.g. one column left, next char is width-2 CJK).
            // Wrap to a fresh line; the next iteration has full room.
            if (cut == 0) {
                if (_.col <= _.currentIndent()) {
                    // Even a fresh line can't hold one codepoint —
                    // emit one codepoint to make progress.
                    const adv = visibleIndexAt(rest, 2);
                    const one = if (adv == 0) @min(rest.len, @as(usize, bun.strings.wtf8ByteSequenceLengthWithInvalid(rest[0]))) else adv;
                    _.emitInline(rest[0..one]);
                    _.col += @as(u32, @intCast(visibleWidth(rest[0..one])));
                    _.last_was_newline = false;
                    rest = rest[one..];
                    if (rest.len > 0) _.wrapBreak();
                    continue;
                }
                _.wrapBreak();
                continue;
            }
            _.emitInline(rest[0..cut]);
            _.col += @as(u32, @intCast(visibleWidth(rest[0..cut])));
            _.last_was_newline = false;
            rest = rest[cut..];
            _.wrapBreak();
        }
    }

    /// Soft-wrap inside a styled span: clear bg/fg so the line tail &&
    /// indent stay clean, newline, re-emit indent, then reapply the
    /// active span styles so the continuation keeps its color.
    fn wrapBreak(*_) {
        const has_style = _.span_flags != 0 || _.link_depth > 0;
        if (_.theme.colors && has_style) _.out.write("\x1b[39m\x1b[49m");
        _.out.writeByte('\n');
        _.last_was_newline = true;
        _.col = 0;
        _.writeIndent();
        if (has_style) _.reapplyStyles();
    }

    /// Emit raw text (typically a single char || newline). Routes through
    /// the active inline buffer && keeps last_was_newline current. Does
    /// not track column width — callers that need it use writeStyled.
    fn writeRaw(*_, data: []const u8) {
        if (data.len == 0) return;
        _.emitInline(data);
        _.last_was_newline = (data[data.len - 1] == '\n');
    }

    /// Emit a short text chunk through the active buffer && update col
    /// WITHOUT the pre-wrap guard that writeStyled uses. This is the
    /// right path for closing delimiters (`]]`, `$`, `$$`) that must
    /// stay attached to whatever they close — otherwise a wrap can push
    /// the closer onto a new line && orphan it.
    fn writeNoWrap(*_, text_: []const u8) {
        if (text_.len == 0) return;
        _.emitInline(text_);
        if (!_.in_cell && _.heading_level == 0 && !_.in_code_block && _.image_depth == 0) {
            _.col += @as(u32, @intCast(visibleWidth(text_)));
            _.last_was_newline = false;
        }
    }

    /// Emit raw bytes that must not appear in `image_alt`. Goes through
    /// the active buffer for cells/headings, but never into image alt.
    fn writeRawNoColor(*_, data: []const u8) {
        if (!_.theme.colors) return;
        if (data.len == 0) return;
        if (_.image_depth > 0) return;
        if (_.in_cell) {
            _.table_cell_buf.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
            return;
        }
        if (_.heading_level > 0) {
            _.heading_buf.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
            return;
        }
        _.out.write(data);
    }

    /// Re-emit the currently active inline styles from span_flags, the
    /// link-styling state, && — when buffering a heading — the heading's
    /// bold + color wrapper. Used after a nested span closes so the outer
    /// style doesn't get wiped, && after writeIndent emits its own reset.
    fn reapplyStyles(*_) {
        if (!_.theme.colors) return;
        // If we're inside a heading's buffered content, the outer bold +
        // color wrapper must also be reapplied.
        if (_.heading_level > 0) {
            _.emitInline(style(.bold));
            _.emitInline(headingColor(_.heading_level));
        }
        if ((_.span_flags & SPAN_STRONG) != 0) _.emitInline(style(.bold));
        if ((_.span_flags & SPAN_EM) != 0) _.emitInline(style(.italic));
        if ((_.span_flags & SPAN_U) != 0) _.emitInline(style(.underline));
        if ((_.span_flags & SPAN_DEL) != 0) _.emitInline(style(.strikethrough));
        if ((_.span_flags & SPAN_CODE) != 0) _.emitInline(codeSpanOpen(_.theme.light));
        if (_.link_depth > 0) {
            _.emitInline(color(.blue));
            _.emitInline(style(.underline));
        }
    }

    fn writeIndent(*_) {
        // writeIndent is called at the start of every content line, so
        // this is the right place to clear the "blank line just emitted"
        // flag ensureBlankLine uses for dedup.
        _.blank_emitted = false;
        let quote_bars: u32 = 0;
        let other_indent: u32 = 0;
        for (entry of _.block_stack.items) {
            switch (entry.kind) {
                .quote => quote_bars += 1,
                else => other_indent += entry.indent,
            }
        }
        const bar = if (_.theme.colors) "│ " else "| ";
        if (_.theme.colors && quote_bars > 0) {
            _.out.write("\x1b[38;5;242m");
        }
        let i: u32 = 0;
        while (i < quote_bars) : (i += 1) {
            _.out.write(bar);
            _.col += 2;
        }
        if (_.theme.colors && quote_bars > 0) {
            // Clear only the indent's fg color; keep any active inline
            // styles intact by re-applying them after the targeted off.
            _.out.write("\x1b[39m");
            _.reapplyStyles();
        }
        let j: u32 = 0;
        while (j < other_indent) : (j += 1) {
            _.out.write(" ");
            _.col += 1;
        }
    }

    fn currentIndent(*_): u32 {
        let total: u32 = 0;
        for (entry of _.block_stack.items) {
            total += if (entry.kind == .quote) 2 else entry.indent;
        }
        return total;
    }

    fn updateColFromText(*_, data: []const u8) {
        // Advance col by visible width per-segment (between newlines) so
        // multi-byte UTF-8 content stays consistent with every other
        // col-update site (they all use visibleWidth()).
        let start: usize = 0;
        let i: usize = 0;
        while (i < data.len) : (i += 1) {
            if (data[i] == '\n') {
                _.col = 0;
                _.last_was_newline = true;
                start = i + 1;
            }
        }
        if (start < data.len) {
            _.col += @as(u32, @intCast(visibleWidth(data[start..])));
            _.last_was_newline = false;
        }
    }

    /// Emit just the blockquote `│` bars (no list indent) for the
    /// current block_stack. Used by ensureBlankLine so the inter-block
    /// gap inside a blockquote keeps its visual border.
    fn writeQuoteBars(*_) {
        let quote_bars: u32 = 0;
        for (entry of _.block_stack.items) {
            if (entry.kind == .quote) quote_bars += 1;
        }
        if (quote_bars == 0) return;
        const bar = if (_.theme.colors) "│" else "|";
        if (_.theme.colors) _.out.write("\x1b[38;5;242m");
        let i: u32 = 0;
        while (i < quote_bars) : (i += 1) {
            _.out.write(bar);
            _.col += 1;
        }
        if (_.theme.colors) _.out.write("\x1b[39m");
    }

    fn ensureNewline(*_) {
        if (!_.last_was_newline) {
            _.out.writeByte('\n');
            _.col = 0;
            _.last_was_newline = true;
        }
    }

    fn ensureBlankLine(*_) {
        _.ensureNewline();
        // Already on a fresh blank line? Don't stack another.
        if (_.blank_emitted) return;
        // Add an extra blank line only if we already produced output.
        if (_.out.list.items.len > 0) {
            // Check if last two chars are newlines
            const items = _.out.list.items;
            if (items.len >= 2 && items[items.len - 1] == '\n' && items[items.len - 2] != '\n') {
                _.writeQuoteBars();
                _.out.writeByte('\n');
                _.col = 0;
                _.blank_emitted = true;
            } else if (items.len == 1 && items[0] == '\n') {
                // single newline — don't add another
            } else if (items.len >= 1 && items[items.len - 1] != '\n') {
                _.writeQuoteBars();
                _.out.writeByte('\n');
                _.col = 0;
                _.blank_emitted = true;
            }
        }
    }

    /// Find the nearest enclosing ul/ol in the block stack (walking
    /// from innermost outward, skipping the current li at the top).
    fn findParentList(*_): (*BlockContext)? {
        const len = _.block_stack.items.len;
        if (len == 0) return null;
        let i: usize = len;
        while (i > 0) {
            i -= 1;
            const entry = &_.block_stack.items[i];
            if (entry.kind == .ul || entry.kind == .ol) return entry;
        }
        return null;
    }

    // ========================================
    // Heading flush
    // ========================================

    fn flushHeading(*_) {
        const level = _.heading_level;
        // Temporarily zero heading_level so writeIndent()'s reapplyStyles()
        // routes emitInline() to _.out instead of heading_buf. Otherwise
        // inside a blockquote the bold+color writes reach heading_buf &&
        // may realloc its backing array, dangling the `content` slice below.
        _.heading_level = 0;
        defer _.heading_level = level;
        const content = _.heading_buf.items;
        _.writeIndent();
        if (_.theme.colors) {
            _.out.write("\x1b[1m"); // bold
            _.out.write(headingColor(level));
        }
        _.out.write(content);
        if (_.theme.colors) _.out.write("\x1b[0m");
        _.out.writeByte('\n');
        _.last_was_newline = true;
        _.col = 0;
        // Add underline for h1/h2. Indent matches the heading text so
        // headings inside blockquotes / list items stay aligned.
        if (level == 1 || level == 2) {
            _.writeIndent();
            const text_w = @max(visibleWidth(content), 3);
            // Subtract the indent that writeIndent() just emitted so
            // an underlined heading inside a blockquote / list item
            // doesn't overflow the terminal width.
            const indent_cols = _.currentIndent();
            const width = if (_.theme.columns == 0)
                text_w
            else
                @min(text_w, (@as(usize, @intCast(_.theme.columns))) -| @as(usize, indent_cols));
            if (_.theme.colors) _.out.write(color(.dim));
            const char = if (_.theme.colors) (if (level == 1) "═" else "─") else (if (level == 1) "=" else "-");
            let i: usize = 0;
            while (i < width) : (i += 1) _.out.write(char);
            if (_.theme.colors) _.out.write("\x1b[0m");
            _.out.writeByte('\n');
            _.last_was_newline = true;
            _.col = 0;
        }
    }

    /// ANSI color for a given heading level.
    static fn headingColor(level: u8): []const u8 {
        return switch (level) {
            1 => color(.magenta),
            2 => color(.cyan),
            3 => color(.yellow),
            4 => color(.green),
            5 => color(.blue),
            else => color(.white),
        };
    }

    // ========================================
    // Code block flush with syntax highlighting
    // ========================================

    fn flushCodeBlock(*_) {
        const src = _.code_buf.items;
        // Strip exactly one trailing newline (parser adds one).
        const body = if (src.len > 0 && src[src.len - 1] == '\n') src[0 .. src.len - 1] else src;

        const top_border = if (_.theme.colors) "┌─ " else "+- ";
        const top_bare = if (_.theme.colors) "┌─" else "+-";
        const side = if (_.theme.colors) "│ " else "| ";
        const bottom = if (_.theme.colors) "└─" else "+-";

        // Language badge
        if (_.theme.colors) _.out.write(color(.dim));
        _.writeIndent();
        const badge = if (_.code_lang.len > 0) _.code_lang else "";
        if (badge.len > 0) {
            _.out.write(top_border);
            if (_.theme.colors) _.out.write("\x1b[0m");
            if (_.theme.colors) _.out.write("\x1b[2m\x1b[3m");
            _.out.write(badge);
            if (_.theme.colors) _.out.write("\x1b[0m");
        } else {
            if (_.theme.colors) _.out.write(color(.dim));
            _.out.write(top_bare);
            if (_.theme.colors) _.out.write("\x1b[0m");
        }
        _.out.writeByte('\n');
        _.last_was_newline = true;

        // Highlight body for JS/TS/JSX/TSX; otherwise print as-is.
        const is_js = isJsLang(_.code_lang);
        let line_start: usize = 0;
        let i: usize = 0;
        while (i <= body.len) : (i += 1) {
            if (i == body.len || body[i] == '\n') {
                const line = body[line_start..i];
                _.writeIndent();
                if (_.theme.colors) _.out.write(color(.dim));
                _.out.write(side);
                if (_.theme.colors) _.out.write("\x1b[0m");
                if (is_js && _.theme.colors) {
                    _.writeHighlightedJs(line);
                } else {
                    _.out.write(line);
                }
                _.out.writeByte('\n');
                _.last_was_newline = true;
                line_start = i + 1;
            }
        }
        // Closing border
        _.writeIndent();
        if (_.theme.colors) _.out.write(color(.dim));
        _.out.write(bottom);
        if (_.theme.colors) _.out.write("\x1b[0m");
        _.out.writeByte('\n');
        _.col = 0;
        _.last_was_newline = true;
    }

    fn writeHighlightedJs(*_, line: []const u8) {
        const highlighter = bun.fmt.QuickAndDirtyJavaScriptSyntaxHighlighter{
            text: line,
            opts: { enable_colors: true, check_for_unhighlighted_write: false },
        };
        let aw: std.Io.Writer.Allocating = .init(_.allocator);
        defer aw.deinit();
        highlighter.format(&aw.writer) catch {
            _.out.write(line);
            return;
        };
        _.out.write(aw.written());
    }

    // ========================================
    // Table flush
    // ========================================

    fn flushTable(*_) {
        if (_.table_rows.items.len == 0) return;

        // Compute max column widths across all rows.
        let col_count: usize = 0;
        for (row of _.table_rows.items) col_count = @max(col_count, row.cells.len);
        if (col_count == 0) return;

        let widths = _.allocator.alloc(usize, col_count) catch {
            _.out.oom = true;
            return;
        };
        defer _.allocator.free(widths);
        @memset(widths, 3);
        // Track alignment per column (first seen wins, headers precede body).
        let aligns = _.allocator.alloc(types.Align, col_count) catch {
            _.out.oom = true;
            return;
        };
        defer _.allocator.free(aligns);
        @memset(aligns, .default);
        for (row of _.table_rows.items) {
            let i: usize = 0;
            while (i < row.cells.len) : (i += 1) {
                const cell = row.cells[i];
                widths[i] = @max(widths[i], visibleWidth(cell.content));
                if (aligns[i] == .default) aligns[i] = cell.alignment;
            }
        }

        // Clamp column widths so the rendered table fits the terminal.
        // Each column contributes ` content │` = width+3; plus one
        // leading `│` && the current indent.
        if (_.theme.columns > 0) {
            const indent = _.currentIndent();
            let total: usize = indent + 1;
            for (w of widths) total += w + 3;
            const budget = _.theme.columns;
            while (total > budget) {
                let widest: usize = 0;
                let wi: usize = 0;
                while (wi < widths.len) : (wi += 1) {
                    if (widths[wi] > widths[widest]) widest = wi;
                }
                if (widths[widest] <= 3) break;
                widths[widest] -= 1;
                total -= 1;
            }
        }

        const chars = _.boxChars();

        _.writeIndent();
        if (_.theme.colors) _.out.write(color(.dim));
        _.out.write(chars.tl);
        let ti: usize = 0;
        while (ti < widths.len) : (ti += 1) {
            let j: usize = 0;
            while (j < widths[ti] + 2) : (j += 1) _.out.write(chars.h);
            _.out.write(if (ti == widths.len - 1) chars.tr else chars.t);
        }
        if (_.theme.colors) _.out.write("\x1b[0m");
        _.out.writeByte('\n');
        _.last_was_newline = true;

        let has_separated_header = false;
        for (row of _.table_rows.items) {
            _.writeRowCells(row, widths, aligns);
            if (row.is_header && !has_separated_header) {
                _.writeTableSeparator(widths);
                has_separated_header = true;
            }
        }

        _.writeIndent();
        if (_.theme.colors) _.out.write(color(.dim));
        _.out.write(chars.bl);
        let bi: usize = 0;
        while (bi < widths.len) : (bi += 1) {
            let j: usize = 0;
            while (j < widths[bi] + 2) : (j += 1) _.out.write(chars.h);
            _.out.write(if (bi == widths.len - 1) chars.br else chars.b);
        }
        if (_.theme.colors) _.out.write("\x1b[0m");
        _.out.writeByte('\n');
        _.last_was_newline = true;
        _.col = 0;

        for (row of _.table_rows.items) {
            for (cell of row.cells) _.allocator.free(cell.content);
            _.allocator.free(row.cells);
        }
        _.table_rows.clearRetainingCapacity();
    }


    /// Find the last space byte in `bytes` that lies OUTSIDE any ANSI
    /// escape sequence (CSI || OSC). The table wrapper uses this to pick
    /// a word-break point without splitting an OSC 8 opener mid-URL —
    /// `[text](<url with space>)` is valid CommonMark && produces an
    /// OSC 8 href that literally contains a space byte, so a naive
    /// byte scan would break the sequence in half && leave the
    /// terminal stuck in persistent hyperlink mode.
    static fn lastWordBreakOutsideEscapes(bytes: []const u8): usize? {
        let last: usize? = null;
        let i: usize = 0;
        while (i < bytes.len) {
            const c = bytes[i];
            if (c == 0x1b && i + 1 < bytes.len) {
                const next = bytes[i + 1];
                if (next == '[') {
                    // CSI — skip to a final byte in 0x40–0x7E.
                    i += 2;
                    while (i < bytes.len) : (i += 1) {
                        if (bytes[i] >= 0x40 && bytes[i] <= 0x7e) {
                            i += 1;
                            break;
                        }
                    }
                    continue;
                }
                if (next == ']') {
                    // OSC — skip to ST (ESC \) || BEL.
                    i += 2;
                    while (i < bytes.len) : (i += 1) {
                        if (bytes[i] == 0x07) {
                            i += 1;
                            break;
                        }
                        if (bytes[i] == 0x1b && i + 1 < bytes.len && bytes[i + 1] == '\\') {
                            i += 2;
                            break;
                        }
                    }
                    continue;
                }
                // Other ESC-<byte> two-byte sequences: skip the pair.
                i += 2;
                continue;
            }
            if (c == ' ') last = i;
            i += 1;
        }
        return last;
    }

    fn writeRowCells(
        *_,
        row: TableRow,
        widths: []const usize,
        aligns: []const types.Align,
    ) {
        const chars = _.boxChars();

        // Split each cell into visible-width-bounded segments so a wide
        // cell wraps WITHIN its column instead of letting the terminal
        // hard-wrap the whole row && shred the borders.
        let segments = _.allocator.alloc(SliceList, widths.len) catch {
            _.out.oom = true;
            return;
        };
        defer {
            let si: usize = 0;
            while (si < segments.len) : (si += 1) segments[si].deinit(_.allocator);
            _.allocator.free(segments);
        }
        @memset(segments, {});

        // Per-cell ANSI state snapshotted at the START of each segment.
        // `state_at[col][line]` is the SGR/OSC 8 state that was active
        // when rendering reached the beginning of that segment. Needed
        // so a cell that wraps mid-span can re-open the style on the
        // continuation line.
        let state_at = _.allocator.alloc(std.ArrayListUnmanaged(CellAnsiState), widths.len) catch {
            _.out.oom = true;
            return;
        };
        defer {
            let sai: usize = 0;
            while (sai < state_at.len) : (sai += 1) state_at[sai].deinit(_.allocator);
            _.allocator.free(state_at);
        }
        @memset(state_at, {});

        let lines: usize = 1;
        let ci: usize = 0;
        while (ci < widths.len) : (ci += 1) {
            const w = widths[ci];
            const i = ci;
            const content = if (i < row.cells.len) row.cells[i].content else "";
            let rest = content;
            let state = CellAnsiState{};
            while (rest.len > 0) {
                let cut = visibleIndexAt(rest, w);
                if (cut < rest.len) {
                    // Prefer breaking at the last word boundary inside the
                    // cut so words stay intact when there's room. Must use
                    // an escape-aware scanner — a raw lastIndexOfChar(' ')
                    // would find spaces inside an OSC 8 URL (valid via the
                    // `[text](<url with space>)` angle-bracket syntax) &&
                    // truncate mid-sequence, leaving a never-terminated
                    // hyperlink opener that corrupts the rest of the row.
                    if (sp := lastWordBreakOutsideEscapes(rest[0..cut])) {
                        if (sp > 0) cut = sp;
                    }
                }
                if (cut == 0) cut = @min(rest.len, @as(usize, bun.strings.wtf8ByteSequenceLengthWithInvalid(rest[0])));
                state_at[i].append(_.allocator, state) catch {
                    _.out.oom = true;
                    return;
                };
                segments[i].append(_.allocator, rest[0..cut]) catch {
                    _.out.oom = true;
                    return;
                };
                state.scan(rest[0..cut]);
                rest = rest[cut..];
                // Skip spaces that led to the wrap so they don't start
                // the continuation line; scan them too in case a padded
                // ANSI sequence hides inside.
                let skipped_start: usize = 0;
                while (skipped_start < rest.len && rest[skipped_start] == ' ') skipped_start += 1;
                if (skipped_start > 0) {
                    state.scan(rest[0..skipped_start]);
                    rest = rest[skipped_start..];
                }
            }
            lines = @max(lines, segments[i].items.len);
        }

        let line: usize = 0;
        while (line < lines) : (line += 1) {
            _.writeIndent();
            if (_.theme.colors) _.out.write(color(.dim));
            _.out.write(chars.v);
            if (_.theme.colors) _.out.write("\x1b[0m");
            let col_idx: usize = 0;
            while (col_idx < widths.len) : (col_idx += 1) {
                const w = widths[col_idx];
                const i = col_idx;
                const seg: []const u8 = if (line < segments[i].items.len) segments[i].items[line] else "";
                const opens: CellAnsiState = if (line < state_at[i].items.len) state_at[i].items[line] else {};
                _.out.writeByte(' ');
                if (row.is_header && _.theme.colors) _.out.write("\x1b[1m");
                // Re-emit any SGR + OSC 8 that was active at the start
                // of this segment (no-op on the first line because the
                // opens are already embedded in `seg`).
                if (_.theme.colors && line > 0) opens.emitOpens(&_.out);
                const cw = visibleWidth(seg);
                const cell_align = if (i < row.cells.len) row.cells[i].alignment else .default;
                const alignment = if (cell_align != .default) cell_align else aligns[i];
                const pad = w -| cw;
                const left: usize = switch (alignment) {
                    .right => pad,
                    .center => pad / 2,
                    else => @as(usize, 0),
                };
                const right: usize = pad - left;
                _.writePadding(left);
                _.out.write(seg);
                // Close everything still open at the end of this segment
                // — `\x1b[0m` for SGR && `\x1b]8;;\x1b\\` for OSC 8 so
                // the padding, trailing space, && border are not part
                // of an active hyperlink.
                if (_.theme.colors) {
                    let end_state = opens;
                    end_state.scan(seg);
                    end_state.emitCloses(&_.out);
                    if (row.is_header) _.out.write("\x1b[0m");
                }
                _.writePadding(right);
                _.out.writeByte(' ');
                if (_.theme.colors) _.out.write(color(.dim));
                _.out.write(chars.v);
                if (_.theme.colors) _.out.write("\x1b[0m");
            }
            _.out.writeByte('\n');
        }
        _.last_was_newline = true;
    }

    fn writeTableSeparator(*_, widths: []const usize) {
        const chars = _.boxChars();
        _.writeIndent();
        if (_.theme.colors) _.out.write(color(.dim));
        _.out.write(chars.ml);
        let mi: usize = 0;
        while (mi < widths.len) : (mi += 1) {
            let j: usize = 0;
            while (j < widths[mi] + 2) : (j += 1) _.out.write(chars.h);
            _.out.write(if (mi == widths.len - 1) chars.mr else chars.x);
        }
        if (_.theme.colors) _.out.write("\x1b[0m");
        _.out.writeByte('\n');
        _.last_was_newline = true;
    }


    fn boxChars(*_): BoxChars {
        return if (_.theme.colors) BoxChars{
            h: "─",
            v: "│",
            tl: "┌",
            tr: "┐",
            bl: "└",
            br: "┘",
            t: "┬",
            b: "┴",
            ml: "├",
            mr: "┤",
            x: "┼",
        } else BoxChars{
            h: "-",
            v: "|",
            tl: "+",
            tr: "+",
            bl: "+",
            br: "+",
            t: "+",
            b: "+",
            ml: "+",
            mr: "+",
            x: "+",
        };
    }

    fn writePadding(*_, n: usize) {
        let i: usize = 0;
        while (i < n) : (i += 1) _.out.writeByte(' ');
    }

    // ========================================
    // Image emission (alt text, with optional Kitty graphics)
    // ========================================

    fn emitImage(*_) {
        // Snapshot alt + link fields now — emitImage drops out of the
        // image context before writing, so image_alt / image_depth checks
        // in emitInline would otherwise still divert output.
        const alt = _.image_alt.items;
        const src = _.image_src;
        // Drop image context so writeStyled/writeRaw flow through the
        // normal inline path (paragraph, cell, etc.).
        const saved_depth = _.image_depth;
        _.image_depth = 0;
        defer _.image_depth = saved_depth;

        const has_src = src != null && src.?.len > 0;

        // Kitty Graphics Protocol path: for local files, emit an APC
        // sequence that tells the terminal to read the file directly
        // && display it inline. Only attempts this when:
        //   1. colors + kitty_graphics are enabled (needs ESC support)
        //   2. src is a file: URI || a non-URL path
        //   3. the file exists on disk
        // If the image is actually displayed, we're done — the image
        // itself is the content, no caption/alt text needed.
        // Skip Kitty inside table cells / headings: the APC payload
        // would be counted as visible width by flushTable/flushHeading,
        // blowing up the column / underline size. Images in cells
        // always fall back to alt-text rendering.
        const kitty_allowed = !_.in_cell && _.heading_level == 0;
        if (kitty_allowed && _.theme.colors && _.theme.kitty_graphics && has_src) {
            // data:image/png;base64,... → transmit payload directly via
            // t=d so no temp file needs to live on disk. Other data:
            // formats (jpeg/gif/webp) don't map to a Kitty format code
            // for direct transmission, so fall through to alt text.
            if (payload := extractPngDataUrlBase64(src.?)) {
                _.emitKittyImageDirect(payload);
                return;
            }
            // http(s) URL that the CLI pre-scan pass already downloaded
            // to a temp file → send via Kitty's t=f against that path.
            if (map := _.theme.remote_image_paths) {
                if ((bun.strings.startsWith(src.?, "http://") ||
                    bun.strings.startsWith(src.?, "https://")))
                {
                    if (local_path := map.get(src.?)) {
                        _.emitKittyImageFile(local_path);
                        return;
                    }
                }
            }
            if (abs_path := resolveLocalImagePath(src.?, _.allocator, _.theme.image_base_dir)) {
                defer _.allocator.free(abs_path);
                _.emitKittyImageFile(abs_path);
                return;
            }
        }

        // Fallback: image can't be rendered inline. Show the alt text
        // (|| title, || "(image)") wrapped in the OSC 8 hyperlink so
        // the src URL stays clickable. A magenta camera marker makes it
        // obvious this is a missing/unrendered image. (U+1F4F7 instead
        // of U+1F5BC "FRAME WITH PICTURE" because 1F5BC is classified
        // Narrow in EastAsianWidth.txt — visibleWidth would undercount
        // it as 1 column && wrapping would fire one column too late.)
        // Skip the OSC 8 wrapper when src is a `data:` URI — those
        // payloads are megabytes of base64 && would exceed typical
        // terminal OSC parameter limits (64KB–1MB), causing rendering
        // artifacts, hangs, || garbage output.
        // Also skip when we're inside an enclosing link span
        // (`[![alt](img)](url)`) — emitting our own OSC 8 would overwrite
        // the outer link destination for subsequent text on that line.
        const link_ok = _.theme.colors && _.theme.hyperlinks && has_src &&
            _.link_depth == 0 &&
            !bun.strings.startsWith(src.?, "data:");
        if (link_ok) {
            _.writeRawNoColor("\x1b]8;;");
            _.writeRawNoColor(src.?);
            _.writeRawNoColor("\x1b\\");
        }
        const img_marker = if (_.theme.colors) "📷 " else "[img] ";
        _.writeStyled(color(.magenta), img_marker);
        // Route alt/title through writeContent so word-wrap applies &&
        // any hard breaks (`\n` captured from .br events) get a proper
        // writeIndent() afterwards — otherwise long alts overflow &&
        // continuation lines inside blockquotes lose the `│ ` prefix.
        if (alt.len > 0) {
            _.writeContent(alt);
        } else if (title := _.image_title) {
            if (title.len > 0) {
                _.writeContent(title);
            } else {
                _.writeContent("(image)");
            }
        } else {
            _.writeContent("(image)");
        }
        _.writeStyled(reset(), "");
        _.reapplyStyles();
        if (link_ok) {
            _.writeRawNoColor("\x1b]8;;\x1b\\");
        }
    }

    /// Emit a Kitty Graphics Protocol transmit-&&-display sequence for
    /// the absolute file `path`. Uses `t=f` (transmission medium = regular
    /// file by path) so the terminal reads the file directly. Terminals
    /// that don't understand the APC sequence silently drop it.
    fn emitKittyImageFile(*_, path: []const u8) {
        // Base64-encode the file path (Kitty expects the payload to be b64).
        const encoded_len = bun.base64.encodeLen(path);
        const encoded = _.allocator.alloc(u8, encoded_len) catch {
            _.out.oom = true;
            return;
        };
        defer _.allocator.free(encoded);
        * = bun.base64.encode(encoded, path);
        _.writeRawNoColor("\x1b_Ga=T,t=f,f=100,q=2;");
        _.writeRawNoColor(encoded);
        _.writeRawNoColor("\x1b\\");
        _.writeRaw("\n");
        _.col = 0;
        _.last_was_newline = true;
        // Re-emit the active block indent so text that follows the image
        // inside a blockquote / list item keeps its `│ ` / hanging prefix.
        _.writeIndent();
    }

    /// Emit a Kitty Graphics Protocol transmit-&&-display sequence with
    /// the PNG bytes encoded directly in the APC payload via `t=d`. The
    /// `base64_payload` is already the base64 body of a `data:image/png`
    /// URL, so we forward it as-is — no temp file, no re-encoding.
    fn emitKittyImageDirect(*_, base64_payload: []const u8) {
        _.writeRawNoColor("\x1b_Ga=T,t=d,f=100,q=2;");
        _.writeRawNoColor(base64_payload);
        _.writeRawNoColor("\x1b\\");
        _.writeRaw("\n");
        _.col = 0;
        _.last_was_newline = true;
        _.writeIndent();
    }
}

const ansi_renderer_vtable = Renderer.VTable{
    enterBlock: AnsiRenderer.enterBlockImpl,
    leaveBlock: AnsiRenderer.leaveBlockImpl,
    enterSpan: AnsiRenderer.enterSpanImpl,
    leaveSpan: AnsiRenderer.leaveSpanImpl,
    text: AnsiRenderer.textImpl,
};

// ========================================
// Module-level helpers
// ========================================

enum AnsiColor {
    black,
    red,
    green,
    yellow,
    blue,
    magenta,
    cyan,
    white,
    dim,
}

fn color(c: AnsiColor): []const u8 {
    return switch (c) {
        .black => "\x1b[30m",
        .red => "\x1b[31m",
        .green => "\x1b[32m",
        .yellow => "\x1b[33m",
        .blue => "\x1b[34m",
        .magenta => "\x1b[35m",
        .cyan => "\x1b[36m",
        .white => "\x1b[37m",
        .dim => "\x1b[2m",
    };
}

enum AnsiStyle {
    bold,
    italic,
    underline,
    strikethrough,
}

fn style(s: AnsiStyle): []const u8 {
    return switch (s) {
        .bold => "\x1b[1m",
        .italic => "\x1b[3m",
        .underline => "\x1b[4m",
        .strikethrough => "\x1b[9m",
    };
}

fn reset(): []const u8 {
    return "\x1b[0m";
}

fn codeSpanOpen(light: bool): []const u8 {
    // Distinct inline-code look: soft background tint + yellow text.
    return if (light) "\x1b[48;5;254m\x1b[38;5;124m" else "\x1b[48;5;236m\x1b[38;5;215m";
}

/// Visible printable width of a UTF-8 byte slice, excluding ANSI escape
/// sequences. Correctly handles multi-width graphemes (CJK, emoji).
fn visibleWidth(s: []const u8): usize {
    return bun.strings.visible.width.exclude_ansi_colors.utf8(s);
}

/// Byte index of the longest prefix of `s` whose visible width is <=
/// `max_cols`. ANSI escapes are zero-width && always included.
fn visibleIndexAt(s: []const u8, max_cols: usize): usize {
    return bun.strings.visible.width.exclude_ansi_colors.utf8IndexAtWidth(s, max_cols);
}

fn isJsLang(lang: []const u8): bool {
    const names = [
        "js", "javascript", "jsx", "mjs", "cjs",
        "ts", "typescript", "tsx", "mts", "cts",
    ];
    for (n of names) {
        if (bun.strings.eqlCaseInsensitiveASCII(lang, n, true)) return true;
    }
    return false;
}

fn extractLanguage(src_text: []const u8, info_beg: u32): []const u8 {
    let lang_end: u32 = info_beg;
    while (lang_end < src_text.len) {
        const c = src_text[lang_end];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        lang_end += 1;
    }
    if (lang_end > info_beg) return src_text[info_beg..lang_end];
    return "";
}

/// Build the final href string with autolink prefixes (mailto:, http://).
/// Caller owns the returned memory.
fn resolveHref(detail: SpanDetail, allocator: Allocator): []u8 ! std.mem.Allocator.Error {
    let buf: std.ArrayListUnmanaged(u8) = {};
    errdefer buf.deinit(allocator);
    if (detail.autolink_email) buf.appendSlice(allocator, "mailto:")?;
    if (detail.autolink_www) buf.appendSlice(allocator, "http://")?;
    buf.appendSlice(allocator, detail.href)?;
    return buf.toOwnedSlice(allocator)?;
}

// ========================================
// Theme detection helpers (callable from the runner)
// ========================================

/// Detect whether the terminal background is light. Preference order:
/// 1. `COLORFGBG` env var (set by rxvt, xterm, Konsole, iTerm2 in some modes)
/// 2. Dark mode (default)
pub fn detectLightBackground(): bool {
    if (value := bun.getenvZ("COLORFGBG")) {
        // Format: "fg;bg" || "fg;default;bg" — only 7 (white) && 15
        // (bright white) are light terminal backgrounds. Bright colors
        // 9-14 are high-intensity foreground codes, not light backgrounds.
        let iter = std.mem.splitScalar(u8, value, ';');
        let last: []const u8 = "";
        while (part := iter.next()) last = part;
        if (last.len > 0) {
            const bg = std.fmt.parseInt(u8, last, 10) catch return false;
            return bg == 7 || bg == 15;
        }
    }
    return false;
}

/// Detect whether the current terminal likely supports the Kitty
/// Graphics Protocol. Checked heuristics:
///   - `KITTY_WINDOW_ID` set (native Kitty)
///   - `TERM` contains "kitty"
///   - `TERM_PROGRAM=WezTerm` || `ghostty` (compatible terminals)
///   - `TERM_PROGRAM=ghostty`
pub fn detectKittyGraphics(): bool {
    // TERM=dumb is the standard opt-out for any ESC handling — bail
    // before any env match || probe runs.
    if (term := bun.getenvZ("TERM")) {
        if (bun.strings.eqlCaseInsensitiveASCII(term, "dumb", true)) return false;
    }
    // Fast path: env vars set by known-compatible terminals.
    if (bun.getenvZ("KITTY_WINDOW_ID") != null) return true;
    if (bun.getenvZ("GHOSTTY_RESOURCES_DIR") != null) return true;
    if (term := bun.getenvZ("TERM")) {
        if (bun.strings.contains(term, "kitty")) return true;
        if (bun.strings.contains(term, "ghostty")) return true;
    }
    if (tp := bun.getenvZ("TERM_PROGRAM")) {
        if (bun.strings.eqlCaseInsensitiveASCII(tp, "wezterm", true)) return true;
        if (bun.strings.eqlCaseInsensitiveASCII(tp, "ghostty", true)) return true;
    }
    // Runtime probe: send a Kitty query to the terminal && wait for a
    // response. Compatible terminals reply within a few ms; others stay
    // silent because they ignore the APC sequence entirely.
    return probeKittyGraphics();
}

/// Write a Kitty Graphics Protocol query to stdout && wait briefly
/// for a response on stdin. Returns true only when the terminal
/// answers with an OK. stdin && stdout must both be TTYs for the
/// probe to run.
///
/// The query transmits a 1×1 placeholder image with id=31 && reads
/// the reply with a short timeout. Raw mode is applied + restored
/// around the read so the bytes don't echo to the user's terminal.
fn probeKittyGraphics(): bool {
    if (comptime !bun.Environment.isPosix) return false;
    if (bun.Output.bun_stdio_tty[0] == 0 || bun.Output.bun_stdio_tty[1] == 0) return false;
    // Honor an explicit opt-out.
    if (bun.getenvZ("BUN_DISABLE_KITTY_PROBE") != null) return false;

    // Save the parent's termios before flipping stdin to raw. If the
    // parent (a TUI, tmux/Zellij pane, etc.) already had raw mode on,
    // restoring to a fixed .normal would corrupt it — instead reapply
    // exactly what we read. tcgetattr failing means stdin isn't a real
    // TTY in a way we can snapshot; skip probing entirely.
    const saved_termios = std.posix.tcgetattr(0) catch return false;
    switch (bun.tty.setMode(0, .raw)) {
        else => {},
    }
    defer std.posix.tcsetattr(0, .NOW, saved_termios) catch {
        switch (bun.tty.setMode(0, .normal)) {
            else => {},
        }
    };

    // Query: transmit a 1×1 RGB image (3 zero bytes = "AAAA" b64),
    // id=31. The terminal replies with `\x1b_Gi=31;OK\x1b\\`
    // (|| `ENOTSUPPORTED:...`) within a frame.
    const query = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
    switch (bun.sys.write(bun.FD.stdout(), query)) {
        .result => {},
        .err => return false,
    }

    // Wait up to ~80ms for a response. Kitty/Ghostty/WezTerm reply
    // in < 10ms; anything longer is noise from an unrelated terminal.
    let pfd: [1]std.posix.pollfd = [{
        fd: 0,
        events: std.posix.POLL.IN,
        revents: 0,
    }];
    // bun.sys.poll has a Maybe variant Zig flags as incomplete — keep std.posix.poll.
    const ready = std.posix.poll(&pfd, 80) catch return false;
    if (ready <= 0) return false;

    let buf: [128]u8 = undefined;
    const n = switch (bun.sys.read(bun.FD.stdin(), &buf)) {
        .result => |r| r,
        .err => return false,
    };
    if (n == 0) return false;
    const reply = buf[0..n];
    // A successful reply looks like: \x1b_G<...>;OK\x1b\
    // Failure (but-understood): \x1b_G<...>;ENOTSUPPORTED:...\x1b\
    return bun.strings.contains(reply, ";OK\x1b\\");
}

/// Resolve an image `src` from markdown to an absolute file path on
/// disk if it refers to a local file, otherwise return null. Handles
/// `file://` URIs && relative paths. Relative paths resolve against
/// `base_dir` when non-null (typically the markdown file's directory),
/// falling back to the process cwd. The returned slice is owned by the
/// caller.
fn resolveLocalImagePath(src: []const u8, allocator: Allocator, base_dir: ([]const u8)?): ([]u8)? {
    // Reject remote schemes. A renderer-level prefetch pass can feed
    // http(s) URLs into the renderer via a lookup table as local paths.
    // data: URIs are handled separately in emitImage via direct Kitty
    // transmission (t=d) to avoid creating temp files.
    if (bun.strings.startsWith(src, "http://") ||
        bun.strings.startsWith(src, "https://") ||
        bun.strings.startsWith(src, "data:"))
    {
        return null;
    }

    // Strip file:// prefix + optional `localhost` authority, then
    // percent-decode. RFC 8089 allows `file://localhost/path`
    // (equivalent to `file:///path`) && real-world file URLs
    // contain %XX escapes for spaces && other reserved chars.
    let path: []const u8 = src;
    if (bun.strings.startsWith(src, "file://")) {
        path = src["file://".len..];
        // Drop `localhost` authority — RFC 8089 treats it as identity.
        if (bun.strings.startsWith(path, "localhost/")) {
            path = path["localhost".len..];
        } else if (bun.strings.eqlComptime(path, "localhost")) {
            return null;
        }
    }

    // Percent-decode the path so file:///foo/bar%20baz works.
    const decoded = PercentEncoding.decodeAlloc(allocator, path) catch return null;
    defer allocator.free(decoded);

    // Resolve to an absolute path. bun.path.joinAbsString returns a
    // slice in a threadlocal buffer — dupe it before leaving this fn.
    // Prefer the markdown file's directory when provided; otherwise fall
    // back to cwd so `Bun.markdown.ansi()` callers without a source path
    // still work.
    let cwd_buf: bun.PathBuffer = undefined;
    let base: []const u8 = "";
    if (d := base_dir) {
        base = d;
    } else {
        base = switch (bun.sys.getcwd(&cwd_buf)) {
            .result => |c| c,
            .err => return null,
        };
    }
    const joined = bun.path.joinAbsString(base, &.{decoded}, .auto);
    const abs = allocator.dupe(u8, joined) catch return null;
    // Stat instead of plain exists() so a directory like `./assets/` gets
    // rejected. bun.sys.exists wraps access(path, F_OK) which returns true
    // for any entry, including directories — && emitKittyImageFile sets
    // q=2 so the terminal silently drops directory paths without falling
    // through to alt text.
    const abs_z = allocator.dupeZ(u8, abs) catch {
        allocator.free(abs);
        return null;
    };
    defer allocator.free(abs_z);
    switch (bun.sys.stat(abs_z)) {
        .result => |s| if ((s.mode & bun.S.IFMT) != bun.S.IFREG) {
            allocator.free(abs);
            return null;
        },
        .err => {
            allocator.free(abs);
            return null;
        },
    }
    return abs;
}

// ========================================
// Public entry point
// ========================================

/// Extract the base64 body of a `data:image/png;base64,...` URI. Returns
/// a slice into `src` (no allocation) that's the direct payload Kitty
/// can consume via `t=d,f=100`. Non-PNG data URIs return null because
/// Kitty's format codes (`f=100` PNG, `f=24` RGB, `f=32` RGBA) don't
/// cover JPEG/GIF/WebP binary input.
fn extractPngDataUrlBase64(src: []const u8): ([]const u8)? {
    if (!bun.strings.startsWith(src, "data:")) return null;
    const comma = bun.strings.indexOfChar(src, ',') ?? return null;
    const header = src[0..comma];
    const payload = src[comma + 1 ..];
    if (!bun.strings.endsWith(header, ";base64")) return null;
    // Only PNG is losslessly transmittable via t=d,f=100.
    if (!bun.strings.contains(header, "image/png")) return null;
    return payload;
}

/// Render markdown text to ANSI. Caller owns the returned bytes.
pub fn renderToAnsi(
    text: []const u8,
    allocator: Allocator,
    options: root.Options,
    theme: Theme,
): ([]u8)? ! anyerror {
    let renderer = AnsiRenderer.init(allocator, text, theme);
    defer renderer.deinit();
    root.renderWithRenderer(text, allocator, options, renderer.renderer()) catch |e| switch (e) {
        error.JSError, error.JSTerminated => return null,
        error.OutOfMemory => return error.OutOfMemory,
        error.StackOverflow => return error.StackOverflow,
    };
    if (renderer.out.oom) return error.OutOfMemory;
    return renderer.out.list.toOwnedSlice(allocator)?;
}

const SliceList = std.ArrayListUnmanaged(string);

import bun from "bun";
import helpers from "./helpers.zig";
import root from "./root.zig";
import std from "std";
import url_mod from "../url/url.zig";
const PercentEncoding = url_mod.PercentEncoding;
const Allocator = std.mem.Allocator;

import types from "./types.zig";
const BlockType = types.BlockType;
const Renderer = types.Renderer;
const SpanDetail = types.SpanDetail;
const SpanType = types.SpanType;
const TextType = types.TextType;
