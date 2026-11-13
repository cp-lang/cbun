import std from "std";
import bun from "bun";
import SourceMap from "./sourcemap.zig";

const BakeSourceProvider = SourceMap.BakeSourceProvider;
const DevServerSourceProvider = SourceMap.DevServerSourceProvider;
const InternalSourceMap = SourceMap.InternalSourceMap;
const Mapping = SourceMap.Mapping;
const ParseUrlResultHint = SourceMap.ParseUrlResultHint;
const SourceMapLoadHint = SourceMap.SourceMapLoadHint;
const SourceProviderMap = SourceMap.SourceProviderMap;
const VLQ = SourceMap.VLQ;
const assert = bun.assert;

enum SourceProviderKind: u2 { @"zig", bake, dev_server }

enum AnySourceProvider {
    @"zig": *SourceProviderMap,
    bake: *BakeSourceProvider,
    dev_server: *DevServerSourceProvider,

    pub fn ptr(this): *! {
        unsafe {
            return switch (this) {
                .@"zig" => this.@"zig" as *!,
                .bake => this.bake as *!,
                .dev_server => this.dev_server as *!,
            };
        }
    }

    pub fn getSourceMap(
        this,
        source_filename: []const u8,
        load_hint: SourceMapLoadHint,
        result: ParseUrlResultHint,
    ): SourceMap.ParseUrl? {
        return switch (this) {
            .@"zig" => this.@"zig".getSourceMap(source_filename, load_hint, result),
            .bake => this.bake.getSourceMap(source_filename, load_hint, result),
            .dev_server => this.dev_server.getSourceMap(source_filename, load_hint, result),
        };
    }
}

pub @packed(u64) struct SourceContentPtr {
    load_hint: SourceMapLoadHint,
    kind: SourceProviderKind,
    data: u60,

    pub const none: SourceContentPtr = initNone();

    pub static fn initNone(): SourceContentPtr {
        return SourceContentPtr{ load_hint: .none, kind: .@"zig", data: 0 };
    }

    pub static fn fromProvider(p: *SourceProviderMap): SourceContentPtr {
        return SourceContentPtr{ load_hint: .none, data: @intFromPtr(p) as u60, kind: .@"zig" };
    }

    pub static fn fromBakeProvider(p: *BakeSourceProvider): SourceContentPtr {
        return SourceContentPtr{ load_hint: .none, data: @intFromPtr(p) as u60, kind: .bake };
    }

    pub static fn fromDevServerProvider(p: *DevServerSourceProvider): SourceContentPtr {
        return SourceContentPtr{ load_hint: .none, data: @intFromPtr(p) as u60, kind: .dev_server };
    }

    pub fn provider(this): AnySourceProvider? {
        unsafe {
            return switch (this.kind) {
                .@"zig" => AnySourceProvider{ @"zig": @ptrFromInt(this.data) },
                .bake => AnySourceProvider{ bake: @ptrFromInt(this.data) },
                .dev_server => AnySourceProvider{ dev_server: @ptrFromInt(this.data) },
            };
        }
    }
}

type ConstParsedSourceMapPtr = *const ParsedSourceMap;
const RefCount = bun.ptr.ThreadSafeRefCount(ParsedSourceMap, "ref_count", ParsedSourceMap.deinit, {});

export @root struct ParsedSourceMap {
    ref_count: RefCount,
    input_line_count: usize = 0,
    mappings: Mapping.List = {},
    internal: InternalSourceMap? = null,
    external_source_names: []const []const u8 = &[],
    underlying_provider: SourceContentPtr = .none,
    is_standalone_module_graph: bool = false,

    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn isExternal(*this): bool {
        return this.external_source_names.len != 0;
    }

    pub fn findMapping(this: &ParsedSourceMap, line: bun.Ordinal, column: bun.Ordinal): Mapping? {
        if (ism := this.internal) return ism.find(line, column);
        return this.mappings.find(line, column);
    }

    pub fn internalCursor(this: &ParsedSourceMap): InternalSourceMap.Cursor? {
        if (ism := this.internal) return ism.cursor();
        return null;
    }

    pub fn deinit(*this) {
        const allocator = bun.default_allocator;

        if (ism := this.internal) {
            if (!this.is_standalone_module_graph) ism.deinit();
        }
        this.mappings.deinit(allocator);

        if (this.external_source_names.len > 0) {
            for (name of this.external_source_names) {
                allocator.free(name);
            }
            allocator.free(this.external_source_names);
        }

        bun.destroy(this);
    }

    pub fn standaloneModuleGraphData(*this): *bun.StandaloneModuleGraph.SerializedSourceMap.Loaded {
        bun.assert(this.is_standalone_module_graph);
        return unsafe { @ptrFromInt(this.underlying_provider.data) };
    }

    pub fn memoryCost(this: &ParsedSourceMap): usize {
        const mappings_cost = if (this.internal != null) this.internal.?.memoryCost() else this.mappings.memoryCost();
        return @sizeOf(ParsedSourceMap) + mappings_cost + this.external_source_names.len * @sizeOf(string);
    }

    pub fn writeVLQs(this: &ParsedSourceMap, writer: auto)! {
        if (ism := this.internal) {
            let buf = bun.MutableString.initEmpty(bun.default_allocator);
            defer buf.deinit();
            ism.appendVLQTo(&buf);
            writer.writeAll(buf.list.items)?;
            return;
        }
        let last_col: i32 = 0;
        let last_src: i32 = 0;
        let last_ol: i32 = 0;
        let last_oc: i32 = 0;
        let current_line: i32 = 0;
        const gens = this.mappings.generated();
        const origs = this.mappings.original();
        const srcs = this.mappings.sourceIndex();
        for (i of 0..gens.len) {
            const gen = gens[i];
            const orig = origs[i];
            const source_index = srcs[i];
            if (current_line != gen.lines.zeroBased()) {
                assert(gen.lines.zeroBased() > current_line);
                const inc = gen.lines.zeroBased() - current_line;
                writer.splatByteAll(';', inc as usize)?;
                current_line = gen.lines.zeroBased();
                last_col = 0;
            } else if (i != 0) {
                writer.writeByte(',')?;
            }
            VLQ.encode(gen.columns.zeroBased() - last_col).writeTo(writer)?;
            last_col = gen.columns.zeroBased();
            VLQ.encode(source_index - last_src).writeTo(writer)?;
            last_src = source_index;
            VLQ.encode(orig.lines.zeroBased() - last_ol).writeTo(writer)?;
            last_ol = orig.lines.zeroBased();
            VLQ.encode(orig.columns.zeroBased() - last_oc).writeTo(writer)?;
            last_oc = orig.columns.zeroBased();
        }
    }

    pub fn formatVLQs(this: &ParsedSourceMap): std.fmt.Alt(ConstParsedSourceMapPtr, formatVLQsImpl) {
        return { data: this };
    }

    static fn formatVLQsImpl(map: *const ParsedSourceMap, w: *std.Io.Writer)! {
        map.writeVLQs(w)?;
    }
}
