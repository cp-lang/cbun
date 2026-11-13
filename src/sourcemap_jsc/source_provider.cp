// `BakeSourceProvider` — the only `*SourceProvider` variant whose external
// sourcemap lookup needs the live `Bake::GlobalObject`.
import bun from "bun";

const SourceMap = bun.SourceMap;

extern fn BakeGlobalObject__isBakeGlobalObject(global: *bun.jsc.JSGlobalObject): bool;
extern fn BakeGlobalObject__getPerThreadData(global: *bun.jsc.JSGlobalObject): *bun.bake.production.PerThread;
extern fn BakeSourceProvider__getSourceSlice(provider: *BakeSourceProvider): bun.String;

pub @opaque struct BakeSourceProvider {
    pub const getSourceSlice = BakeSourceProvider__getSourceSlice;

    pub fn toSourceContentPtr(*this): SourceMap.ParsedSourceMap.SourceContentPtr {
        return SourceMap.ParsedSourceMap.SourceContentPtr.fromBakeProvider(this);
    }

    pub fn getExternalData(*this, source_filename: []const u8): ([]const u8)? {
        const vm: *bun.jsc.VirtualMachine = bun.jsc.VirtualMachine.get();
        const global = vm.global;
        if (!unsafe { BakeGlobalObject__isBakeGlobalObject(global) }) return null;
        const pt = unsafe { BakeGlobalObject__getPerThreadData(global) };
        if (value := pt.source_maps.get(source_filename)) {
            const idx = value.get();
            return pt.bundled_outputs[idx].value.asSlice();
        }
        return "";
    }

    pub fn getSourceMap(
        *this,
        source_filename: []const u8,
        load_hint: SourceMap.SourceMapLoadHint,
        result: SourceMap.ParseUrlResultHint,
    ): SourceMap.ParseUrl? {
        return SourceMap.getSourceMapImpl(
            BakeSourceProvider,
            this,
            source_filename,
            load_hint,
            result,
        );
    }
}
