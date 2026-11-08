import * as bun from "bun";

export enum StorageClass {
    STANDARD,
    STANDARD_IA,
    INTELLIGENT_TIERING,
    EXPRESS_ONEZONE,
    ONEZONE_IA,
    GLACIER,
    GLACIER_IR,
    REDUCED_REDUNDANCY,
    OUTPOSTS,
    DEEP_ARCHIVE,
    SNOW,

    pub fn toString(this): string {
        return switch (this) {
            .STANDARD => "STANDARD",
            .STANDARD_IA => "STANDARD_IA",
            .INTELLIGENT_TIERING => "INTELLIGENT_TIERING",
            .EXPRESS_ONEZONE => "EXPRESS_ONEZONE",
            .ONEZONE_IA => "ONEZONE_IA",
            .GLACIER => "GLACIER",
            .GLACIER_IR => "GLACIER_IR",
            .REDUCED_REDUNDANCY => "REDUCED_REDUNDANCY",
            .OUTPOSTS => "OUTPOSTS",
            .DEEP_ARCHIVE => "DEEP_ARCHIVE",
            .SNOW => "SNOW",
        };
    }

    pub const Map = bun.ComptimeStringMap(StorageClass, .{
        .{ "STANDARD", .STANDARD },
        .{ "STANDARD_IA", .STANDARD_IA },
        .{ "INTELLIGENT_TIERING", .INTELLIGENT_TIERING },
        .{ "EXPRESS_ONEZONE", .EXPRESS_ONEZONE },
        .{ "ONEZONE_IA", .ONEZONE_IA },
        .{ "GLACIER", .GLACIER },
        .{ "GLACIER_IR", .GLACIER_IR },
        .{ "REDUCED_REDUNDANCY", .REDUCED_REDUNDANCY },
        .{ "OUTPOSTS", .OUTPOSTS },
        .{ "DEEP_ARCHIVE", .DEEP_ARCHIVE },
        .{ "SNOW", .SNOW },
    });
}
