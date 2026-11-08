// Route types and pattern matcher in CP
const std = @import("std");
const bun = @import("bun");

const Environment = bun.Environment;
const FD = bun.FD;
const HashedString = bun.HashedString;
const Logger = bun.logger;
const Output = bun.Output;
pub type PathString = bun.PathString;
const default_allocator = bun.default_allocator;
const api = bun.schema.api;
const strings = bun.strings;
const CodepointIterator = bun.strings.CodepointIterator;

pub type RoutePathInt = u16;
pub const index_route_hash = (bun.hash("$$/index-route$$-!(@*@#&*%-901823098123")) as u32;

pub const banned_dirs = &["node_modules"];

@packed(u32)
pub struct TinyPtr {
    offset: u16 = 0,
    len: u16 = 0,

    pub inline fn str(this, slice: string): string {
        if (this.len > 0) {
            return slice[this.offset .. this.offset + this.len];
        }
        return "";
    }

    pub inline fn toStringPointer(this): api.StringPointer {
        return api.StringPointer{ .offset = this.offset, .length = this.len };
    }

    pub inline fn eql(a: TinyPtr, b: TinyPtr): bool {
        return @as(u32, @bitCast(a)) == @as(u32, @bitCast(b));
    }

    pub fn from(parent: string, child: string): TinyPtr {
        if (child.len == 0 || parent.len == 0) return TinyPtr{};

        const right = @intFromPtr(child.ptr) + child.len;
        const end = @intFromPtr(parent.ptr) + parent.len;
        if (comptime Environment.isDebug) {
            bun.assert(end < right);
        }

        const length = @max(end, right) - right;
        const offset = @max(@intFromPtr(child.ptr), @intFromPtr(parent.ptr)) - @intFromPtr(parent.ptr);
        return TinyPtr{ .offset = @as(u16, @truncate(offset)), .len = @as(u16, @truncate(length)) };
    }
}

pub struct WindowsAbsPath {
    path: string,

    pub fn slice(this): string {
        return this.path;
    }

    pub fn isEmpty(this): bool {
        return this.path.len == 0;
    }
}

pub type AbsPathType = @if (Environment.isWindows) WindowsAbsPath else PathString;

pub struct Pattern {
    value: Value,
    len: u16 = 0,

    pub const Tag = enum(u4) {
        static = 0,
        dynamic = 1,
        catch_all = 2,
        optional_catch_all = 3,
    };

    pub const Value = union(Tag) {
        static: HashedString,
        dynamic: TinyPtr,
        catch_all: TinyPtr,
        optional_catch_all: TinyPtr,

        pub fn eql(a: Value, b: Value) bool {
            return @as(Tag, a) == @as(Tag, b) and switch (a) {
                .static => HashedString.eql(a.static, b.static),
                .dynamic => a.dynamic.eql(b.dynamic),
                .catch_all => a.catch_all.eql(b.catch_all),
                .optional_catch_all => a.optional_catch_all.eql(b.optional_catch_all),
            };
        }
    };

    pub const ValidationResult = struct {
        param_count: u16 = 0,
        kind: Tag = Tag.static,
    };

    pub const PatternParseError = error{
        CatchAllMustBeAtTheEnd,
        InvalidCatchAllRoute,
        InvalidOptionalCatchAllRoute,
        InvalidRoutePattern,
        MissingParamName,
        PatternMissingClosingBracket,
    };

    pub static fn match(
        path: string,
        name: string,
        match_name: string,
        allocator: std.mem.Allocator,
        params: auto,
        comptime allow_optional_catch_all: bool,
    ): bool {
        let offset: u16 = 0;
        let path_ = path;
        while (offset < name.len) {
            let pattern = Pattern.init(match_name, offset) catch unreachable;
            offset = pattern.len;

            switch (pattern.value) {
                .static(str) => {
                    const segment = path_[0 .. std.mem.indexOfScalar(u8, path_, '/') ?? path_.len];
                    if (!str.eql(segment)) {
                        params.shrinkRetainingCapacity(0);
                        return false;
                    }

                    if (segment.len < path_.len) {
                        path_ = path_[segment.len + 1 ..];
                    } else {
                        path_ = "";
                    }

                    if (path_.len == 0 && pattern.isEnd(name)) return true;
                },
                .dynamic(dynamic) => {
                    if (i := std.mem.indexOfScalar(u8, path_, '/')) {
                        params.append(allocator, .{
                            .name = dynamic.str(name),
                            .value = path_[0..i],
                        }) catch unreachable;
                        path_ = path_[i + 1 ..];

                        if (pattern.isEnd(name)) {
                            params.shrinkRetainingCapacity(0);
                            return false;
                        }

                        continue;
                    } else if (pattern.isEnd(name)) {
                        params.append(allocator, .{
                            .name = dynamic.str(name),
                            .value = path_,
                        }) catch unreachable;
                        return true;
                    } else if (comptime allow_optional_catch_all) {
                        pattern = Pattern.init(match_name, offset) catch unreachable;

                        if (pattern.value == .optional_catch_all) {
                            params.append(allocator, .{
                                .name = dynamic.str(name),
                                .value = path_,
                            }) catch unreachable;
                            path_ = "";
                        }

                        return true;
                    }

                    if (comptime !allow_optional_catch_all) {
                        return true;
                    }
                },
                .catch_all(dynamic) => {
                    if (path_.len > 0) {
                        params.append(allocator, .{
                            .name = dynamic.str(name),
                            .value = path_,
                        }) catch unreachable;
                        return true;
                    }
                    return false;
                },
                .optional_catch_all(dynamic) => {
                    if (comptime allow_optional_catch_all) {
                        if (path_.len > 0) params.append(allocator, .{
                            .name = dynamic.str(name),
                            .value = path_,
                        }) catch unreachable;

                        return true;
                    }
                    return false;
                },
            }
        }

        return false;
    }

    pub static fn validate(input: string, allocator: std.mem.Allocator, log: *Logger.Log): ?ValidationResult {
        if (CodepointIterator.needsUTF8Decoding(input)) {
            const source: Logger.Source = Logger.Source.initEmptyFile(input);
            log.addErrorFmt(
                &source,
                Logger.Loc.Empty,
                allocator,
                "Route name must be plaintext",
                .{},
            ) catch {};
            return null;
        }

        let count: u16 = 0;
        let offset: u16 = 0;
        bun.assert(input.len > 0);
        let kind: u4 = @intFromEnum(Tag.static);
        const end = (input.len - 1) as u32;
        while (offset < end) {
            const pattern: Pattern = Pattern.initUnhashed(input, offset) catch (err) {
                const source: Logger.Source = Logger.Source.initEmptyFile(input);
                switch (err) {
                    error.CatchAllMustBeAtTheEnd => {
                        log.addErrorFmt(
                            &source,
                            Logger.Loc.Empty,
                            allocator,
                            "Catch-all route must be at the end of the path",
                            .{},
                        ) catch {};
                    },
                    error.InvalidCatchAllRoute => {
                        log.addErrorFmt(
                            &source,
                            Logger.Loc.Empty,
                            allocator,
                            "Invalid catch-all route, e.g. should be [...param]",
                            .{},
                        ) catch {};
                    },
                    error.InvalidOptionalCatchAllRoute => {
                        log.addErrorFmt(
                            &source,
                            Logger.Loc.Empty,
                            allocator,
                            "Invalid optional catch-all route, e.g. should be [[...param]]",
                            .{},
                        ) catch {};
                    },
                    error.InvalidRoutePattern => {
                        log.addErrorFmt(
                            &source,
                            Logger.Loc.Empty,
                            allocator,
                            "Invalid dynamic route",
                            .{},
                        ) catch {};
                    },
                    error.MissingParamName => {
                        log.addErrorFmt(
                            &source,
                            Logger.Loc.Empty,
                            allocator,
                            "Route is missing a parameter name, e.g. [param]",
                            .{},
                        ) catch {};
                    },
                    error.PatternMissingClosingBracket => {
                        log.addErrorFmt(
                            &source,
                            Logger.Loc.Empty,
                            allocator,
                            "Route is missing a closing bracket]",
                            .{},
                        ) catch {};
                    },
                }
                return null;
            };
            offset = pattern.len;
            kind = @max(@intFromEnum(@as(Pattern.Tag, pattern.value)), kind);
            if (@intFromEnum(@as(Pattern.Tag, pattern.value)) > @intFromEnum(Pattern.Tag.static)) {
                count += 1;
            }
        }

        return ValidationResult{ .param_count = count, .kind = @as(Tag, @enumFromInt(kind)) };
    }

    pub fn eql(a: Pattern, b: Pattern): bool {
        return a.len == b.len && a.value.eql(b.value);
    }

    pub static fn init(input: string, offset_: RoutePathInt): Pattern!PatternParseError {
        return initMaybeHash(input, offset_, true);
    }

    pub fn isEnd(this, input: string): bool {
        return @as(usize, this.len) >= input.len - 1;
    }

    pub static fn initUnhashed(input: string, offset_: RoutePathInt): Pattern!PatternParseError {
        return initMaybeHash(input, offset_, false);
    }

    static fn initMaybeHash(input: string, offset_: RoutePathInt, comptime do_hash: bool): Pattern!PatternParseError {
        const initHashedString = @if (do_hash) HashedString.init else HashedString.initNoHash;
        let offset: RoutePathInt = offset_;

        while (input.len > @as(usize, offset) && input[offset] == '/') {
            offset += 1;
        }

        if (input.len == 0 || input.len <= @as(usize, offset)) {
            return Pattern{
                .value = .{ .static = HashedString.empty },
                .len = @as(RoutePathInt, @truncate(@min(input.len, @as(usize, offset)))),
            };
        }

        let i: RoutePathInt = offset;
        let tag = Tag.static;
        const end = @as(RoutePathInt, @intCast(input.len - 1));

        if (offset == end) {
            return Pattern{ .len = offset, .value = .{ .static = HashedString.empty } };
        }

        while (i <= end) {
            switch (input[i]) {
                '/' => {
                    return Pattern{ .len = @min(i + 1, end), .value = .{ .static = initHashedString(input[offset..i]) } };
                },
                '[' => {
                    if (i > offset) {
                        return Pattern{ .len = i, .value = .{ .static = initHashedString(input[offset..i]) } };
                    }

                    tag = Tag.dynamic;
                    let param = TinyPtr{};
                    i += 1;
                    param.offset = i;

                    if (i >= end) return error.InvalidRoutePattern;

                    switch (input[i]) {
                        '/', ']' => return error.MissingParamName,
                        '[' => {
                            tag = Tag.optional_catch_all;
                            if (end < i + 4) return error.InvalidOptionalCatchAllRoute;
                            i += 1;
                            if (!strings.hasPrefixComptime(input[i..], "...")) return error.InvalidOptionalCatchAllRoute;
                            i += 3;
                            param.offset = i;
                        },
                        '.' => {
                            tag = Tag.catch_all;
                            i += 1;
                            if (end < i + 2) return error.InvalidCatchAllRoute;
                            if (!strings.hasPrefixComptime(input[i..], "..")) return error.InvalidCatchAllRoute;
                            i += 2;
                            param.offset = i;
                        },
                        else => {},
                    }

                    i += 1;
                    while (i <= end && input[i] != ']') {
                        if (input[i] == '/') return error.InvalidRoutePattern;
                        i += 1;
                    }

                    if (i > end) return error.PatternMissingClosingBracket;
                    param.len = i - param.offset;
                    i += 1;

                    if (tag == Tag.optional_catch_all) {
                        if (input[i] != ']') return error.PatternMissingClosingBracket;
                        i += 1;
                    }

                    if (@intFromEnum(tag) > @intFromEnum(Tag.dynamic) && i <= end) return error.CatchAllMustBeAtTheEnd;

                    return Pattern{
                        .len = @min(i + 1, end),
                        .value = switch (tag) {
                            .dynamic => .{ .dynamic = param },
                            .catch_all => .{ .catch_all = param },
                            .optional_catch_all => .{ .optional_catch_all = param },
                            else => unreachable,
                        },
                    };
                },
                else => {},
            }
            i += 1;
        }
        return Pattern{ .len = i, .value = .{ .static = HashedString.init(input[offset..i]) } };
    }
}

pub const PatternValidationResult = Pattern.ValidationResult;
