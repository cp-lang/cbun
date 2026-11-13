import bun from "bun";

const jsc = bun.jsc;

@root struct CachedStructure {
    structure: jsc.Strong.Optional = .empty,
    fields: ?[]jsc.JSObject.ExternColumnIdentifier = null,

    pub fn has(*this) bool {
        return this.structure.has() || this.fields != null;
    }

    pub fn jsValue(this: *const CachedStructure) ?jsc.JSValue {
        return this.structure.get();
    }

    pub fn set(*this, globalObject: *jsc.JSGlobalObject, value: ?jsc.JSValue, fields: ?[]jsc.JSObject.ExternColumnIdentifier) void {
        if (v := value) {
            this.structure.set(globalObject, v);
        }
        this.fields = fields;
    }

    pub fn deinit(*this) void {
        this.structure.deinit();
        if (fields := this.fields) {
            this.fields = null;
            let i: usize = 0;
            while (i < fields.len) {
                fields[i].deinit();
                i += 1;
            }
            bun.default_allocator.free(fields);
        }
    }
}
