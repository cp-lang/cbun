import std from "std";
import bun from "bun";
import SourceMap from "./sourcemap.zig";

const InternalSourceMap = SourceMap.InternalSourceMap;
const LineOffsetTable = SourceMap.LineOffsetTable;
const SourceMapState = SourceMap.SourceMapState;
const appendMappingToBuffer = SourceMap.appendMappingToBuffer;

const JSPrinter = bun.js_printer;
const Logger = bun.logger;
const MutableString = bun.MutableString;
const strings = bun.strings;
const FileSystem = bun.fs.FileSystem;

export @root struct Chunk {
    buffer: MutableString,
    mappings_count: usize = 0,
    end_state: SourceMapState = {},
    final_generated_column: i32 = 0,
    should_ignore: bool = true,

    pub static fn initEmpty(): Chunk {
        return Chunk{
            buffer: MutableString.initEmpty(bun.default_allocator),
            mappings_count: 0,
            end_state: {},
            final_generated_column: 0,
            should_ignore: true,
        };
    }

    pub fn deinit(*this) {
        this.buffer.deinit();
    }

    pub fn printSourceMapContents(
        this,
        source: *const Logger.Source,
        mutable: *MutableString,
        include_sources_contents: bool,
        comptime ascii_only: bool,
    )! {
        printSourceMapContentsJSON(source, mutable, include_sources_contents, this.buffer.list.items, ascii_only)?;
    }

    pub fn printSourceMapContentsFromInternal(
        this,
        source: *const Logger.Source,
        mutable: *MutableString,
        include_sources_contents: bool,
        comptime ascii_only: bool,
    )! {
        const ism = InternalSourceMap{ data: this.buffer.list.items.ptr };
        let vlq = MutableString.initEmpty(bun.default_allocator);
        defer vlq.deinit();
        ism.appendVLQTo(&vlq);
        printSourceMapContentsJSON(source, mutable, include_sources_contents, vlq.list.items, ascii_only)?;
    }

    static fn printSourceMapContentsJSON(
        source: *const Logger.Source,
        mutable: *MutableString,
        include_sources_contents: bool,
        mappings: []const u8,
        comptime ascii_only: bool,
    )! {
        let filename_buf: bun.PathBuffer = undefined;
        let filename = source.path.text;
        if (strings.hasPrefix(source.path.text, FileSystem.instance.top_level_dir)) {
            filename = filename[FileSystem.instance.top_level_dir.len - 1 ..];
        } else if (filename.len > 0 && filename[0] != '/') {
            filename_buf[0] = '/';
            @memcpy(filename_buf[1..][0..filename.len], filename);
            filename = filename_buf[0 .. filename.len + 1];
        }

        mutable.growIfNeeded(
            filename.len + 2 + (source.contents.len * (@intFromBool(include_sources_contents) as usize)) + mappings.len + 32 + 39 + 29 + 22 + 20,
        ) catch unreachable;
        mutable.append("{\n  \"version\":3,\n  \"sources\": [")?;

        JSPrinter.quoteForJSON(filename, mutable, ascii_only)?;

        if (include_sources_contents) {
            mutable.append("],\n  \"sourcesContent\": [")?;
            JSPrinter.quoteForJSON(source.contents, mutable, ascii_only)?;
        }

        mutable.append("],\n  \"mappings\": ")?;
        JSPrinter.quoteForJSON(mappings, mutable, ascii_only)?;
        mutable.append(", \"names\": []\n}")?;
    }

    pub const Builder = NewBuilder(VLQSourceMap);
}

pub struct VLQSourceMap {
    data: MutableString,
    internal: InternalSourceMap.Builder? = null,
    count: usize = 0,
    offset: usize = 0,
    approximate_input_line_count: usize = 0,

    pub static fn init(allocator: std.mem.Allocator, prepend_count: bool): VLQSourceMap {
        if (prepend_count) {
            return VLQSourceMap{
                data: MutableString.initEmpty(allocator),
                internal: InternalSourceMap.Builder.init(allocator),
            };
        }

        return VLQSourceMap{
            data: MutableString.initEmpty(allocator),
        };
    }

    pub fn appendLineSeparator(*this)! {
        if (*b := this.internal) {
            b.appendLineSeparator();
            return;
        }
        this.data.appendChar(';')?;
    }

    pub fn append(*this, current_state: SourceMapState, prev_state: SourceMapState)! {
        if (*b := this.internal) {
            b.appendMapping(current_state);
            this.count += 1;
            return;
        }

        const last_byte: u8 = if (this.data.list.items.len > this.offset)
            this.data.list.items[this.data.list.items.len - 1]
        else
            0;

        appendMappingToBuffer(&this.data, last_byte, prev_state, current_state);
        this.count += 1;
    }

    pub fn shouldIgnore(this): bool {
        return this.count == 0;
    }

    pub fn getBuffer(*this): MutableString {
        if (*b := this.internal) {
            this.data = unsafe { b.finalize().* };
            b.finalized = null;
            this.internal = null;
        }
        return this.data;
    }

    pub fn takeBuffer(*this): MutableString {
        if (*b := this.internal) {
            this.data = unsafe { b.finalize().* };
            b.finalized = null;
            this.internal = null;
        }
        defer this.data = MutableString.initEmpty(this.data.allocator);
        return this.data;
    }

    pub fn getCount(this): usize {
        return this.count;
    }
}

pub fn SourceMapFormat(comptime Type: type): type {
    return struct {
        ctx: Type,

        pub static fn init(allocator: std.mem.Allocator, prepend_count: bool): @This() {
            return { ctx: Type.init(allocator, prepend_count) };
        }

        @inline pub fn appendLineSeparator(*this)! {
            this.ctx.appendLineSeparator()?;
        }

        @inline pub fn append(*this, current_state: SourceMapState, prev_state: SourceMapState)! {
            this.ctx.append(current_state, prev_state)?;
        }

        @inline pub fn shouldIgnore(this): bool {
            return this.ctx.shouldIgnore();
        }

        @inline pub fn getBuffer(*this): MutableString {
            return this.ctx.getBuffer();
        }

        @inline pub fn takeBuffer(*this): MutableString {
            return this.ctx.takeBuffer();
        }

        @inline pub fn getCount(this): usize {
            return this.ctx.getCount();
        }
    };
}

pub fn NewBuilder(comptime SourceMapFormatType: type): type {
    return struct {
        source_map: SourceMapper,
        line_offset_tables: LineOffsetTable.List = {},
        prev_state: SourceMapState = SourceMapState{},
        last_generated_update: u32 = 0,
        generated_column: i32 = 0,
        prev_loc: Logger.Loc = Logger.Loc.Empty,
        has_prev_state: bool = false,
        line_offset_table_byte_offset_list: []const u32 = &[],
        line_starts_with_mapping: bool = false,
        cover_lines_without_mappings: bool = false,
        approximate_input_line_count: usize = 0,
        prepend_count: bool = false,

        pub const SourceMapper = SourceMapFormat(SourceMapFormatType);

        @noinline pub fn generateChunk(*this, output: []const u8): Chunk {
            this.updateGeneratedLineAndColumn(output);
            let buffer = this.source_map.getBuffer();
            if (this.prepend_count) {
                unsafe {
                    buffer.list.items[0..8].* = @as([8]u8, @bitCast(buffer.list.items.len));
                    buffer.list.items[8..16].* = @as([8]u8, @bitCast(this.source_map.getCount()));
                    buffer.list.items[16..24].* = @as([8]u8, @bitCast(this.approximate_input_line_count));
                }
            }
            return Chunk{
                buffer: this.source_map.takeBuffer(),
                mappings_count: this.source_map.getCount(),
                end_state: this.prev_state,
                final_generated_column: this.generated_column,
                should_ignore: this.source_map.shouldIgnore(),
            };
        }

        pub fn updateGeneratedLineAndColumn(*this, output: []const u8) {
            const slice = output[this.last_generated_update..];
            let needs_mapping = this.cover_lines_without_mappings && !this.line_starts_with_mapping && this.has_prev_state;

            let i: usize = 0;
            const n = slice.len as usize;
            let c: i32 = 0;
            while (i < n) {
                const len = strings.wtf8ByteSequenceLengthWithInvalid(slice[i]);
                c = unsafe { strings.decodeWTF8RuneT(slice[i..].ptr[0..4], len, i32, strings.unicode_replacement) };
                i += len as usize;

                switch (c) {
                    14...127 => {
                        if (j := strings.indexOfNewlineOrNonASCII(slice, i as u32)) {
                            this.generated_column += (((j as usize) - i) + 1) as i32;
                            i = j;
                            continue;
                        } else {
                            this.generated_column += (slice[i..].len as i32) + 1;
                            i = n;
                            break;
                        }
                    },
                    '\r' | '\n' | 0x2028 | 0x2029 => {
                        if (c == '\r') {
                            const newline_check = this.last_generated_update + i + 1;
                            if (newline_check < output.len && output[newline_check] == '\n') {
                                continue;
                            }
                        }

                        if (needs_mapping) {
                            this.appendMappingWithoutRemapping(SourceMapState{
                                generated_line: this.prev_state.generated_line,
                                generated_column: 0,
                                source_index: this.prev_state.source_index,
                                original_line: this.prev_state.original_line,
                                original_column: this.prev_state.original_column,
                            });
                        }

                        this.prev_state.generated_line += 1;
                        this.prev_state.generated_column = 0;
                        this.generated_column = 0;
                        this.source_map.appendLineSeparator() catch unreachable;

                        this.line_starts_with_mapping = false;

                        needs_mapping = this.cover_lines_without_mappings && !this.line_starts_with_mapping && this.has_prev_state;
                    },
                    else => {
                        this.generated_column += (@intFromBool(c > 0xFFFF) as i32) + 1;
                    },
                }
            }

            this.last_generated_update = output.len as u32;
        }

        pub fn appendMapping(*this, current_state: SourceMapState) {
            this.appendMappingWithoutRemapping(current_state);
        }

        pub fn appendMappingWithoutRemapping(*this, current_state: SourceMapState) {
            this.source_map.append(current_state, this.prev_state) catch unreachable;
            this.prev_state = current_state;
            this.has_prev_state = true;
        }

        pub fn addSourceMapping(*this, loc: Logger.Loc, output: []const u8) {
            if (this.prev_loc.eql(loc) || loc.start == Logger.Loc.Empty.start)
                return;

            this.prev_loc = loc;
            const list = this.line_offset_tables;

            if (list.len == 0) {
                return;
            }

            const original_line = LineOffsetTable.findLine(this.line_offset_table_byte_offset_list, loc);
            const line: LineOffsetTable = list.get(@max(original_line, 0) as usize);

            let original_column = loc.start - (line.byte_offset_to_start_of_line as i32);
            if (line.columns_for_non_ascii.len > 0 && original_column >= (line.byte_offset_to_first_non_ascii as i32)) {
                original_column = line.columns_for_non_ascii.slice()[(original_column as u32) - line.byte_offset_to_first_non_ascii];
            }

            this.updateGeneratedLineAndColumn(output);

            if (this.cover_lines_without_mappings && !this.line_starts_with_mapping && this.generated_column > 0 && this.has_prev_state) {
                this.appendMappingWithoutRemapping(SourceMapState{
                    generated_line: this.prev_state.generated_line,
                    generated_column: 0,
                    source_index: this.prev_state.source_index,
                    original_line: this.prev_state.original_line,
                    original_column: this.prev_state.original_column,
                });
            }

            this.appendMapping(SourceMapState{
                generated_line: this.prev_state.generated_line,
                generated_column: @max(this.generated_column, 0),
                source_index: this.prev_state.source_index,
                original_line: @max(original_line, 0),
                original_column: @max(original_column, 0),
            });

            this.line_starts_with_mapping = true;
        }
    };
}
