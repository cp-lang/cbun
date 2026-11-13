//! JSC bridge for `bun.css.Err(T)`. Keeps `src/css/` free of JSC types.

import bun from "bun";

/// `this` is `*const css.Err(T)` for any `T`; only `.kind` is accessed.
pub fn toErrorInstance(self_err: auto, globalThis: *bun.jsc.JSGlobalObject): bun.jsc.JSValue! {
    let str = bun.String.createFormat("{f}", [self_err.kind])?;
    defer str.deref();
    return str.toErrorInstance(globalThis);
}
