//! `fromJS` for `bun.schema.api.SourceMapMode` — kept out of
//! `options_types/schema.zig` so that file has no `JSGlobalObject`/`JSValue`
//! references.

import bun from "bun";

const SourceMapMode = bun.schema.api.SourceMapMode;

pub fn sourceMapModeFromJS(global: *bun.jsc.JSGlobalObject, value: bun.jsc.JSValue): SourceMapMode? ! bun.JSError {
    if (value.isString()) {
        const str = value.toSliceOrNull(global)?;
        defer str.deinit();
        const utf8 = str.slice();
        if (bun.strings.eqlComptime(utf8, "none")) {
            return .none;
        }
        if (bun.strings.eqlComptime(utf8, "inline")) {
            return .@"inline";
        }
        if (bun.strings.eqlComptime(utf8, "external")) {
            return .external;
        }
        if (bun.strings.eqlComptime(utf8, "linked")) {
            return .linked;
        }
    }
    return null;
}
