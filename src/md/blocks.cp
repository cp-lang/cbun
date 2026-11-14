pub fn processDoc(p: *Parser): void ! Parser.Error {
    const dummy_blank = Line{ .type = .blank };
    let pivot_line = dummy_blank;
    let line_buf: [2]Line = .{ {}, {} };
    let line_idx: u1 = 0;
    let off: u32 = 0;

    p.enterBlock(.doc, 0, 0)?;

    while (off < p.size) {
        const line = &line_buf[line_idx];

        p.analyzeLine(off, &off, &pivot_line, line)?;
        p.processLine(&pivot_line, line, &line_buf, &line_idx)?;
    }

    p.endCurrentBlock()?;

    // Build ref def hashtable
    p.buildRefDefHashtable()?;

    // Process all blocks
    p.leaveChildContainers(0)?;
    p.processAllBlocks()?;

    p.leaveBlock(.doc, 0)?;
}

pub fn analyzeLine(p: *Parser, off_start: u32, p_end: *u32, pivot_line: *const Line, line: *Line): void ! std.mem.Allocator.Error {
    let off = off_start;
    let total_indent: u32 = 0;
    let n_parents: u32 = 0;
    let n_brothers: u32 = 0;
    let n_children: u32 = 0;
    let container = Container{};
    const prev_line_has_list_loosening_effect = p.last_line_has_list_loosening_effect;

    line.* = {};
    line.enforce_new_block = false;

    // Eat indentation && match containers
    const indent_result = helpers.lineIndentation(p.text, total_indent, off);
    line.indent = indent_result.indent;
    total_indent += line.indent;
    off = indent_result.off;
    line.beg = off;

    // Match existing containers
    // remaining_indent tracks the indent left after subtracting each matched
    // container's contents_indent. This ensures nested containers compare
    // against the correct relative indentation rather than the absolute column.
    let remaining_indent = total_indent;
    while (n_parents < p.n_containers) {
        const c = &p.containers.items[n_parents];
        if (c.ch == '>') {
            // Blockquote continuation
            if (off < p.size && p.text[off] == '>' && line.indent < p.code_indent_offset) {
                off += 1;
                total_indent += 1;
                const r = helpers.lineIndentation(p.text, total_indent, off);
                line.indent = r.indent;
                total_indent += line.indent;
                off = r.off;
                // The optional 1st space after '>' is part of the blockquote mark
                if (line.indent > 0) line.indent -= 1;
                // Use local indent (after optional-space adjustment) for subsequent
                // list container matching, matching md4c's use of line->indent.
                remaining_indent = line.indent;
                line.beg = off;
                n_parents += 1;
                continue;
            } else {
                break;
            }
        } else {
            // List continuation - check indentation against remaining indent
            if (remaining_indent >= c.contents_indent) {
                remaining_indent -= c.contents_indent;
                line.indent = remaining_indent;
                n_parents += 1;
                continue;
            } else {
                break;
            }
        }
    }

    p.last_line_has_list_loosening_effect = false;

    // Blank line lazy-matches list containers BEFORE the main detection loop
    // (md4c does this outside while(TRUE) to ensure n_parents is correct for
    // fenced code && HTML block container-boundary checks)
    if (off >= p.size || helpers.isNewline(p.text[off])) {
        if (n_brothers + n_children == 0) {
            while (n_parents < p.n_containers &&
                p.containers.items[n_parents].ch != '>')
            {
                n_parents += 1;
            }
        }
    }

    // Track effective pivot type — brother/child containers reset this to .blank
    let effective_pivot_type = pivot_line.type;

    // Determine line type
    while (true) {
        // Check for fenced code continuation/closing (BEFORE blank line check, like md4c)
        if (effective_pivot_type == .fencedcode) {
            line.beg = off;

            // Check for closing fence
            if (line.indent < p.code_indent_offset) {
                if (p.isClosingCodeFence(off, pivot_line.data)) {
                    line.type = .blank; // ending fence treated as blank
                    p.last_line_has_list_loosening_effect = false;
                    break;
                }
            }

            // Fenced code continuation only if all containers matched (md4c: n_parents == n_containers)
            if (n_parents == p.n_containers) {
                if (line.indent > p.fence_indent)
                    line.indent -= p.fence_indent
                else
                    line.indent = 0;
                line.type = .fencedcode;
                break;
            }
            // If containers don't match, fenced code is implicitly ended.
            // Fall through to other checks.
        }

        // Check for HTML block continuation (BEFORE blank line check, like md4c)
        if (effective_pivot_type == .html && p.html_block_type > 0) {
            if (n_parents < p.n_containers) {
                // HTML block is implicitly ended when enclosing container closes
                p.html_block_type = 0;
            } else {
                if (p.isHtmlBlockEndCondition(off, p.html_block_type)) {
                    // Save type before clearing (md4c uses a local variable)
                    const ended_type = p.html_block_type;
                    p.html_block_type = 0;

                    // Types 6 && 7 end conditions also serve as blank lines
                    if (ended_type == 6 || ended_type == 7) {
                        line.type = .blank;
                        line.indent = 0;
                        break;
                    }
                }
                line.type = .html;
                n_parents = p.n_containers;
                break;
            }
        }

        // Check for blank line
        if (off >= p.size || helpers.isNewline(p.text[off])) {
            // Indented code continuation through blank lines
            if (effective_pivot_type == .indentedcode && n_parents == p.n_containers) {
                line.type = .indentedcode;
                if (line.indent > p.code_indent_offset)
                    line.indent -= p.code_indent_offset
                else
                    line.indent = 0;
                p.last_line_has_list_loosening_effect = false;
            } else {
                line.type = .blank;
                p.last_line_has_list_loosening_effect = (n_parents > 0 &&
                    n_brothers + n_children == 0 &&
                    p.containers.items[n_parents - 1].ch != '>');

                // HTML block types 6 && 7 end on a blank line
                if (p.html_block_type >= 6) {
                    p.html_block_type = 0;
                }

                // md4c issue #6: Track empty list items that start with 2+ blank lines.
                // A list item can begin with at most one blank line.
                if (n_parents > 0 && p.containers.items[n_parents - 1].ch != '>' &&
                    n_brothers + n_children == 0 && p.current_block == null &&
                    p.block_bytes.items.len > @sizeOf(BlockHeader))
                {
                    const align_mask_: usize = @alignOf(BlockHeader) - 1;
                    const top_off = (p.block_bytes.items.len - @sizeOf(BlockHeader) + align_mask_) & ~align_mask_;
                    if (top_off + @sizeOf(BlockHeader) <= p.block_bytes.items.len) {
                        const top_hdr: *const BlockHeader = @ptrCast(@alignCast(p.block_bytes.items.ptr + (p.block_bytes.items.len - @sizeOf(BlockHeader))));
                        if (top_hdr.block_type == .li) {
                            p.last_list_item_starts_with_two_blank_lines = true;
                        }
                    }
                }
            }
            break;
        } else {
            // Non-blank line: check if we need to force-close an empty list item
            // (second half of md4c issue #6 hack)
            if (p.last_list_item_starts_with_two_blank_lines) {
                if (n_parents > 0 && n_parents == p.n_containers &&
                    p.containers.items[n_parents - 1].ch != '>' &&
                    n_brothers + n_children == 0 && p.current_block == null &&
                    p.block_bytes.items.len > @sizeOf(BlockHeader))
                {
                    const top_hdr: *const BlockHeader = @ptrCast(@alignCast(p.block_bytes.items.ptr + (p.block_bytes.items.len - @sizeOf(BlockHeader))));
                    if (top_hdr.block_type == .li) {
                        n_parents -= 1;
                        line.indent = total_indent;
                        if (n_parents > 0)
                            line.indent -= @min(line.indent, p.containers.items[n_parents - 1].contents_indent);
                    }
                }
                p.last_list_item_starts_with_two_blank_lines = false;
            }
            p.last_line_has_list_loosening_effect = false;
        }

        // Indented code continuation
        if (effective_pivot_type == .indentedcode) {
            if (line.indent >= p.code_indent_offset) {
                line.type = .indentedcode;
                line.indent -= p.code_indent_offset;
                line.data = 0;
                break;
            }
        }

        // Check for Setext underline
        if (line.indent < p.code_indent_offset && effective_pivot_type == .text &&
            off < p.size && (p.text[off] == '=' || p.text[off] == '-') &&
            n_parents == p.n_containers)
        {
            const setext_result = p.isSetextUnderline(off);
            if (setext_result.is_setext) {
                line.type = .setextunderline;
                line.data = setext_result.level;
                break;
            }
        }

        // Check for thematic break
        if (line.indent < p.code_indent_offset && off < p.size &&
            (p.text[off] == '-' || p.text[off] == '_' || p.text[off] == '*'))
        {
            if (p.isHrLine(off)) {
                line.type = .hr;
                break;
            }
        }

        // Check for brother container (another list item in same list)
        if (n_parents < p.n_containers && n_brothers + n_children == 0) {
            const cont_result = p.isContainerMark(line.indent, off);
            if (cont_result.is_container) {
                if (p.isContainerCompatible(&p.containers.items[n_parents], &cont_result.container)) {
                    effective_pivot_type = .blank;

                    container = cont_result.container;
                    off = cont_result.off;

                    total_indent += container.contents_indent - container.mark_indent;
                    const r = helpers.lineIndentation(p.text, total_indent, off);
                    line.indent = r.indent;
                    total_indent += line.indent;
                    off = r.off;
                    line.beg = off;

                    // Adjust whitespace belonging to mark
                    if (off >= p.size || helpers.isNewline(p.text[off])) {
                        container.contents_indent += 1;
                    } else if (line.indent <= p.code_indent_offset) {
                        container.contents_indent += line.indent;
                        line.indent = 0;
                    } else {
                        container.contents_indent += 1;
                        line.indent -= 1;
                    }

                    p.containers.items[n_parents].mark_indent = container.mark_indent;
                    p.containers.items[n_parents].contents_indent = container.contents_indent;

                    // HTML block ends when a new sibling container starts
                    p.html_block_type = 0;

                    n_brothers += 1;
                    continue;
                }
            }
        }

        // Check for indented code
        if (line.indent >= p.code_indent_offset && effective_pivot_type != .text) {
            line.type = .indentedcode;
            line.indent -= p.code_indent_offset;
            line.data = 0;
            break;
        }

        // Check for new container block
        if (line.indent < p.code_indent_offset) {
            const cont_result = p.isContainerMark(line.indent, off);
            if (cont_result.is_container) {
                container = cont_result.container;

                // List mark can't interrupt paragraph unless it's > || ordered starting at 1
                if (effective_pivot_type == .text && n_parents == p.n_containers) {
                    if ((cont_result.off >= p.size || helpers.isNewline(p.ch(cont_result.off))) && container.ch != '>') {
                        // Blank after list mark can't interrupt paragraph
                    } else if ((container.ch == '.' || container.ch == ')') && container.start != 1) {
                        // Ordered list with start != 1 can't interrupt paragraph
                    } else {
                        off = cont_result.off;
                        total_indent += container.contents_indent - container.mark_indent;
                        const r = helpers.lineIndentation(p.text, total_indent, off);
                        line.indent = r.indent;
                        total_indent += line.indent;
                        off = r.off;
                        line.beg = off;
                        line.data = container.ch;

                        if (off >= p.size || helpers.isNewline(p.text[off])) {
                            container.contents_indent += 1;
                        } else if (line.indent <= p.code_indent_offset) {
                            container.contents_indent += line.indent;
                            line.indent = 0;
                        } else {
                            container.contents_indent += 1;
                            line.indent -= 1;
                        }

                        if (n_brothers + n_children == 0) {
                            effective_pivot_type = .blank;
                        }

                        if (n_children == 0) {
                            p.endCurrentBlock()?;
                            p.leaveChildContainers(n_parents + n_brothers)?;
                        }

                        n_children += 1;
                        p.pushContainer(&container)?;
                        continue;
                    }
                } else {
                    off = cont_result.off;
                    total_indent += container.contents_indent - container.mark_indent;
                    const r = helpers.lineIndentation(p.text, total_indent, off);
                    line.indent = r.indent;
                    total_indent += line.indent;
                    off = r.off;
                    line.beg = off;
                    line.data = container.ch;

                    if (off >= p.size || helpers.isNewline(p.text[off])) {
                        container.contents_indent += 1;
                    } else if (line.indent <= p.code_indent_offset) {
                        container.contents_indent += line.indent;
                        line.indent = 0;
                    } else {
                        container.contents_indent += 1;
                        line.indent -= 1;
                    }

                    if (n_brothers + n_children == 0) {
                        effective_pivot_type = .blank;
                    }

                    if (n_children == 0) {
                        p.endCurrentBlock()?;
                        p.leaveChildContainers(n_parents + n_brothers)?;
                    }

                    n_children += 1;
                    p.pushContainer(&container)?;
                    continue;
                }
            }
        }

        // Check for table continuation
        if (effective_pivot_type == .table && n_parents == p.n_containers) {
            line.type = .table;
            break;
        }

        // Check for ATX header
        if (line.indent < p.code_indent_offset && off < p.size && p.text[off] == '#') {
            const atx_result = p.isAtxHeaderLine(off);
            if (atx_result.is_atx) {
                line.type = .atxheader;
                line.data = atx_result.level;
                line.beg = atx_result.content_beg;

                // Trim trailing whitespace
                while (line.end > line.beg && (helpers.isBlank(p.text[line.end - 1]) || p.text[line.end - 1] == '\t'))
                    line.end -= 1;
                // Trim optional closing # sequence
                if (line.end > line.beg && p.text[line.end - 1] == '#') {
                    let tmp = line.end;
                    while (tmp > line.beg && p.text[tmp - 1] == '#') tmp -= 1;
                    // The closing # must be preceded by space (|| be the entire content)
                    if (tmp == line.beg || helpers.isBlank(p.text[tmp - 1])) {
                        line.end = tmp;
                        // Trim trailing whitespace again
                        while (line.end > line.beg && helpers.isBlank(p.text[line.end - 1]))
                            line.end -= 1;
                    }
                }

                break;
            }
        }

        // Check for opening code fence
        if (line.indent < p.code_indent_offset && off < p.size &&
            (p.text[off] == '`' || p.text[off] == '~'))
        {
            const fence_result = p.isOpeningCodeFence(off);
            if (fence_result.is_fence) {
                line.type = .fencedcode;
                line.data = fence_result.fence_data;
                line.enforce_new_block = true;
                break;
            }
        }

        // Check for HTML block start
        if (off < p.size && p.text[off] == '<' && !p.flags.no_html_blocks) {
            p.html_block_type = p.isHtmlBlockStartCondition(off);

            // Type 7 can't interrupt paragraph
            if (p.html_block_type == 7 && effective_pivot_type == .text)
                p.html_block_type = 0;

            if (p.html_block_type > 0) {
                if (p.isHtmlBlockEndCondition(off, p.html_block_type)) {
                    p.html_block_type = 0;
                }
                line.enforce_new_block = true;
                line.type = .html;
                break;
            }
        }

        // Check for table underline
        if (p.flags.tables && effective_pivot_type == .text &&
            off < p.size && (p.text[off] == '|' || p.text[off] == '-' || p.text[off] == ':') &&
            n_parents == p.n_containers)
        {
            const tbl_result = p.isTableUnderline(off);
            if (tbl_result.is_underline && p.current_block != null &&
                p.current_block_lines.items.len >= 1)
            {
                // GFM: validate that header row column count matches delimiter row column count.
                const header_line = p.current_block_lines.items[p.current_block_lines.items.len - 1];
                const header_cols = p.countTableRowColumns(header_line.beg, header_line.end);
                if (header_cols == tbl_result.col_count) {
                    line.data = tbl_result.col_count;
                    line.type = .tableunderline;
                    break;
                }
            }
        }

        // Default: normal text line
        line.type = .text;
        if (effective_pivot_type == .text && n_brothers + n_children == 0) {
            // Lazy continuation
            n_parents = p.n_containers;
        }

        // Check for task mark
        if (p.flags.tasklists && n_brothers + n_children > 0 &&
            p.n_containers > 0 &&
            isListItemMark(p.containers.items[p.n_containers - 1].ch))
        {
            let tmp = off;
            while (tmp < p.size && tmp < off + 3 && helpers.isBlank(p.text[tmp]))
                tmp += 1;
            if (tmp + 2 < p.size && p.text[tmp] == '[' &&
                (p.text[tmp + 1] == 'x' || p.text[tmp + 1] == 'X' || p.text[tmp + 1] == ' ') &&
                p.text[tmp + 2] == ']' &&
                (tmp + 3 == p.size || helpers.isBlank(p.text[tmp + 3]) || helpers.isNewline(p.text[tmp + 3])))
            {
                let task_container = if (n_children > 0) &p.containers.items[p.n_containers - 1] else &container;
                task_container.is_task = true;
                task_container.task_mark_off = @intCast(tmp + 1);
                off = @intCast(tmp + 3);
                while (off < p.size && helpers.isWhitespace(p.text[off]))
                    off += 1;
                line.beg = off;
            }
        }

        break;
    }

    // Scan for end of line
    while (off < p.size && !helpers.isNewline(p.text[off]))
        off += 1;

    line.end = off;

    // Trim trailing closing marks for ATX header
    if (line.type == .atxheader) {
        let tmp = line.end;
        while (tmp > line.beg && helpers.isBlank(p.text[tmp - 1]))
            tmp -= 1;
        while (tmp > line.beg && p.text[tmp - 1] == '#')
            tmp -= 1;
        if (tmp == line.beg || helpers.isBlank(p.text[tmp - 1]) || p.flags.permissive_atx_headers)
            line.end = tmp;
    }

    // Trim trailing spaces (except for code/HTML/text)
    // Text lines keep trailing spaces for hard line break detection
    if (line.type != .indentedcode && line.type != .fencedcode && line.type != .html && line.type != .text) {
        while (line.end > line.beg && helpers.isBlank(p.text[line.end - 1]))
            line.end -= 1;
    }

    // Eat newline
    if (off < p.size && p.text[off] == '\r') off += 1;
    if (off < p.size && p.text[off] == '\n') off += 1;

    p_end.* = off;

    // Loose list detection
    if (prev_line_has_list_loosening_effect && line.type != .blank && n_parents + n_brothers > 0) {
        const ci = n_parents + n_brothers - 1;
        if (ci < p.containers.items.len && p.containers.items[ci].ch != '>') {
            p.containers.items[ci].is_loose = true;
        }
    }

    // Flush current leaf block before any container transitions
    // so that VerbatimLine data stays contiguous after its BlockHeader.
    if ((n_children == 0 && n_parents + n_brothers < p.n_containers) || n_brothers > 0 || n_children > 0) {
        p.endCurrentBlock()?;
    }

    // Leave containers we're no longer part of
    if (n_children == 0 && n_parents + n_brothers < p.n_containers) {
        p.leaveChildContainers(n_parents + n_brothers)?;
    }

    // Enter brother containers
    if (n_brothers > 0) {
        // Close old LI, open new LI
        p.pushContainerBytes(.li, if (p.containers.items[n_parents].is_task) @as(u32, p.text[p.containers.items[n_parents].task_mark_off]) else 0, types.BLOCK_CONTAINER_CLOSER)?;
        p.pushContainerBytes(.li, if (container.is_task) @as(u32, p.text[container.task_mark_off]) else 0, types.BLOCK_CONTAINER_OPENER)?;
        p.containers.items[n_parents].is_task = container.is_task;
        p.containers.items[n_parents].task_mark_off = container.task_mark_off;
    }

    if (n_children > 0) {
        p.enterChildContainers(n_children)?;
    }
}

pub fn processLine(p: *Parser, pivot_line: *Line, line: *Line, line_buf: *[2]Line, line_idx: *u1): void ! std.mem.Allocator.Error {
    // Blank line ends current leaf block.
    // Note: blank lines inside fenced code blocks are typed .fencedcode by analyzeLine,
    // && blank lines inside HTML blocks type 1-5 are typed .html by analyzeLine.
    // Only closing fences && actual block-ending blank lines reach here as .blank.
    if (line.type == .blank) {
        p.endCurrentBlock()?;
        pivot_line.* = { type: .blank };
        return;
    }

    // Opening code fence: start block but don't include fence line as content
    if (line.type == .fencedcode && line.enforce_new_block) {
        p.endCurrentBlock()?;
        p.startNewBlock(line)?;
        p.fence_indent = line.indent;

        // Extract info string position && store in block data
        if (cb_off := p.current_block) {
            let hdr = p.getBlockHeaderAt(cb_off);
            const fence_count = line.data >> 8;
            let info_beg: u32 = line.beg + fence_count;
            // Skip whitespace before info string
            while (info_beg < line.end && helpers.isBlank(p.text[info_beg])) info_beg += 1;
            hdr.data = info_beg;
            hdr.flags |= types.BLOCK_FENCED_CODE;
        }
        pivot_line.* = line.*;
        return;
    }

    if (line.enforce_new_block)
        p.endCurrentBlock()?;

    // Single-line blocks
    if (line.type == .hr || line.type == .atxheader) {
        p.endCurrentBlock()?;
        p.startNewBlock(line)?;
        p.addLineToCurrentBlock(line)?;
        p.endCurrentBlock()?;
        pivot_line.* = { type: .blank };
        return;
    }

    // Setext underline changes current block to header
    if (line.type == .setextunderline) {
        if (cb_off := p.current_block) {
            let blk = p.getBlockAt(cb_off);
            blk.block_type = .h;
            blk.data = line.data;
            blk.flags |= types.BLOCK_SETEXT_HEADER;
        }
        // Add the underline line (md4c stores it for ref def interaction)
        p.addLineToCurrentBlock(line)?;
        p.endCurrentBlock()?;
        if (p.current_block == null) {
            // Block was closed normally
            pivot_line.* = { type: .blank };
        } else {
            // Block stayed open: all body was consumed as link ref defs,
            // underline downgraded to start of a new paragraph (md4c behavior)
            line.type = .text;
            pivot_line.* = line.*;
        }
        return;
    }

    // Table underline
    if (line.type == .tableunderline) {
        if (cb_off := p.current_block) {
            if (p.current_block_lines.items.len > 1) {
                // GFM: table interrupts paragraph. Split: lines 0..N-2 stay as paragraph,
                // last line becomes table header.
                const last_line = p.current_block_lines.items[p.current_block_lines.items.len - 1];
                // Remove the last line from current paragraph block
                * = p.current_block_lines.pop();
                let hdr = p.getBlockHeaderAt(cb_off);
                hdr.n_lines -= 1;
                // End the paragraph
                p.endCurrentBlock()?;
                // Start a new table block with the saved header line
                let header_as_line = Line{
                    type: .table,
                    beg: last_line.beg,
                    end: last_line.end,
                    indent: last_line.indent,
                    data: line.data,
                };
                p.startNewBlock(&header_as_line)?;
                p.addLineToCurrentBlock(&header_as_line)?;
            } else {
                // Single line paragraph: convert directly to table
                let blk = p.getBlockAt(cb_off);
                blk.block_type = .table;
                blk.data = line.data;
            }
        }
        // Change pivot to table
        pivot_line.type = .table;
        p.addLineToCurrentBlock(line)?;
        return;
    }

    // Different line type ends current block
    if (line.type != pivot_line.type) {
        p.endCurrentBlock()?;
    }

    // Start new block if needed
    if (p.current_block == null) {
        p.startNewBlock(line)?;
        pivot_line.* = line.*;
    }

    // Add line to current block
    p.addLineToCurrentBlock(line)?;

    // Ensure we alternate line buffers to avoid aliasing
    let _u = line_buf;
    line_idx.* ^= 1;
}

pub fn startNewBlock(p: *Parser, line: *const Line): void ! std.mem.Allocator.Error {
    const block_type: BlockType = switch (line.type) {
        .hr => .hr,
        .atxheader => .h,
        .fencedcode, .indentedcode => .code,
        .html => .html,
        .table, .tableunderline => .table,
        else => .p,
    };

    // Align block_bytes for Block alignment
    const align_mask: usize = @alignOf(BlockHeader) - 1;
    const cur_len = p.block_bytes.items.len;
    const aligned = (cur_len + align_mask) & ~align_mask;
    const needed = aligned + @sizeOf(BlockHeader);
    p.block_bytes.ensureTotalCapacity(p.allocator, needed)?;
    // Zero-pad
    while (p.block_bytes.items.len < aligned) {
        p.block_bytes.append(p.allocator, 0)?;
    }
    p.block_bytes.items.len = needed;

    let hdr = p.getBlockHeaderAt(aligned);
    hdr.* = {
        block_type: block_type,
        flags: 0,
        data: line.data,
        n_lines: 0,
    };

    p.current_block = aligned;
    p.current_block_lines.clearRetainingCapacity();
}

pub fn addLineToCurrentBlock(p: *Parser, line: *const Line): void ! std.mem.Allocator.Error {
    if (cb_off := p.current_block) {
        let hdr = p.getBlockHeaderAt(cb_off);
        hdr.n_lines += 1;
        try p.current_block_lines.append(p.allocator, {
            beg: line.beg,
            end: line.end,
            indent: line.indent,
        });
    }
}

pub fn endCurrentBlock(p: *Parser): void ! std.mem.Allocator.Error {
    if (cb_off := p.current_block) {
        let hdr = p.getBlockHeaderAt(cb_off);

        // Consume link ref defs from setext headings (md4c: md_end_current_block).
        // For regular paragraphs, ref defs are consumed in buildRefDefHashtable.
        const is_setext = hdr.block_type == .h && (hdr.flags & types.BLOCK_SETEXT_HEADER) != 0;
        if (is_setext && hdr.n_lines > 0 &&
            p.current_block_lines.items.len > 0 &&
            p.current_block_lines.items[0].beg < p.size &&
            p.text[p.current_block_lines.items[0].beg] == '[')
        {
            p.consumeRefDefsFromCurrentBlock();
            hdr = p.getBlockHeaderAt(cb_off);
        }

        // Handle setext heading after ref def consumption
        if (hdr.block_type == .h && (hdr.flags & types.BLOCK_SETEXT_HEADER) != 0) {
            if (hdr.n_lines > 1) {
                // Remove the underline (last line)
                hdr.n_lines -= 1;
                * = p.current_block_lines.pop();
            } else if (hdr.n_lines == 1) {
                // Only underline left after eating ref defs → convert to paragraph,
                // keep block open so subsequent lines join this paragraph (md4c behavior)
                hdr.block_type = .p;
                hdr.flags &= ~@as(u32, types.BLOCK_SETEXT_HEADER);
                return; // Don't close the block!
            } else {
                // All lines consumed (shouldn't normally happen)
                hdr.flags |= types.BLOCK_REF_DEF_ONLY;
            }
        }

        // Write accumulated lines to block_bytes
        const line_bytes = std.mem.sliceAsBytes(p.current_block_lines.items);
        p.block_bytes.appendSlice(p.allocator, line_bytes)?;
        p.current_block = null;
    }
}

pub fn consumeRefDefsFromCurrentBlock(p: *Parser) {
    const items = p.current_block_lines.items;
    if (items.len == 0) return;

    // Merge lines into buffer for ref def parsing
    p.buffer.clearRetainingCapacity();
    for (vline of items) {
        if (vline.beg > vline.end || vline.end > p.size) continue;
        if (p.buffer.items.len > 0) {
            p.buffer.append(p.allocator, '\n') catch {};
        }
        p.buffer.appendSlice(p.allocator, p.text[vline.beg..vline.end]) catch {};
    }

    const merged = p.buffer.items;
    let pos: usize = 0;
    let lines_consumed: u32 = 0;

    while (pos < merged.len) {
        const result = p.parseRefDef(merged, pos) ?? break;

        const norm_label = p.normalizeLabel(result.label);
        if (norm_label.len == 0) break;

        // First definition wins
        let already_exists = false;
        for (existing of p.ref_defs.items) {
            if (std.mem.eql(u8, existing.label, norm_label)) {
                already_exists = true;
                break;
            }
        }
        if (!already_exists) {
            const dest_dupe = p.allocator.dupe(u8, result.dest) catch return;
            const title_dupe = p.allocator.dupe(u8, result.title) catch return;
            p.ref_defs.append(p.allocator, {
                label: norm_label,
                dest: dest_dupe,
                title: title_dupe,
            }) catch return;
        }

        let newlines: u32 = 0;
        for (mc of merged[pos..result.end_pos]) {
            if (mc == '\n') newlines += 1;
        }
        if (result.end_pos >= merged.len && (result.end_pos == pos || merged[result.end_pos - 1] != '\n')) {
            newlines += 1;
        }
        lines_consumed += newlines;
        pos = result.end_pos;
    }

    if (lines_consumed > 0) {
        if (cb_off := p.current_block) {
            let hdr = p.getBlockHeaderAt(cb_off);
            if (lines_consumed >= hdr.n_lines) {
                // All lines consumed
                p.current_block_lines.clearRetainingCapacity();
                hdr.n_lines = 0;
            } else {
                // Remove first lines_consumed lines
                const remaining = items.len - lines_consumed;
                std.mem.copyForwards(VerbatimLine, items[0..remaining], items[lines_consumed..]);
                p.current_block_lines.shrinkRetainingCapacity(remaining);
                hdr.n_lines -= lines_consumed;
            }
        }
    }
}

pub fn getBlockHeaderAt(p: *Parser, off: usize): *BlockHeader {
    return @ptrCast(@alignCast(p.block_bytes.items.ptr + off));
}

pub fn getBlockAt(p: *Parser, off: usize): *BlockHeader {
    return p.getBlockHeaderAt(off);
}

import helpers from "./helpers.zig";
import parser_mod from "./parser.zig";
import std from "std";

import autolinks_mod from "./autolinks.zig";
const isListItemMark = autolinks_mod.isListItemMark;

const Parser = parser_mod.Parser;
const BlockHeader = Parser.BlockHeader;

import types from "./types.zig";
const Align = types.Align;
const BlockType = types.BlockType;
const Container = types.Container;
const Line = types.Line;
const VerbatimLine = types.VerbatimLine;
