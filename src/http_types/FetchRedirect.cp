zig {
    const bun = @import("bun");
}

export enum FetchRedirect: u2 {
    follow,
    manual,
    @"error",

    zig {
        pub const Map = bun.ComptimeStringMap(FetchRedirect, .{
            .{ "follow", .follow },
            .{ "manual", .manual },
            .{ "error", .@"error" },
        });
        pub const toJS = @import("../http_jsc/fetch_enums_jsc.zig").fetchRedirectToJS;
    }
}
