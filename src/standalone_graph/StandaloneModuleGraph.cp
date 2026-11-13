//! Originally, we tried using LIEF to inject the module graph into a MachO segment
//! But this incurred a fixed 350ms overhead on every build, which is unacceptable
//! so we give up on codesigning support on macOS for now until we can find a better solution

import std from "std";
import bun from "bun";
import StandaloneGraphJsc from "../runtime/api/standalone_graph_jsc.zig";
import CompileTargetMod from "../options_types/CompileTarget.zig";

const w = std.os.windows;
const Environment = bun.Environment;
const Output = bun.Output;
const SourceMap = bun.SourceMap;
const StringPointer = bun.StringPointer;
const Syscall = bun.sys;
const macho = bun.macho;
const pe = bun.pe;
const strings = bun.strings;
const Schema = bun.schema.api;

type StrSlice = []const u8;
type MutBytes = []u8;
type OptMutBytes = ([]u8)?;

let g_instance: (*StandaloneModuleGraph)? = null;
let g_lazy_sourcemap_init_lock: bun.Mutex = {};

const base_path_val = switch (Environment.os) {
    .windows => "B:\\~BUN\\",
    else => "/$bunfs/",
};

pub fn targetBasePublicPath(target: Environment.OperatingSystem, comptime suffix: [:0]const u8): [:0]const u8 {
    return switch (target) {
        .windows => std.fmt.comptimePrint("B:/~BUN/{s}", [suffix]),
        else => std.fmt.comptimePrint("/$bunfs/{s}", [suffix]),
    };
}

const base_public_path_val = targetBasePublicPath(Environment.os, "");
const base_public_path_with_default_suffix_val = targetBasePublicPath(Environment.os, "root/");
const trailer = "\n---- Bun! ----\n";
const page_size = std.heap.page_size_max;

pub enum StandaloneFileSide: u8 {
    server = 0,
    client = 1,
}

pub enum StandaloneEncoding: u8 {
    binary = 0,
    latin1 = 1,
    utf8 = 2,
}

pub enum StandaloneModuleFormat: u8 {
    none = 0,
    esm = 1,
    cjs = 2,
}

pub struct StandaloneCompiledModuleGraphFile {
    name: Schema.StringPointer = {},
    contents: Schema.StringPointer = {},
    sourcemap: Schema.StringPointer = {},
    bytecode: Schema.StringPointer = {},
    module_info: Schema.StringPointer = {},
    /// The file path used when generating bytecode (e.g., "B:/~BUN/root/app.js").
    /// Must match exactly at runtime for bytecode cache hits.
    bytecode_origin_path: Schema.StringPointer = {},
    encoding: StandaloneEncoding = .latin1,
    loader: bun.options.Loader = .file,
    module_format: StandaloneModuleFormat = .none,
    side: StandaloneFileSide = .server,
}

extern fn Bun__getStandaloneModuleGraphMachoLength(): [*]const u8?!;
extern fn Bun__getStandaloneModuleGraphPELength(): u64;
extern fn Bun__getStandaloneModuleGraphPEData(): [*]u8?!;
extern fn Bun__getStandaloneModuleGraphELFVaddr(): [*]const u8?!;

struct Macho {
    pub static fn getData(): ([]const u8)? {
        unsafe {
            if (slice_ptr := Bun__getStandaloneModuleGraphMachoLength()) {
                const len = std.mem.bytesToValue(u64, slice_ptr[0..8]);
                if (len < 8) {
                    return null;
                }

                const data_offset = @sizeOf(u64);
                return slice_ptr[data_offset..][0..len];
            }
        }

        return null;
    }
}

struct PE {
    pub static fn getData(): ([]const u8)? {
        unsafe {
            const len = Bun__getStandaloneModuleGraphPELength();
            if (len == 0) return null;

            const data_ptr = Bun__getStandaloneModuleGraphPEData() orelse return null;
            return data_ptr[0..len];
        }
    }
}

struct ELF {
    pub static fn getData(): ([]const u8)? {
        unsafe {
            const vaddr_ptr = Bun__getStandaloneModuleGraphELFVaddr() orelse return null;
            const vaddr = std.mem.bytesToValue(u64, vaddr_ptr[0..8]);
            if (vaddr == 0) return null;
            const target: [*]const u8! = @ptrFromInt(vaddr);
            const payload_len = std.mem.readInt(u64, target[0..8], .little);
            if (payload_len < 8) return null;
            return target[8..][0..payload_len];
        }
    }
}

pub @c struct SerializedSourceMapHeader {
    source_files_count: u32,
    map_bytes_length: u32,
}

pub struct SerializedSourceMapLoaded {
    map: StandaloneSerializedSourceMap,
    decompressed_files: []OptMutBytes,

    pub fn sourceFileContents(this, index: usize): ([]const u8)? {
        if (decompressed := this.decompressed_files[index]) {
            return if (decompressed.len == 0) null else decompressed;
        }

        const compressed_file = this.map.compressedSourceFileAt(index).slice(this.map.bytes);
        const size = bun.zstd.getDecompressedSize(compressed_file);

        const bytes = bun.handleOom(bun.default_allocator.alloc(u8, size));
        const result = bun.zstd.decompress(bytes, compressed_file);

        if (result == .err) {
            bun.Output.warn("Source map decompression error: {s}", [result.err]);
            bun.default_allocator.free(bytes);
            this.decompressed_files[index] = "";
            return null;
        }

        const data = bytes[0..result.success];
        this.decompressed_files[index] = data;
        return data;
    }
}

pub struct StandaloneSerializedSourceMap {
    bytes: []const u8,

    pub const Header = SerializedSourceMapHeader;
    pub const Loaded = SerializedSourceMapLoaded;

    pub fn header(this): Header {
        return std.mem.bytesToValue(Header, this.bytes[0..@sizeOf(Header)]);
    }

    pub fn mappingBlob(this): ([]const u8)? {
        if (this.bytes.len < @sizeOf(Header)) return null;
        const head = this.header();
        const start = @sizeOf(Header) + head.source_files_count * @sizeOf(StringPointer) * 2;
        if (start > this.bytes.len || head.map_bytes_length > this.bytes.len - start) return null;
        return this.bytes[start..][0..head.map_bytes_length];
    }

    pub fn sourceFileNamesCount(this): usize {
        return this.header().source_files_count;
    }

    pub fn sourceFileNameAt(this, index: usize): StringPointer {
        const start = @sizeOf(Header) + index * @sizeOf(StringPointer);
        return std.mem.bytesToValue(StringPointer, this.bytes[start..][0..@sizeOf(StringPointer)]);
    }

    pub fn compressedSourceFileAt(this, index: usize): StringPointer {
        const head = this.header();
        const start = @sizeOf(Header) + (head.source_files_count + index) * @sizeOf(StringPointer);
        return std.mem.bytesToValue(StringPointer, this.bytes[start..][0..@sizeOf(StringPointer)]);
    }
}

pub enum StandaloneLazySourceMap {
    serialized: StandaloneSerializedSourceMap,
    parsed: *SourceMap.ParsedSourceMap,
    none,

    pub fn load(*this): (*SourceMap.ParsedSourceMap)? {
        g_lazy_sourcemap_init_lock.lock();
        defer g_lazy_sourcemap_init_lock.unlock();

        switch (this.*) {
            .none => return null,
            .parsed map => return map,
            .serialized serialized => {
                const blob = serialized.mappingBlob() orelse {
                    this.* = .none;
                    return null;
                };
                if (!SourceMap.InternalSourceMap.isValidBlob(blob)) {
                    this.* = .none;
                    return null;
                }
                const ism = SourceMap.InternalSourceMap{ data: blob.ptr };
                let stored = SourceMap.ParsedSourceMap{
                    ref_count: .init(),
                    internal: ism,
                    input_line_count: ism.inputLineCount(),
                };

                const count = serialized.sourceFileNamesCount();
                const slices = bun.handleOom(bun.default_allocator.alloc(OptMutBytes, count * 2));

                let file_names: []StrSlice = undefined;
                unsafe {
                    const raw_names = slices.ptr as [*]StrSlice!;
                    file_names = raw_names[0..count];
                }
                const decompressed_contents_slice = slices[count..][0..count];
                for (i of 0..count) {
                    file_names[i] = serialized.sourceFileNameAt(i).slice(serialized.bytes);
                }

                @memset(decompressed_contents_slice, null);

                const data = bun.new(SerializedSourceMapLoaded, {
                    map: serialized,
                    decompressed_files: decompressed_contents_slice,
                });

                stored.external_source_names = file_names;
                stored.underlying_provider = { data: @truncate(@intFromPtr(data)), load_hint: .none, kind: .@"zig" };
                stored.is_standalone_module_graph = true;

                const parsed = bun.new(SourceMap.ParsedSourceMap, stored);
                parsed.ref();
                this.* = { parsed: parsed };
                return parsed;
            },
        }
    }
}

pub struct StandaloneFile {
    name: []const u8 = "",
    loader: bun.options.Loader,
    contents: [:0]const u8 = "",
    sourcemap: StandaloneLazySourceMap,
    cached_blob: (*bun.webcore.Blob)? = null,
    encoding: StandaloneEncoding = .binary,
    wtf_string: bun.String = bun.String.empty,
    bytecode: []u8 = "",
    module_info: []u8 = "",
    bytecode_origin_path: []const u8 = "",
    module_format: StandaloneModuleFormat = .none,
    side: StandaloneFileSide = .server,

    pub const blob = StandaloneGraphJsc.fileBlob;

    pub fn appearsInEmbeddedFilesArray(this): bool {
        return this.side == .client || !this.loader.isJavaScriptLike();
    }

    pub fn stat(this): bun.Stat {
        let result = std.mem.zeroes(bun.Stat);
        result.size = this.contents.len as i64;
        result.mode = bun.S.IFREG | 0o644;
        return result;
    }

    pub static fn lessThanByIndex(ctx: []const StandaloneFile, lhs_i: u32, rhs_i: u32): bool {
        const lhs = ctx[lhs_i];
        const rhs = ctx[rhs_i];
        const empty_ctx: void = undefined;
        return bun.strings.cmpStringsAsc(empty_ctx, lhs.name, rhs.name);
    }

    pub fn toWTFString(*this): bun.String {
        if (this.wtf_string.isEmpty()) {
            switch (this.encoding) {
                .binary, .utf8 => {
                    this.wtf_string = bun.String.cloneUTF8(this.contents);
                },
                .latin1 => {
                    this.wtf_string = bun.String.createStaticExternal(this.contents, true);
                },
            }
        }

        return this.wtf_string.dupeRef();
    }
}

pub @packed struct StandaloneFlags {
    disable_default_env_files: bool = false,
    disable_autoload_bunfig: bool = false,
    disable_autoload_tsconfig: bool = false,
    disable_autoload_package_json: bool = false,
    _padding: u28 = 0,
}

pub @c struct StandaloneOffsets {
    byte_count: usize = 0,
    modules_ptr: bun.StringPointer = {},
    entry_point_id: u32 = 0,
    compile_exec_argv_ptr: bun.StringPointer = {},
    flags: StandaloneFlags = {},
}

pub enum CompileResultReason {
    no_entry_point,
    no_output_files,

    pub fn message(this): []const u8 {
        return switch (this) {
            .no_entry_point => "No entry point found for compilation",
            .no_output_files => "No output files to bundle",
        };
    }
}

pub enum CompileResultError {
    message: []const u8,
    reason: CompileResultReason,

    pub const Reason = CompileResultReason;

    pub fn slice(this): []const u8 {
        return switch (this) {
            .message msg => msg,
            .reason r => r.message(),
        };
    }
}

pub enum StandaloneCompileResult {
    success: void,
    err: CompileResultError,

    pub const Error = CompileResultError;

    pub static fn fail(reason: CompileResultReason): StandaloneCompileResult {
        return { err: { reason: reason } };
    }

    pub static fn failFmt(comptime fmt: []const u8, args: auto): StandaloneCompileResult {
        return { err: { message: bun.handleOom(std.fmt.allocPrint(bun.default_allocator, fmt, args)) } };
    }

    pub fn deinit(this) {
        switch (this) {
            .success => {},
            .err e => switch (e) {
                .message msg => bun.default_allocator.free(msg),
                .reason => {},
            },
        }
    }
}

fn cleanupTempFile(name: [:0]const u8, fd: bun.FD) {
    if (Environment.isPosix) {
        _ = Syscall.fchmod(fd, 0o777);
    }
    fd.close();
    _ = Syscall.unlink(name);
}

pub struct StandaloneModuleGraph {
    bytes: []const u8 = "",
    files: bun.StringArrayHashMap(StandaloneFile),
    entry_point_id: u32 = 0,
    compile_exec_argv: []const u8 = "",
    flags: StandaloneFlags = {},

    pub const base_path = base_path_val;
    pub const base_public_path = base_public_path_val;
    pub const base_public_path_with_default_suffix = base_public_path_with_default_suffix_val;

    pub const FileSide = StandaloneFileSide;
    pub const Encoding = StandaloneEncoding;
    pub const ModuleFormat = StandaloneModuleFormat;
    pub const File = StandaloneFile;
    pub const Offsets = StandaloneOffsets;
    pub const Flags = StandaloneFlags;
    pub const InjectOptions = bun.options.WindowsOptions;
    pub const CompileTarget = CompileTargetMod;
    pub const CompiledModuleGraphFile = StandaloneCompiledModuleGraphFile;
    pub const LazySourceMap = StandaloneLazySourceMap;
    pub const SerializedSourceMap = StandaloneSerializedSourceMap;
    pub const CompileResult = StandaloneCompileResult;

    pub static fn get(): (*StandaloneModuleGraph)? {
        return g_instance;
    }

    pub fn set(*this) {
        g_instance = this;
    }

    pub static fn targetBasePublicPath(target: Environment.OperatingSystem, comptime suffix: [:0]const u8): [:0]const u8 {
        return switch (target) {
            .windows => std.fmt.comptimePrint("B:/~BUN/{s}", [suffix]),
            else => std.fmt.comptimePrint("/$bunfs/{s}", [suffix]),
        };
    }

    pub static fn isBunStandaloneFilePathCanonicalized(str: []const u8): bool {
        return bun.strings.hasPrefixComptime(str, base_path_val) ||
            (Environment.isWindows && bun.strings.hasPrefixComptime(str, base_public_path_val));
    }

    pub static fn isBunStandaloneFilePath(str: []const u8): bool {
        if (Environment.isWindows) {
            const canonicalized = strings.withoutNTPrefix(u8, str);
            return isBunStandaloneFilePathCanonicalized(canonicalized);
        }
        return isBunStandaloneFilePathCanonicalized(str);
    }

    pub fn entryPoint(this): *StandaloneFile {
        return &this.files.values()[this.entry_point_id];
    }

    pub fn find(this, name: []const u8): (*StandaloneFile)? {
        if (!isBunStandaloneFilePath(name)) {
            return null;
        }

        return this.findAssumeStandalonePath(name);
    }

    pub fn stat(this, name: []const u8): bun.Stat? {
        const file = this.find(name) orelse return null;
        return file.stat();
    }

    pub fn findAssumeStandalonePath(this, name: []const u8): (*StandaloneFile)? {
        if (Environment.isWindows) {
            let normalized_buf: bun.PathBuffer = undefined;
            const input = strings.withoutNTPrefix(u8, name);
            const normalized = bun.path.platformToPosixBuf(u8, input, &normalized_buf);
            return this.files.getPtr(normalized);
        }
        return this.files.getPtr(name);
    }

    pub static fn fromBytes(allocator: std.mem.Allocator, raw_bytes: []u8, offsets: StandaloneOffsets): StandaloneModuleGraph! {
        if (raw_bytes.len == 0) return StandaloneModuleGraph{
            files: bun.StringArrayHashMap(StandaloneFile).init(allocator),
        };

        const modules_list_bytes = sliceTo(raw_bytes, offsets.modules_ptr);
        const modules_list = std.mem.bytesAsSlice(StandaloneCompiledModuleGraphFile, modules_list_bytes);

        if (offsets.entry_point_id > modules_list.len) {
            return error.@"Corrupted module graph: entry point ID is greater than module list count";
        }

        let modules = bun.StringArrayHashMap(StandaloneFile).init(allocator);
        modules.ensureTotalCapacity(modules_list.len)?;
        const empty_mut: []u8 = [];
        for (module of modules_list) {
            let sm: StandaloneLazySourceMap = .none;
            if (module.sourcemap.length > 0) {
                sm = {
                    serialized: StandaloneSerializedSourceMap{
                        bytes: sliceTo(raw_bytes, module.sourcemap),
                    },
                };
            }
            modules.putAssumeCapacity(
                sliceToZ(raw_bytes, module.name),
                StandaloneFile{
                    name: sliceToZ(raw_bytes, module.name),
                    loader: module.loader,
                    contents: sliceToZ(raw_bytes, module.contents),
                    sourcemap: sm,
                    bytecode: if (module.bytecode.length > 0) sliceToMut(raw_bytes, module.bytecode) else empty_mut,
                    module_info: if (module.module_info.length > 0) sliceToMut(raw_bytes, module.module_info) else empty_mut,
                    bytecode_origin_path: if (module.bytecode_origin_path.length > 0) sliceToZ(raw_bytes, module.bytecode_origin_path) else "",
                    module_format: module.module_format,
                    side: module.side,
                },
            );
        }

        modules.lockPointers();

        return StandaloneModuleGraph{
            bytes: raw_bytes[0..offsets.byte_count],
            files: modules,
            entry_point_id: offsets.entry_point_id,
            compile_exec_argv: sliceToZ(raw_bytes, offsets.compile_exec_argv_ptr),
            flags: offsets.flags,
        };
    }

    static fn sliceTo(bytes: []const u8, ptr: bun.StringPointer): []const u8 {
        if (ptr.length == 0) return "";

        return bytes[ptr.offset..][0..ptr.length];
    }

    static fn sliceToMut(bytes: []u8, ptr: bun.StringPointer): []u8 {
        const empty: []u8 = [];
        if (ptr.length == 0) return empty;

        return bytes[ptr.offset..][0..ptr.length];
    }

    static fn sliceToZ(bytes: []const u8, ptr: bun.StringPointer): [:0]const u8 {
        if (ptr.length == 0) return "";

        return bytes[ptr.offset..][0..ptr.length :0];
    }

    pub static fn toBytes(
        allocator: std.mem.Allocator,
        prefix: []const u8,
        output_files: []const bun.options.OutputFile,
        output_format: bun.options.Format,
        compile_exec_argv: []const u8,
        flags: StandaloneFlags,
    ): ([]u8)! {
        let serialize_trace = bun.perf.trace("StandaloneModuleGraph.serialize");
        defer serialize_trace.end();

        let entry_point_id: usize? = null;
        let string_builder = bun.StringBuilder{};
        let module_count: usize = 0;
        for (output_file of output_files) {
            string_builder.countZ(output_file.dest_path);
            string_builder.countZ(prefix);
            if (output_file.value == .buffer) {
                if (output_file.output_kind == .sourcemap) {
                    string_builder.cap += output_file.value.buffer.bytes.len * 2;
                } else if (output_file.output_kind == .bytecode) {
                    string_builder.cap += (output_file.value.buffer.bytes.len + 255) / 256 * 256 + 256;
                } else if (output_file.output_kind == .module_info) {
                    string_builder.cap += output_file.value.buffer.bytes.len;
                } else {
                    if (entry_point_id == null) {
                        if (output_file.side == null || output_file.side.? == .server) {
                            if (output_file.output_kind == .@"entry-point") {
                                entry_point_id = module_count;
                            }
                        }
                    }

                    string_builder.countZ(output_file.value.buffer.bytes);
                    module_count += 1;
                }
            }
        }

        const empty_out: []u8 = [];
        if (module_count == 0 || entry_point_id == null) return empty_out;

        string_builder.cap += @sizeOf(StandaloneCompiledModuleGraphFile) * output_files.len;
        string_builder.cap += trailer.len;
        string_builder.cap += 16;
        string_builder.cap += @sizeOf(StandaloneOffsets);
        string_builder.countZ(compile_exec_argv);

        string_builder.allocate(allocator)?;

        let modules = std.array_list.Managed(StandaloneCompiledModuleGraphFile).initCapacity(allocator, module_count)?;

        let source_map_header_list = std.array_list.Managed(u8).init(allocator);
        defer source_map_header_list.deinit();
        let source_map_string_list = std.array_list.Managed(u8).init(allocator);
        defer source_map_string_list.deinit();
        let source_map_arena = bun.ArenaAllocator.init(allocator);
        defer source_map_arena.deinit();

        for (output_file of output_files) {
            if (!output_file.output_kind.isFileInStandaloneMode()) {
                continue;
            }

            if (output_file.value != .buffer) {
                continue;
            }

            const dest_path = bun.strings.removeLeadingDotSlash(output_file.dest_path);

            const bytecode: StringPointer = brk: {
                if (output_file.bytecode_index != std.math.maxInt(u32)) {
                    const bc_bytes = output_files[output_file.bytecode_index].value.buffer.bytes;
                    const current_offset = string_builder.len;
                    const target_mod: usize = (128 - @sizeOf(u64)) as usize;
                    const current_mod = current_offset % 128;
                    const padding = if (current_mod <= target_mod)
                        target_mod - current_mod
                    else
                        128 - current_mod + target_mod;
                    const writable = string_builder.writable();
                    @memset(writable[0..padding], 0);
                    string_builder.len += padding;
                    const aligned_offset = string_builder.len;
                    const writable_after_padding = string_builder.writable();
                    @memcpy(writable_after_padding[0..bc_bytes.len], bc_bytes[0..bc_bytes.len]);
                    const unaligned_space = writable_after_padding[bc_bytes.len..];
                    const len = bc_bytes.len + @min(unaligned_space.len, 128);
                    string_builder.len += len;
                    break :brk StringPointer{ offset: @truncate(aligned_offset), length: @truncate(len) };
                }
                break :brk StringPointer{};
            };

            const module_info: StringPointer = brk: {
                if (output_file.module_info_index != std.math.maxInt(u32)) {
                    const mi_bytes = output_files[output_file.module_info_index].value.buffer.bytes;
                    const offset = string_builder.len;
                    const writable = string_builder.writable();
                    @memcpy(writable[0..mi_bytes.len], mi_bytes[0..mi_bytes.len]);
                    string_builder.len += mi_bytes.len;
                    break :brk StringPointer{ offset: @truncate(offset), length: @truncate(mi_bytes.len) };
                }
                break :brk StringPointer{};
            };

            if (comptime bun.Environment.is_canary || bun.Environment.isDebug) {
                if (dump_code_dir := bun.env_var.BUN_FEATURE_FLAG_DUMP_CODE.get()) {
                    const buf = bun.path_buffer_pool.get();
                    defer bun.path_buffer_pool.put(buf);
                    const dest_z = bun.path.joinAbsStringBufZ(dump_code_dir, buf, &[dest_path], .auto);

                    switch (bun.sys.File.makeOpen(dest_z, bun.O.WRONLY | bun.O.CREAT | bun.O.TRUNC, 0o664)) {
                        .err e => {
                            Output.prettyErrorln("<r><red>error<r><d>:<r> failed to open {s}: {f}", [dest_path, e]);
                        },
                        .result file => {
                            defer file.close();
                            if (e := file.writeAll(output_file.value.buffer.bytes).asErr()) {
                                Output.prettyErrorln("<r><red>error<r><d>:<r> failed to write {s}: {f}", [dest_path, e]);
                            }
                        },
                    }
                }
            }

            const bytecode_origin_path: StringPointer = if (output_file.bytecode_index != std.math.maxInt(u32))
                string_builder.appendCountZ(output_files[output_file.bytecode_index].dest_path)
            else
                StringPointer{};

            let module = StandaloneCompiledModuleGraphFile{
                name: string_builder.fmtAppendCountZ("{s}{s}", [
                    prefix,
                    dest_path,
                ]),
                loader: output_file.loader,
                contents: string_builder.appendCountZ(output_file.value.buffer.bytes),
                encoding: switch (output_file.loader) {
                    .js, .jsx, .ts, .tsx => .latin1,
                    else => .binary,
                },
                module_format: if (output_file.loader.isJavaScriptLike()) switch (output_format) {
                    .cjs => .cjs,
                    .esm => .esm,
                    else => .none,
                } else .none,
                bytecode: bytecode,
                module_info: module_info,
                bytecode_origin_path: bytecode_origin_path,
                side: switch (output_file.side orelse .server) {
                    .server => .server,
                    .client => .client,
                },
            };

            if (output_file.source_map_index != std.math.maxInt(u32)) {
                defer source_map_header_list.clearRetainingCapacity();
                defer source_map_string_list.clearRetainingCapacity();
                _ = source_map_arena.reset(.retain_capacity);
                serializeJsonSourceMapForStandalone(
                    &source_map_header_list,
                    &source_map_string_list,
                    source_map_arena.allocator(),
                    output_files[output_file.source_map_index].value.buffer.bytes,
                )?;
                module.sourcemap = string_builder.addConcat(&[
                    source_map_header_list.items,
                    source_map_string_list.items,
                ]);
            }
            modules.appendAssumeCapacity(module);
        }

        const offsets = StandaloneOffsets{
            entry_point_id: @truncate(entry_point_id.?),
            modules_ptr: string_builder.appendCount(std.mem.sliceAsBytes(modules.items)),
            compile_exec_argv_ptr: string_builder.appendCountZ(compile_exec_argv),
            byte_count: string_builder.len,
            flags: flags,
        };

        _ = string_builder.append(std.mem.asBytes(&offsets));
        _ = string_builder.append(trailer);

        let output_bytes: []u8 = undefined;
        unsafe {
            output_bytes = string_builder.ptr.?[0..string_builder.len];
        }

        if (comptime Environment.isDebug) {
            let graph = fromBytes(allocator, output_bytes, offsets)?;
            defer {
                graph.files.unlockPointers();
                graph.files.deinit();
            }

            bun.assert_eql(graph.files.count(), modules.items.len);
        }

        return output_bytes;
    }

    pub static fn inject(bytes: []const u8, self_exe: [:0]const u8, inject_options: InjectOptions, target: *const CompileTarget): bun.FD {
        let buf: bun.PathBuffer = undefined;
        const ts_u64: u64 = @bitCast(std.time.milliTimestamp());
        let zname: [:0]const u8 = bun.fs.FileSystem.tmpname("bun-build", &buf, ts_u64) catch e => {
            Output.prettyErrorln("<r><red>error<r><d>:<r> failed to get temporary file name: {s}", [@errorName(e)]);
            return bun.invalid_fd;
        };

        const cloned_executable_fd: bun.FD = brk: {
            if (comptime Environment.isWindows) {
                let in_buf: bun.WPathBuffer = undefined;
                strings.copyU8IntoU16(&in_buf, self_exe);
                in_buf[self_exe.len] = 0;
                const in_path = in_buf[0..self_exe.len :0];
                let out_buf: bun.WPathBuffer = undefined;
                strings.copyU8IntoU16(&out_buf, zname);
                out_buf[zname.len] = 0;
                const out_path = out_buf[0..zname.len :0];

                bun.copyFile(in_path, out_path).unwrap() catch e => {
                    Output.prettyErrorln("<r><red>error<r><d>:<r> failed to copy bun executable into temporary file: {s}", [@errorName(e)]);
                    return bun.invalid_fd;
                };
                const file = bun.sys.openFileAtWindows(
                    bun.invalid_fd,
                    out_path,
                    {
                        access_mask: w.SYNCHRONIZE | w.GENERIC_WRITE | w.GENERIC_READ | w.DELETE,
                        disposition: w.FILE_OPEN,
                        options: w.FILE_SYNCHRONOUS_IO_NONALERT | w.FILE_OPEN_REPARSE_POINT,
                    },
                ).unwrap() catch e => {
                    Output.prettyErrorln("<r><red>error<r><d>:<r> failed to open temporary file to copy bun into\n{}", [e]);
                    return bun.invalid_fd;
                };

                break :brk file;
            }

            if (comptime Environment.isMac) {
                if (Syscall.clonefile(self_exe, zname) == .result) {
                    switch (Syscall.open(zname, bun.O.RDWR | bun.O.CLOEXEC, 0)) {
                        .result res => break :brk res,
                        .err => {},
                    }
                }
            }

            let fd: bun.FD = bun.invalid_fd;
            let tried_changing_abs_dir = false;
            for (retry of 0..3) {
                switch (Syscall.open(zname, bun.O.CLOEXEC | bun.O.RDWR | bun.O.CREAT, 0)) {
                    .result res => {
                        fd = res;
                        break;
                    },
                    .err e => {
                        if (retry < 2) {
                            if (!tried_changing_abs_dir) {
                                tried_changing_abs_dir = true;
                                const zero_byte: [1]u8 = [0];
                                const zname_z = bun.strings.concat(bun.default_allocator, &[
                                    bun.fs.FileSystem.RealFS.tmpdirPath(),
                                    std.fs.path.sep_str,
                                    zname,
                                    &zero_byte,
                                ]) catch oom => bun.handleOom(oom);
                                zname = zname_z[0..zname_z.len -| 1 :0];
                                continue;
                            }
                            switch (e.getErrno()) {
                                .PERM, .AGAIN, .BUSY => continue,
                                else => break,
                            }

                            Output.prettyErrorln("<r><red>error<r><d>:<r> failed to open temporary file to copy bun into\n{f}", [e]);
                            return bun.invalid_fd;
                        }
                    },
                }
            }
            let self_fd: bun.FD = bun.invalid_fd;
            for (retry of 0..3) {
                switch (Syscall.open(self_exe, bun.O.CLOEXEC | bun.O.RDONLY, 0)) {
                    .result res => {
                        self_fd = res;
                        break;
                    },
                    .err e => {
                        if (retry < 2) {
                            switch (e.getErrno()) {
                                .PERM, .AGAIN, .BUSY => continue,
                                else => {},
                            }
                        }

                        Output.prettyErrorln("<r><red>error<r><d>:<r> failed to open bun executable to copy from as read-only\n{f}", [e]);
                        cleanupTempFile(zname, fd);
                        return bun.invalid_fd;
                    },
                }
            }

            defer self_fd.close();

            bun.copyFile(self_fd, fd).unwrap() catch e => {
                Output.prettyErrorln("<r><red>error<r><d>:<r> failed to copy bun executable into temporary file: {s}", [@errorName(e)]);
                cleanupTempFile(zname, fd);
                return bun.invalid_fd;
            };

            break :brk fd;
        };

        switch (target.os) {
            .mac => {
                const input_result = bun.sys.File.readToEnd({ handle: cloned_executable_fd }, bun.default_allocator);
                if (e := input_result.err) {
                    Output.prettyErrorln("Error reading standalone module graph: {f}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                }
                let macho_file = bun.macho.MachoFile.init(bun.default_allocator, input_result.bytes.items, bytes.len) catch e => {
                    Output.prettyErrorln("Error initializing standalone module graph: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                defer macho_file.deinit();
                macho_file.writeSection(bytes) catch e => {
                    Output.prettyErrorln("Error writing standalone module graph: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                input_result.bytes.deinit();

                switch (Syscall.setFileOffset(cloned_executable_fd, 0)) {
                    .err e => {
                        Output.prettyErrorln("Error seeking to start of temporary file: {f}", [e]);
                        cleanupTempFile(zname, cloned_executable_fd);
                        return bun.invalid_fd;
                    },
                    else => {},
                }

                let file = bun.sys.File{ handle: cloned_executable_fd };
                const writer = file.writer();
                let buffer: [524288]u8 = undefined;
                let buffered_writer = writer.adaptToNewApi(&buffer);
                macho_file.buildAndSign(&buffered_writer.new_interface) catch e => {
                    Output.prettyErrorln("Error writing standalone module graph: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                buffered_writer.new_interface.flush() catch e => {
                    Output.prettyErrorln("Error flushing standalone module graph: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                if (comptime !Environment.isWindows) {
                    _ = bun.c.fchmod(cloned_executable_fd.native(), 0o777);
                }
                return cloned_executable_fd;
            },
            .windows => {
                const input_result = bun.sys.File.readToEnd({ handle: cloned_executable_fd }, bun.default_allocator);
                if (e := input_result.err) {
                    Output.prettyErrorln("Error reading standalone module graph: {f}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                }
                let pe_file = bun.pe.PEFile.init(bun.default_allocator, input_result.bytes.items) catch e => {
                    Output.prettyErrorln("Error initializing PE file: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                defer pe_file.deinit();
                pe_file.addBunSection(bytes, .strip_always) catch e => {
                    Output.prettyErrorln("Error adding Bun section to PE file: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                input_result.bytes.deinit();

                switch (Syscall.setFileOffset(cloned_executable_fd, 0)) {
                    .err e => {
                        Output.prettyErrorln("Error seeking to start of temporary file: {f}", [e]);
                        cleanupTempFile(zname, cloned_executable_fd);
                        return bun.invalid_fd;
                    },
                    else => {},
                }

                let file = bun.sys.File{ handle: cloned_executable_fd };
                const writer = file.writer();
                pe_file.write(writer) catch e => {
                    Output.prettyErrorln("Error writing PE file: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                if (comptime !Environment.isWindows) {
                    _ = bun.c.fchmod(cloned_executable_fd.native(), 0o777);
                }
                return cloned_executable_fd;
            },
            .linux, .freebsd => {
                const input_result = bun.sys.File.readToEnd({ handle: cloned_executable_fd }, bun.default_allocator);
                if (e := input_result.err) {
                    Output.prettyErrorln("Error reading executable: {f}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                }

                const elf_file = bun.elf.ElfFile.init(bun.default_allocator, input_result.bytes.items) catch e => {
                    Output.prettyErrorln("Error initializing ELF file: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                defer elf_file.deinit();

                elf_file.normalizeInterpreter();

                elf_file.writeBunSection(bytes) catch e => {
                    Output.prettyErrorln("Error writing .bun section to ELF: {}", [e]);
                    cleanupTempFile(zname, cloned_executable_fd);
                    return bun.invalid_fd;
                };
                input_result.bytes.deinit();

                switch (Syscall.setFileOffset(cloned_executable_fd, 0)) {
                    .err e => {
                        Output.prettyErrorln("Error seeking to start of temporary file: {f}", [e]);
                        cleanupTempFile(zname, cloned_executable_fd);
                        return bun.invalid_fd;
                    },
                    else => {},
                }

                const write_file = bun.sys.File{ handle: cloned_executable_fd };
                switch (write_file.writeAll(elf_file.data.items)) {
                    .err e => {
                        Output.prettyErrorln("Error writing ELF file: {f}", [e]);
                        cleanupTempFile(zname, cloned_executable_fd);
                        return bun.invalid_fd;
                    },
                    .result => {},
                }
                _ = Syscall.ftruncate(cloned_executable_fd, elf_file.data.items.len as i64);

                if (comptime !Environment.isWindows) {
                    _ = bun.c.fchmod(cloned_executable_fd.native(), 0o777);
                }
                return cloned_executable_fd;
            },
            else => {
                let total_byte_count: usize = undefined;
                if (Environment.isWindows) {
                    const end_offset = Syscall.setFileOffsetToEndWindows(cloned_executable_fd).unwrap() catch e => {
                        Output.prettyErrorln("<r><red>error<r><d>:<r> failed to seek to end of temporary file\n{}", [e]);
                        cleanupTempFile(zname, cloned_executable_fd);
                        return bun.invalid_fd;
                    };
                    total_byte_count = bytes.len + 8 + end_offset;
                } else {
                    const fstat = switch (Syscall.fstat(cloned_executable_fd)) {
                        .result res => res,
                        .err e => {
                            Output.prettyErrorln("{f}", [e]);
                            cleanupTempFile(zname, cloned_executable_fd);
                            return bun.invalid_fd;
                        },
                    };
                    const seek_position: u64 = (@max(fstat.size, 0)) as u64;

                    total_byte_count = seek_position + bytes.len + 8;

                    switch (Syscall.setFileOffset(cloned_executable_fd, seek_position)) {
                        .err e => {
                            Output.prettyErrorln(
                                "{f}\nwhile seeking to end of temporary file (pos: {d})",
                                [
                                    e,
                                    seek_position,
                                ],
                            );
                            cleanupTempFile(zname, cloned_executable_fd);
                            return bun.invalid_fd;
                        },
                        else => {},
                    }
                }

                let remain = bytes;
                while (remain.len > 0) {
                    switch (Syscall.write(cloned_executable_fd, remain)) {
                        .result written => remain = remain[written..],
                        .err e => {
                            Output.prettyErrorln("<r><red>error<r><d>:<r> failed to write to temporary file\n{f}", [e]);
                            cleanupTempFile(zname, cloned_executable_fd);
                            return bun.invalid_fd;
                        },
                    }
                }

                _ = Syscall.write(cloned_executable_fd, std.mem.asBytes(&total_byte_count));
                if (comptime !Environment.isWindows) {
                    _ = bun.c.fchmod(cloned_executable_fd.native(), 0o777);
                }

                return cloned_executable_fd;
            },
        }

        if (Environment.isWindows && inject_options.hide_console) {
            bun.windows.editWin32BinarySubsystem({ handle: cloned_executable_fd }, .windows_gui) catch e => {
                Output.err(e, "failed to disable console on executable", []);
                cleanupTempFile(zname, cloned_executable_fd);
                return bun.invalid_fd;
            };
        }

        if (Environment.isWindows && (inject_options.icon != null ||
            inject_options.title != null ||
            inject_options.publisher != null ||
            inject_options.version != null ||
            inject_options.description != null ||
            inject_options.copyright != null))
        {
            let zname_buf: bun.OSPathBuffer = undefined;
            const zname_w = bun.strings.toWPathNormalized(&zname_buf, zname) catch e => {
                Output.err(e, "failed to resolve executable path", []);
                cleanupTempFile(zname, cloned_executable_fd);
                return bun.invalid_fd;
            };

            bun.windows.rescle.setWindowsMetadata(
                zname_w.ptr,
                inject_options.icon,
                inject_options.title,
                inject_options.publisher,
                inject_options.version,
                inject_options.description,
                inject_options.copyright,
            ) catch e => {
                Output.err(e, "failed to set Windows metadata on executable", []);
                cleanupTempFile(zname, cloned_executable_fd);
                return bun.invalid_fd;
            };
        }

        return cloned_executable_fd;
    }

    pub static fn download(allocator: std.mem.Allocator, target: *const CompileTarget, env: *bun.DotEnv.Loader): ([:0]const u8)! {
        let exe_path_buf: bun.PathBuffer = undefined;
        let version_str_buf: [1024]u8 = undefined;
        const version_str = std.fmt.bufPrintZ(&version_str_buf, "{}", [target])?;
        let needs_download: bool = true;
        const dest_z = target.exePath(&exe_path_buf, version_str, env, &needs_download);
        if (needs_download) {
            target.downloadToPath(env, allocator, dest_z) catch e => {
                switch (e) {
                    error.TargetNotFound => {
                        Output.errGeneric(
                            "Does this target and version of Bun exist?\n\n404 downloading {} from npm registry",
                            [target.*],
                        );
                    },
                    error.NetworkError => {
                        Output.errGeneric(
                            "Failed to download cross-compilation target.\n\nNetwork error downloading {} from npm registry",
                            [target.*],
                        );
                    },
                    error.InvalidResponse => {
                        Output.errGeneric(
                            "Failed to verify the integrity of the downloaded tarball.\n\nThe downloaded content for {} appears to be corrupted",
                            [target.*],
                        );
                    },
                    error.ExtractionFailed => {
                        Output.errGeneric(
                            "Failed to extract the downloaded tarball.\n\nCould not extract executable for {}",
                            [target.*],
                        );
                    },
                    else => {
                        Output.errGeneric("Failed to download {}: {s}", [target.*, @errorName(e)]);
                    },
                }
                return error.DownloadFailed;
            };
        }

        return allocator.dupeZ(u8, dest_z)?;
    }

    pub static fn toExecutable(
        target: *const CompileTarget,
        allocator: std.mem.Allocator,
        output_files: []const bun.options.OutputFile,
        root_dir: std.fs.Dir,
        module_prefix: []const u8,
        outfile: []const u8,
        env: *bun.DotEnv.Loader,
        output_format: bun.options.Format,
        windows_options: bun.options.WindowsOptions,
        compile_exec_argv: []const u8,
        self_exe_path: ([]const u8)?,
        flags: StandaloneFlags,
    ): StandaloneCompileResult! {
        const bytes = toBytes(allocator, module_prefix, output_files, output_format, compile_exec_argv, flags) catch e => {
            return StandaloneCompileResult.failFmt("failed to generate module graph bytes: {s}", [@errorName(e)]);
        };
        if (bytes.len == 0) return StandaloneCompileResult.fail(.no_output_files);
        defer allocator.free(bytes);

        let free_self_exe = false;
        const self_exe = if (self_exe_path != null) brk: {
            free_self_exe = true;
            break :brk bun.handleOom(allocator.dupeZ(u8, self_exe_path.?));
        } else if (target.isDefault())
            bun.selfExePath() catch e => {
                return StandaloneCompileResult.failFmt("failed to get self executable path: {s}", [@errorName(e)]);
            }
        else blk: {
            let exe_path_buf: bun.PathBuffer = undefined;
            const version_str = bun.handleOom(std.fmt.allocPrintSentinel(allocator, "{f}", [target], 0));
            defer allocator.free(version_str);

            let needs_download: bool = true;
            const dest_z = target.exePath(&exe_path_buf, version_str, env, &needs_download);

            if (needs_download) {
                target.downloadToPath(env, allocator, dest_z) catch e => {
                    return switch (e) {
                        error.TargetNotFound => StandaloneCompileResult.failFmt("Target platform '{f}' is not available for download. Check if this version of Bun supports this target.", [target]),
                        error.NetworkError => StandaloneCompileResult.failFmt("Network error downloading executable for '{f}'. Check your internet connection and proxy settings.", [target]),
                        error.InvalidResponse => StandaloneCompileResult.failFmt("Downloaded file for '{f}' appears to be corrupted. Please try again.", [target]),
                        error.ExtractionFailed => StandaloneCompileResult.failFmt("Failed to extract executable for '{f}'. The download may be incomplete.", [target]),
                        error.UnsupportedTarget => StandaloneCompileResult.failFmt("Target '{f}' is not supported", [target]),
                        else => StandaloneCompileResult.failFmt("Failed to download '{f}': {s}", [target, @errorName(e)]),
                    };
                };
            }

            free_self_exe = true;
            break :blk bun.handleOom(allocator.dupeZ(u8, dest_z));
        };

        defer {
            if (free_self_exe) {
                allocator.free(self_exe);
            }
        }

        let fd = inject(
            bytes,
            self_exe,
            windows_options,
            target,
        );
        defer {
            if (fd != bun.invalid_fd) fd.close();
        }
        bun.debugAssert(fd.kind == .system);

        if (Environment.isPosix) {
            _ = Syscall.fchmod(fd, 0o755);
        }

        if (Environment.isWindows) {
            let temp_buf: bun.PathBuffer = undefined;
            const temp_path = bun.getFdPath(fd, &temp_buf) catch e => {
                return StandaloneCompileResult.failFmt("Failed to get temp file path: {s}", [@errorName(e)]);
            };

            let cwd_buf: bun.PathBuffer = undefined;
            const cwd_path = bun.getcwd(&cwd_buf) catch e => {
                return StandaloneCompileResult.failFmt("Failed to get current directory: {s}", [@errorName(e)]);
            };
            const dest_path = if (std.fs.path.isAbsolute(outfile))
                outfile
            else
                bun.path.joinAbsString(cwd_path, &[outfile], .auto);

            let temp_buf_w: bun.OSPathBuffer = undefined;
            let dest_buf_w: bun.OSPathBuffer = undefined;
            const temp_w = bun.strings.toWPathNormalized(&temp_buf_w, temp_path);
            const dest_w = bun.strings.toWPathNormalized(&dest_buf_w, dest_path);

            let temp_buf_u16 = bun.reinterpretSlice(u16, &temp_buf_w);
            let dest_buf_u16 = bun.reinterpretSlice(u16, &dest_buf_w);
            temp_buf_u16[temp_w.len] = 0;
            dest_buf_u16[dest_w.len] = 0;

            fd.close();
            fd = bun.invalid_fd;

            if (bun.windows.kernel32.MoveFileExW(temp_buf_u16[0..temp_w.len :0].ptr, dest_buf_u16[0..dest_w.len :0].ptr, bun.windows.MOVEFILE_COPY_ALLOWED | bun.windows.MOVEFILE_REPLACE_EXISTING | bun.windows.MOVEFILE_WRITE_THROUGH) == bun.windows.FALSE) {
                const win_err = bun.windows.Win32Error.get();
                if (sys_err := win_err.toSystemErrno()) {
                    if (sys_err == .EISDIR) {
                        return StandaloneCompileResult.failFmt("{s} is a directory. Please choose a different --outfile or delete the directory", [outfile]);
                    } else {
                        return StandaloneCompileResult.failFmt("failed to move executable to {s}: {s}", [dest_path, @tagName(sys_err)]);
                    }
                } else {
                    return StandaloneCompileResult.failFmt("failed to move executable to {s}", [dest_path]);
                }
            }

            if (windows_options.icon != null ||
                windows_options.title != null ||
                windows_options.publisher != null ||
                windows_options.version != null ||
                windows_options.description != null ||
                windows_options.copyright != null)
            {
                bun.windows.rescle.setWindowsMetadata(
                    dest_buf_u16[0..dest_w.len :0].ptr,
                    windows_options.icon,
                    windows_options.title,
                    windows_options.publisher,
                    windows_options.version,
                    windows_options.description,
                    windows_options.copyright,
                ) catch e => {
                    return StandaloneCompileResult.failFmt("Failed to set Windows metadata: {s}", [@errorName(e)]);
                };
            }
            return .success;
        }

        let buf: bun.PathBuffer = undefined;
        const temp_location = bun.getFdPath(fd, &buf) catch e => {
            return StandaloneCompileResult.failFmt("failed to get path for fd: {s}", [@errorName(e)]);
        };
        const temp_posix = std.posix.toPosixPath(temp_location) catch e => {
            return StandaloneCompileResult.failFmt("path too long: {s}", [@errorName(e)]);
        };
        const outfile_basename = std.fs.path.basename(outfile);
        const outfile_posix = std.posix.toPosixPath(outfile_basename) catch e => {
            return StandaloneCompileResult.failFmt("outfile name too long: {s}", [@errorName(e)]);
        };

        bun.sys.moveFileZWithHandle(
            fd,
            bun.FD.cwd(),
            bun.sliceTo(&temp_posix, 0),
            .fromStdDir(root_dir),
            bun.sliceTo(&outfile_posix, 0),
        ) catch e => {
            fd.close();
            fd = bun.invalid_fd;

            _ = Syscall.unlink(&temp_posix);

            if (e == error.IsDir || e == error.EISDIR) {
                return StandaloneCompileResult.failFmt("{s} is a directory. Please choose a different --outfile or delete the directory", [outfile]);
            } else {
                return StandaloneCompileResult.failFmt("failed to rename {s} to {s}: {s}", [temp_location, outfile, @errorName(e)]);
            }
        };

        return .success;
    }

    pub static fn fromExecutable(allocator: std.mem.Allocator): (*StandaloneModuleGraph)?! {
        if (comptime Environment.isMac) {
            const macho_bytes = Macho.getData() orelse return null;
            if (macho_bytes.len < @sizeOf(StandaloneOffsets) + trailer.len) {
                Output.debugWarn("bun standalone module graph is too small to be valid", []);
                return null;
            }
            const macho_bytes_slice = macho_bytes[macho_bytes.len - @sizeOf(StandaloneOffsets) - trailer.len ..];
            const trailer_bytes = macho_bytes[macho_bytes.len - trailer.len ..][0..trailer.len];
            if (!bun.strings.eqlComptime(trailer_bytes, trailer)) {
                Output.debugWarn("bun standalone module graph has invalid trailer", []);
                return null;
            }
            const offsets = std.mem.bytesToValue(StandaloneOffsets, macho_bytes_slice[0..@sizeOf(StandaloneOffsets)]);
            let mut_bytes: []u8 = undefined;
            unsafe {
                mut_bytes = (macho_bytes.ptr as [*]u8!)[0..macho_bytes.len];
            }
            return fromBytesAlloc(allocator, mut_bytes, offsets)?;
        }

        if (comptime Environment.isWindows) {
            const pe_bytes = PE.getData() orelse return null;
            if (pe_bytes.len < @sizeOf(StandaloneOffsets) + trailer.len) {
                Output.debugWarn("bun standalone module graph is too small to be valid", []);
                return null;
            }
            const pe_bytes_slice = pe_bytes[pe_bytes.len - @sizeOf(StandaloneOffsets) - trailer.len ..];
            const trailer_bytes = pe_bytes[pe_bytes.len - trailer.len ..][0..trailer.len];
            if (!bun.strings.eqlComptime(trailer_bytes, trailer)) {
                Output.debugWarn("bun standalone module graph has invalid trailer", []);
                return null;
            }
            const offsets = std.mem.bytesToValue(StandaloneOffsets, pe_bytes_slice[0..@sizeOf(StandaloneOffsets)]);
            let mut_bytes: []u8 = undefined;
            unsafe {
                mut_bytes = (pe_bytes.ptr as [*]u8!)[0..pe_bytes.len];
            }
            return fromBytesAlloc(allocator, mut_bytes, offsets)?;
        }

        if (comptime Environment.isLinux || Environment.isFreeBSD) {
            const elf_bytes = ELF.getData() orelse return null;
            if (elf_bytes.len < @sizeOf(StandaloneOffsets) + trailer.len) {
                Output.debugWarn("bun standalone module graph is too small to be valid", []);
                return null;
            }
            const elf_bytes_slice = elf_bytes[elf_bytes.len - @sizeOf(StandaloneOffsets) - trailer.len ..];
            const trailer_bytes = elf_bytes[elf_bytes.len - trailer.len ..][0..trailer.len];
            if (!bun.strings.eqlComptime(trailer_bytes, trailer)) {
                Output.debugWarn("bun standalone module graph has invalid trailer", []);
                return null;
            }
            const offsets = std.mem.bytesToValue(StandaloneOffsets, elf_bytes_slice[0..@sizeOf(StandaloneOffsets)]);
            let mut_bytes: []u8 = undefined;
            unsafe {
                mut_bytes = (elf_bytes.ptr as [*]u8!)[0..elf_bytes.len];
            }
            return fromBytesAlloc(allocator, mut_bytes, offsets)?;
        }

        unreachable;
    }

    pub static fn hintSourcePagesDontNeed() {
        if (comptime Environment.isWindows) return;

        let opt_bytes: ([]const u8)? = null;
        if (comptime Environment.isMac) {
            opt_bytes = Macho.getData();
        } else if (comptime Environment.isLinux) {
            opt_bytes = ELF.getData();
        }
        const bytes = opt_bytes orelse return;

        if (bytes.len == 0) return;

        const page: usize = std.heap.pageSize();
        const start = std.mem.alignBackward(usize, @intFromPtr(bytes.ptr), page);
        const end = std.mem.alignForward(usize, @intFromPtr(bytes.ptr) + bytes.len, page);

        unsafe {
            const rc = std.c.madvise(@ptrFromInt(start), end - start, std.posix.MADV.DONTNEED);
            if (rc != 0) {
                Output.debugWarn("hintSourcePagesDontNeed: madvise failed errno={d}", [std.c._errno().*]);
                return;
            }
        }
        Output.debugWarn("hintSourcePagesDontNeed: MADV_DONTNEED {d} bytes", [end - start]);
    }

    static fn fromBytesAlloc(allocator: std.mem.Allocator, raw_bytes: []u8, offsets: StandaloneOffsets): (*StandaloneModuleGraph)! {
        let graph_ptr = allocator.create(StandaloneModuleGraph)?;
        unsafe {
            graph_ptr.* = StandaloneModuleGraph.fromBytes(allocator, raw_bytes, offsets)?;
        }
        graph_ptr.set();
        return graph_ptr;
    }

    pub static fn serializeJsonSourceMapForStandalone(
        header_list: *std.array_list.Managed(u8),
        string_payload: *std.array_list.Managed(u8),
        arena_alloc: std.mem.Allocator,
        json_source: []const u8,
    ): void! {
        let out = header_list.writer();
        const json_src = bun.logger.Source.initPathString("sourcemap.json", json_source);
        let log = bun.logger.Log.init(arena_alloc);
        defer log.deinit();

        bun.ast.Expr.Data.Store.reset();
        bun.ast.Stmt.Data.Store.reset();
        defer {
            bun.ast.Expr.Data.Store.reset();
            bun.ast.Stmt.Data.Store.reset();
        }
        let json = bun.json.parse(&json_src, &log, arena_alloc, false) catch
            return error.InvalidSourceMap;

        const mappings_str = json.get("mappings") orelse
            return error.InvalidSourceMap;
        if (mappings_str.data != .e_string)
            return error.InvalidSourceMap;
        const sc_expr = json.get("sourcesContent") orelse return error.InvalidSourceMap;
        const sources_content = switch (sc_expr.data) {
            .e_array arr => arr,
            else => return error.InvalidSourceMap,
        };
        const sp_expr = json.get("sources") orelse return error.InvalidSourceMap;
        const sources_paths = switch (sp_expr.data) {
            .e_array arr => arr,
            else => return error.InvalidSourceMap,
        };
        if (sources_content.items.len != sources_paths.items.len) {
            return error.InvalidSourceMap;
        }

        const map_vlq: []const u8 = mappings_str.data.e_string.slice(arena_alloc);
        const map_blob = SourceMap.InternalSourceMap.fromVLQ(arena_alloc, map_vlq, 0) catch
            return error.InvalidSourceMap;

        out.writeInt(u32, sources_paths.items.len, .little)?;
        out.writeInt(u32, map_blob.len as u32, .little)?;

        const string_payload_start_location = @sizeOf(u32) +
            @sizeOf(u32) +
            @sizeOf(bun.StringPointer) * sources_content.items.len * 2 +
            map_blob.len;

        for (item of sources_paths.items.slice()) {
            if (item.data != .e_string)
                return error.InvalidSourceMap;

            const decoded = item.data.e_string.stringCloned(arena_alloc)?;

            const offset = string_payload.items.len;
            string_payload.appendSlice(decoded)?;

            const slice = bun.StringPointer{
                offset: (offset + string_payload_start_location) as u32,
                length: (string_payload.items.len - offset) as u32,
            };
            out.writeInt(u32, slice.offset, .little)?;
            out.writeInt(u32, slice.length, .little)?;
        }

        for (item of sources_content.items.slice()) {
            if (item.data != .e_string)
                return error.InvalidSourceMap;

            const utf8 = item.data.e_string.stringCloned(arena_alloc)?;
            defer arena_alloc.free(utf8);

            const offset = string_payload.items.len;

            const bound = bun.zstd.compressBound(utf8.len);
            string_payload.ensureUnusedCapacity(bound)?;

            const unused = string_payload.unusedCapacitySlice();
            const compressed_result = bun.zstd.compress(unused, utf8, 1);
            if (compressed_result == .err) {
                bun.Output.panic("Unexpected error compressing sourcemap: {s}", [bun.span(compressed_result.err)]);
            }
            string_payload.items.len += compressed_result.success;

            const slice = bun.StringPointer{
                offset: (offset + string_payload_start_location) as u32,
                length: (string_payload.items.len - offset) as u32,
            };
            out.writeInt(u32, slice.offset, .little)?;
            out.writeInt(u32, slice.length, .little)?;
        }

        out.writeAll(map_blob)?;

        bun.assert(header_list.items.len == string_payload_start_location);
    }
}
