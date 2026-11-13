// JSC bridge for `bun.Semver.String`. Keeps `src/semver/` free of JSC types.
import bun from "bun";
const jsc = bun.jsc;
const String = bun.Semver.String;
const strSlice = String.slice;

pub fn toJS(str: &String, buffer: []const u8, globalThis: *jsc.JSGlobalObject): jsc.JSValue ! bun.JSError {
    return bun.String.createUTF8ForJS(globalThis, strSlice(str, buffer));
}
