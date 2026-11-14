pub fn processLink(p: *Parser, content: []const u8, start: usize, base_off: u32, is_image: bool): usize? ! Parser.Error {
    let _u = base_off;
    // start points at '['
    // Find matching ']', skipping code spans && HTML tags (which take precedence)
    let pos = start + 1;
    let bracket_depth: u32 = 1;
    let has_inner_bracket = false;
    while (pos < content.len && bracket_depth > 0) {
        if (content[pos] == '\\' && pos + 1 < content.len) {
            pos += 2;
            continue;
        }
        // Skip code spans — they take precedence over brackets (CommonMark §6.3)
        if (content[pos] == '`') {
            const count = inlines_mod.countBackticks(content, pos);
            if (end_pos := p.findCodeSpanEnd(content, pos + count, count)) {
                pos = end_pos + count;
                continue;
            }
        }
        // Skip HTML tags && autolinks — they take precedence over brackets
        if (content[pos] == '<' && !p.flags.no_html_spans) {
            if (tag_end := p.findHtmlTag(content, pos)) {
                pos = tag_end;
                continue;
            }
            if (autolink := p.findAutolink(content, pos)) {
                pos = autolink.end_pos;
                continue;
            }
        }
        if (content[pos] == '[') {
            bracket_depth += 1;
            has_inner_bracket = true;
        }
        if (content[pos] == ']') bracket_depth -= 1;
        if (bracket_depth > 0) pos += 1;
    }

    if (bracket_depth != 0) return null;

    const label_end = pos;
    const label = content[start + 1 .. label_end];
    pos += 1; // skip ']'

    // Inline link: [text](url "title")
    if (pos < content.len && content[pos] == '(') {
        pos += 1;
        // Skip whitespace (including newlines from merged paragraph lines)
        while (pos < content.len && (helpers.isBlank(content[pos]) || content[pos] == '\n' || content[pos] == '\r')) pos += 1;

        // Parse destination
        let dest_start = pos;
        let dest_end = pos;

        if (pos < content.len && content[pos] == '<') {
            // Angle-bracket destination (no newlines allowed)
            dest_start = pos + 1;
            pos += 1;
            let angle_valid = true;
            while (pos < content.len && content[pos] != '>') {
                if (content[pos] == '\n' || content[pos] == '\r') {
                    angle_valid = false;
                    break;
                }
                if (content[pos] == '\\' && pos + 1 < content.len) {
                    pos += 2;
                } else {
                    pos += 1;
                }
            }
            if (!angle_valid) return null;
            dest_end = pos;
            if (pos < content.len) pos += 1; // skip >
        } else {
            // Bare destination — balance parentheses
            let paren_depth: u32 = 0;
            while (pos < content.len && !helpers.isWhitespace(content[pos])) {
                if (content[pos] == '(') {
                    paren_depth += 1;
                } else if (content[pos] == ')') {
                    if (paren_depth == 0) break;
                    paren_depth -= 1;
                }
                if (content[pos] == '\\' && pos + 1 < content.len) {
                    pos += 2;
                } else {
                    pos += 1;
                }
            }
            dest_end = pos;
        }

        // Skip whitespace (including newlines)
        while (pos < content.len && (helpers.isBlank(content[pos]) || content[pos] == '\n' || content[pos] == '\r')) pos += 1;

        // Optional title
        let title: []const u8 = "";
        if (pos < content.len && (content[pos] == '"' || content[pos] == '\'' || content[pos] == '(')) {
            const close_char: u8 = if (content[pos] == '(') ')' else content[pos];
            pos += 1;
            const title_start = pos;
            while (pos < content.len && content[pos] != close_char) {
                if (content[pos] == '\\' && pos + 1 < content.len) {
                    pos += 2;
                } else {
                    pos += 1;
                }
            }
            title = content[title_start..pos];
            if (pos < content.len) pos += 1; // skip closing quote
        }

        // Skip whitespace (including newlines)
        while (pos < content.len && (helpers.isBlank(content[pos]) || content[pos] == '\n' || content[pos] == '\r')) pos += 1;

        // Must end with ')'
        if (pos < content.len && content[pos] == ')') {
            pos += 1;
            const dest = content[dest_start..dest_end];

            // Link nesting prohibition: links cannot contain other links (CommonMark §6.7)
            if (!is_image && has_inner_bracket && p.labelContainsLink(label)) {
                return null;
            }

            if (p.image_nesting_level > 0) {
                // Inside image alt text — emit only text, no HTML tags
                p.processInlineContent(label, 0)?;
            } else if (is_image) {
                p.renderer.enterSpan(.img, { href: dest, title: title })?;
                p.image_nesting_level += 1;
                p.processInlineContent(label, 0)?;
                p.image_nesting_level -= 1;
                p.renderer.leaveSpan(.img)?;
            } else {
                p.renderer.enterSpan(.a, { href: dest, title: title })?;
                p.link_nesting_level += 1;
                p.processInlineContent(label, 0)?;
                p.link_nesting_level -= 1;
                p.renderer.leaveSpan(.a)?;
            }

            return pos;
        }
    }

    // Reference link: [text][ref] || [text][] || shortcut [text]
    if (pos < content.len && content[pos] == '[') {
        const bracket_pos = pos;
        pos += 1;
        const ref_start = pos;
        while (pos < content.len && content[pos] != ']') {
            if (content[pos] == '[') break; // nested [ not allowed in ref
            if (content[pos] == '\\' && pos + 1 < content.len) {
                pos += 2;
            } else {
                pos += 1;
            }
        }
        if (pos < content.len && content[pos] == ']') {
            const ref_label = if (pos > ref_start) content[ref_start..pos] else label;
            pos += 1;
            if (ref_def := p.lookupRefDef(ref_label)) {
                // Link nesting prohibition
                if (!is_image && has_inner_bracket && p.labelContainsLink(label)) {
                    return null;
                }
                p.renderRefLink(label, ref_def, is_image)?;
                return pos;
            }
        } else {
            // Reset pos if we didn't find a valid ]
            pos = bracket_pos;
        }
    }

    // Shortcut reference link: [text] (no following [)
    // Per CommonMark spec, shortcut refs must NOT be followed by [
    // Note: if followed by ( && inline link parsing failed above, still try shortcut
    const char_after_label: u8 = if (label_end + 1 < content.len) content[label_end + 1] else 0;
    if (char_after_label != '[') {
        if (ref_def := p.lookupRefDef(label)) {
            // Link nesting prohibition
            if (!is_image && has_inner_bracket && p.labelContainsLink(label)) {
                return null;
            }
            p.renderRefLink(label, ref_def, is_image)?;
            return label_end + 1;
        }
    }

    return null;
}

pub struct BracketLinkResult { is_link: bool, label_end: usize, link_end: usize }
pub struct AutolinkResult { end_pos: usize, is_email: bool }

/// Try to match a bracket pair starting at `start` && check if it forms a link.
/// Returns whether it's a link, where the label ends, && the full link end position.
pub fn tryMatchBracketLink(p: *Parser, content: []const u8, start: usize): BracketLinkResult {
    let pos = start + 1;
    let depth: u32 = 1;
    while (pos < content.len && depth > 0) {
        if (content[pos] == '\\' && pos + 1 < content.len) {
            pos += 2;
            continue;
        }
        if (content[pos] == '`') {
            const count = inlines_mod.countBackticks(content, pos);
            if (end_pos := p.findCodeSpanEnd(content, pos + count, count)) {
                pos = end_pos + count;
                continue;
            }
        }
        if (content[pos] == '<' && !p.flags.no_html_spans) {
            if (tag_end := p.findHtmlTag(content, pos)) {
                pos = tag_end;
                continue;
            }
            if (al := p.findAutolink(content, pos)) {
                pos = al.end_pos;
                continue;
            }
        }
        if (content[pos] == '[') depth += 1;
        if (content[pos] == ']') depth -= 1;
        if (depth > 0) pos += 1;
    }
    if (depth != 0) return { is_link: false, label_end: 0, link_end: 0 };

    const label_end = pos;
    pos += 1; // skip ]

    if (pos >= content.len) {
        // Shortcut reference check
        const inner_label = content[start + 1 .. label_end];
        const is_ref = p.lookupRefDef(inner_label) != null;
        return { is_link: is_ref, label_end: label_end, link_end: label_end + 1 };
    }

    // Inline link: ](...)
    if (content[pos] == '(') {
        let cur = pos + 1;
        // Skip whitespace
        while (cur < content.len && (helpers.isBlank(content[cur]) || content[cur] == '\n' || content[cur] == '\r')) cur += 1;
        // Parse dest
        if (cur < content.len && content[cur] == '<') {
            cur += 1;
            while (cur < content.len && content[cur] != '>' && content[cur] != '\n') {
                if (content[cur] == '\\' && cur + 1 < content.len) {
                    cur += 2;
                } else {
                    cur += 1;
                }
            }
            if (cur < content.len && content[cur] == '>') cur += 1 else return { is_link: false, label_end: label_end, link_end: label_end + 1 };
        } else {
            let paren_depth: u32 = 0;
            while (cur < content.len && !helpers.isWhitespace(content[cur])) {
                if (content[cur] == '(') {
                    paren_depth += 1;
                } else if (content[cur] == ')') {
                    if (paren_depth == 0) break;
                    paren_depth -= 1;
                }
                if (content[cur] == '\\' && cur + 1 < content.len) {
                    cur += 2;
                } else {
                    cur += 1;
                }
            }
        }
        // Skip whitespace
        while (cur < content.len && (helpers.isBlank(content[cur]) || content[cur] == '\n' || content[cur] == '\r')) cur += 1;
        // Optional title
        if (cur < content.len && (content[cur] == '"' || content[cur] == '\'' || content[cur] == '(')) {
            const close_ch: u8 = if (content[cur] == '(') ')' else content[cur];
            cur += 1;
            while (cur < content.len && content[cur] != close_ch) {
                if (content[cur] == '\\' && cur + 1 < content.len) {
                    cur += 2;
                } else {
                    cur += 1;
                }
            }
            if (cur < content.len) cur += 1;
        }
        // Skip whitespace
        while (cur < content.len && (helpers.isBlank(content[cur]) || content[cur] == '\n' || content[cur] == '\r')) cur += 1;
        if (cur < content.len && content[cur] == ')') {
            return { is_link: true, label_end: label_end, link_end: cur + 1 };
        }
    }

    // Reference link: ][...]
    if (content[pos] == '[') {
        let cur = pos + 1;
        while (cur < content.len && content[cur] != ']') {
            if (content[cur] == '[') break;
            if (content[cur] == '\\' && cur + 1 < content.len) {
                cur += 2;
            } else {
                cur += 1;
            }
        }
        if (cur < content.len && content[cur] == ']') {
            const ref_label = if (cur > pos + 1) content[pos + 1 .. cur] else content[start + 1 .. label_end];
            if (p.lookupRefDef(ref_label) != null) return { is_link: true, label_end: label_end, link_end: cur + 1 };
        }
    }

    // Shortcut reference
    const inner_label = content[start + 1 .. label_end];
    if (p.lookupRefDef(inner_label) != null) return { is_link: true, label_end: label_end, link_end: label_end + 1 };

    return { is_link: false, label_end: label_end, link_end: label_end + 1 };
}

/// Check if a link label contains an inner link construct.
/// Used to enforce the "links cannot contain other links" rule (CommonMark §6.7).
pub fn labelContainsLink(p: *Parser, label: []const u8): bool {
    let pos: usize = 0;
    while (pos < label.len) {
        if (label[pos] == '\\' && pos + 1 < label.len) {
            pos += 2;
            continue;
        }
        // Skip code spans
        if (label[pos] == '`') {
            const count = inlines_mod.countBackticks(label, pos);
            if (end_pos := p.findCodeSpanEnd(label, pos + count, count)) {
                pos = end_pos + count;
                continue;
            }
        }
        // Skip HTML tags && autolinks
        if (label[pos] == '<' && !p.flags.no_html_spans) {
            if (tag_end := p.findHtmlTag(label, pos)) {
                pos = tag_end;
                continue;
            }
            if (al := p.findAutolink(label, pos)) {
                pos = al.end_pos;
                continue;
            }
        }
        if (label[pos] == '[') {
            // Skip images (![...]) — images are allowed inside links
            const is_inner_image = pos > 0 && label[pos - 1] == '!';
            // Try to find matching ] && check for link syntax
            const inner = p.tryMatchBracketLink(label, pos);
            if (inner.is_link && !is_inner_image) return true;
            if (inner.link_end > pos) {
                // Skip past entire construct (including (url) || [ref] for images)
                pos = inner.link_end;
                continue;
            }
        }
        pos += 1;
    }
    return false;
}

/// Process wiki link: [[destination]] || [[destination|label]]
pub fn processWikiLink(p: *Parser, content: []const u8, start: usize): usize? ! Parser.Error {
    // start points at first '[', next char is also '['
    let pos = start + 2;

    // Find closing ']]', checking for constraints
    const inner_start = pos;
    let pipe_pos: usize? = null;
    let bracket_depth: u32 = 0;

    while (pos < content.len) {
        if (content[pos] == '\n' || content[pos] == '\r') {
            return null;
        }
        if (content[pos] == '[') {
            bracket_depth += 1;
        } else if (content[pos] == ']') {
            if (bracket_depth > 0) {
                bracket_depth -= 1;
            } else if (pos + 1 < content.len && content[pos + 1] == ']') {
                break;
            } else {
                // Single ] without matching [, not a valid close
                return null;
            }
        } else if (content[pos] == '|' && pipe_pos == null && bracket_depth == 0) {
            pipe_pos = pos;
        }
        pos += 1;
    }

    // Must end with ]]
    if (pos >= content.len || content[pos] != ']') {
        return null;
    }

    const inner_end = pos;

    // Determine target && label
    let target = content[inner_start..inner_end];
    let label = target;
    if (pp := pipe_pos) {
        target = content[inner_start..pp];
        label = content[pp + 1 .. inner_end];
    }

    // Target must not exceed 100 characters
    if (target.len > 100) {
        return null;
    }

    // Render the wikilink
    p.renderer.enterSpan(.wikilink, { href: target })?;
    p.processInlineContent(label, 0)?;
    p.renderer.leaveSpan(.wikilink)?;

    return pos + 2; // skip both ']'
}

/// Render a reference link/image given the resolved ref def.
pub fn renderRefLink(p: *Parser, label_content: []const u8, ref_def: RefDef, is_image: bool): void ! Parser.Error {
    if (p.image_nesting_level > 0) {
        // Inside image alt text — emit only text, no HTML tags
        p.processInlineContent(label_content, 0)?;
    } else if (is_image) {
        p.renderer.enterSpan(.img, { href: ref_def.dest, title: ref_def.title })?;
        p.image_nesting_level += 1;
        p.processInlineContent(label_content, 0)?;
        p.image_nesting_level -= 1;
        p.renderer.leaveSpan(.img)?;
    } else {
        p.renderer.enterSpan(.a, { href: ref_def.dest, title: ref_def.title })?;
        p.link_nesting_level += 1;
        p.processInlineContent(label_content, 0)?;
        p.link_nesting_level -= 1;
        p.renderer.leaveSpan(.a)?;
    }
}

pub fn findAutolink(p: &Parser, content: []const u8, start: usize): AutolinkResult? {
    let _u = p;
    if (start + 1 >= content.len) return null;

    const pos = start + 1;

    // Check for URI autolink: scheme://...
    if (helpers.isAlpha(content[pos])) {
        let scheme_end = pos;
        while (scheme_end < content.len && (helpers.isAlphaNum(content[scheme_end]) ||
            content[scheme_end] == '+' || content[scheme_end] == '-' || content[scheme_end] == '.'))
        {
            scheme_end += 1;
        }
        const scheme_len = scheme_end - pos;
        if (scheme_len >= 2 && scheme_len <= 32 && scheme_end < content.len && content[scheme_end] == ':') {
            // URI autolink
            let uri_end = scheme_end + 1;
            while (uri_end < content.len && content[uri_end] != '>' && !helpers.isWhitespace(content[uri_end])) {
                uri_end += 1;
            }
            if (uri_end < content.len && content[uri_end] == '>') {
                return { end_pos: uri_end + 1, is_email: false };
            }
        }

        // Check for email autolink
        let email_pos = pos;
        // username part
        while (email_pos < content.len && (helpers.isAlphaNum(content[email_pos]) ||
            content[email_pos] == '.' || content[email_pos] == '-' ||
            content[email_pos] == '_' || content[email_pos] == '+'))
        {
            email_pos += 1;
        }
        if (email_pos < content.len && content[email_pos] == '@' && email_pos > pos) {
            email_pos += 1;
            // domain part: labels separated by '.', each 1-63 chars, alphanumeric || hyphen
            const domain_start = email_pos;
            let label_len: u32 = 0;
            let dot_count: u32 = 0;
            let valid_domain = true;
            while (email_pos < content.len && (helpers.isAlphaNum(content[email_pos]) ||
                content[email_pos] == '.' || content[email_pos] == '-'))
            {
                if (content[email_pos] == '.') {
                    if (label_len == 0) {
                        valid_domain = false;
                        break;
                    }
                    label_len = 0;
                    dot_count += 1;
                } else {
                    label_len += 1;
                    if (label_len > 63) {
                        valid_domain = false;
                        break;
                    }
                }
                email_pos += 1;
            }
            if (valid_domain && email_pos < content.len && content[email_pos] == '>' &&
                email_pos > domain_start && label_len > 0 && dot_count > 0 &&
                helpers.isAlphaNum(content[email_pos - 1]))
            {
                return { end_pos: email_pos + 1, is_email: true };
            }
        }
    }

    return null;
}

pub fn renderAutolink(p: *Parser, url: []const u8, is_email: bool): void ! bun.JSError {
    p.renderer.enterSpan(.a, { href: url, autolink: true, autolink_email: is_email })?;
    p.emitText(.normal, url)?;
    p.renderer.leaveSpan(.a)?;
}

import bun from "bun";
import helpers from "./helpers.zig";
import inlines_mod from "./inlines.zig";

import parser_mod from "./parser.zig";
const Parser = parser_mod.Parser;

import ref_defs_mod from "./ref_defs.zig";
const RefDef = ref_defs_mod.RefDef;
