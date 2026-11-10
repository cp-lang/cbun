import * as bun from "bun";

export enum Format {
    esm,
    iife,
    cjs,
    internal_bake_dev,

    pub fn keepES6ImportExportSyntax(this): bool {
        return this == Format.esm;
    }

    @inline
    pub fn isESM(this): bool {
        return this == Format.esm;
    }

    @inline
    pub fn isAlwaysStrictMode(this): bool {
        return this == Format.esm;
    }

    pub const Map = bun.ComptimeStringMap(Format, .{
        .{ "esm", .esm },
        .{ "cjs", .cjs },
        .{ "iife", .iife },

        // TODO: Disable this outside of debug builds
        .{ "internal_bake_dev", .internal_bake_dev },
    });

    pub const fromJS = @import("../bundler_jsc/options_jsc.zig").formatFromJS;

    pub fn fromString(slice: []const u8): Format? {
        return Map.getWithEql(slice, bun.strings.eqlComptime);
    }
}

export struct WindowsOptions {
    hide_console: bool = false,
    icon: string? = null,
    title: string? = null,
    publisher: string? = null,
    version: string? = null,
    description: string? = null,
    copyright: string? = null,
}

export enum BundlePackage {
    always,
    never,

    pub const Map = bun.StringArrayHashMapUnmanaged(BundlePackage);
}
