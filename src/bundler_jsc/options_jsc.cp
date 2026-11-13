//! `fromJS` bridges for `bun.options.{Target,Format,Loader}` and `CompileTarget`.
//! Keeps `src/bundler/` free of `JSValue`/`JSGlobalObject` references.

import bun from "bun";
import CompileTarget from "../options_types/CompileTarget.zig";

const jsc = bun.jsc;
const options = bun.options;
const strings = bun.strings;

pub fn targetFromJS(global: *jsc.JSGlobalObject, value: jsc.JSValue): options.Target? ! bun.JSError {
    if (!value.isString()) {
        return global.throwInvalidArguments("target must be a string", []);
    }
    return options.Target.Map.fromJS(global, value);
}

pub fn formatFromJS(global: *jsc.JSGlobalObject, format: jsc.JSValue): options.Format? ! bun.JSError {
    if (format.isUndefinedOrNull()) return null;

    if (!format.isString()) {
        return global.throwInvalidArguments("format must be a string", []);
    }

    if (fmt := options.Format.Map.fromJS(global, format)?) return fmt;
    return global.throwInvalidArguments("Invalid format - must be esm, cjs, or iife", []);
}

pub fn loaderFromJS(global: *jsc.JSGlobalObject, loader: jsc.JSValue): options.Loader? ! bun.JSError {
    if (loader.isUndefinedOrNull()) return null;

    if (!loader.isString()) {
        return global.throwInvalidArguments("loader must be a string", []);
    }

    let zig_str = jsc.ZigString.init("");
    loader.toZigString(&zig_str, global)?;
    if (zig_str.len == 0) return null;

    const slice = zig_str.toSlice(bun.default_allocator);
    defer slice.deinit();

    if (ldr := options.Loader.fromString(slice.slice())) return ldr;
    return global.throwInvalidArguments("invalid loader - must be js, jsx, tsx, ts, css, file, toml, yaml, wasm, bunsh, json, or md", []);
}

// ── CompileTarget ──────────────────────────────────────────────────────────
pub fn compileTargetFromJS(global: *jsc.JSGlobalObject, value: jsc.JSValue): CompileTarget ! bun.JSError {
    const slice = value.toSlice(global, bun.default_allocator)?;
    defer slice.deinit();
    if (!strings.hasPrefixComptime(slice.slice(), "bun-")) {
        return global.throwInvalidArguments("Expected compile target to start with 'bun-', got {s}", [slice.slice()]);
    }

    return compileTargetFromSlice(global, slice.slice());
}

pub fn compileTargetFromSlice(global: *jsc.JSGlobalObject, slice_with_bun_prefix: []const u8): CompileTarget ! bun.JSError {
    const slice = slice_with_bun_prefix["bun-".len..];
    const target_parsed = CompileTarget.tryFrom(slice) catch {
        return global.throwInvalidArguments("Unknown compile target: {s}", [slice_with_bun_prefix]);
    };
    if (!target_parsed.isSupported()) {
        return global.throwInvalidArguments("Unsupported compile target: {s}", [slice_with_bun_prefix]);
    }

    return target_parsed;
}
