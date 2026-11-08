import bun from "bun";

export enum FetchRedirect: u2 {
    follow,
    manual,
    @"error",

    pub const Map = bun.ComptimeStringMap(FetchRedirect, .{
        .{ "follow", .follow },
        .{ "manual", .manual },
        .{ "error", .@"error" },
    });

    pub const toJS = @import("../http_jsc/fetch_enums_jsc.zig").fetchRedirectToJS;
}
