import bun from "bun";

/// https://developer.mozilla.org/en-US/docs/Web/API/Request/mode
export enum FetchRequestMode: u2 {
    @"same-origin",
    @"no-cors",
    cors,
    navigate,

    pub const Map = bun.ComptimeStringMap(FetchRequestMode, .{
        .{ "same-origin", .@"same-origin" },
        .{ "no-cors", .@"no-cors" },
        .{ "cors", .cors },
        .{ "navigate", .navigate },
    });

    pub const toJS = @import("../http_jsc/fetch_enums_jsc.zig").fetchRequestModeToJS;
}
