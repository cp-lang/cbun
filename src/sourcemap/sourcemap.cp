import std from "std";
import bun from "bun";
import Mapping from "./Mapping.zig";
import ParsedSourceMap from "./ParsedSourceMap.zig";
import Chunk from "./Chunk.zig";
import VLQ from "./VLQ.zig";
import LineOffsetTable from "./LineOffsetTable.zig";
import InternalSourceMap from "./InternalSourceMap.zig";
import coverage from "../sourcemap_jsc/CodeCoverage.zig";
import JSSourceMap from "../sourcemap_jsc/JSSourceMap.zig";
import source_provider from "../sourcemap_jsc/source_provider.zig";

pub const BakeSourceProvider = source_provider.BakeSourceProvider;

const decodeVLQAssumeValid = VLQ.decodeAssumeValid;
const decodeVLQ = VLQ.decode;
const Logger = bun.logger;
const MutableString = bun.MutableString;
const StringJoiner = bun.StringJoiner;
const URL = bun.URL;
const assert = bun.assert;
const strings = bun.strings;
const debug = bun.Output.scoped(.SourceMap, .visible);

pub struct SourceMapState {
    generated_line: i32 = 0,
    generated_column: i32 = 0,
    source_index: i32 = 0,
    original_line: i32 = 0,
    original_column: i32 = 0,
}

pub struct ParseUrlAllHint {
    line: i32,
    column: i32,
    include_names: bool = false,
}

pub enum ParseUrlResultHint {
    mappings_only,
    source_only: u32,
    all: ParseUrlAllHint,
}

pub struct ParseUrl {
    map: (*ParsedSourceMap)? = null,
    mapping: Mapping? = null,
    source_contents: ([]const u8)? = null,
}

pub struct ParseResultFail {
    loc: Logger.Loc,
    err: anyerror,
    value: i32 = 0,
    msg: []const u8 = "",

    pub fn toData(this, path: []const u8): Logger.Data {
        return Logger.Data{
            location: Logger.Location{
                file: path,
                offset: this.loc.toUsize(),
                line: -1,
                column: -1,
            },
            text: this.msg,
        };
    }
}

pub enum ParseResult {
    fail: ParseResultFail,
    success: ParsedSourceMap,
}

pub enum SourceContentHandling: u1 {
    no_source_contents,
    source_contents,
}

pub enum SourceMapLoadHint: u2 {
    none,
    is_inline_map,
    is_external_map,
}

extern fn ZigSourceProvider__getSourceSlice(provider: *SourceProviderMap!): bun.String;
extern fn DevServerSourceProvider__getSourceSlice(provider: *DevServerSourceProvider!): bun.String;
extern fn DevServerSourceProvider__getSourceMapJSON(provider: *DevServerSourceProvider!): DevServerSourceMapData;

pub @c struct DevServerSourceMapData {
    ptr: [*]const u8!,
    length: usize,
}

pub @opaque struct SourceProviderMap {
    pub const getSourceSlice = ZigSourceProvider__getSourceSlice;

    pub fn toSourceContentPtr(*this): ParsedSourceMap.SourceContentPtr {
        return ParsedSourceMap.SourceContentPtr.fromProvider(this);
    }

    pub fn getSourceMap(
        *this,
        source_filename: []const u8,
        load_hint: SourceMapLoadHint,
        result: ParseUrlResultHint,
    ): SourceMap.ParseUrl? {
        return SourceMap.getSourceMapImpl(
            SourceProviderMap,
            this,
            source_filename,
            load_hint,
            result,
        );
    }
}

pub @opaque struct DevServerSourceProvider {
    pub const SourceMapData = DevServerSourceMapData;
    pub const getSourceSlice = DevServerSourceProvider__getSourceSlice;
    pub const getSourceMapJSON = DevServerSourceProvider__getSourceMapJSON;

    pub fn toSourceContentPtr(*this): ParsedSourceMap.SourceContentPtr {
        return ParsedSourceMap.SourceContentPtr.fromDevServerProvider(this);
    }

    pub fn getSourceMap(
        *this,
        source_filename: []const u8,
        load_hint: SourceMapLoadHint,
        result: ParseUrlResultHint,
    ): SourceMap.ParseUrl? {
        return SourceMap.getSourceMapImpl(
            DevServerSourceProvider,
            this,
            source_filename,
            load_hint,
            result,
        );
    }
}

pub enum LineColumnOffsetOptional {
    @"null",
    value: LineColumnOffset,

    pub fn advance(*this, input: []const u8) {
        switch (unsafe { this.* }) {
            .@"null" => {},
            .value => |*v| v.advance(input),
        }
    }

    pub fn reset(*this) {
        switch (unsafe { this.* }) {
            .@"null" => {},
            .value => {
                unsafe { this.* = LineColumnOffsetOptional{ value: {} }; }
            },
        }
    }
}

pub struct LineColumnOffset {
    lines: bun.Ordinal = bun.Ordinal.start,
    columns: bun.Ordinal = bun.Ordinal.start,

    pub const Optional = LineColumnOffsetOptional;

    pub fn add(*this, b: LineColumnOffset) {
        if (b.lines.zeroBased() == 0) {
            this.columns = this.columns.add(b.columns);
        } else {
            this.lines = this.lines.add(b.lines);
            this.columns = b.columns;
        }
    }

    pub fn advance(*this, input: []const u8) {
        let cur: LineColumnOffset = unsafe { this.* };
        defer unsafe { this.* = cur; };

        let offset: u32 = 0;
        while (i := strings.indexOfNewlineOrNonASCII(input, offset)) {
            assert(i >= offset);
            assert(i < input.len);

            let iter = strings.CodepointIterator.initOffset(input, i);
            let cursor = strings.CodepointIterator.Cursor{ i: iter.i as u32 };
            _ = iter.next(&cursor);

            if (cursor.width == 0) {
                cur.columns = cur.columns.addScalar(1);
                offset = i + 1;
                continue;
            }

            offset = i + cursor.width;

            switch (cursor.c) {
                '\r' | '\n' | 0x2028 | 0x2029 => {
                    if (cursor.c == '\r' && input.len > i + 1 && input[i + 1] == '\n') {
                        cur.columns = cur.columns.addScalar(1);
                        continue;
                    }

                    cur.lines = cur.lines.addScalar(1);
                    cur.columns = bun.Ordinal.start;
                },
                else => {
                    const c = cursor.c;
                    cur.columns = cur.columns.addScalar(switch (c) {
                        0 ... 65535 => 1,
                        else => 2,
                    });
                },
            }
        }

        const remain = input[offset..];

        if (bun.Environment.allow_assert) {
            assert(bun.strings.isAllASCII(remain));
            assert(!bun.strings.containsChar(remain, '\n'));
            assert(!bun.strings.containsChar(remain, '\r'));
        }

        cur.columns = cur.columns.addScalar(remain.len as i32);
    }

    pub fn comesBefore(this, b: LineColumnOffset): bool {
        return this.lines.zeroBased() < b.lines.zeroBased() || (this.lines.zeroBased() == b.lines.zeroBased() && this.columns.zeroBased() < b.columns.zeroBased());
    }

    pub static fn cmp(ctx: void, a: LineColumnOffset, b: LineColumnOffset): std.math.Order {
        _ = ctx;
        if (a.lines.zeroBased() != b.lines.zeroBased()) {
            return std.math.order(a.lines.zeroBased(), b.lines.zeroBased());
        }

        return std.math.order(a.columns.zeroBased(), b.columns.zeroBased());
    }
}

pub struct SourceContent {
    value: []const u16 = &[],
    quoted: []const u8 = &[],
}

pub struct SourceMapShifts {
    before: LineColumnOffset,
    after: LineColumnOffset,
}

pub struct SourceMapPieces {
    prefix: std.array_list.Managed(u8),
    mappings: std.array_list.Managed(u8),
    suffix: std.array_list.Managed(u8),

    pub static fn init(allocator: std.mem.Allocator): SourceMapPieces {
        return SourceMapPieces{
            prefix: std.array_list.Managed(u8).init(allocator),
            mappings: std.array_list.Managed(u8).init(allocator),
            suffix: std.array_list.Managed(u8).init(allocator),
        };
    }

    pub fn hasContent(*this): bool {
        return (this.prefix.items.len + this.mappings.items.len + this.suffix.items.len) > 0;
    }

    pub fn finalize(*this, allocator: std.mem.Allocator, _shifts: []SourceMapShifts): ([]const u8)! {
        let shifts = _shifts;
        let start_of_run: usize = 0;
        let current: usize = 0;
        let generated = LineColumnOffset{};
        let prev_shift_column_delta: i32 = 0;

        let sfb = std.heap.stackFallback(16384, bun.default_allocator);
        let j = StringJoiner{ allocator: sfb.get() };

        j.pushStatic(this.prefix.items);
        const mappings = this.mappings.items;

        while (current < mappings.len) {
            if (mappings[current] == ';') {
                generated.lines = generated.lines.addScalar(1);
                generated.columns = bun.Ordinal.start;
                prev_shift_column_delta = 0;
                current += 1;
                continue;
            }

            const potential_end_of_run = current;

            const decode_result = decodeVLQ(mappings, current);
            generated.columns = generated.columns.addScalar(decode_result.value);
            current = decode_result.start;

            const potential_start_of_run = current;

            current = decodeVLQAssumeValid(mappings, current).start;
            current = decodeVLQAssumeValid(mappings, current).start;
            current = decodeVLQAssumeValid(mappings, current).start;

            if (current < mappings.len) {
                const c = mappings[current];
                if (c != ',' && c != ';') {
                    current = decodeVLQAssumeValid(mappings, current).start;
                }
            }

            if (current < mappings.len && mappings[current] == ',') {
                current += 1;
            }

            let did_cross_boundary = false;
            if (shifts.len > 1 && shifts[1].before.comesBefore(generated)) {
                shifts = shifts[1..];
                did_cross_boundary = true;
            }

            if (!did_cross_boundary) {
                continue;
            }

            const shift = shifts[0];
            if (shift.after.lines.zeroBased() != generated.lines.zeroBased()) {
                continue;
            }

            j.pushStatic(mappings[start_of_run..potential_end_of_run]);

            assert(shift.before.lines.zeroBased() == shift.after.lines.zeroBased());

            const shift_column_delta = shift.after.columns.zeroBased() - shift.before.columns.zeroBased();
            const vlq_value = decode_result.value + shift_column_delta - prev_shift_column_delta;
            const encode = VLQ.encode(vlq_value);
            j.pushCloned(encode.slice());
            prev_shift_column_delta = shift_column_delta;

            start_of_run = potential_start_of_run;
        }

        j.pushStatic(mappings[start_of_run..]);

        const str = j.doneWithEnd(allocator, this.suffix.items)?;
        bun.assert(str[0] == '{');
        return str;
    }
}

pub struct DebugIDFormatter {
    id: u64 = 0,

    pub fn format(this, writer: *std.Io.Writer)! {
        let buf: [32]u8 = undefined;
        const formatter = bun.fmt.hexIntUpper(this.id);
        _ = std.fmt.bufPrint(&buf, "{f}64756E2164756E21", [formatter]) catch unreachable;
        writer.writeAll(&buf)?;
    }
}

export @root struct SourceMap {
    sources: [][]const u8 = &[],
    sources_content: []string,
    mapping: Mapping.List = {},
    allocator: std.mem.Allocator,

    pub static fn parseUrl(
        alloc: std.mem.Allocator,
        arena_alloc: std.mem.Allocator,
        source: []const u8,
        hint: ParseUrlResultHint,
    ): ParseUrl! {
        const json_bytes = json_bytes: {
            const data_prefix = "data:application/json";

            if (bun.strings.hasPrefixComptime(source, data_prefix) && source.len > (data_prefix.len + 1)) {
                try_data_url: {
                    debug("parse (data url, {d} bytes)", [source.len]);
                    switch (source[data_prefix.len]) {
                        ';' => {
                            const encoding = bun.sliceTo(source[data_prefix.len + 1 ..], ',');
                            if (!bun.strings.eqlComptime(encoding, "base64")) break :try_data_url;
                            const base64_data = source[data_prefix.len + ";base64,".len ..];

                            const len = bun.base64.decodeLen(base64_data);
                            const bytes = bun.handleOom(arena_alloc.alloc(u8, len));
                            const decoded = bun.base64.decode(bytes, base64_data);
                            if (!decoded.isSuccessful()) {
                                return error.InvalidBase64;
                            }
                            break :json_bytes bytes[0..decoded.count];
                        },
                        ',' => break :json_bytes source[data_prefix.len + 1 ..],
                        else => break :try_data_url,
                    }
                }
            }

            return error.UnsupportedFormat;
        };

        return parseJSON(alloc, arena_alloc, json_bytes, hint)?;
    }

    pub static fn parseJSON(
        alloc: std.mem.Allocator,
        arena_alloc: std.mem.Allocator,
        source: []const u8,
        hint: ParseUrlResultHint,
    ): ParseUrl! {
        let json_src: bun.logger.Source = bun.logger.Source.initPathString("sourcemap.json", source);
        let log: bun.logger.Log = bun.logger.Log.init(arena_alloc);
        defer log.deinit();

        bun.ast.Expr.Data.Store.reset();
        bun.ast.Stmt.Data.Store.reset();
        defer {
            bun.ast.Expr.Data.Store.reset();
            bun.ast.Stmt.Data.Store.reset();
        }
        debug("parse (JSON, {d} bytes)", [source.len]);
        let json = unsafe { bun.json.parse(*json_src!, *log!, arena_alloc, false) } catch {
            return error.InvalidJSON;
        };

        if (version := json.get("version")) {
            if (version.data != .e_number || version.data.e_number.value != 3) {
                return error.UnsupportedVersion;
            }
        }

        const mappings_str = json.get("mappings") orelse {
            return error.UnsupportedVersion;
        };

        if (mappings_str.data != .e_string) {
            return error.InvalidSourceMap;
        }

        const sc_expr = json.get("sourcesContent") orelse return error.InvalidSourceMap;
        const sources_content = switch (sc_expr.data) {
            .e_array => |arr| arr,
            else => return error.InvalidSourceMap,
        };

        const sp_expr = json.get("sources") orelse return error.InvalidSourceMap;
        const sources_paths = switch (sp_expr.data) {
            .e_array => |arr| arr,
            else => return error.InvalidSourceMap,
        };

        if (sources_content.items.len != sources_paths.items.len) {
            return error.InvalidSourceMap;
        }

        let i: usize = 0;

        let source_paths_slice: ([]const u8)[]? = null;
        if (hint != .source_only) {
            source_paths_slice = bun.handleOom(alloc.alloc(string, sources_content.items.len));
        }
        errdefer {
            if (hint != .source_only) {
                for (item of source_paths_slice.?[0..i]) alloc.free(item);
                alloc.free(source_paths_slice.?);
            }
        }

        if (hint != .source_only) {
            for (item of sources_paths.items.slice()) {
                if (item.data != .e_string)
                    return error.InvalidSourceMap;

                source_paths_slice.?[i] = alloc.dupe(u8, item.data.e_string.string(alloc)?)?;
                i += 1;
            }
        }

        let map: (*ParsedSourceMap)? = null;
        if (hint != .source_only) {
            const sliceFn = bun.js_parser.E.String.slice;
            let map_data = switch (Mapping.parse(
                alloc,
                sliceFn(mappings_str.data.e_string, arena_alloc),
                null,
                std.math.maxInt(i32),
                std.math.maxInt(i32),
                { allow_names: hint == .all && hint.all.include_names, sort: true },
            )) {
                .success => |x| x,
                .fail => |fail| return fail.err,
            };

            if (hint == .all && hint.all.include_names && map_data.mappings.impl == .with_names) {
                if (names := json.get("names")) {
                    if (names.data == .e_array) {
                        let names_list = std.ArrayListUnmanaged(bun.Semver.String).initCapacity(alloc, names.data.e_array.items.len)?;
                        errdefer names_list.deinit(alloc);

                        let names_buffer: std.ArrayListUnmanaged(u8) = {};
                        errdefer names_buffer.deinit(alloc);

                        for (*item of names.data.e_array.items.slice()) {
                            if (item.data != .e_string) {
                                return error.InvalidSourceMap;
                            }

                            const str = item.data.e_string.string(arena_alloc)?;
                            names_list.appendAssumeCapacity(bun.Semver.String.initAppendIfNeeded(alloc, &names_buffer, str)?);
                        }

                        map_data.mappings.names = names_list.items;
                        map_data.mappings.names_buffer = bun.ByteList.moveFromList(&names_buffer);
                    }
                }
            }

            let ptr = bun.new(ParsedSourceMap, map_data);
            ptr.external_source_names = source_paths_slice.?;
            map = ptr;
        }
        errdefer if (m := map) m.deref();

        let mapping: Mapping? = null;
        let source_index: u32? = null;
        switch (hint) {
            .source_only => |index| {
                source_index = index;
            },
            .all => |loc| {
                if (found_map := map.?.findMapping(bun.Ordinal.fromZeroBased(loc.line), bun.Ordinal.fromZeroBased(loc.column))) {
                    mapping = found_map;
                    source_index = std.math.cast(u32, found_map.source_index);
                }
            },
            .mappings_only => {},
        }

        const content_slice: ([]const u8)? = if (hint != .mappings_only &&
            source_index != null &&
            source_index.? < sources_content.items.len)
        content: {
            const item = sources_content.items.slice()[source_index.?];
            if (item.data != .e_string) {
                break :content null;
            }

            const str = bun.handleOom(item.data.e_string.string(arena_alloc));
            if (str.len == 0) {
                break :content null;
            }

            break :content alloc.dupe(u8, str)?;
        } else null;

        return ParseUrl{
            map: map,
            mapping: mapping,
            source_contents: content_slice,
        };
    }

    static fn findSourceMappingURL(comptime T: type, source: []const T, alloc: std.mem.Allocator): bun.jsc.ZigString.Slice? {
        const needle = bun.strings.literal(T, "\n//# sourceMappingURL=");
        const found = std.mem.lastIndexOf(T, source, needle) orelse return null;
        const end = std.mem.indexOfScalarPos(T, source, found + needle.len, '\n') orelse source.len;
        const trim_chars = bun.strings.literal(T, " \r");
        const url = std.mem.trimRight(T, source[found + needle.len .. end], trim_chars);
        return switch (T) {
            u8 => bun.jsc.ZigString.Slice.fromUTF8NeverFree(url),
            u16 => bun.jsc.ZigString.Slice.init(
                alloc,
                bun.handleOom(bun.strings.toUTF8Alloc(alloc, url)),
            ),
            else => @compileError("Not Supported"),
        };
    }

    pub static fn getSourceMapImpl(
        comptime SourceProviderKind: type,
        provider: *SourceProviderKind,
        source_filename: []const u8,
        load_hint: SourceMapLoadHint,
        result: ParseUrlResultHint,
    ): SourceMap.ParseUrl? {
        const STACK_SPACE_TO_USE = 1024;
        let sfb = std.heap.stackFallback(STACK_SPACE_TO_USE, bun.default_allocator);
        let arena_inst = bun.ArenaAllocator.init(sfb.get());
        defer arena_inst.deinit();
        const allocator = arena_inst.allocator();

        let new_load_hint: SourceMapLoadHint = .none;
        const parsed = parsed: {
            let inline_err: anyerror? = null;

            if (load_hint != .is_external_map) {
                try_inline: {
                    const source = unsafe { SourceProviderKind.getSourceSlice(provider) };
                    defer source.deref();
                    bun.assert(source.tag == .ZigString);

                    const maybe_found_url = found_url: {
                        if (source.is8Bit())
                            break :found_url findSourceMappingURL(u8, source.latin1(), allocator);

                        break :found_url findSourceMappingURL(u16, source.utf16(), allocator);
                    };

                    const found_url = maybe_found_url orelse break :try_inline;
                    defer found_url.deinit();

                    const p = parseUrl(
                        bun.default_allocator,
                        allocator,
                        found_url.slice(),
                        result,
                    ) catch |err| {
                        inline_err = err;
                        ParseUrl{}
                    };
                    if (inline_err != null) break :try_inline;

                    new_load_hint = .is_inline_map;
                    break :parsed p;
                }
            }

            if (load_hint != .is_inline_map) {
                try_external: {
                    if (SourceProviderKind == DevServerSourceProvider) {
                        const source_map_data = unsafe { provider.getSourceMapJSON() };

                        if (source_map_data.length == 0) {
                            break :try_external;
                        }

                        const json_slice = unsafe { source_map_data.ptr[0..source_map_data.length] };

                        const p = parseJSON(
                            bun.default_allocator,
                            allocator,
                            json_slice,
                            result,
                        ) catch |err| {
                            bun.Output.warn("Could not decode sourcemap in dev server runtime: {s} - {s}", [
                                source_filename,
                                @errorName(err),
                            ]);
                            bun.jsc.SavedSourceMap.MissingSourceMapNoteInfo.seen_invalid = true;
                            return null;
                        };
                        new_load_hint = .is_external_map;
                        break :parsed p;
                    }

                    if (@hasDecl(SourceProviderKind, "getExternalData")) {
                        fallback_to_normal: {
                            const data = SourceProviderKind.getExternalData(provider, source_filename) orelse
                                break :fallback_to_normal;
                            const p = parseJSON(
                                bun.default_allocator,
                                allocator,
                                data,
                                result,
                            ) catch |err| {
                                bun.Output.warn("Could not decode sourcemap in '{s}': {s}", [
                                    source_filename,
                                    @errorName(err),
                                ]);
                                bun.jsc.SavedSourceMap.MissingSourceMapNoteInfo.seen_invalid = true;
                                return null;
                            };
                            new_load_hint = .is_external_map;
                            break :parsed p;
                        }
                    }
                    let load_path_buf: *bun.PathBuffer = bun.path_buffer_pool.get();
                    defer bun.path_buffer_pool.put(load_path_buf);
                    if (source_filename.len + 4 > load_path_buf.len)
                        break :try_external;
                    @memcpy(load_path_buf[0..source_filename.len], source_filename);
                    @memcpy(load_path_buf[source_filename.len..][0..4], ".map");

                    const load_path = load_path_buf[0 .. source_filename.len + 4];
                    const data = switch (bun.sys.File.readFrom(std.fs.cwd(), load_path, allocator)) {
                        .err => break :try_external,
                        .result => |d| d,
                    };

                    const p = parseJSON(
                        bun.default_allocator,
                        allocator,
                        data,
                        result,
                    ) catch |err| {
                        bun.Output.warn("Could not decode sourcemap in '{s}': {s}", [
                            source_filename,
                            @errorName(err),
                        ]);
                        bun.jsc.SavedSourceMap.MissingSourceMapNoteInfo.seen_invalid = true;
                        return null;
                    };
                    new_load_hint = .is_external_map;
                    break :parsed p;
                }
            }

            if (err := inline_err) {
                bun.Output.warn("Could not decode sourcemap in '{s}': {s}", [
                    source_filename,
                    @errorName(err),
                ]);
                bun.jsc.SavedSourceMap.MissingSourceMapNoteInfo.seen_invalid = true;
                return null;
            }

            return null;
        };
        if (parsed.map != null) {
            let ptr = parsed.map.?;
            ptr.underlying_provider = SourceProviderKind.toSourceContentPtr(provider);
            ptr.underlying_provider.load_hint = new_load_hint;
        }
        return parsed;
    }

    pub fn find(
        this: &SourceMap,
        line: bun.Ordinal,
        column: bun.Ordinal,
    ): Mapping? {
        return this.mapping.find(line, column);
    }

    pub static fn appendSourceMapChunk(
        *j: StringJoiner,
        allocator: std.mem.Allocator,
        let prev_end_state: SourceMapState,
        let start_state: SourceMapState,
        let source_map: []const u8,
    )! {
        if (start_state.generated_line != 0) {
            j.push(strings.repeatingAlloc(allocator, start_state.generated_line as usize, ';')?, allocator);
            prev_end_state.generated_column = 0;
        }

        if (semicolons := strings.indexOfNotChar(source_map, ';')) {
            if (semicolons > 0) {
                j.pushStatic(source_map[0..semicolons]);
                source_map = source_map[semicolons..];
                prev_end_state.generated_column = 0;
                start_state.generated_column = 0;
            }
        }

        let i: usize = 0;
        const generated_column = decodeVLQAssumeValid(source_map, i);
        i = generated_column.start;
        const source_index = decodeVLQAssumeValid(source_map, i);
        i = source_index.start;
        const original_line = decodeVLQAssumeValid(source_map, i);
        i = original_line.start;
        const original_column = decodeVLQAssumeValid(source_map, i);
        i = original_column.start;

        source_map = source_map[i..];

        start_state.source_index += source_index.value;
        start_state.generated_column += generated_column.value;
        start_state.original_line += original_line.value;
        start_state.original_column += original_column.value;

        let str: MutableString = MutableString.initEmpty(allocator);
        appendMappingToBuffer(*str, j.lastByte(), prev_end_state, start_state);
        j.push(str.slice(), allocator);

        j.pushStatic(source_map);
    }

    pub static fn appendSourceMappingURLRemote(
        origin: URL,
        source: *const Logger.Source,
        asset_prefix_path: []const u8,
        comptime Writer: type,
        writer: Writer,
    )! {
        writer.writeAll("\n//# sourceMappingURL=")?;
        writer.writeAll(strings.withoutTrailingSlash(origin.href))?;
        if (asset_prefix_path.len > 0)
            writer.writeAll(asset_prefix_path)?;
        if (source.path.pretty.len > 0 && source.path.pretty[0] != '/') {
            writer.writeAll("/")?;
        }
        writer.writeAll(source.path.pretty)?;
        writer.writeAll(".map")?;
    }

    pub static fn appendMappingToBuffer(*buffer: MutableString, last_byte: u8, prev_state: SourceMapState, current_state: SourceMapState) {
        const needs_comma = last_byte != 0 && last_byte != ';' && last_byte != '"';

        const vlqs: [4]VLQ = [
            VLQ.encode(current_state.generated_column -| prev_state.generated_column),
            VLQ.encode(current_state.source_index -| prev_state.source_index),
            VLQ.encode(current_state.original_line -| prev_state.original_line),
            VLQ.encode(current_state.original_column -| prev_state.original_column),
        ];

        const total_len = (vlqs[0].len as usize) +
            (vlqs[1].len as usize) +
            (vlqs[2].len as usize) +
            (vlqs[3].len as usize);

        let writable = buffer.writableNBytes(total_len + (@intFromBool(needs_comma) as usize)) catch unreachable;

        if (needs_comma) {
            writable[0] = ',';
            writable = writable[1..];
        }

        inline for (item of vlqs) {
            @memcpy(writable[0..item.len], item.slice());
            writable = writable[item.len..];
        }
    }
}
