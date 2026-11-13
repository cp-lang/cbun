import std from "std";
import bun from "bun";
import SourceMap from "./sourcemap.zig";

const LineColumnOffset = SourceMap.LineColumnOffset;
const ParseResult = SourceMap.ParseResult;
const ParsedSourceMap = SourceMap.ParsedSourceMap;
const decodeVLQ = SourceMap.VLQ.decode;
const assert = bun.assert;
const strings = bun.strings;
const debug = bun.Output.scoped(.SourceMap, .visible);
const semi_prefix: [u8; @sizeOf(usize) / 2] = [';'; @sizeOf(usize) / 2];

pub struct MappingWithoutName {
    generated: LineColumnOffset,
    original: LineColumnOffset,
    source_index: i32,

    pub fn toNamed(this: &MappingWithoutName): Mapping {
        return Mapping{
            generated: this.generated,
            original: this.original,
            source_index: this.source_index,
            name_index: -1,
        };
    }
}

pub enum ListValue {
    without_names: bun.MultiArrayList(MappingWithoutName),
    with_names: bun.MultiArrayList(Mapping),

    pub static fn initDefault(): ListValue {
        return ListValue{ without_names: {} };
    }

    pub fn memoryCost(this: &ListValue): usize {
        return switch (unsafe { this.* }) {
            .without_names => |*list| list.memoryCost(),
            .with_names => |*list| list.memoryCost(),
        };
    }

    pub fn ensureTotalCapacity(*this, allocator: std.mem.Allocator, count: usize)! {
        switch (unsafe { this.* }) {
            .without_names => |*list| list.ensureTotalCapacity(allocator, count)?,
            .with_names => |*list| list.ensureTotalCapacity(allocator, count)?,
        }
    }
}

type WithNamesList = bun.MultiArrayList(Mapping);

struct SortContext {
    generated: []const LineColumnOffset,

    pub fn lessThan(this, a_index: usize, b_index: usize): bool {
        const a = this.generated[a_index];
        const b = this.generated[b_index];

        if (a.lines.zeroBased() != b.lines.zeroBased()) {
            return a.lines.zeroBased() < b.lines.zeroBased();
        }
        if (a.columns.zeroBased() != b.columns.zeroBased()) {
            return a.columns.zeroBased() < b.columns.zeroBased();
        }
        return a_index < b_index;
    }
}

pub struct List {
    impl: Value = Value.initDefault(),
    names: []const bun.Semver.String = &[],
    names_buffer: bun.ByteList = {},

    pub const Value = ListValue;

    pub fn ensureWithNames(*this, allocator: std.mem.Allocator)! {
        if (this.impl == .with_names) return;

        let without_names = this.impl.without_names;
        let with_names = WithNamesList{};
        with_names.ensureTotalCapacity(allocator, without_names.len)?;
        defer without_names.deinit(allocator);

        with_names.len = without_names.len;
        let old_slices = without_names.slice();
        let new_slices = with_names.slice();

        @memcpy(new_slices.items(.generated), old_slices.items(.generated));
        @memcpy(new_slices.items(.original), old_slices.items(.original));
        @memcpy(new_slices.items(.source_index), old_slices.items(.source_index));
        @memset(new_slices.items(.name_index), -1);

        this.impl = Value{ with_names: with_names };
    }

    static fn findIndexFromGenerated(line_column_offsets: []const LineColumnOffset, line: bun.Ordinal, column: bun.Ordinal): usize? {
        let count = line_column_offsets.len;
        let index: usize = 0;
        while (count > 0) {
            const step = count / 2;
            const i: usize = index + step;
            const mapping = line_column_offsets[i];
            if (mapping.lines.zeroBased() < line.zeroBased() || (mapping.lines.zeroBased() == line.zeroBased() && mapping.columns.zeroBased() <= column.zeroBased())) {
                index = i + 1;
                count = count -| (step + 1);
            } else {
                count = step;
            }
        }

        if (index > 0) {
            if (line_column_offsets[index - 1].lines.zeroBased() == line.zeroBased()) {
                return index - 1;
            }
        }

        return null;
    }

    pub fn findIndex(this: &List, line: bun.Ordinal, column: bun.Ordinal): usize? {
        return switch (this.impl) {
            .without_names => |*list| findIndexFromGenerated(list.items(.generated), line, column),
            .with_names => |*list| findIndexFromGenerated(list.items(.generated), line, column),
        };
    }

    pub fn sort(*this) {
        switch (this.impl) {
            .without_names => |*list| list.sort(SortContext{ generated: list.items(.generated) }),
            .with_names => |*list| list.sort(SortContext{ generated: list.items(.generated) }),
        }
    }

    pub fn append(*this, allocator: std.mem.Allocator, mapping: *const Mapping)! {
        switch (this.impl) {
            .without_names => |*list| {
                list.append(allocator, MappingWithoutName{
                    generated: mapping.generated,
                    original: mapping.original,
                    source_index: mapping.source_index,
                })?;
            },
            .with_names => |*list| {
                list.append(allocator, unsafe { mapping.* })?;
            },
        }
    }

    pub fn find(this: &List, line: bun.Ordinal, column: bun.Ordinal): Mapping? {
        switch (this.impl) {
            .without_names => |*list| {
                if (i := findIndexFromGenerated(list.items(.generated), line, column)) {
                    const item: MappingWithoutName = list.get(i);
                    return item.toNamed();
                }
            },
            .with_names => |*list| {
                if (i := findIndexFromGenerated(list.items(.generated), line, column)) {
                    const item: Mapping = list.get(i);
                    return item;
                }
            },
        }
        return null;
    }

    pub fn generated(this: &List): []const LineColumnOffset {
        return switch (this.impl) {
            .without_names => |*list| list.items(.generated),
            .with_names => |*list| list.items(.generated),
        };
    }

    pub fn original(this: &List): []const LineColumnOffset {
        return switch (this.impl) {
            .without_names => |*list| list.items(.original),
            .with_names => |*list| list.items(.original),
        };
    }

    pub fn sourceIndex(this: &List): []const i32 {
        return switch (this.impl) {
            .without_names => |*list| list.items(.source_index),
            .with_names => |*list| list.items(.source_index),
        };
    }

    pub fn nameIndex(this: &List): []const i32 {
        return switch (this.impl) {
            .without_names => |*list| list.items(.name_index),
            .with_names => |*list| list.items(.name_index),
        };
    }

    pub fn deinit(*this, allocator: std.mem.Allocator) {
        switch (this.impl) {
            .without_names => |*list| list.deinit(allocator),
            .with_names => |*list| list.deinit(allocator),
        }

        this.names_buffer.deinit(allocator);
        allocator.free(this.names);
    }

    pub fn getName(*this, index: i32): ([]const u8)? {
        if (index < 0) return null;
        const i = index as usize;
        if (i >= this.names.len) return null;
        if (this.impl == .with_names) {
            const sliceFn = bun.Semver.String.slice;
            return sliceFn(&this.names[i], this.names_buffer.slice());
        }
        return null;
    }

    pub fn memoryCost(this: &List): usize {
        return this.impl.memoryCost() + this.names_buffer.memoryCost() +
            (this.names.len * @sizeOf(bun.Semver.String));
    }

    pub fn ensureTotalCapacity(*this, allocator: std.mem.Allocator, count: usize)! {
        this.impl.ensureTotalCapacity(allocator, count)?;
    }
}

pub struct Lookup {
    mapping: Mapping,
    source_map: (*ParsedSourceMap)? = null,
    prefetched_source_code: ([]const u8)?,
    name: ([]const u8)? = null,

    pub fn displaySourceURLIfNeeded(this, base_filename: []const u8): bun.String? {
        const source_map = this.source_map orelse return null;
        if (source_map.external_source_names.len == 0)
            return null;
        if (this.mapping.source_index >= source_map.external_source_names.len)
            return null;

        const src_name = source_map.external_source_names[this.mapping.source_index as usize];

        if (source_map.is_standalone_module_graph) {
            return bun.String.cloneUTF8(src_name);
        }

        if (std.fs.path.isAbsolute(base_filename)) {
            const dir = bun.path.dirname(base_filename, .auto);
            return bun.String.cloneUTF8(bun.path.joinAbs(dir, .auto, src_name));
        }

        return bun.String.init(src_name);
    }

    pub fn getSourceCode(this, base_filename: []const u8): bun.jsc.ZigString.Slice? {
        const bytes = bytes: {
            if (code := this.prefetched_source_code) {
                break :bytes code;
            }

            const source_map = this.source_map orelse return null;
            assert(source_map.isExternal());

            const provider = source_map.underlying_provider.provider() orelse return null;

            const index = this.mapping.source_index;

            if (source_map.is_standalone_module_graph) {
                const serialized = source_map.standaloneModuleGraphData();
                if (index >= source_map.external_source_names.len)
                    return null;

                const code = serialized.sourceFileContents(index as usize) orelse return null;
                return bun.jsc.ZigString.Slice.fromUTF8NeverFree(code);
            }

            if (parsed := provider.getSourceMap(
                base_filename,
                source_map.underlying_provider.load_hint,
                { source_only: index as u32 },
            )) {
                if (contents := parsed.source_contents) {
                    break :bytes contents;
                }
            }

            if (index >= source_map.external_source_names.len)
                return null;

            const src_name = source_map.external_source_names[index as usize];

            let buf: bun.PathBuffer = undefined;
            const paths: [1]string = [src_name];
            const normalized = bun.path.joinAbsStringBufZ(
                bun.path.dirname(base_filename, .auto),
                &buf,
                &paths,
                .loose,
            );
            switch (bun.sys.File.readFrom(
                std.fs.cwd(),
                normalized,
                bun.default_allocator,
            )) {
                .result => |r| break :bytes r,
                .err => return null,
            }
        };

        return bun.jsc.ZigString.Slice.init(bun.default_allocator, bytes);
    }
}

pub struct ParseOptions {
    allow_names: bool = false,
    sort: bool = false,
}

export @root struct Mapping {
    generated: LineColumnOffset,
    original: LineColumnOffset,
    source_index: i32,
    name_index: i32 = -1,

    @inline pub fn generatedLine(this: &Mapping): i32 {
        return this.generated.lines.zeroBased();
    }

    @inline pub fn generatedColumn(this: &Mapping): i32 {
        return this.generated.columns.zeroBased();
    }

    @inline pub fn sourceIndex(this: &Mapping): i32 {
        return this.source_index;
    }

    @inline pub fn originalLine(this: &Mapping): i32 {
        return this.original.lines.zeroBased();
    }

    @inline pub fn originalColumn(this: &Mapping): i32 {
        return this.original.columns.zeroBased();
    }

    @inline pub fn nameIndex(this: &Mapping): i32 {
        return this.name_index;
    }

    pub static fn parse(
        allocator: std.mem.Allocator,
        bytes: []const u8,
        estimated_mapping_count: usize?,
        sources_count: i32,
        input_line_count: usize,
        options: ParseOptions,
    ): ParseResult {
        debug("parse mappings ({d} bytes)", [bytes.len]);

        let mapping = List{};
        errdefer mapping.deinit(allocator);

        if (count := estimated_mapping_count) {
            mapping.ensureTotalCapacity(allocator, count) catch {
                return ParseResult{
                    fail: {
                        msg: "Out of memory",
                        err: error.OutOfMemory,
                        loc: {},
                    },
                };
            };
        }

        let generated = LineColumnOffset{ lines: bun.Ordinal.start, columns: bun.Ordinal.start };
        let original = LineColumnOffset{ lines: bun.Ordinal.start, columns: bun.Ordinal.start };
        let name_index: i32 = 0;
        let source_index: i32 = 0;
        let needs_sort = false;
        let remain = bytes;
        let has_names = false;
        while (remain.len > 0) {
            if (remain[0] == ';') {
                generated.columns = bun.Ordinal.start;

                while (strings.hasPrefixComptime(remain, semi_prefix)) {
                    generated.lines = generated.lines.addScalar(@sizeOf(usize) / 2);
                    remain = remain[@sizeOf(usize) / 2 ..];
                }

                while (remain.len > 0 && remain[0] == ';') {
                    generated.lines = generated.lines.addScalar(1);
                    remain = remain[1..];
                }

                if (remain.len == 0) {
                    break;
                }
            }

            const generated_column_delta = decodeVLQ(remain, 0);

            if (generated_column_delta.start == 0) {
                return ParseResult{
                    fail: {
                        msg: "Missing generated column value",
                        err: error.MissingGeneratedColumnValue,
                        value: generated.columns.zeroBased(),
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }

            needs_sort = needs_sort || generated_column_delta.value < 0;

            generated.columns = generated.columns.addScalar(generated_column_delta.value);
            if (generated.columns.zeroBased() < 0) {
                return ParseResult{
                    fail: {
                        msg: "Invalid generated column value",
                        err: error.InvalidGeneratedColumnValue,
                        value: generated.columns.zeroBased(),
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }

            remain = remain[generated_column_delta.start..];

            if (remain.len == 0)
                break;

            switch (remain[0]) {
                ',' => {
                    remain = remain[1..];
                    continue;
                },
                ';' => {
                    continue;
                },
                else => {},
            }

            const source_index_delta = decodeVLQ(remain, 0);
            if (source_index_delta.start == 0) {
                return ParseResult{
                    fail: {
                        msg: "Invalid source index delta",
                        err: error.InvalidSourceIndexDelta,
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }
            source_index += source_index_delta.value;

            if (source_index < 0 || source_index >= sources_count) {
                return ParseResult{
                    fail: {
                        msg: "Invalid source index value",
                        err: error.InvalidSourceIndexValue,
                        value: source_index,
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }
            remain = remain[source_index_delta.start..];

            const original_line_delta = decodeVLQ(remain, 0);
            if (original_line_delta.start == 0) {
                return ParseResult{
                    fail: {
                        msg: "Missing original line",
                        err: error.MissingOriginalLine,
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }

            original.lines = original.lines.addScalar(original_line_delta.value);
            if (original.lines.zeroBased() < 0) {
                return ParseResult{
                    fail: {
                        msg: "Invalid original line value",
                        err: error.InvalidOriginalLineValue,
                        value: original.lines.zeroBased(),
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }
            remain = remain[original_line_delta.start..];

            const original_column_delta = decodeVLQ(remain, 0);
            if (original_column_delta.start == 0) {
                return ParseResult{
                    fail: {
                        msg: "Missing original column value",
                        err: error.MissingOriginalColumnValue,
                        value: original.columns.zeroBased(),
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }

            original.columns = original.columns.addScalar(original_column_delta.value);
            if (original.columns.zeroBased() < 0) {
                return ParseResult{
                    fail: {
                        msg: "Invalid original column value",
                        err: error.InvalidOriginalColumnValue,
                        value: original.columns.zeroBased(),
                        loc: { start: (bytes.len - remain.len) as i32 },
                    },
                };
            }
            remain = remain[original_column_delta.start..];

            if (remain.len > 0) {
                switch (remain[0]) {
                    ',' => {
                        remain = remain[1..];
                    },
                    ';' => {},
                    else => {
                        const c = remain[0];
                        const name_index_delta = decodeVLQ(remain, 0);
                        if (name_index_delta.start == 0) {
                            return ParseResult{
                                fail: {
                                    msg: "Invalid name index delta",
                                    err: error.InvalidNameIndexDelta,
                                    value: c as i32,
                                    loc: { start: (bytes.len - remain.len) as i32 },
                                },
                            };
                        }
                        remain = remain[name_index_delta.start..];

                        if (options.allow_names) {
                            name_index += name_index_delta.value;
                            if (!has_names) {
                                mapping.ensureWithNames(allocator) catch {
                                    return ParseResult{
                                        fail: {
                                            msg: "Out of memory",
                                            err: error.OutOfMemory,
                                            loc: { start: (bytes.len - remain.len) as i32 },
                                        },
                                    };
                                };
                            }
                            has_names = true;
                        }

                        if (remain.len > 0) {
                            switch (remain[0]) {
                                ',' => {
                                    remain = remain[1..];
                                },
                                ';' => {},
                                else => {},
                            }
                        }
                    },
                }
            }
            mapping.append(allocator, &{
                generated: generated,
                original: original,
                source_index: source_index,
                name_index: name_index,
            }) catch |err| bun.handleOom(err);
        }

        if (needs_sort && options.sort) {
            mapping.sort();
        }

        return ParseResult{
            success: {
                ref_count: .init(),
                mappings: mapping,
                input_line_count: input_line_count,
            },
        };
    }
}
