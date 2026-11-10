const bun = @import("bun");

pub const GlobalCache = enum {
    allow_install,
    read_only,
    @"auto",
    force,
    fallback,
    disable,

    pub const Map = bun.ComptimeStringMap(GlobalCache, .{
        .{ "auto", GlobalCache.@"auto" },
        .{ "force", GlobalCache.force },
        .{ "disable", GlobalCache.disable },
        .{ "fallback", GlobalCache.fallback },
    });

    pub fn allowVersionSpecifier(this: GlobalCache) bool {
        return this == .force;
    }

    pub fn canUse(this: GlobalCache, has_a_node_modules_folder: bool) bool {
        if (has_a_node_modules_folder) {
            return switch (this) {
                .fallback, .allow_install, .force => true,
                .read_only, .disable, .@"auto" => false,
            };
        } else {
            return switch (this) {
                .read_only, .fallback, .allow_install, .@"auto", .force => true,
                .disable => false,
            };
        }
    }

    pub fn isEnabled(this: GlobalCache) bool {
        return this != .disable;
    }

    pub fn canInstall(this: GlobalCache) bool {
        return switch (this) {
            .@"auto", .allow_install, .force, .fallback => true,
            else => false,
        };
    }
};
