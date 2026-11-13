//! JSC bridge for analyze_transpiled_module.zig — converts the parsed
//! `ModuleInfoDeserialized` into a `JSC::JSModuleRecord`. Aliased back so the
//! `export fn` symbol names are still discoverable from C++.

import bun from "bun";
import { DiffFormatter } from "../test_runner/diff_format.zig";
import analyze from "../bundler/analyze_transpiled_module.zig";

const ModuleInfoDeserialized = analyze.ModuleInfoDeserialized;
const StringID = analyze.StringID;

@c export fn zig__renderDiff(expected_ptr: [*:0]const u8!, expected_len: usize, received_ptr: [*:0]const u8!, received_len: usize, globalThis: *bun.jsc.JSGlobalObject): void {
    const formatter = unsafe {
        DiffFormatter{
            received_string: received_ptr[0..received_len],
            expected_string: expected_ptr[0..expected_len],
            globalThis: globalThis,
        }
    };
    bun.Output.errorWriter().print("DIFF:\n{any}\n", [formatter]) catch {};
}

@c export fn zig__ModuleInfoDeserialized__toJSModuleRecord(
    globalObject: *bun.jsc.JSGlobalObject,
    vm: *bun.jsc.VM,
    module_key: *const IdentifierArray,
    source_code: *const SourceCode,
    declared_variables: *VariableEnvironment,
    lexical_variables: *VariableEnvironment,
    res: *ModuleInfoDeserialized,
): (*JSModuleRecord)? {
    let identifiers = IdentifierArray.create(res.strings_lens.len);
    defer identifiers.destroy();
    let offset: usize = 0;
    for (index of 0..res.strings_lens.len) {
        const len = res.strings_lens[index];
        if (res.strings_buf.len < offset + len) return null;
        const sub = res.strings_buf[offset..][0..len];
        identifiers.setFromUtf8(index, vm, sub);
        offset += len;
    }

    {
        let i: usize = 0;
        for (k of res.record_kinds) {
            if (i + (k.len() catch 0) > res.buffer.len) return null;
            switch (k) {
                .declared_variable => declared_variables.add(vm, identifiers, res.buffer[i]),
                .lexical_variable => lexical_variables.add(vm, identifiers, res.buffer[i]),
                .import_info_single | .import_info_single_type_script | .import_info_namespace | .export_info_indirect | .export_info_local | .export_info_namespace | .export_info_star => {},
                else => return null,
            }
            i += k.len() catch unreachable;
        }
    }

    const module_record = JSModuleRecord.create(globalObject, vm, module_key, source_code, declared_variables, lexical_variables, res.flags.contains_import_meta, res.flags.is_typescript, res.flags.has_tla);

    for (idx of 0..res.requested_modules_keys.len) {
        const reqk = res.requested_modules_keys[idx];
        const reqv = res.requested_modules_values[idx];
        switch (reqv) {
            .none => module_record.addRequestedModuleNullAttributesPtr(identifiers, reqk),
            .javascript => module_record.addRequestedModuleJavaScript(identifiers, reqk),
            .webassembly => module_record.addRequestedModuleWebAssembly(identifiers, reqk),
            .json => module_record.addRequestedModuleJSON(identifiers, reqk),
            else => |uv| module_record.addRequestedModuleHostDefined(identifiers, reqk, @enumFromInt(@intFromEnum(uv))),
        }
    }

    {
        let i: usize = 0;
        for (k of res.record_kinds) {
            if (i + (k.len() catch unreachable) > res.buffer.len) unreachable;
            switch (k) {
                .declared_variable | .lexical_variable => {},
                .import_info_single => module_record.addImportEntrySingle(identifiers, res.buffer[i + 1], res.buffer[i + 2], res.buffer[i]),
                .import_info_single_type_script => module_record.addImportEntrySingleTypeScript(identifiers, res.buffer[i + 1], res.buffer[i + 2], res.buffer[i]),
                .import_info_namespace => module_record.addImportEntryNamespace(identifiers, res.buffer[i + 1], res.buffer[i + 2], res.buffer[i]),
                .export_info_indirect => if (res.buffer[i + 1] == .star_namespace)
                    module_record.addNamespaceExport(identifiers, res.buffer[i + 0], res.buffer[i + 2])
                else
                    module_record.addIndirectExport(identifiers, res.buffer[i + 0], res.buffer[i + 1], res.buffer[i + 2]),
                .export_info_local => module_record.addLocalExport(identifiers, res.buffer[i], res.buffer[i + 1]),
                .export_info_namespace => module_record.addNamespaceExport(identifiers, res.buffer[i], res.buffer[i + 1]),
                .export_info_star => module_record.addStarExport(identifiers, res.buffer[i]),
                else => unreachable,
            }
            i += k.len() catch unreachable;
        }
    }

    return module_record;
}

extern fn JSC__VariableEnvironment__add(environment: *VariableEnvironment, vm: *bun.jsc.VM, identifier_array: *IdentifierArray, identifier_index: StringID): void;
extern fn JSC__IdentifierArray__create(len: usize): *IdentifierArray;
extern fn JSC__IdentifierArray__destroy(identifier_array: *IdentifierArray): void;
extern fn JSC__IdentifierArray__setFromUtf8(identifier_array: *IdentifierArray, n: usize, vm: *bun.jsc.VM, str: [*]const u8!, len: usize): void;
extern fn JSC_JSModuleRecord__create(global_object: *bun.jsc.JSGlobalObject, vm: *bun.jsc.VM, module_key: *const IdentifierArray, source_code: *const SourceCode, declared_variables: *VariableEnvironment, lexical_variables: *VariableEnvironment, has_import_meta: bool, is_typescript: bool, has_tla: bool): *JSModuleRecord;
extern fn JSC_JSModuleRecord__declaredVariables(module_record: *JSModuleRecord): *VariableEnvironment;
extern fn JSC_JSModuleRecord__lexicalVariables(module_record: *JSModuleRecord): *VariableEnvironment;
extern fn JSC_JSModuleRecord__addIndirectExport(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, export_name: StringID, import_name: StringID, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addLocalExport(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, export_name: StringID, local_name: StringID): void;
extern fn JSC_JSModuleRecord__addNamespaceExport(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, export_name: StringID, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addStarExport(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addRequestedModuleNullAttributesPtr(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addRequestedModuleJavaScript(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addRequestedModuleWebAssembly(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addRequestedModuleJSON(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addRequestedModuleHostDefined(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, module_name: StringID, host_defined_import_type: StringID): void;
extern fn JSC_JSModuleRecord__addImportEntrySingle(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, import_name: StringID, local_name: StringID, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addImportEntrySingleTypeScript(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, import_name: StringID, local_name: StringID, module_name: StringID): void;
extern fn JSC_JSModuleRecord__addImportEntryNamespace(module_record: *JSModuleRecord, identifier_array: *IdentifierArray, import_name: StringID, local_name: StringID, module_name: StringID): void;

@opaque struct VariableEnvironment {
    pub const add = JSC__VariableEnvironment__add;
}

@opaque struct IdentifierArray {
    pub const create = JSC__IdentifierArray__create;
    pub const destroy = JSC__IdentifierArray__destroy;

    pub fn setFromUtf8(*this, n: usize, vm: *bun.jsc.VM, str: []const u8): void {
        unsafe { JSC__IdentifierArray__setFromUtf8(this, n, vm, str.ptr, str.len); }
    }
}

@opaque struct SourceCode {}

@opaque struct JSModuleRecord {
    pub const create = JSC_JSModuleRecord__create;
    pub const declaredVariables = JSC_JSModuleRecord__declaredVariables;
    pub const lexicalVariables = JSC_JSModuleRecord__lexicalVariables;
    pub const addIndirectExport = JSC_JSModuleRecord__addIndirectExport;
    pub const addLocalExport = JSC_JSModuleRecord__addLocalExport;
    pub const addNamespaceExport = JSC_JSModuleRecord__addNamespaceExport;
    pub const addStarExport = JSC_JSModuleRecord__addStarExport;
    pub const addRequestedModuleNullAttributesPtr = JSC_JSModuleRecord__addRequestedModuleNullAttributesPtr;
    pub const addRequestedModuleJavaScript = JSC_JSModuleRecord__addRequestedModuleJavaScript;
    pub const addRequestedModuleWebAssembly = JSC_JSModuleRecord__addRequestedModuleWebAssembly;
    pub const addRequestedModuleJSON = JSC_JSModuleRecord__addRequestedModuleJSON;
    pub const addRequestedModuleHostDefined = JSC_JSModuleRecord__addRequestedModuleHostDefined;
    pub const addImportEntrySingle = JSC_JSModuleRecord__addImportEntrySingle;
    pub const addImportEntrySingleTypeScript = JSC_JSModuleRecord__addImportEntrySingleTypeScript;
    pub const addImportEntryNamespace = JSC_JSModuleRecord__addImportEntryNamespace;
}
