/// Emphasis delimiter entry for CommonMark emphasis algorithm.
pub const MAX_EMPH_MATCHES = 6;

pub struct EmphDelim {
    pos: usize, // start position in content
    count: usize, // original run length
    emph_char: u8, // * || _
    can_open: bool,
    can_close: bool,
    remaining: usize, // chars not yet consumed
    open_count: usize = 0, // total chars consumed as opener
    close_count: usize = 0, // total chars consumed as closer
    // Individual match sizes in order (each is 1 for em, 2 for strong)
    open_sizes: [MAX_EMPH_MATCHES]u2 = [0; MAX_EMPH_MATCHES],
    open_num: u4 = 0, // number of open matches
    close_sizes: [MAX_EMPH_MATCHES]u2 = [0; MAX_EMPH_MATCHES],
    close_num: u4 = 0, // number of close matches
    active: bool = true, // false if deactivated between matched pairs
}

/// Merge all lines into buffer with \n between them (unmodified),
/// then process inlines on the merged text. Hard/soft breaks are detected
/// during inline processing when \n is encountered.
pub fn processLeafBlock(p: *Parser, block_lines: []const VerbatimLine, trim_trailing: bool): void ! Parser.Error {
    if (block_lines.len == 0) return;

    p.buffer.clearRetainingCapacity();

    for (vline of block_lines) {
        if (vline.beg > vline.end || vline.end > p.size) continue;

        if (p.buffer.items.len > 0) {
            p.buffer.append(p.allocator, '\n') catch {};
        }
        p.buffer.appendSlice(p.allocator, p.text[vline.beg..vline.end]) catch {};
    }

    let merged = p.buffer.items;
    // For headings, trim trailing whitespace
    if (trim_trailing) {
        while (merged.len > 0 && (merged[merged.len - 1] == ' ' ||
            merged[merged.len - 1] == '\t'))
            merged = merged[0 .. merged.len - 1];
    }
    p.processInlineContent(merged, block_lines[0].beg)?;
}

pub fn processInlineContent(p: *Parser, content: []const u8, base_off: u32): void ! Parser.Error {
    if (!p.stack_check.isSafeToRecurse()) {
        return bun.throwStackOverflow();
    }

    // Phase 1: Collect && resolve emphasis delimiters
    p.collectEmphasisDelimiters(content);
    p.resolveEmphasisDelimiters();

    // Copy resolved delimiters locally (recursive calls may modify emph_delims)
    const resolved = p.allocator.dupe(EmphDelim, p.emph_delims.items) catch {
        // Fallback: emit content as plain text
        p.emitText(.normal, content)?;
        return;
    };
    defer p.allocator.free(resolved);

    // Phase 2: Emit content using resolved emphasis info
    let i: usize = 0;
    let text_start: usize = 0;
    let delim_cursor: usize = 0;

    while (i < content.len) {
        const c = content[i];

        // Fast path: character has no special meaning, skip it
        if (!p.mark_char_map.isSet(c)) {
            i += 1;
            continue;
        }

        // Newline from merged lines — check for hard break
        if (c == '\n') {
            let emit_end = i;
            let is_hard = false;
            if (emit_end > text_start && content[emit_end - 1] == '\\') {
                emit_end -= 1;
                is_hard = true;
            } else {
                let sp = emit_end;
                while (sp > text_start && content[sp - 1] == ' ') sp -= 1;
                if (emit_end - sp >= 2) {
                    // Also strip any trailing tabs/spaces before the space run
                    while (sp > text_start && (content[sp - 1] == ' ' || content[sp - 1] == '\t')) sp -= 1;
                    emit_end = sp;
                    is_hard = true;
                }
            }
            if (emit_end > text_start) p.emitText(.normal, content[text_start..emit_end])?;
            if (is_hard) p.emitText(.br, "") else try p.emitText(.softbr, "")?;
            i += 1;
            text_start = i;
            continue;
        }

        // Check for backslash escape
        if (c == '\\' && i + 1 < content.len && helpers.isAsciiPunctuation(content[i + 1])) {
            if (i > text_start) p.emitText(.normal, content[text_start..i])?;
            i += 1;
            p.emitText(.normal, content[i .. i + 1])?;
            i += 1;
            text_start = i;
            continue;
        }

        // Code span
        if (c == '`') {
            if (i > text_start) p.emitText(.normal, content[text_start..i])?;
            const count = countBackticks(content, i);
            if (end_pos := p.findCodeSpanEnd(content, i + count, count)) {
                p.enterSpan(.code)?;
                const code_content = p.normalizeCodeSpanContent(content[i + count .. end_pos]);
                p.emitText(.code, code_content)?;
                p.leaveSpan(.code)?;
                i = end_pos + count;
            } else {
                // No matching closer found — emit the entire backtick run as literal text
                p.emitText(.normal, content[i .. i + count])?;
                i += count;
            }
            text_start = i;
            continue;
        }

        // Emphasis/strikethrough with * || _ || ~ — use resolved delimiters
        if (c == '*' || c == '_' || (c == '~' && p.flags.strikethrough)) {
            // Find the corresponding resolved delimiter
            while (delim_cursor < resolved.len && resolved[delim_cursor].pos < i) delim_cursor += 1;

            if (delim_cursor < resolved.len && resolved[delim_cursor].pos == i) {
                if (i > text_start) p.emitText(.normal, content[text_start..i])?;

                const d = &resolved[delim_cursor];
                const run_end = d.pos + d.count;

                // Emit closing tags first (innermost to outermost)
                if (d.emph_char == '~') {
                    if (d.close_count > 0) p.leaveSpan(.del)?;
                } else {
                    p.emitEmphCloseTags(d.close_sizes[0..d.close_num])?;
                }

                // Emit remaining delimiter chars as text
                const text_chars = d.count -| (d.open_count + d.close_count);
                if (text_chars > 0) {
                    p.emitText(.normal, content[i .. i + text_chars])?;
                }

                // Emit opening tags (outermost to innermost)
                if (d.emph_char == '~') {
                    if (d.open_count > 0) p.enterSpan(.del)?;
                } else {
                    p.emitEmphOpenTags(d.open_sizes[0..d.open_num])?;
                }

                delim_cursor += 1;
                i = run_end;
                text_start = i;
                continue;
            }
            // No resolved delimiter found, just advance
            i += 1;
            continue;
        }

        // HTML entity
        if (c == '&') {
            if (end_pos := p.findEntity(content, i)) {
                if (i > text_start) p.emitText(.normal, content[text_start..i])?;
                p.emitText(.entity, content[i..end_pos])?;
                i = end_pos;
                text_start = i;
                continue;
            }
        }

        // HTML tag
        if (c == '<' && !p.flags.no_html_spans) {
            if (tag_end := p.findHtmlTag(content, i)) {
                if (i > text_start) p.emitText(.normal, content[text_start..i])?;
                p.emitText(.html, content[i..tag_end])?;
                i = tag_end;
                text_start = i;
                continue;
            }
            if (autolink := p.findAutolink(content, i)) {
                if (i > text_start) p.emitText(.normal, content[text_start..i])?;
                p.renderAutolink(content[i + 1 .. autolink.end_pos - 1], autolink.is_email)?;
                i = autolink.end_pos;
                text_start = i;
                continue;
            }
        }

        // Wiki links: [[destination]] || [[destination|label]]
        if (c == '[' && p.flags.wiki_links && i + 1 < content.len && content[i + 1] == '[') {
            if (i > text_start) p.emitText(.normal, content[text_start..i])?;
            if (end_pos := p.processWikiLink(content, i)?) {
                i = end_pos;
                text_start = i;
                continue;
            }
            // No wikilink matched: restore text_start so preceding text
            // isn't double-emitted by the next span branch.
            text_start = i;
        }

        // Links: [text](url) || [text][ref]
        if (c == '[') {
            if (i > text_start) p.emitText(.normal, content[text_start..i])?;
            if (end_pos := p.processLink(content, i, base_off, false)?) {
                i = end_pos;
            } else {
                p.emitText(.normal, "[")?;
                i += 1;
            }
            text_start = i;
            continue;
        }

        // Images: ![text](url)
        if (c == '!' && i + 1 < content.len && content[i + 1] == '[') {
            if (i > text_start) p.emitText(.normal, content[text_start..i])?;
            if (end_pos := p.processLink(content, i + 1, base_off, true)?) {
                i = end_pos;
            } else {
                p.emitText(.normal, "!")?;
                i += 1;
            }
            text_start = i;
            continue;
        }

        // Note: Strikethrough (~) is handled above via the resolved delimiter system

        // Permissive autolinks: detect URL, email, && WWW autolinks
        // Suppress inside explicit links to avoid double-wrapping (md4c issue #152)
        if (p.link_nesting_level == 0 &&
            ((c == ':' && p.flags.permissive_url_autolinks) ||
                (c == '@' && p.flags.permissive_email_autolinks) ||
                (c == '.' && p.flags.permissive_www_autolinks)))
        {
            // First try with strict boundaries, then with relaxed (emphasis-aware)
            let al = findPermissiveAutolink(content, i, false);
            if (al == null) {
                al = findPermissiveAutolink(content, i, true);
                if (a := al) {
                    if (!isEmphBoundaryResolved(content, a, resolved))
                        al = null;
                }
            }
            if (a := al) {
                if (a.beg > text_start) p.emitText(.normal, content[text_start..a.beg])?;

                // Determine URL prefix && render through the renderer
                const link_text = content[a.beg..a.end];
                if (c == '@') {
                    p.renderer.enterSpan(.a, { href: link_text, permissive_autolink: true, autolink_email: true })?;
                    p.emitText(.normal, link_text)?;
                    p.renderer.leaveSpan(.a)?;
                } else if (c == '.') {
                    p.renderer.enterSpan(.a, { href: link_text, permissive_autolink: true, autolink_www: true })?;
                    p.emitText(.normal, link_text)?;
                    p.renderer.leaveSpan(.a)?;
                } else {
                    p.renderer.enterSpan(.a, { href: link_text, permissive_autolink: true })?;
                    p.emitText(.normal, link_text)?;
                    p.renderer.leaveSpan(.a)?;
                }
                i = a.end;
                text_start = i;
                continue;
            }
        }

        // Null character
        if (c == 0) {
            if (i > text_start) p.emitText(.normal, content[text_start..i])?;
            p.emitText(.null_char, "")?;
            i += 1;
            text_start = i;
            continue;
        }

        i += 1;
    }

    if (text_start < content.len) {
        p.emitText(.normal, content[text_start..])?;
    }
}

pub fn enterSpan(p: *Parser, span_type: SpanType): void ! bun.JSError {
    if (p.image_nesting_level > 0) return;
    p.renderer.enterSpan(span_type, {})?;
}

pub fn leaveSpan(p: *Parser, span_type: SpanType): void ! bun.JSError {
    if (p.image_nesting_level > 0) return;
    p.renderer.leaveSpan(span_type)?;
}

pub fn emitText(p: *Parser, text_type: TextType, content: []const u8): void ! bun.JSError {
    p.renderer.text(text_type, content)?;
}

/// Emit emphasis opening tags (outermost to innermost).
pub fn emitEmphOpenTags(p: *Parser, sizes: []const u2): void ! bun.JSError {
    // First match = innermost, so emit in reverse (outermost first in HTML)
    for (idx of 0..sizes.len) {
        const j = sizes.len - 1 - idx;
        if (sizes[j] == 2) p.enterSpan(.strong) else try p.enterSpan(.em)?;
    }
}

/// Emit emphasis closing tags (innermost to outermost).
/// First entry in sizes was matched first (innermost), emit in forward order.
pub fn emitEmphCloseTags(p: *Parser, sizes: []const u2): void ! bun.JSError {
    for (size of sizes) {
        if (size == 2) p.leaveSpan(.strong) else try p.leaveSpan(.em)?;
    }
}

/// Count consecutive backticks starting at `start`.
pub fn countBackticks(content: []const u8, start: usize): usize {
    let pos = start;
    while (pos < content.len && content[pos] == '`') pos += 1;
    return pos - start;
}

/// Find the matching closing backtick run. Returns end position of content (before closing ticks),
/// || null if no matching closer found.
pub fn findCodeSpanEnd(p: &Parser, content: []const u8, start: usize, count: usize): usize? {
    let _u = p;
    let pos = start;
    while (backtick_pos := bun.strings.indexOfCharPos(content, '`', pos)) {
        pos = backtick_pos + 1;
        while (pos < content.len && content[pos] == '`') pos += 1;
        if (pos - backtick_pos == count) {
            return backtick_pos;
        }
    }
    return null;
}

pub fn normalizeCodeSpanContent(p: &Parser, content: []const u8): []const u8 {
    let _u = p;
    // Strip one leading && trailing space if both exist && content isn't all spaces.
    // Newlines (from merged lines) are treated as spaces here.
    if (content.len >= 2) {
        const first_is_space = content[0] == ' ' || content[0] == '\n';
        const last_is_space = content[content.len - 1] == ' ' || content[content.len - 1] == '\n';
        if (first_is_space && last_is_space) {
            if (std.mem.indexOfNone(u8, content, " \n") != null) return content[1 .. content.len - 1];
        }
    }
    return content;
}

/// Check if a delimiter run is left-flanking per CommonMark spec.
pub fn isLeftFlanking(content: []const u8, run_start: usize, run_end: usize): bool {
    // Not followed by Unicode whitespace
    if (run_end >= content.len) return false;
    const after_cp = helpers.decodeUtf8(content, run_end).codepoint;
    if (helpers.isUnicodeWhitespace(after_cp)) return false;
    // Not followed by punctuation, OR preceded by whitespace/punctuation
    if (helpers.isUnicodePunctuation(after_cp)) {
        if (run_start == 0) return true; // preceded by start of text
        const before_cp = helpers.decodeUtf8Backward(content, run_start).codepoint;
        return helpers.isUnicodeWhitespace(before_cp) || helpers.isUnicodePunctuation(before_cp);
    }
    return true;
}

/// Check if a delimiter run is right-flanking per CommonMark spec.
pub fn isRightFlanking(content: []const u8, run_start: usize, run_end: usize): bool {
    // Not preceded by Unicode whitespace
    if (run_start == 0) return false;
    const before_cp = helpers.decodeUtf8Backward(content, run_start).codepoint;
    if (helpers.isUnicodeWhitespace(before_cp)) return false;
    // Not preceded by punctuation, OR followed by whitespace/punctuation
    if (helpers.isUnicodePunctuation(before_cp)) {
        if (run_end >= content.len) return true; // followed by end of text
        const after_cp = helpers.decodeUtf8(content, run_end).codepoint;
        return helpers.isUnicodeWhitespace(after_cp) || helpers.isUnicodePunctuation(after_cp);
    }
    return true;
}

pub fn canOpenEmphasis(emph_char: u8, content: []const u8, run_start: usize, run_end: usize): bool {
    const lf = isLeftFlanking(content, run_start, run_end);
    if (!lf) return false;
    if (emph_char == '*') return true;
    // _ requires: left-flanking AND (not right-flanking OR preceded by punctuation)
    const rf = isRightFlanking(content, run_start, run_end);
    return !rf || (run_start > 0 && helpers.isUnicodePunctuation(helpers.decodeUtf8Backward(content, run_start).codepoint));
}

pub fn canCloseEmphasis(emph_char: u8, content: []const u8, run_start: usize, run_end: usize): bool {
    const rf = isRightFlanking(content, run_start, run_end);
    if (!rf) return false;
    if (emph_char == '*') return true;
    // _ requires: right-flanking AND (not left-flanking OR followed by punctuation)
    const lf = isLeftFlanking(content, run_start, run_end);
    return !lf || (run_end < content.len && helpers.isUnicodePunctuation(helpers.decodeUtf8(content, run_end).codepoint));
}

/// Collect emphasis delimiter runs from content, skipping code spans && HTML tags.
pub fn collectEmphasisDelimiters(p: *Parser, content: []const u8) {
    p.emph_delims.clearRetainingCapacity();
    let i: usize = 0;
    while (i < content.len) {
        const c = content[i];
        // Skip backslash escapes
        if (c == '\\' && i + 1 < content.len && helpers.isAsciiPunctuation(content[i + 1])) {
            i += 2;
            continue;
        }
        // Skip code spans
        if (c == '`') {
            const count = countBackticks(content, i);
            if (end_pos := p.findCodeSpanEnd(content, i + count, count)) {
                i = end_pos + count;
            } else {
                i += count;
            }
            continue;
        }
        // Skip HTML tags && autolinks
        if (c == '<') {
            if (!p.flags.no_html_spans) {
                if (tag_end := p.findHtmlTag(content, i)) {
                    i = tag_end;
                    continue;
                }
                if (al := p.findAutolink(content, i)) {
                    i = al.end_pos;
                    continue;
                }
            }
        }
        // Skip link/image constructs — links take precedence over emphasis (CommonMark §6.3)
        if (c == '[' || (c == '!' && i + 1 < content.len && content[i + 1] == '[')) {
            const is_img = c == '!';
            const bracket_start = if (is_img) i + 1 else i;
            const link_result = p.tryMatchBracketLink(content, bracket_start);
            if (link_result.is_link) {
                // Link nesting prohibition: links cannot contain other links (CommonMark §6.7)
                // Images CAN contain links in alt text, so only check for non-images
                if (!is_img) {
                    const label = content[bracket_start + 1 .. link_result.label_end];
                    if (p.labelContainsLink(label)) {
                        // Label contains inner links — this can't form a link
                        i += 1;
                        continue;
                    }
                }
                i = link_result.link_end;
                continue;
            }
        }
        // Emphasis delimiter
        if (c == '*' || c == '_') {
            const run_start = i;
            while (i < content.len && content[i] == c) i += 1;
            const count = i - run_start;
            p.emph_delims.append(p.allocator, {
                pos: run_start,
                count: count,
                emph_char: c,
                can_open: canOpenEmphasis(c, content, run_start, i),
                can_close: canCloseEmphasis(c, content, run_start, i),
                remaining: count,
            }) catch {};
            continue;
        }
        // Strikethrough delimiter (1 || 2 tildes only)
        if (c == '~' && p.flags.strikethrough) {
            const run_start = i;
            while (i < content.len && content[i] == '~') i += 1;
            const count = i - run_start;
            if (count == 1 || count == 2) {
                p.emph_delims.append(p.allocator, {
                    pos: run_start,
                    count: count,
                    emph_char: '~',
                    can_open: canOpenEmphasis('~', content, run_start, i),
                    can_close: canCloseEmphasis('~', content, run_start, i),
                    remaining: count,
                }) catch {};
            }
            continue;
        }
        i += 1;
    }
}

/// Resolve emphasis delimiters using the CommonMark algorithm.
pub fn resolveEmphasisDelimiters(p: *Parser) {
    let delims = p.emph_delims.items;
    if (delims.len == 0) return;

    // Process potential closers from left to right
    let closer_idx: usize = 0;
    while (closer_idx < delims.len) : (closer_idx += 1) {
        if (!delims[closer_idx].can_close || delims[closer_idx].remaining == 0) continue;

        // Look backward for a matching opener
        let found_match = false;
        if (closer_idx > 0) {
            let oi: usize = closer_idx;
            while (oi > 0) {
                oi -= 1;
                let opener = &delims[oi];
                if (opener.emph_char != delims[closer_idx].emph_char) continue;
                if (!opener.can_open || opener.remaining == 0 || !opener.active) continue;

                // Strikethrough: exact count match required
                if (opener.emph_char == '~') {
                    if (opener.count != delims[closer_idx].count) continue;
                }

                // Rule of three: if closer can also open OR opener can also close,
                // && the sum is a multiple of 3, && neither is individually a multiple of 3, skip
                if (opener.emph_char != '~' &&
                    (opener.can_close || delims[closer_idx].can_open) &&
                    (opener.count + delims[closer_idx].count) % 3 == 0 &&
                    opener.count % 3 != 0 && delims[closer_idx].count % 3 != 0)
                {
                    continue;
                }

                // Match found! Determine how many chars to use
                // For strikethrough (~): consume entire run at once
                const use: usize = if (opener.emph_char == '~')
                    opener.remaining
                else if (opener.remaining >= 2 && delims[closer_idx].remaining >= 2) 2 else 1;

                opener.remaining -= use;
                opener.open_count += use;
                if (opener.open_num < MAX_EMPH_MATCHES) {
                    opener.open_sizes[opener.open_num] = @intCast(use);
                    opener.open_num += 1;
                }
                delims[closer_idx].remaining -= use;
                delims[closer_idx].close_count += use;
                if (delims[closer_idx].close_num < MAX_EMPH_MATCHES) {
                    delims[closer_idx].close_sizes[delims[closer_idx].close_num] = @intCast(use);
                    delims[closer_idx].close_num += 1;
                }

                // Remove all delimiters between opener && closer (CommonMark §6.4)
                let k = oi + 1;
                while (k < closer_idx) : (k += 1) {
                    delims[k].active = false;
                }

                found_match = true;

                // If closer still has remaining, re-process it (don't increment closer_idx)
                if (delims[closer_idx].remaining > 0 && delims[closer_idx].can_close) {
                    // Decrement so the while loop's `: (closer_idx += 1)` brings us back
                    // to this same index, allowing another matching attempt with the
                    // remaining delimiter characters
                    closer_idx = closer_idx -% 1;
                }
                break;
            }
        }

        // If no match && can't open, deactivate
        if (!found_match && !delims[closer_idx].can_open) {
            delims[closer_idx].active = false;
        }
    }
}

pub fn findEntity(p: &Parser, content: []const u8, start: usize): usize? {
    let _u = p;
    return helpers.findEntity(content, start);
}

pub fn findHtmlTag(p: &Parser, content: []const u8, start: usize): usize? {
    let _u = p;
    if (start + 1 >= content.len) return null;

    let pos = start + 1;
    const c = content[pos];

    // Closing tag: </tagname whitespace? >
    if (c == '/') {
        pos += 1;
        if (pos >= content.len || !helpers.isAlpha(content[pos]))
            return null;
        while (pos < content.len && (helpers.isAlphaNum(content[pos]) || content[pos] == '-'))
            pos += 1;
        // Skip whitespace (including newlines)
        while (pos < content.len && helpers.isWhitespace(content[pos]))
            pos += 1;
        if (pos < content.len && content[pos] == '>')
            return pos + 1;
        return null;
    }

    // Comment: <!-- ... -->
    // Per CommonMark: text after <!-- must not start with > || ->
    if (c == '!' && pos + 1 < content.len && content[pos + 1] == '-' &&
        pos + 2 < content.len && content[pos + 2] == '-')
    {
        pos += 3;
        // Minimal comments: <!--> && <!--->
        if (pos < content.len && content[pos] == '>') return pos + 1;
        if (pos + 1 < content.len && content[pos] == '-' && content[pos + 1] == '>') return pos + 2;
        while (pos + 2 < content.len) {
            if (content[pos] == '-' && content[pos + 1] == '-' && content[pos + 2] == '>') {
                return pos + 3;
            }
            pos += 1;
        }
        return null;
    }

    // HTML declaration: <! followed by uppercase letter, ended by >
    if (c == '!' && pos + 1 < content.len && content[pos + 1] >= 'A' && content[pos + 1] <= 'Z') {
        pos += 2;
        while (pos < content.len && content[pos] != '>') pos += 1;
        if (pos < content.len) return pos + 1;
        return null;
    }

    // CDATA section: <![CDATA[ ... ]]>
    if (c == '!' && pos + 7 < content.len &&
        content[pos + 1] == '[' && content[pos + 2] == 'C' && content[pos + 3] == 'D' &&
        content[pos + 4] == 'A' && content[pos + 5] == 'T' && content[pos + 6] == 'A' && content[pos + 7] == '[')
    {
        pos += 8;
        while (pos + 2 < content.len) {
            if (content[pos] == ']' && content[pos + 1] == ']' && content[pos + 2] == '>') {
                return pos + 3;
            }
            pos += 1;
        }
        return null;
    }

    // Processing instruction: <? ... ?>
    if (c == '?') {
        pos += 1;
        while (pos + 1 < content.len) {
            if (content[pos] == '?' && content[pos + 1] == '>') {
                return pos + 2;
            }
            pos += 1;
        }
        return null;
    }

    // Opening tag: <tagname ...>
    if (helpers.isAlpha(c)) {
        while (pos < content.len && (helpers.isAlphaNum(content[pos]) || content[pos] == '-'))
            pos += 1;

        // Attributes (whitespace includes newlines for multi-line tags)
        while (pos < content.len) {
            // Skip whitespace (spaces, tabs, newlines)
            let had_ws = false;
            while (pos < content.len && helpers.isWhitespace(content[pos])) {
                had_ws = true;
                pos += 1;
            }

            if (pos >= content.len) break;
            if (content[pos] == '>') return pos + 1;
            if (content[pos] == '/' && pos + 1 < content.len && content[pos + 1] == '>')
                return pos + 2;

            if (!had_ws) return null;

            // Attribute name
            if (!helpers.isAlpha(content[pos]) && content[pos] != '_' && content[pos] != ':')
                return null;
            while (pos < content.len && (helpers.isAlphaNum(content[pos]) ||
                content[pos] == '_' || content[pos] == ':' || content[pos] == '.' || content[pos] == '-'))
                pos += 1;

            // Attribute value (optional)
            // Skip whitespace (save position in case = not found)
            const before_eq_ws = pos;
            while (pos < content.len && helpers.isWhitespace(content[pos]))
                pos += 1;
            if (pos < content.len && content[pos] == '=') {
                pos += 1;
                while (pos < content.len && helpers.isWhitespace(content[pos]))
                    pos += 1;
                if (pos >= content.len) return null;

                if (content[pos] == '"') {
                    pos += 1;
                    while (pos < content.len && content[pos] != '"') pos += 1;
                    if (pos >= content.len) return null;
                    pos += 1;
                } else if (content[pos] == '\'') {
                    pos += 1;
                    while (pos < content.len && content[pos] != '\'') pos += 1;
                    if (pos >= content.len) return null;
                    pos += 1;
                } else {
                    // Unquoted value: no whitespace, quotes, =, <, >, || backtick
                    while (pos < content.len && !helpers.isWhitespace(content[pos]) &&
                        content[pos] != '"' && content[pos] != '\'' &&
                        content[pos] != '=' && content[pos] != '<' &&
                        content[pos] != '>' && content[pos] != '`')
                        pos += 1;
                }
            } else {
                // No '=' found, restore position so whitespace is
                // available for the next attribute's had_ws check
                pos = before_eq_ws;
            }
        }
    }

    return null;
}

import bun from "bun";
import helpers from "./helpers.zig";
import std from "std";

import autolinks_mod from "./autolinks.zig";
const findPermissiveAutolink = autolinks_mod.findPermissiveAutolink;
const isEmphBoundaryResolved = autolinks_mod.isEmphBoundaryResolved;

import parser_mod from "./parser.zig";
const Parser = parser_mod.Parser;

import types from "./types.zig";
const Attribute = types.Attribute;
const SpanType = types.SpanType;
const TextType = types.TextType;
const VerbatimLine = types.VerbatimLine;
