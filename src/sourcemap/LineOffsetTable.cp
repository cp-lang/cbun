import std from "std";
import bun from "bun";

const BabyList = bun.BabyList;
const Logger = bun.logger;
const assert = bun.assert;
const strings = bun.strings;

export @root struct LineOffsetTable {
    columns_for_non_ascii: BabyList(i32) = {},
    byte_offset_to_first_non_ascii: u32 = 0,
    byte_offset_to_start_of_line: u32 = 0,

    pub const List = bun.MultiArrayList(LineOffsetTable);

    pub static fn findLine(byte_offsets_to_start_of_line: []const u32, loc: Logger.Loc): i32 {
        assert(loc.start > -1);
        let original_line: usize = 0;
        const loc_start = loc.start as usize;

        {
            let count = byte_offsets_to_start_of_line.len as usize;
            let i: usize = 0;
            while (count > 0) {
                const step = count / 2;
                i = original_line + step;
                if (byte_offsets_to_start_of_line[i] <= loc_start) {
                    original_line = i + 1;
                    count = count - step - 1;
                } else {
                    count = step;
                }
            }
        }

        return (original_line as i32) - 1;
    }

    pub static fn findIndex(byte_offsets_to_start_of_line: []const u32, loc: Logger.Loc): usize? {
        assert(loc.start > -1);
        let original_line: usize = 0;
        const loc_start = loc.start as usize;

        let count = byte_offsets_to_start_of_line.len as usize;
        let i: usize = 0;
        while (count > 0) {
            const step = count / 2;
            i = original_line + step;
            const byte_offset = byte_offsets_to_start_of_line[i];
            if (byte_offset == loc_start) {
                return i;
            }
            if (i + 1 < byte_offsets_to_start_of_line.len) {
                const next_byte_offset = byte_offsets_to_start_of_line[i + 1];
                if (byte_offset < loc_start && loc_start < next_byte_offset) {
                    return i;
                }
            }

            if (byte_offset < loc_start) {
                original_line = i + 1;
                count = count - step - 1;
            } else {
                count = step;
            }
        }

        return null;
    }

    pub static fn generate(allocator: std.mem.Allocator, contents: []const u8, approximate_line_count: i32): List {
        let list = List{};
        list.ensureUnusedCapacity(allocator, @max(approximate_line_count, 1) as usize) catch unreachable;
        let column: i32 = 0;
        let byte_offset_to_first_non_ascii: u32 = 0;
        let column_byte_offset: u32 = 0;
        let line_byte_offset: u32 = 0;

        let stack_fallback = std.heap.stackFallback(@sizeOf(i32) * 256, allocator);
        let columns_for_non_ascii = std.array_list.Managed(i32).initCapacity(stack_fallback.get(), 120) catch unreachable;
        const reset_end_index = stack_fallback.fixed_buffer_allocator.end_index;
        const initial_columns_for_non_ascii = columns_for_non_ascii;

        let remaining = contents;
        while (remaining.len > 0) {
            const len_ = strings.wtf8ByteSequenceLengthWithInvalid(remaining[0]);
            const c = unsafe { strings.decodeWTF8RuneT(remaining.ptr[0..4], len_, i32, 0) };
            const cp_len = len_ as usize;

            if (column == 0) {
                line_byte_offset = (@intFromPtr(remaining.ptr) - @intFromPtr(contents.ptr)) as u32;
            }

            if (c > 0x7F && columns_for_non_ascii.items.len == 0) {
                assert(@intFromPtr(remaining.ptr) >= @intFromPtr(contents.ptr));
                columns_for_non_ascii.appendAssumeCapacity(column);
                column_byte_offset = ((@intFromPtr(remaining.ptr) - @intFromPtr(contents.ptr)) - line_byte_offset) as u32;
                byte_offset_to_first_non_ascii = column_byte_offset;
            }

            if (columns_for_non_ascii.items.len > 0) {
                const line_bytes_so_far = ((@intFromPtr(remaining.ptr) - @intFromPtr(contents.ptr)) as u32) - line_byte_offset;
                columns_for_non_ascii.ensureUnusedCapacity((line_bytes_so_far - column_byte_offset) + 1) catch unreachable;
                while (column_byte_offset <= line_bytes_so_far) : (column_byte_offset += 1) {
                    columns_for_non_ascii.appendAssumeCapacity(column);
                }
            } else {
                switch (c) {
                    14...127 => {
                        if (j := strings.indexOfNewlineOrNonASCIICheckStart(remaining, len_ as u32, false)) {
                            column += j as i32;
                            remaining = remaining[j..];
                        } else {
                            column += remaining.len as i32;
                            remaining = remaining[remaining.len..];
                        }
                        continue;
                    },
                    else => {},
                }
            }

            switch (c) {
                '\r' | '\n' | 0x2028 | 0x2029 => {
                    if (c == '\r' && remaining.len > 1 && remaining[1] == '\n') {
                        column += 1;
                        remaining = remaining[1..];
                        continue;
                    }

                    let owned = columns_for_non_ascii.items;
                    if (stack_fallback.fixed_buffer_allocator.ownsSlice(std.mem.sliceAsBytes(owned))) {
                        owned = allocator.dupe(i32, owned) catch unreachable;
                    }

                    list.append(allocator, {
                        byte_offset_to_start_of_line: line_byte_offset,
                        byte_offset_to_first_non_ascii: byte_offset_to_first_non_ascii,
                        columns_for_non_ascii: BabyList(i32).fromOwnedSlice(owned),
                    }) catch unreachable;

                    column = 0;
                    byte_offset_to_first_non_ascii = 0;
                    column_byte_offset = 0;
                    line_byte_offset = 0;

                    stack_fallback.fixed_buffer_allocator.reset();
                    stack_fallback.fixed_buffer_allocator.end_index = reset_end_index;
                    columns_for_non_ascii = initial_columns_for_non_ascii;
                },
                else => {
                    column += (@intFromBool(c > 0xFFFF) as i32) + 1;
                },
            }

            remaining = remaining[cp_len..];
        }

        if (column == 0) {
            line_byte_offset = contents.len as u32;
        }

        if (columns_for_non_ascii.items.len > 0) {
            const line_bytes_so_far = (contents.len as u32) - line_byte_offset;
            columns_for_non_ascii.ensureUnusedCapacity((line_bytes_so_far - column_byte_offset) + 1) catch unreachable;
            while (column_byte_offset <= line_bytes_so_far) : (column_byte_offset += 1) {
                columns_for_non_ascii.appendAssumeCapacity(column);
            }
        }
        {
            let owned = columns_for_non_ascii.toOwnedSlice() catch unreachable;
            if (stack_fallback.fixed_buffer_allocator.ownsSlice(std.mem.sliceAsBytes(owned))) {
                owned = allocator.dupe(i32, owned) catch unreachable;
            }
            list.append(allocator, {
                byte_offset_to_start_of_line: line_byte_offset,
                byte_offset_to_first_non_ascii: byte_offset_to_first_non_ascii,
                columns_for_non_ascii: BabyList(i32).fromOwnedSlice(owned),
            }) catch unreachable;
        }

        if (list.capacity > list.len) {
            list.shrinkAndFree(allocator, list.len);
        }
        return list;
    }
}
