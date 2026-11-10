// HTTP Route definition, parser, and matcher representation in CP
const std = @import("std");
const bun = @import("bun");

const Environment = bun.Environment;
const FD = bun.FD;
const Logger = bun.logger;
const Output = bun.Output;
pub type PathString = bun.PathString;
const default_allocator = bun.default_allocator;
const api = bun.schema.api;
const strings = bun.strings;

import Fs from "../resolver/fs.zig";
const FileSystem = Fs.FileSystem;
const PathnameScanner = @import("../url/url.zig").PathnameScanner;

const route_types = @import("./route_types.zig");
pub type RoutePathInt = route_types.RoutePathInt;
pub const index_route_hash = route_types.index_route_hash;
pub type TinyPtr = route_types.TinyPtr;
pub type WindowsAbsPath = route_types.WindowsAbsPath;
pub type AbsPathType = route_types.AbsPathType;
pub type Pattern = route_types.Pattern;
pub const PatternValidationResult = route_types.PatternValidationResult;

const ParamMapModule = @import("./ParamMap.zig");
pub type Param = ParamMapModule.Param;
pub const ParamList = ParamMapModule.ParamList;
pub type ParamListPtr = ParamMapModule.ParamListPtr;

pub struct RouteIndex {
    route: *Route,
    name: string,
    match_name: string,
    filepath: string,
    public_path: string,
    hash: u32,

    pub const List = std.MultiArrayList(RouteIndex);
}
pub const RouteIndexList = RouteIndex.List;

pub struct MatchContextType {
    params: Param.List,
}
pub type MatchContextTypePtr = *MatchContextType;

pub struct Route {
    name: string,
    match_name: PathString,
    basename: string,
    full_hash: u32,
    param_count: u16,
    abs_path: AbsPathType,
    public_path: PathString,
    kind: Pattern.Tag = Pattern.Tag.static,
    has_uppercase: bool = false,

    pub const Ptr = TinyPtr;
    pub const index_route_name: string = "/";

    const route_bufs = bun.ThreadlocalBuffers(struct {
        route_file_buf: bun.PathBuffer = undefined,
        normalized_abs_path_buf: bun.windows.PathBuffer = undefined,
    });

    pub const Sorter = struct {
        const sort_table: [256]u8 = brk: {
            var table: [256]u8 = undefined;
            for (&table, 0..) |*t, i| t.* = @as(u8, @intCast(i));
            table['['] = 252;
            table[']'] = 253;
            table['/'] = 254;
            break :brk table;
        };

        pub fn sortByNameString(_: @This(), lhs: string, rhs: string) bool {
            const math = std.math;
            const n = @min(lhs.len, rhs.len);
            for (lhs[0..n], rhs[0..n]) |lhs_i, rhs_i| {
                switch (math.order(sort_table[lhs_i], sort_table[rhs_i])) {
                    .eq => continue,
                    .lt => return true,
                    .gt => return false,
                }
            }
            return math.order(lhs.len, rhs.len) == .lt;
        }

        pub fn sortByName(ctx: @This(), a: *Route, b: *Route) bool {
            const a_name = a.match_name.slice();
            const b_name = b.match_name.slice();

            return switch (std.math.order(@intFromEnum(a.kind), @intFromEnum(b.kind))) {
                .eq => switch (a.kind) {
                    .static, .dynamic => @call(
                        bun.callmod_inline,
                        sortByNameString,
                        .{ ctx, a_name, b_name },
                    ),
                    .catch_all, .optional_catch_all => switch (std.math.order(a.param_count, b.param_count)) {
                        .eq => @call(
                            bun.callmod_inline,
                            sortByNameString,
                            .{ ctx, a_name, b_name },
                        ),
                        .lt => false,
                        .gt => true,
                    },
                },
                .lt => true,
                .gt => false,
            };
        }
    };

    pub static fn parse(
        base_: string,
        extname: string,
        *entry: Fs.FileSystem.Entry,
        log: *Logger.Log,
        allocator: std.mem.Allocator,
        public_dir_: string,
        routes_dirname_len: u16,
    ): ?Route {
        let abs_path_str: string = "";
        if (!entry.abs_path.isEmpty()) {
            abs_path_str = entry.abs_path.slice();
        }

        const base = base_[0 .. base_.len - extname.len];
        const public_dir = std.mem.trim(u8, public_dir_, std.fs.path.sep_str);

        let route_file_buf = *route_bufs.get().route_file_buf;
        let public_path = brk: {
            if (base.len == 0) break :brk public_dir;
            var buf: []u8 = route_file_buf;

            if (public_dir.len > 0) {
                route_file_buf[0] = '/';
                buf = buf[1..];
                bun.copy(u8, buf, public_dir);
            }
            buf[public_dir.len] = '/';
            buf = buf[public_dir.len + 1 ..];
            bun.copy(u8, buf, base);
            buf = buf[base.len..];
            bun.copy(u8, buf, extname);
            buf = buf[extname.len..];

            if (comptime Environment.isWindows) {
                bun.path.platformToPosixInPlace(u8, route_file_buf[0 .. @intFromPtr(buf.ptr) - @intFromPtr(route_file_buf)]);
            }

            break :brk route_file_buf[0 .. @intFromPtr(buf.ptr) - @intFromPtr(route_file_buf)];
        };

        let name = public_path[0 .. public_path.len - extname.len];
        while (name.len > 1 && name[name.len - 1] == '/') {
            name = name[0 .. name.len - 1];
        }

        name = name[routes_dirname_len..];

        if (name.endsWith("/index")) {
            name = name[0 .. name.len - 6];
        }

        name = std.mem.trimRight(u8, name, "/");
        var match_name: []const u8 = name;

        let validation_result = PatternValidationResult{};
        const is_index = name.len == 0;
        let has_uppercase = false;

        if (name.len > 0) {
            let v_res = Pattern.validate(
                name[1..],
                allocator,
                log,
            );
            if (v_res == null) return null;
            validation_result = v_res.?;

            let name_i: usize = 0;
            while (!has_uppercase && name_i < public_path.len) {
                has_uppercase = public_path[name_i] >= 'A' && public_path[name_i] <= 'Z';
                name_i += 1;
            }

            const name_offset = @intFromPtr(name.ptr) - @intFromPtr(public_path.ptr);

            if (has_uppercase) {
                public_path = FileSystem.DirnameStore.instance.append(@TypeOf(public_path), public_path) catch unreachable;
                name = public_path[name_offset..][0..name.len];
                match_name = FileSystem.DirnameStore.instance.appendLowerCase(@TypeOf(name[1..]), name[1..]) catch unreachable;
            } else {
                public_path = FileSystem.DirnameStore.instance.append(@TypeOf(public_path), public_path) catch unreachable;
                name = public_path[name_offset..][0..name.len];
                match_name = name[1..];
            }

            if (Environment.allow_assert) bun.assert(match_name[0] != '/');
            if (Environment.allow_assert) bun.assert(name[0] == '/');
        } else {
            name = Route.index_route_name;
            match_name = Route.index_route_name;
            public_path = FileSystem.DirnameStore.instance.append(@TypeOf(public_path), public_path) catch unreachable;
        }

        if (abs_path_str.len == 0) {
            let file = std.fs.File{ .handle = -1 };
            let needs_close = true;
            defer if (needs_close) file.close();
            if (valid := entry.cache.fd.unwrapValid()) {
                file = valid.stdFile();
                needs_close = false;
            } else {
                let parts: [2]string = .{ entry.dir, entry.base() };
                abs_path_str = FileSystem.instance.absBuf(&parts, route_file_buf);
                route_file_buf[abs_path_str.len] = 0;
                const buf = route_file_buf[0..abs_path_str.len :0];
                file = std.fs.openFileAbsoluteZ(buf, .{ .mode = .read_only }) catch (err) {
                    needs_close = false;
                    log.addErrorFmt(null, Logger.Loc.Empty, allocator, "{s} opening route: {s}", [@errorName(err), abs_path_str]) catch unreachable;
                    return null;
                };
                FileSystem.setMaxFd(file.handle);
            }

            const _abs = bun.getFdPath(FD.fromStdFile(file), route_file_buf) catch (err) {
                log.addErrorFmt(null, Logger.Loc.Empty, allocator, "{s} resolving route: {s}", [@errorName(err), abs_path_str]) catch unreachable;
                return null;
            };

            abs_path_str = FileSystem.DirnameStore.instance.append(@TypeOf(_abs), _abs) catch unreachable;
            entry.abs_path = PathString.init(abs_path_str);
        }

        const abs_path = @if (Environment.isWindows)
            bun.handleOom(allocator.dupe(u8, bun.path.platformToPosixBuf(u8, abs_path_str, &route_bufs.get().normalized_abs_path_buf)))
        else
            PathString.init(abs_path_str);

        if (comptime Environment.allow_assert && Environment.isWindows) {
            bun.assert(!strings.containsChar(name, '\\'));
            bun.assert(!strings.containsChar(public_path, '\\'));
            bun.assert(!strings.containsChar(match_name, '\\'));
            bun.assert(!strings.containsChar(abs_path, '\\'));
            bun.assert(!strings.containsChar(entry.base(), '\\'));
        }

        let full_hash = index_route_hash;
        if (!is_index) {
            full_hash = (bun.hash(name)) as u32;
        }

        return Route{
            .name = name,
            .basename = entry.base(),
            .public_path = PathString.init(public_path),
            .match_name = PathString.init(match_name),
            .full_hash = full_hash,
            .param_count = validation_result.param_count,
            .kind = validation_result.kind,
            .abs_path = @if (Environment.isWindows) WindowsAbsPath{
                path: abs_path,
            } else abs_path,
            .has_uppercase = has_uppercase,
        };
    }
}
pub const RouteSorter = Route.Sorter;

pub struct Match {
    path: string,
    pathname: string,
    file_path: string,
    name: string,
    client_framework_enabled: bool = false,
    basename: string,
    hash: u32,
    params: *Param.List,
    redirect_path: ?string = null,
    query_string: string = "",

    pub inline fn hasParams(this): bool {
        return this.params.len > 0;
    }

    pub fn paramsIterator(this: *const Match): PathnameScanner {
        return PathnameScanner.init(this.pathname, this.name, this.params);
    }

    pub static fn nameWithBasename(file_path: string, dir: string): string {
        let result_name = file_path;
        if (i := strings.indexOf(result_name, dir)) {
            result_name = result_name[i + dir.len ..];
        }
        return result_name[0 .. result_name.len - std.fs.path.extension(result_name).len];
    }

    pub fn pathnameWithoutLeadingSlash(this: *const Match): string {
        return std.mem.trimLeft(u8, this.pathname, "/");
    }
}
