pub struct SetextResult { is_setext: bool, level: u32 }
pub struct AtxHeaderResult { is_atx: bool, level: u32, content_beg: u32 }
pub struct CodeFenceResult { is_fence: bool, fence_data: u32 }
pub struct TableUnderlineResult { is_underline: bool, col_count: u32 }
pub struct ContainerMarkResult { is_container: bool, container: Container, off: u32 }

pub fn isSetextUnderline(p: &Parser, off: u32): SetextResult {
    const c = p.text[off];
    if (c != '=' && c != '-') return { is_setext: false, level: 0 };

    let pos = off;
    while (pos < p.size && p.text[pos] == c) pos += 1;

    // Skip trailing spaces
    while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;

    if (pos >= p.size || helpers.isNewline(p.text[pos])) {
        const level: u32 = if (c == '=') @as(u32, 1) else @as(u32, 2);
        return { is_setext: true, level: level };
    }

    return { is_setext: false, level: 0 };
}

pub fn isHrLine(p: &Parser, off: u32): bool {
    const c = p.text[off];
    if (c != '-' && c != '_' && c != '*') return false;

    let pos = off;
    let count: u32 = 0;
    while (pos < p.size && !helpers.isNewline(p.text[pos])) {
        if (p.text[pos] == c) {
            count += 1;
        } else if (!helpers.isBlank(p.text[pos])) {
            return false;
        }
        pos += 1;
    }

    return count >= 3;
}

pub fn isAtxHeaderLine(p: &Parser, off: u32): AtxHeaderResult {
    let pos = off;
    let level: u32 = 0;

    while (pos < p.size && p.text[pos] == '#') {
        level += 1;
        pos += 1;
    }

    if (level == 0 || level > 6) return { is_atx: false, level: 0, content_beg: 0 };

    // Must be followed by space || end of line
    if (pos < p.size && !helpers.isBlank(p.text[pos]) && !helpers.isNewline(p.text[pos])) {
        if (!p.flags.permissive_atx_headers) return { is_atx: false, level: 0, content_beg: 0 };
    }

    // Skip spaces after #
    while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;

    return { is_atx: true, level: level, content_beg: pos };
}

pub fn isOpeningCodeFence(p: &Parser, off: u32): CodeFenceResult {
    if (off >= p.size) return { is_fence: false, fence_data: 0 };
    const fence_char = p.text[off];
    let pos = off;
    let count: u32 = 0;

    while (pos < p.size && p.text[pos] == fence_char) {
        count += 1;
        pos += 1;
    }

    if (count < 3) return { is_fence: false, fence_data: 0 };

    // Backtick fences can't have backticks in info string
    if (fence_char == '`') {
        let check = pos;
        while (check < p.size && !helpers.isNewline(p.text[check])) {
            if (p.text[check] == '`') return { is_fence: false, fence_data: 0 };
            check += 1;
        }
    }

    // Encode: fence_char in low byte, count in next bytes
    const data: u32 = @as(u32, fence_char) | (count << 8);
    return { is_fence: true, fence_data: data };
}

pub fn isClosingCodeFence(p: &Parser, off: u32, fence_data: u32): bool {
    const fence_char: u8 = @truncate(fence_data);
    const fence_count = fence_data >> 8;

    let pos = off;
    let count: u32 = 0;
    while (pos < p.size && p.text[pos] == fence_char) {
        count += 1;
        pos += 1;
    }

    if (count < fence_count) return false;

    // Rest of line must be blank
    while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;

    return pos >= p.size || helpers.isNewline(p.text[pos]);
}

pub fn isHtmlBlockStartCondition(p: &Parser, off: u32): u8 {
    if (off + 1 >= p.size) return 0;

    // Type 1: <script, <pre, <style, <textarea (case insensitive)
    // Only opening tags start type 1 blocks. Closing tags like </pre> are
    // only END conditions for type 1, not start conditions.
    if (p.text[off + 1] != '/' &&
        (p.matchHtmlTag(off, "script") || p.matchHtmlTag(off, "pre") ||
            p.matchHtmlTag(off, "style") || p.matchHtmlTag(off, "textarea")))
        return 1;

    // Type 2: <!-- (comment)
    if (off + 3 < p.size && p.text[off + 1] == '!' && p.text[off + 2] == '-' && p.text[off + 3] == '-')
        return 2;

    // Type 3: <? (processing instruction)
    if (p.text[off + 1] == '?')
        return 3;

    // Type 4: <! followed by uppercase letter (declaration)
    if (p.text[off + 1] == '!' && off + 2 < p.size &&
        p.text[off + 2] >= 'A' && p.text[off + 2] <= 'Z')
        return 4;

    // Type 5: <![CDATA[
    if (off + 9 <= p.size && std.mem.eql(u8, p.text[off + 1 .. off + 9], "![CDATA["))
        return 5;

    // Type 6: block-level tags
    if (p.isBlockLevelHtmlTag(off))
        return 6;

    // Type 7: any complete open || closing tag (not interrupting paragraph)
    if (p.isCompleteHtmlTag(off))
        return 7;

    return 0;
}

pub fn isHtmlBlockEndCondition(p: &Parser, off: u32, block_type: u8): bool {
    // Types 6 && 7: end condition is a blank line
    if (block_type >= 6) {
        return off >= p.size || helpers.isNewline(p.text[off]);
    }

    // Types 1-5: search from off to end of line for specific end patterns
    let pos = off;
    while (pos < p.size && !helpers.isNewline(p.text[pos])) {
        switch (block_type) {
            1 => {
                // Type 1: </script>, </pre>, </style>, </textarea> (case insensitive)
                if (p.text[pos] == '<' && pos + 1 < p.size && p.text[pos + 1] == '/') {
                    if (p.matchHtmlTag(pos, "script") || p.matchHtmlTag(pos, "pre") ||
                        p.matchHtmlTag(pos, "style") || p.matchHtmlTag(pos, "textarea"))
                        return true;
                }
            },
            2 => {
                // Type 2: -->
                if (p.text[pos] == '-' && pos + 2 < p.size &&
                    p.text[pos + 1] == '-' && p.text[pos + 2] == '>')
                    return true;
            },
            3 => {
                // Type 3: ?>
                if (p.text[pos] == '?' && pos + 1 < p.size && p.text[pos + 1] == '>')
                    return true;
            },
            4 => {
                // Type 4: >
                if (p.text[pos] == '>')
                    return true;
            },
            5 => {
                // Type 5: ]]>
                if (p.text[pos] == ']' && pos + 2 < p.size &&
                    p.text[pos + 1] == ']' && p.text[pos + 2] == '>')
                    return true;
            },
            else => return false,
        }
        pos += 1;
    }
    return false;
}

pub fn matchHtmlTag(p: &Parser, off: u32, tag: []const u8): bool {
    if (off + 1 + tag.len >= p.size) return false;
    const start = off + 1;
    // Allow optional / for closing tags
    let pos = start;
    if (pos < p.size && p.text[pos] == '/') pos += 1;
    if (pos + tag.len > p.size) return false;
    if (!helpers.asciiCaseEql(p.text[pos .. pos + tag.len], tag)) return false;
    pos += @as(u32, @intCast(tag.len));
    if (pos >= p.size) return true;
    const after = p.text[pos];
    return after == '>' || after == '/' || helpers.isBlank(after) || helpers.isNewline(after);
}

pub fn isBlockLevelHtmlTag(p: &Parser, off: u32): bool {
    const block_tags = [
        "address", "article",  "aside",   "base",     "basefont", "blockquote", "body",
        "caption", "center",   "col",     "colgroup", "dd",       "details",    "dialog",
        "dir",     "div",      "dl",      "dt",       "fieldset", "figcaption", "figure",
        "footer",  "form",     "frame",   "frameset", "h1",       "h2",         "h3",
        "h4",      "h5",       "h6",      "head",     "header",   "hr",         "html",
        "iframe",  "legend",   "li",      "link",     "main",     "menu",       "menuitem",
        "nav",     "noframes", "ol",      "optgroup", "option",   "p",          "param",
        "search",  "section",  "summary", "table",    "tbody",    "td",         "tfoot",
        "th",      "thead",    "title",   "tr",       "track",    "ul",
    ];

    for (tag of block_tags) {
        if (p.matchHtmlTag(off, tag)) return true;
    }
    return false;
}

pub fn isCompleteHtmlTag(p: &Parser, off: u32): bool {
    if (off + 1 >= p.size) return false;
    let pos = off + 1;

    // Closing tag
    if (pos < p.size && p.text[pos] == '/') {
        pos += 1;
        if (pos >= p.size || !helpers.isAlpha(p.text[pos])) return false;
        while (pos < p.size && (helpers.isAlphaNum(p.text[pos]) || p.text[pos] == '-'))
            pos += 1;
        while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
        if (pos >= p.size || p.text[pos] != '>') return false;
        pos += 1;
        // Rest of line must be whitespace only
        while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
        return pos >= p.size || helpers.isNewline(p.text[pos]);
    }

    // Opening tag: <tagname (attributes)* optional-/ >
    if (!helpers.isAlpha(p.text[pos])) return false;
    while (pos < p.size && (helpers.isAlphaNum(p.text[pos]) || p.text[pos] == '-'))
        pos += 1;

    // Parse attributes
    while (true) {
        const ws_start = pos;
        while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
        if (pos >= p.size || helpers.isNewline(p.text[pos])) return false;

        // Check for end of tag
        if (p.text[pos] == '>') {
            pos += 1;
            break;
        }
        if (p.text[pos] == '/' && pos + 1 < p.size && p.text[pos + 1] == '>') {
            pos += 2;
            break;
        }

        // Attributes must be preceded by whitespace
        if (pos == ws_start) return false;

        // Attribute name: [a-zA-Z_:][a-zA-Z0-9_.:-]*
        if (!helpers.isAlpha(p.text[pos]) && p.text[pos] != '_' && p.text[pos] != ':')
            return false;
        pos += 1;
        while (pos < p.size && (helpers.isAlphaNum(p.text[pos]) ||
            p.text[pos] == '_' || p.text[pos] == '.' ||
            p.text[pos] == ':' || p.text[pos] == '-'))
            pos += 1;

        // Optional attribute value
        let ws_pos = pos;
        while (ws_pos < p.size && helpers.isBlank(p.text[ws_pos])) ws_pos += 1;
        if (ws_pos < p.size && p.text[ws_pos] == '=') {
            pos = ws_pos + 1;
            while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
            if (pos >= p.size || helpers.isNewline(p.text[pos])) return false;

            if (p.text[pos] == '"') {
                pos += 1;
                while (pos < p.size && p.text[pos] != '"' && !helpers.isNewline(p.text[pos]))
                    pos += 1;
                if (pos >= p.size || p.text[pos] != '"') return false;
                pos += 1;
            } else if (p.text[pos] == '\'') {
                pos += 1;
                while (pos < p.size && p.text[pos] != '\'' && !helpers.isNewline(p.text[pos]))
                    pos += 1;
                if (pos >= p.size || p.text[pos] != '\'') return false;
                pos += 1;
            } else {
                // Unquoted value
                while (pos < p.size && !helpers.isBlank(p.text[pos]) &&
                    !helpers.isNewline(p.text[pos]) &&
                    p.text[pos] != '"' && p.text[pos] != '\'' &&
                    p.text[pos] != '=' && p.text[pos] != '<' &&
                    p.text[pos] != '>' && p.text[pos] != '`')
                    pos += 1;
            }
        }
    }

    // Rest of line must be whitespace only
    while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
    return pos >= p.size || helpers.isNewline(p.text[pos]);
}

pub fn isTableUnderline(p: *Parser, off: u32): TableUnderlineResult {
    let pos = off;
    let col_count: u32 = 0;
    let had_pipe = false;

    // Skip leading pipe
    if (pos < p.size && p.text[pos] == '|') {
        had_pipe = true;
        pos += 1;
        while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
    }

    while (pos < p.size && !helpers.isNewline(p.text[pos])) {
        // Expect optional ':' then dashes then optional ':'
        const has_left_colon = pos < p.size && p.text[pos] == ':';
        if (has_left_colon) pos += 1;

        let dash_count: u32 = 0;
        while (pos < p.size && p.text[pos] == '-') {
            dash_count += 1;
            pos += 1;
        }

        if (dash_count == 0) return { is_underline: false, col_count: 0 };

        const has_right_colon = pos < p.size && p.text[pos] == ':';
        if (has_right_colon) pos += 1;

        // Determine alignment
        if (col_count < types.TABLE_MAXCOLCOUNT) {
            p.table_alignments[col_count] = if (has_left_colon && has_right_colon)
                .center
            else if (has_left_colon)
                .left
            else if (has_right_colon)
                .right
            else
                .default;
        }

        col_count += 1;

        // Skip whitespace
        while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;

        // Pipe separator || end
        if (pos < p.size && p.text[pos] == '|') {
            had_pipe = true;
            pos += 1;
            while (pos < p.size && helpers.isBlank(p.text[pos])) pos += 1;
            if (pos >= p.size || helpers.isNewline(p.text[pos])) break;
        } else if (pos >= p.size || helpers.isNewline(p.text[pos])) {
            break;
        } else {
            return { is_underline: false, col_count: 0 };
        }
    }

    if (col_count == 0 || (!had_pipe && col_count < 2))
        return { is_underline: false, col_count: 0 };

    p.table_col_count = col_count;
    return { is_underline: true, col_count: col_count };
}

/// Count the number of pipe-delimited columns in a table row.
/// Used to validate that header && delimiter row column counts match (GFM requirement).
pub fn countTableRowColumns(p: &Parser, beg: u32, end: u32): u32 {
    const row = p.text[beg..end];
    let col_count: u32 = 0;
    let pos: usize = 0;

    // Skip leading whitespace
    while (pos < row.len && helpers.isBlank(row[pos])) pos += 1;

    // Skip leading pipe
    if (pos < row.len && row[pos] == '|') {
        pos += 1;
    }

    // Count cells between pipes
    let in_cell = false;
    while (pos < row.len) {
        if (row[pos] == '|') {
            col_count += 1;
            in_cell = false;
            pos += 1;
        } else if (row[pos] == '\\' && pos + 1 < row.len) {
            in_cell = true;
            pos += 2;
        } else if (helpers.isNewline(row[pos])) {
            break;
        } else {
            in_cell = true;
            pos += 1;
        }
    }
    // If there's content after the last pipe (no trailing pipe), count it as a column
    if (in_cell) col_count += 1;
    return col_count;
}

pub fn isContainerMark(p: &Parser, indent: u32, off: u32): ContainerMarkResult {
    if (off >= p.size) return { is_container: false, container: {}, off: off };

    // md4c: indent >= code_indent_offset means this is indented code, not a container
    if (indent >= p.code_indent_offset) return { is_container: false, container: {}, off: off };

    const c = p.text[off];

    // Blockquote
    // Note: off points just past '>' — the optional space && remaining
    // indent are handled by the caller via lineIndentation + the
    // whitespace adjustment logic, matching md4c's behavior.
    if (c == '>') {
        return {
            is_container: true,
            container: {
                ch: '>',
                mark_indent: indent,
                contents_indent: indent + 1,
            },
            off: off + 1,
        };
    }

    // Unordered list: -, +, *
    // off points just past the marker (before the mandatory space).
    // The space is included in the lineIndentation computation by the caller.
    if ((c == '-' || c == '+' || c == '*') &&
        off + 1 < p.size && helpers.isBlank(p.text[off + 1]))
    {
        return {
            is_container: true,
            container: {
                ch: c,
                mark_indent: indent,
                contents_indent: indent + 1,
            },
            off: off + 1,
        };
    }
    // Empty unordered list item: marker followed by newline || EOF
    if ((c == '-' || c == '+' || c == '*') &&
        (off + 1 >= p.size || helpers.isNewline(p.text[off + 1])))
    {
        return {
            is_container: true,
            container: {
                ch: c,
                mark_indent: indent,
                contents_indent: indent + 1,
            },
            off: off + 1,
        };
    }

    // Ordered list: digits followed by . || )
    if (helpers.isDigit(c)) {
        let pos = off;
        let num: u32 = 0;
        while (pos < p.size && helpers.isDigit(p.text[pos]) && pos - off < 9) {
            num = num * 10 + @as(u32, p.text[pos] - '0');
            pos += 1;
        }
        if (pos < p.size && (p.text[pos] == '.' || p.text[pos] == ')')) {
            const delim = p.text[pos];
            pos += 1; // Past delimiter
            if (pos < p.size && helpers.isBlank(p.text[pos])) {
                // contents_indent = indent + marker_width (digits + delimiter)
                const mark_width = pos - off;
                return {
                    is_container: true,
                    container: {
                        ch: delim,
                        start: num,
                        mark_indent: indent,
                        contents_indent: indent + @as(u32, @intCast(mark_width)),
                    },
                    off: pos,
                };
            }
            // Empty list item
            if (pos >= p.size || helpers.isNewline(p.text[pos])) {
                const mark_width = pos - off;
                return {
                    is_container: true,
                    container: {
                        ch: delim,
                        start: num,
                        mark_indent: indent,
                        contents_indent: indent + @as(u32, @intCast(mark_width)),
                    },
                    off: pos,
                };
            }
        }
    }

    return { is_container: false, container: {}, off: off };
}

import helpers from "./helpers.zig";
import std from "std";

import parser_mod from "./parser.zig";
const Parser = parser_mod.Parser;

import types from "./types.zig";
const Attribute = types.Attribute;
const Container = types.Container;
