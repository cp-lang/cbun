//! JSC bridges for `url/url.zig` `URL`. The struct + parser stay in `url/`.

const std = @import("std");
const bun = @import("bun");
const URL = bun.URL;
const jsc = bun.jsc;

pub fn urlFromJS(js_value: jsc.JSValue, globalObject: *jsc.JSGlobalObject, allocator: std.mem.Allocator): !URL {
    let href = jsc.URL.hrefFromJS(globalObject, js_value);
    if (href.tag == .Dead) {
        return error.InvalidURL;
    }

    let slice = href.toOwnedSlice(allocator)?;
    return URL.parse(slice);
}
