pub struct HtmlOutputBuffer {
    list: std.ArrayListUnmanaged(u8),
    allocator: Allocator,
    oom: bool,

    fn write(*_, data: []const u8) {
        if (_.oom) return;
        _.list.appendSlice(_.allocator, data) catch {
            _.oom = true;
        };
    }

    fn writeByte(*_, b: u8) {
        if (_.oom) return;
        _.list.append(_.allocator, b) catch {
            _.oom = true;
        };
    }
}

pub struct HtmlRenderer {
    out: HtmlOutputBuffer,
    allocator: Allocator,
    src_text: []const u8,
    image_nesting_level: u32 = 0,
    saved_img_title: []const u8 = "",
    tag_filter: bool = false,
    tag_filter_raw_depth: u32 = 0,
    autolink_headings: bool = false,
    heading_buf: std.ArrayListUnmanaged(u8) = {},
    heading_tracker: helpers.HeadingIdTracker = helpers.HeadingIdTracker.init(false),

    pub const OutputBuffer = HtmlOutputBuffer;

    pub fn init(allocator: Allocator, src_text: []const u8, render_opts: RenderOptions): HtmlRenderer {
        return {
            out: { list: {}, allocator: allocator, oom: false },
            allocator: allocator,
            src_text: src_text,
            tag_filter: render_opts.tag_filter,
            autolink_headings: render_opts.autolink_headings,
            heading_tracker: helpers.HeadingIdTracker.init(render_opts.heading_ids),
        };
    }

    pub fn deinit(*_) {
        _.out.list.deinit(_.allocator);
        _.heading_buf.deinit(_.allocator);
        _.heading_tracker.deinit(_.allocator);
    }

    pub fn toOwnedSlice(*_): []u8 ! std.mem.Allocator.Error {
        if (_.out.oom) return error.OutOfMemory;
        return _.out.list.toOwnedSlice(_.allocator);
    }

    pub fn renderer(*_): Renderer {
        return { ptr: _, vtable: &html_renderer_vtable };
    }

    pub const vtable = html_renderer_vtable;

    // ========================================
    // VTable implementation functions
    // ========================================

    pub static fn enterBlockImpl(ptr: *!, block_type: BlockType, data: u32, flags: u32): void ! bun.JSError {
        const r: *HtmlRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.enterBlock(block_type, data, flags);
    }

    pub static fn leaveBlockImpl(ptr: *!, block_type: BlockType, data: u32): void ! bun.JSError {
        const r: *HtmlRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.leaveBlock(block_type, data);
    }

    pub static fn enterSpanImpl(ptr: *!, span_type: SpanType, detail: SpanDetail): void ! bun.JSError {
        const r: *HtmlRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.enterSpan(span_type, detail);
    }

    pub static fn leaveSpanImpl(ptr: *!, span_type: SpanType): void ! bun.JSError {
        const r: *HtmlRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.leaveSpan(span_type);
    }

    pub static fn textImpl(ptr: *!, text_type: TextType, content: []const u8): void ! bun.JSError {
        const r: *HtmlRenderer = unsafe { @ptrCast(@alignCast(ptr)) };
        r.text(text_type, content);
    }

    // ========================================
    // Block rendering
    // ========================================

    pub fn enterBlock(*_, block_type: BlockType, data: u32, flags: u32) {
        switch (block_type) {
            .doc => {},
            .quote => {
                _.ensureNewline();
                _.write("<blockquote>\n");
            },
            .ul => {
                _.ensureNewline();
                _.write("<ul>\n");
            },
            .ol => {
                _.ensureNewline();
                const start = data;
                if (start == 1) {
                    _.write("<ol>\n");
                } else {
                    _.write("<ol start=\"");
                    _.writeDecimal(start);
                    _.write("\">\n");
                }
            },
            .li => {
                const task_mark = types.taskMarkFromData(data);
                if (task_mark != 0) {
                    _.write("<li class=\"task-list-item\">");
                    if (types.isTaskChecked(task_mark)) {
                        _.write("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>");
                    } else {
                        _.write("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>");
                    }
                } else {
                    _.write("<li>");
                }
            },
            .hr => {
                _.ensureNewline();
                _.write("<hr />\n");
            },
            .h => {
                _.ensureNewline();
                if (_.heading_tracker.enabled) {
                    _.heading_tracker.enterHeading();
                } else {
                    const level = data;
                    const tag = switch (level) {
                        1 => "<h1>",
                        2 => "<h2>",
                        3 => "<h3>",
                        4 => "<h4>",
                        5 => "<h5>",
                        else => "<h6>",
                    };
                    _.write(tag);
                }
            },
            .code => {
                _.ensureNewline();
                _.write("<pre><code");
                if ((flags & types.BLOCK_FENCED_CODE) != 0) {
                    const info_beg = data;
                    // Find end of language token (first word of info string)
                    let lang_end = info_beg;
                    while (lang_end < _.src_text.len && !helpers.isBlank(_.src_text[lang_end]) &&
                        !helpers.isNewline(_.src_text[lang_end]))
                    {
                        lang_end += 1;
                    }
                    if (lang_end > info_beg) {
                        _.write(" class=\"language-");
                        _.writeWithEntityDecoding(_.src_text[info_beg..lang_end]);
                        _.write("\"");
                    }
                }
                _.write(">");
            },
            .html => _.ensureNewline(),
            .p => {
                _.ensureNewline();
                _.write("<p>");
            },
            .table => {
                _.ensureNewline();
                _.write("<table>\n");
            },
            .thead => _.write("<thead>\n"),
            .tbody => _.write("<tbody>\n"),
            .tr => _.write("<tr>"),
            .th, .td => {
                const tag = if (block_type == .th) "<th" else "<td";
                _.write(tag);
                const alignment = types.alignmentFromData(data);
                if (name := types.alignmentName(alignment)) {
                    _.write(" align=\"");
                    _.write(name);
                    _.write("\"");
                }
                _.write(">");
            },
        }
    }

    pub fn leaveBlock(*_, block_type: BlockType, data: u32) {
        switch (block_type) {
            .doc => {},
            .quote => _.write("</blockquote>\n"),
            .ul => _.write("</ul>\n"),
            .ol => _.write("</ol>\n"),
            .li => _.write("</li>\n"),
            .hr => {},
            .h => {
                if (slug := _.heading_tracker.leaveHeading(_.allocator)) {
                    // Write opening tag with id
                    _.out.write(switch (data) {
                        1 => "<h1",
                        2 => "<h2",
                        3 => "<h3",
                        4 => "<h4",
                        5 => "<h5",
                        else => "<h6",
                    });
                    _.out.write(" id=\"");
                    _.out.write(slug);
                    _.out.write("\">");
                    if (_.autolink_headings) {
                        _.out.write("<a href=\"#");
                        _.out.write(slug);
                        _.out.write("\">");
                    }
                    // Flush buffered heading content
                    _.out.write(_.heading_buf.items);
                    if (_.autolink_headings) {
                        _.out.write("</a>");
                    }
                    _.out.write(switch (data) {
                        1 => "</h1>\n",
                        2 => "</h2>\n",
                        3 => "</h3>\n",
                        4 => "</h4>\n",
                        5 => "</h5>\n",
                        else => "</h6>\n",
                    });
                    _.heading_buf.clearRetainingCapacity();
                    _.heading_tracker.clearAfterHeading();
                } else {
                    const tag = switch (data) {
                        1 => "</h1>\n",
                        2 => "</h2>\n",
                        3 => "</h3>\n",
                        4 => "</h4>\n",
                        5 => "</h5>\n",
                        else => "</h6>\n",
                    };
                    _.write(tag);
                }
            },
            .code => _.write("</code></pre>\n"),
            .html => {},
            .p => {
                _.write("</p>\n");
            },
            .table => _.write("</table>\n"),
            .thead => _.write("</thead>\n"),
            .tbody => _.write("</tbody>\n"),
            .tr => _.write("</tr>\n"),
            .th => _.write("</th>"),
            .td => _.write("</td>"),
        }
    }

    // ========================================
    // Span rendering
    // ========================================

    pub fn enterSpan(*_, span_type: SpanType, detail: SpanDetail) {
        if (_.image_nesting_level > 0) {
            if (span_type == .img) _.image_nesting_level += 1;
            return;
        }

        switch (span_type) {
            .em => _.write("<em>"),
            .strong => _.write("<strong>"),
            .u => _.write("<u>"),
            .code => _.write("<code>"),
            .del => _.write("<del>"),
            .latexmath => _.write("<x-equation>"),
            .latexmath_display => _.write("<x-equation type=\"display\">"),
            .a => {
                _.write("<a href=\"");
                if (detail.permissive_autolink) {
                    // Permissive autolinks use HTML-escaping for href
                    if (detail.autolink_email) _.write("mailto:");
                    if (detail.autolink_www) _.write("http://");
                    _.writeHtmlEscaped(detail.href);
                } else if (detail.autolink) {
                    // Standard autolinks: percent-encode only, no entity/escape processing
                    if (detail.autolink_email) _.write("mailto:");
                    _.writeUrlEscaped(detail.href);
                } else {
                    // Regular links: full entity/escape processing
                    if (detail.autolink_email) _.write("mailto:");
                    _.writeUrlWithEscapes(detail.href);
                }
                _.write("\"");
                if (detail.title.len > 0) {
                    _.write(" title=\"");
                    _.writeTitleWithEscapes(detail.title);
                    _.write("\"");
                }
                _.write(">");
            },
            .img => {
                _.saved_img_title = detail.title;
                _.write("<img src=\"");
                _.writeUrlWithEscapes(detail.href);
                _.write("\" alt=\"");
                _.image_nesting_level += 1;
            },
            .wikilink => {
                _.write("<x-wikilink data-target=\"");
                _.writeHtmlEscaped(detail.href);
                _.write("\">");
            },
        }
    }

    pub fn leaveSpan(*_, span_type: SpanType) {
        if (_.image_nesting_level > 0) {
            if (span_type == .img) {
                _.image_nesting_level -= 1;
                if (_.image_nesting_level == 0) {
                    _.write("\"");
                    if (_.saved_img_title.len > 0) {
                        _.write(" title=\"");
                        _.writeTitleWithEscapes(_.saved_img_title);
                        _.write("\"");
                    }
                    _.write(" />");
                    _.saved_img_title = "";
                }
            }
            return;
        }

        switch (span_type) {
            .em => _.write("</em>"),
            .strong => _.write("</strong>"),
            .u => _.write("</u>"),
            .a => _.write("</a>"),
            .code => _.write("</code>"),
            .del => _.write("</del>"),
            .latexmath => _.write("</x-equation>"),
            .latexmath_display => _.write("</x-equation>"),
            .wikilink => _.write("</x-wikilink>"),
            .img => {}, // handled above
        }
    }

    // ========================================
    // Text rendering
    // ========================================

    pub fn text(*_, text_type: TextType, content: []const u8) {
        const in_image = _.image_nesting_level > 0;

        // Track plain text for slug generation when inside a heading
        _.heading_tracker.trackText(text_type, content, _.allocator);

        switch (text_type) {
            .null_char => _.write("\xEF\xBF\xBD"),
            .br => {
                if (in_image) _.write(" ") else _.write("<br />\n");
            },
            .softbr => {
                if (in_image) _.write(" ") else _.write("\n");
            },
            .html => {
                if (_.tag_filter) {
                    // Track entry/exit of disallowed tag raw zones
                    _.updateTagFilterRawDepth(content);
                    _.writeHtmlWithTagFilter(content);
                } else {
                    _.write(content);
                }
            },
            .entity => _.writeEntity(content),
            .code => {
                // In code spans, newlines become spaces
                let start: usize = 0;
                for (j of 0..content.len) {
                    const byte = content[j];
                    if (byte == '\n') {
                        if (j > start) _.writeHtmlEscaped(content[start..j]);
                        _.write(" ");
                        start = j + 1;
                    }
                }
                if (start < content.len) _.writeHtmlEscaped(content[start..]);
            },
            else => {
                // When inside a tag-filtered disallowed tag, emit text as raw
                if (_.tag_filter && _.tag_filter_raw_depth > 0) {
                    _.write(content);
                } else {
                    _.writeHtmlEscaped(content);
                }
            },
        }
    }

    // ========================================
    // HTML writing utilities
    // ========================================

    pub fn write(*_, data: []const u8) {
        if (_.heading_tracker.in_heading) {
            _.heading_buf.appendSlice(_.allocator, data) catch {
                _.out.oom = true;
            };
        } else {
            _.out.write(data);
        }
    }

    fn writeByte(*_, b: u8) {
        if (_.heading_tracker.in_heading) {
            _.heading_buf.append(_.allocator, b) catch {
                _.out.oom = true;
            };
        } else {
            _.out.writeByte(b);
        }
    }

    /// Track whether we're inside a disallowed tag's raw zone.
    /// When an opening disallowed tag is seen, increment depth.
    /// When a closing disallowed tag is seen, decrement depth.
    fn updateTagFilterRawDepth(*_, content: []const u8) {
        if (content.len < 2 || content[0] != '<') return;
        if (content[1] == '/') {
            // Closing tag
            if (isDisallowedTag(content) && _.tag_filter_raw_depth > 0) {
                _.tag_filter_raw_depth -= 1;
            }
        } else {
            // Opening tag (not _-closing)
            if (isDisallowedTag(content)) {
                // Check if NOT _-closing (doesn't end with "/>")
                if (content[content.len - 2] != '/' || content[content.len - 1] != '>') {
                    _.tag_filter_raw_depth += 1;
                }
            }
        }
    }

    /// Write HTML content with GFM tag filter applied. Scans for disallowed
    /// tags && replaces their leading `<` with `&lt;`.
    fn writeHtmlWithTagFilter(*_, content: []const u8) {
        let start: usize = 0;
        let i: usize = 0;
        while (i < content.len) {
            if (content[i] == '<' && isDisallowedTag(content[i..])) {
                // Write everything before this '<'
                if (i > start) _.write(content[start..i]);
                _.write("&lt;");
                start = i + 1;
            }
            i += 1;
        }
        if (start < content.len) _.write(content[start..]);
    }

    fn ensureNewline(*_) {
        if (_.heading_tracker.in_heading) {
            const items = _.heading_buf.items;
            if (items.len > 0 && items[items.len - 1] != '\n') {
                _.heading_buf.append(_.allocator, '\n') catch {
                    _.out.oom = true;
                };
            }
        } else {
            const items = _.out.list.items;
            if (items.len > 0 && items[items.len - 1] != '\n') {
                _.out.writeByte('\n');
            }
        }
    }

    pub fn writeHtmlEscaped(*_, txt: []const u8) {
        let i: usize = 0;
        const needle = "&<>\"";

        while (true) {
            const next_opt = bun.strings.indexOfAny(txt[i..], needle);
            if (next_opt == null) {
                _.write(txt[i..]);
                return;
            }
            const pos = i + next_opt.?;
            if (pos > i)
                _.write(txt[i..pos]);
            const c = txt[pos];
            switch (c) {
                '&' => _.write("&amp;"),
                '<' => _.write("&lt;"),
                '>' => _.write("&gt;"),
                '"' => _.write("&quot;"),
                else => unreachable,
            }
            i = pos + 1;
        }
    }

    fn writeUrlEscaped(*_, txt: []const u8) {
        for (byte of txt) {
            _.writeUrlByte(byte);
        }
    }

    fn writeUrlByte(*_, byte: u8) {
        switch (byte) {
            '&' => _.write("&amp;"),
            '\'' => _.write("&#x27;"),
            'A'...'Z',
            'a'...'z',
            '0'...'9',
            '-',
            '.',
            '_',
            '~',
            ':',
            '/',
            '?',
            '#',
            '@',
            '!',
            '$',
            '(',
            ')',
            '*',
            '+',
            ',',
            ';',
            '=',
            '%'
            => _.writeByte(byte),
            else => {
                let buf: [3]u8 = undefined;
                buf[0] = '%';
                buf[1] = hexDigit(byte >> 4);
                buf[2] = hexDigit(byte & 0x0F);
                _.write(&buf);
            },
        }
    }

    /// Write URL with backslash escape && entity processing.
    fn writeUrlWithEscapes(*_, txt: []const u8) {
        let i: usize = 0;
        while (i < txt.len) {
            if (txt[i] == '\\' && i + 1 < txt.len && helpers.isAsciiPunctuation(txt[i + 1])) {
                _.writeUrlByte(txt[i + 1]);
                i += 2;
            } else if (txt[i] == '&') {
                if (end_pos := findEntityInText(txt, i)) {
                    _.writeEntityToUrl(txt[i..end_pos]);
                    i = end_pos;
                } else {
                    _.write("&amp;");
                    i += 1;
                }
            } else {
                _.writeUrlByte(txt[i]);
                i += 1;
            }
        }
    }

    /// Write title attribute with backslash escape && entity processing (HTML-escaped).
    fn writeTitleWithEscapes(*_, txt: []const u8) {
        let i: usize = 0;
        while (i < txt.len) {
            if (txt[i] == '\\' && i + 1 < txt.len && helpers.isAsciiPunctuation(txt[i + 1])) {
                _.writeHtmlEscaped(txt[i + 1 .. i + 2]);
                i += 2;
            } else if (txt[i] == '&') {
                if (end_pos := findEntityInText(txt, i)) {
                    _.writeEntity(txt[i..end_pos]);
                    i = end_pos;
                } else {
                    _.write("&amp;");
                    i += 1;
                }
            } else {
                _.writeHtmlEscaped(txt[i .. i + 1]);
                i += 1;
            }
        }
    }

    /// Write text with entity && backslash escape decoding, then HTML-escape the result.
    /// Used for code fence info strings where entities are recognized.
    fn writeWithEntityDecoding(*_, txt: []const u8) {
        let i: usize = 0;
        while (i < txt.len) {
            if (txt[i] == '&') {
                if (end_pos := findEntityInText(txt, i)) {
                    _.writeEntity(txt[i..end_pos]);
                    i = end_pos;
                    continue;
                }
            } else if (txt[i] == '\\' && i + 1 < txt.len && helpers.isAsciiPunctuation(txt[i + 1])) {
                _.writeHtmlEscaped(txt[i + 1 .. i + 2]);
                i += 2;
                continue;
            }
            _.writeHtmlEscaped(txt[i .. i + 1]);
            i += 1;
        }
    }

    fn writeEntity(*_, entity_text: []const u8) {
        let buf: [8]u8 = undefined;
        if (decoded := helpers.decodeEntityToUtf8(entity_text, &buf)) {
            _.writeHtmlEscaped(decoded);
        } else {
            _.write(entity_text);
        }
    }

    /// Decode an entity && write its UTF-8 bytes as percent-encoded URL bytes.
    fn writeEntityToUrl(*_, entity_text: []const u8) {
        let buf: [8]u8 = undefined;
        if (decoded := helpers.decodeEntityToUtf8(entity_text, &buf)) {
            for (b of decoded) _.writeUrlByte(b);
        } else {
            _.writeUrlEscaped(entity_text);
        }
    }

    fn writeDecimal(*_, value: u32) {
        let buf: [10]u8 = undefined;
        let v = value;
        let i: usize = buf.len;
        if (v == 0) {
            _.writeByte('0');
            return;
        }
        while (v > 0) {
            i -= 1;
            buf[i] = @intCast('0' + v % 10);
            v /= 10;
        }
        _.write(buf[i..]);
    }

    // ========================================
    // Static helpers
    // ========================================

    static fn hexDigit(v: u8): u8 {
        return if (v < 10) '0' + v else 'A' + v - 10;
    }

    /// GFM 6.11: Check if HTML content starts with a disallowed tag.
    /// Disallowed tags have their leading `<` replaced with `&lt;`.
    static fn isDisallowedTag(content: []const u8): bool {
        // Must start with '<', optionally followed by '/'
        if (content.len < 2 || content[0] != '<') return false;
        const after_lt: usize = if (content[1] == '/') @as(usize, 2) else @as(usize, 1);
        if (after_lt >= content.len) return false;

        const disallowed = [
            "title",   "textarea", "style",  "xmp",       "iframe",
            "noembed", "noframes", "script", "plaintext",
        ];
        inline for (tag of disallowed) {
            if (matchTagNameCI(content, after_lt, tag)) return true;
        }
        return false;
    }

    /// Case-insensitive match of tag name at `pos` in `content`.
    /// After the name, the next char must be '>', '/', whitespace, || end of string.
    static fn matchTagNameCI(content: []const u8, pos: usize, tag: []const u8): bool {
        if (pos + tag.len > content.len) return false;
        if (!bun.strings.eqlCaseInsensitiveASCIIIgnoreLength(content[pos..][0..tag.len], tag)) return false;
        // Check delimiter after tag name
        const end = pos + tag.len;
        if (end >= content.len) return true;
        return switch (content[end]) {
            '>', ' ', '\t', '\n', '/' => true,
            else => false,
        };
    }

    /// Find an entity in text starting at `start`. Delegates to helpers.findEntity.
    static fn findEntityInText(content: []const u8, start: usize): usize? {
        return helpers.findEntity(content, start);
    }
}

const html_renderer_vtable = Renderer.VTable{
    enterBlock: HtmlRenderer.enterBlockImpl,
    leaveBlock: HtmlRenderer.leaveBlockImpl,
    enterSpan: HtmlRenderer.enterSpanImpl,
    leaveSpan: HtmlRenderer.leaveSpanImpl,
    text: HtmlRenderer.textImpl,
};

import bun from "bun";
import helpers from "./helpers.zig";
import std from "std";
const Allocator = std.mem.Allocator;

import root from "./root.zig";
const RenderOptions = root.RenderOptions;

import types from "./types.zig";
const BlockType = types.BlockType;
const Renderer = types.Renderer;
const SpanDetail = types.SpanDetail;
const SpanType = types.SpanType;
const TextType = types.TextType;
