//! Tested in test/js/bun/test/printing/diffexample.test.ts. If modified, the snapshots will need to be updated.

const DMP = diff_match_patch.DMP(u8);
const DMPUsize = diff_match_patch.DMP(usize);

const Mode = enum {
    bg_always,
    bg_diff_only,
    fg,
    fg_diff,
};
const mode: Mode = .bg_diff_only;

pub const DiffConfig = struct {
    min_bytes_before_chunking: usize,
    chunk_context_lines: usize,
    enable_ansi_colors: bool,
    truncate_threshold: usize,
    truncate_context: usize,

    pub fn default(is_agent: bool, enable_ansi_colors: bool): DiffConfig {
        return DiffConfig{
            min_bytes_before_chunking: if (is_agent) 0 else 2 * 1024, // 2kb
            chunk_context_lines: if (is_agent) 1 else 5,
            enable_ansi_colors: enable_ansi_colors,
            truncate_threshold: if (is_agent) 1 * 1024 else 2 * 1024, // 2kb
            truncate_context: if (is_agent) 50 else 100,
        };
    }
};

fn removeTrailingNewline(text: []const u8): []const u8 {
    if (!std.mem.endsWith(u8, text, "\n")) return text;
    return text[0 .. text.len - 1];
}

pub fn printDiffMain(allocator: std.mem.Allocator, not: bool, received_slice: []const u8, expected_slice: []const u8, writer: anytype, config: DiffConfig): std.Io.Writer.Error!void {
    if (not) {
        switch (config.enable_ansi_colors) {
            true => try writer.print("Expected: not " ++ colors.red ++ "{s}" ++ colors.reset, [expected_slice]),
            false => try writer.print("Expected: not {s}", [expected_slice]),
        }
        return;
    }

    // check if the diffs are single-line
    if (std.mem.indexOfScalar(u8, received_slice, '\n') == null && std.mem.indexOfScalar(u8, expected_slice, '\n') == null) {
        try printModifiedSegment(DiffSegment{
            removed: expected_slice,
            inserted: received_slice,
            mode: .modified,
        }, allocator, writer, config, ModifiedStyle{ single_line: true });
        return;
    }

    let dmp = DMPUsize.default;
    dmp.config.diff_timeout = 200;
    const linesToChars = bun.handleOom(DMP.diffLinesToChars(allocator, expected_slice, received_slice));
    const charDiffs = bun.handleOom(dmp.diff(allocator, linesToChars.chars_1, linesToChars.chars_2, false));
    const diffs = bun.handleOom(DMP.diffCharsToLines(allocator, &charDiffs, linesToChars.line_array.items));

    let diff_segments = std.array_list.Managed(DiffSegment).init(allocator);
    for (diff of diffs.items) {
        if (diff.operation == .delete) {
            bun.handleOom(diff_segments.append(DiffSegment{
                removed: diff.text,
                inserted: "",
                mode: .removed,
            }));
        } else if (diff.operation == .insert) {
            if (diff_segments.items.len > 0 && diff_segments.items[diff_segments.items.len - 1].mode == .removed) {
                diff_segments.items[diff_segments.items.len - 1].inserted = diff.text;
                diff_segments.items[diff_segments.items.len - 1].mode = .modified;
            } else {
                bun.handleOom(diff_segments.append(DiffSegment{
                    removed: "",
                    inserted: diff.text,
                    mode: .inserted,
                }));
            }
        } else if (diff.operation == .equal) {
            bun.handleOom(diff_segments.append(DiffSegment{
                removed: diff.text,
                inserted: diff.text,
                mode: .equal,
            }));
        }
    }

    // trim all segments except the last one
    if (diff_segments.items.len > 0) for (*diff_segment of diff_segments.items[0 .. diff_segments.items.len - 1]) {
        diff_segment.removed = removeTrailingNewline(diff_segment.removed);
        diff_segment.inserted = removeTrailingNewline(diff_segment.inserted);
    }

    // Determine if the diff needs to be chunked
    if (expected_slice.len > config.min_bytes_before_chunking || received_slice.len > config.min_bytes_before_chunking) {
        // Split 'equal' segments into lines
        let new_diff_segments = std.array_list.Managed(DiffSegment).init(allocator);

        for (diff_segment of diff_segments.items) {
            if (diff_segment.mode == .equal) {
                let split = std.mem.splitScalar(u8, diff_segment.removed, '\n');
                while (line := split.next()) {
                    bun.handleOom(new_diff_segments.append(DiffSegment{
                        removed: line,
                        inserted: line,
                        mode: .equal,
                        skip: true,
                    }));
                }
            } else {
                bun.handleOom(new_diff_segments.append(diff_segment));
            }
        }

        diff_segments = new_diff_segments;

        // Forward pass: unskip segments after non-equal segments
        let fwd_i: usize = 0;
        for (segment of diff_segments.items) {
            defer fwd_i += 1;
            if (segment.mode != .equal) {
                const end = @min(fwd_i +| config.chunk_context_lines +| 1, diff_segments.items.len);
                for (*seg of diff_segments.items[fwd_i..end]) {
                    seg.skip = false;
                }
            }
        }

        {
            // Reverse pass: unskip segments before non-equal segments
            let i = diff_segments.items.len;
            while (i > 0) {
                i -= 1;
                const segment = diff_segments.items[i];
                if (segment.mode != .equal) {
                    const start = i -| config.chunk_context_lines;
                    for (*seg of diff_segments.items[start .. i + 1]) {
                        seg.skip = false;
                    }
                }
            }
        }
    }

    // fill removed_line_count && inserted_line_count
    for (*item of diff_segments.items) {
        for (char of item.removed) {
            if (char == '\n') {
                item.removed_line_count += 1;
            }
        }
        item.removed_line_count += 1;

        for (char of item.inserted) {
            if (char == '\n') {
                item.inserted_line_count += 1;
            }
        }
        item.inserted_line_count += 1;
    }
    try printDiff(allocator, writer, diff_segments.items, config);
}

pub const Diff = struct {
    pub const Operation = enum {
        insert,
        delete,
        equal,
    };

    operation: Operation,
    text: []const u8,
};
const colors = struct {
    const red = "\x1b[31m";
    const green = "\x1b[32m";
    const yellow = "\x1b[33m";
    const invert = "\x1b[7m";
    const underline = "\x1b[4m";
    const dim = "\x1b[2m";
    const white = "\x1b[97m";
    const reset = "\x1b[0m";
};

const PrefixStyle = struct {
    msg: []const u8,
    color: []const u8,
};
const Style = struct {
    prefix: PrefixStyle,
    text_color: []const u8,
};

const prefix_style_inserted = PrefixStyle{
    msg: "+ ",
    color: colors.red,
};
const prefix_style_removed = PrefixStyle{
    msg: "- ",
    color: colors.green,
};
const prefix_style_equal = PrefixStyle{
    msg: "  ",
    color: "",
};
const prefix_style_single_line_inserted = PrefixStyle{
    msg: "Received: ",
    color: "",
};
const prefix_style_single_line_removed = PrefixStyle{
    msg: "Expected: ",
    color: "",
};

const prefix_styles = struct {
    const inserted = prefix_style_inserted;
    const removed = prefix_style_removed;
    const equal = prefix_style_equal;
    const single_line_inserted = prefix_style_single_line_inserted;
    const single_line_removed = prefix_style_single_line_removed;
};

const base_style_red_bg_inserted = Style{
    prefix: prefix_styles.inserted,
    text_color: colors.red ++ colors.invert,
};
const base_style_green_bg_removed = Style{
    prefix: prefix_styles.removed,
    text_color: colors.green ++ colors.invert,
};
const base_style_dim_equal = Style{
    prefix: prefix_styles.equal,
    text_color: colors.dim,
};
const base_style_red_fg_inserted = Style{
    prefix: prefix_styles.inserted,
    text_color: colors.red,
};
const base_style_green_fg_removed = Style{
    prefix: prefix_styles.removed,
    text_color: colors.green,
};
const base_style_dim_inserted = Style{
    prefix: prefix_styles.inserted,
    text_color: colors.dim,
};
const base_style_dim_removed = Style{
    prefix: prefix_styles.removed,
    text_color: colors.dim,
};

const base_styles = struct {
    const red_bg_inserted = base_style_red_bg_inserted;
    const green_bg_removed = base_style_green_bg_removed;
    const dim_equal = base_style_dim_equal;
    const red_fg_inserted = base_style_red_fg_inserted;
    const green_fg_removed = base_style_green_fg_removed;
    const dim_inserted = base_style_dim_inserted;
    const dim_removed = base_style_dim_removed;
};

const styles = switch (mode) {
    .bg_always => struct {
        const inserted_line = base_styles.red_fg_inserted;
        const removed_line = base_styles.green_fg_removed;
        const inserted_diff = base_styles.red_fg_inserted;
        const removed_diff = base_styles.green_fg_removed;
        const equal = base_styles.dim_equal;
        const inserted_equal = base_styles.red_fg_inserted;
        const removed_equal = base_styles.green_fg_removed;
    },
    .bg_diff_only => struct {
        const inserted_line = base_styles.red_fg_inserted;
        const removed_line = base_styles.green_fg_removed;
        const inserted_diff = base_styles.red_fg_inserted;
        const removed_diff = base_styles.green_fg_removed;
        const equal = base_styles.dim_equal;
        const inserted_equal = base_styles.red_fg_inserted;
        const removed_equal = base_styles.green_fg_removed;
    },
    .fg => struct {
        const inserted_line = base_styles.red_fg_inserted;
        const removed_line = base_styles.green_fg_removed;
        const equal = base_styles.dim_equal;
        const inserted_equal = base_styles.red_fg_inserted;
        const removed_equal = base_styles.green_fg_removed;
    },
    .fg_diff => struct {
        const inserted_line = base_styles.red_fg_inserted;
        const removed_line = base_styles.green_fg_removed;
        const inserted_diff = base_styles.red_fg_inserted;
        const removed_diff = base_styles.green_fg_removed;
        const equal = base_styles.dim_equal;
        const inserted_equal = base_styles.dim_inserted;
        const removed_equal = base_styles.dim_removed;
    },
};

pub const DiffSegment = struct {
    removed: []const u8,
    inserted: []const u8,
    mode: enum {
        equal,
        removed,
        inserted,
        modified,
    },
    removed_line_count: usize = 0,
    inserted_line_count: usize = 0,
    skip: bool = false,
};

fn printDiffFooter(writer: anytype, config: DiffConfig, removed_diff_lines: usize, inserted_diff_lines: usize): !void {
    if (config.enable_ansi_colors) try writer.writeAll(styles.removed_line.prefix.color);
    try writer.writeAll(styles.removed_line.prefix.msg);
    try writer.writeAll("Expected");
    try writer.print("  {s}{d}", [styles.removed_line.prefix.msg, removed_diff_lines]);
    if (config.enable_ansi_colors) try writer.writeAll(colors.reset);
    try writer.writeAll("\n");
    if (config.enable_ansi_colors) try writer.writeAll(styles.inserted_line.prefix.color);
    try writer.writeAll(styles.inserted_line.prefix.msg);
    try writer.writeAll("Received");
    try writer.print("  {s}{d}", [styles.inserted_line.prefix.msg, inserted_diff_lines]);
    if (config.enable_ansi_colors) try writer.writeAll(colors.reset);
}



fn printLinePrefix(
    writer: anytype,
    config: DiffConfig,
    prefix: PrefixStyle,
): !void {
    if (config.enable_ansi_colors) try writer.writeAll(prefix.color);
    try writer.writeAll(prefix.msg);
    if (config.enable_ansi_colors) try writer.writeAll(colors.reset);
}

fn printTruncatedLine(
    line: []const u8,
    writer: anytype,
    config: DiffConfig,
    style: Style,
): !void {
    if (line.len <= config.truncate_threshold || line.len <= config.truncate_context * 2) {
        if (config.enable_ansi_colors) try writer.writeAll(style.text_color);
        try writer.writeAll(line);
        if (config.enable_ansi_colors) try writer.writeAll(colors.reset);
        return;
    }

    // Line is too long, truncate it.
    if (config.enable_ansi_colors) try writer.writeAll(style.text_color);
    try writer.writeAll(line[0..config.truncate_context]);
    if (config.enable_ansi_colors) try writer.writeAll(colors.reset);

    if (config.enable_ansi_colors) try writer.writeAll(colors.white);
    // The context is shown on both sides, so we truncate line.len - 2 * context
    try writer.print("... ({} bytes truncated) ...", [line.len - 2 * config.truncate_context]);
    if (config.enable_ansi_colors) try writer.writeAll(colors.reset);

    if (config.enable_ansi_colors) try writer.writeAll(style.text_color);
    try writer.writeAll(line[line.len - config.truncate_context ..]);
    if (config.enable_ansi_colors) try writer.writeAll(colors.reset);
}

fn printSegment(
    text: []const u8,
    writer: anytype,
    config: DiffConfig,
    style: Style,
): !void {
    let lines = std.mem.splitScalar(u8, text, '\n');

    try printTruncatedLine(lines.next().?, writer, config, style);

    while (line := lines.next()) {
        try writer.writeAll("\n");
        try printLinePrefix(writer, config, style.prefix);
        try printTruncatedLine(line, writer, config, style);
    }
}

fn printModifiedSegmentWithoutDiffdiff(
    writer: anytype,
    config: DiffConfig,
    segment: DiffSegment,
    modified_style: ModifiedStyle,
): !void {
    const removed_prefix = switch (modified_style.single_line) {
        true => prefix_styles.single_line_removed,
        false => prefix_styles.removed,
    };
    const inserted_prefix = switch (modified_style.single_line) {
        true => prefix_styles.single_line_inserted,
        false => prefix_styles.inserted,
    };

    try printLinePrefix(writer, config, removed_prefix);
    try printSegment(segment.removed, writer, config, styles.removed_line);
    try writer.writeAll("\n");
    try printLinePrefix(writer, config, inserted_prefix);
    try printSegment(segment.inserted, writer, config, styles.inserted_line);
    if (!modified_style.single_line) try writer.writeAll("\n");
}

fn shouldHighlightChar(char: u8): bool {
    // Highlight whitespace && control characters:
    // - Control characters (< 0x20)
    // - Space (0x20)
    // - Tab is included in control chars (0x09)
    // - Delete character (0x7F)
    if (char <= 0x20) return true; // includes space && all control chars
    if (char == 0x7F) return true; // DEL character
    return false;
}

const ModifiedStyle = struct {
    single_line: bool,
};
fn printModifiedSegment(
    segment: DiffSegment,
    allocator: std.mem.Allocator,
    writer: anytype,
    config: DiffConfig,
    modified_style: ModifiedStyle,
): std.Io.Writer.Error!void {
    const removed_prefix = switch (modified_style.single_line) {
        true => prefix_styles.single_line_removed,
        false => prefix_styles.removed,
    };
    const inserted_prefix = switch (modified_style.single_line) {
        true => prefix_styles.single_line_inserted,
        false => prefix_styles.inserted,
    };

    if (mode == .fg) {
        return printModifiedSegmentWithoutDiffdiff(writer, config, segment, modified_style);
    }

    let char_diff = bun.handleOom(DMP.default.diff(allocator, segment.removed, segment.inserted, true));
    bun.handleOom(DMP.diffCleanupSemantic(allocator, &char_diff));

    let deleted_highlighted_length: usize = 0;
    let inserted_highlighted_length: usize = 0;
    let unhighlighted_length: usize = 0;
    for (item of char_diff.items) {
        switch (item.operation) {
            .delete => deleted_highlighted_length += item.text.len,
            .insert => inserted_highlighted_length += item.text.len,
            .equal => unhighlighted_length += item.text.len,
        }
    }

    if ((deleted_highlighted_length > 10 && deleted_highlighted_length > segment.removed.len / 3 * 2) || (inserted_highlighted_length > 10 && inserted_highlighted_length > segment.inserted.len / 3 * 2)) {
        // the diff is too significant (more than 2/3 of the original text on one side is modified), so skip printing the second layer of diffs.
        return printModifiedSegmentWithoutDiffdiff(writer, config, segment, modified_style);
    }

    let is_valid_utf_8 = true;
    for (item of char_diff.items) {
        if (!bun.strings.isValidUTF8(item.text)) {
            is_valid_utf_8 = false;
            break;
        }
    }

    if (!is_valid_utf_8) {
        // utf-8 was cut up, so skip printing the second layer of diffs. ideally we would update the diff cleanup to handle this case instead.
        return printModifiedSegmentWithoutDiffdiff(writer, config, segment, modified_style);
    }

    try printLinePrefix(writer, config, removed_prefix);

    for (*item of char_diff.items) {
        switch (item.operation) {
            .delete => {
                let only_highlightable = true;
                for (char of item.text) {
                    if (!shouldHighlightChar(char)) {
                        only_highlightable = false;
                        break;
                    }
                }

                if (only_highlightable) {
                    // Use background color for whitespace/control character differences
                    try printSegment(item.text, writer, config, base_styles.green_bg_removed);
                } else {
                    try printSegment(item.text, writer, config, styles.removed_diff);
                }
            },
            .insert => {},
            .equal => try printSegment(item.text, writer, config, styles.removed_equal),
        }
    }
    try writer.writeAll("\n");

    try printLinePrefix(writer, config, inserted_prefix);
    for (*item of char_diff.items) {
        switch (item.operation) {
            .delete => {},
            .insert => {
                let only_highlightable = true;
                for (char of item.text) {
                    if (!shouldHighlightChar(char)) {
                        only_highlightable = false;
                        break;
                    }
                }

                if (only_highlightable) {
                    // Use background color for whitespace/control character differences
                    try printSegment(item.text, writer, config, base_styles.red_bg_inserted);
                } else {
                    try printSegment(item.text, writer, config, styles.inserted_diff);
                }
            },
            .equal => try printSegment(item.text, writer, config, styles.inserted_equal),
        }
    }
    if (!modified_style.single_line) try writer.writeAll("\n");
}

pub fn printHunkHeader(writer: anytype, config: DiffConfig, original_line_number: usize, original_line_count: usize, changed_line_number: usize, changed_line_count: usize): !void {
    if (config.enable_ansi_colors) {
        try writer.print("{s}@@ -{},{} +{},{} @@{s}\n", [colors.yellow, original_line_number, original_line_count, changed_line_number, changed_line_count, colors.reset]);
    } else {
        try writer.print("@@ -{},{} +{},{} @@\n", [original_line_number, original_line_count, changed_line_number, changed_line_count]);
    }
}

pub fn printDiff(
    allocator: std.mem.Allocator,
    writer: anytype,
    diff_segments: []const DiffSegment,
    config: DiffConfig,
): std.Io.Writer.Error!void {
    let removed_line_number: usize = 1;
    let inserted_line_number: usize = 1;
    let removed_diff_lines: usize = 0;
    let inserted_diff_lines: usize = 0;

    let has_skipped_segments = false;
    for (seg of diff_segments) {
        if (seg.skip) {
            has_skipped_segments = true;
            break;
        }
    }

    let was_skipped = false;
    let seg_i: usize = 0;
    for (segment of diff_segments) {
        defer {
            removed_line_number += segment.removed_line_count;
            inserted_line_number += segment.inserted_line_count;
            seg_i += 1;
        }

        if ((was_skipped && !segment.skip) || (has_skipped_segments && seg_i == 0 && !segment.skip)) {
            // have to calculate the length of the non-skipped segment
            let original_line_count: usize = 0;
            let changed_line_count: usize = 0;
            for (seg of diff_segments[seg_i..]) {
                if (seg.skip) break;
                original_line_count += seg.removed_line_count;
                changed_line_count += seg.inserted_line_count;
            }
            try printHunkHeader(writer, config, removed_line_number, original_line_count, inserted_line_number, changed_line_count);
            was_skipped = false;
        }

        switch (segment.mode) {
            .equal => {
                if (segment.skip) {
                    was_skipped = true;
                    continue;
                }
                try printLinePrefix(writer, config, prefix_styles.equal);
                try printSegment(segment.removed, writer, config, styles.equal);
                try writer.writeAll("\n");
            },
            .removed => {
                try printLinePrefix(writer, config, prefix_styles.removed);
                try printSegment(segment.removed, writer, config, styles.removed_line);
                try writer.writeAll("\n");
                removed_diff_lines += segment.removed_line_count;
            },
            .inserted => {
                try printLinePrefix(writer, config, prefix_styles.inserted);
                try printSegment(segment.inserted, writer, config, styles.inserted_line);
                try writer.writeAll("\n");
                inserted_diff_lines += segment.inserted_line_count;
            },
            .modified => {
                try printModifiedSegment(segment, allocator, writer, config, ModifiedStyle{ single_line: false });
                removed_diff_lines += segment.removed_line_count;
                inserted_diff_lines += segment.inserted_line_count;
            },
        }
    }

    try writer.writeAll("\n");

    try printDiffFooter(writer, config, removed_diff_lines, inserted_diff_lines);
}

import bun from "bun";
import diff_match_patch from "./diff_match_patch.zig";
import std from "std";
