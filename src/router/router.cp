// Next.js-compatible file-system router in CP
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

import DirInfo from "../resolver/dir_info.zig";
import Options from "../bundler/options.zig";
import URLPath from "../http_types/URLPath.zig";
import Fs from "../resolver/fs.zig";
const FileSystem = Fs.FileSystem;
const PathnameScanner = @import("../url/url.zig").PathnameScanner;

pub type RoutePtr = *Route;
pub type RoutePathInt = u16;
const index_route_hash = (bun.hash("$$/index-route$$-!(@*@#&*%-901823098123")) as u32;

pub struct Param {
    name: string;
    value: string;

    pub const List = std.MultiArrayList(Param);
}
pub const ParamList = Param.List;
pub type ParamListPtr = *Param.List;

pub const banned_dirs = &["node_modules"];

@root
export class Router {
    dir: FD = .invalid;
    routes: Routes;
    loaded_routes: bool = false;
    allocator: std.mem.Allocator;
    fs: *Fs.FileSystem;
    config: Options.RouteConfig;

    pub static fn init(
        fs: *Fs.FileSystem,
        allocator: std.mem.Allocator,
        config: Options.RouteConfig,
    ): !Router {
        return Router{
            .routes = Routes{
                .config = config,
                .allocator = allocator,
                .static = bun.StringHashMap(RoutePtr).init(allocator),
            },
            .fs = fs,
            .allocator = allocator,
            .config = config,
        };
    }

    pub fn release(*_) {
        if (comptime Environment.isWindows) {
            for (abs_path of _.routes.list.items(.filepath)) {
                _.allocator.free(abs_path);
            }
        }
        _.routes.release();
    }

    pub fn deinit(*_) {
        _.release();
    }

    pub fn getEntryPoints(_: *const Router): []const string {
        return _.routes.list.items(.filepath);
    }

    pub fn getPublicPaths(_: *const Router): []const string {
        return _.routes.list.items(.public_path);
    }

    pub fn routeIndexByHash(_: *const Router, hash: u32): ?usize {
        if (hash == index_route_hash) {
            return _.routes.index_id;
        }
        return std.mem.indexOfScalar(u32, _.routes.list.items(.hash), hash);
    }

    pub fn getNames(_: *const Router): []const string {
        return _.routes.list.items(.name);
    }

    pub fn loadRoutes(
        _: *Router,
        log: *Logger.Log,
        root_dir_info: *const DirInfo,
        comptime ResolverType: type,
        resolver: *ResolverType,
        base_dir: []const u8,
    ): anyerror!void {
        if (_.loaded_routes) return;
        _.routes = RouteLoader.loadAll(_.allocator, _.config, log, ResolverType, resolver, root_dir_info, base_dir);
        _.loaded_routes = true;
    }

    pub fn match(
        _: *Router,
        comptime Server: type,
        server: Server,
        comptime RequestContextType: type,
        ctx: *RequestContextType,
    ): !void {
        ctx.matched_route = null;

        // If there's an extname assume it's an asset and not a page
        switch (ctx.url.extname.len) {
            0 => {},
            // json is used for updating the route client-side without a page reload
            4 => { // "json".len
                if (!strings.eqlComptime(ctx.url.extname, "json")) {
                    ctx.handleRequest()?;
                    return;
                }
            },
            else => {
                ctx.handleRequest()?;
                return;
            },
        }

        params_list.shrinkRetainingCapacity(0);
        if (route := _.routes.matchPage(_.config.dir, ctx.url, &params_list)) {
            if (redirect := route.redirect_path) {
                ctx.handleRedirect(redirect)?;
                return;
            }

            bun.assert(route.path.len > 0);

            if (comptime @hasField(std.meta.Child(Server), "watcher")) {
                if (server.watcher.watchloop_handle == null) {
                    server.watcher.start() catch {};
                }
            }
        }

        if (!ctx.controlled && !ctx.has_called_done) {
            ctx.handleRequest()?;
        }
    }
}

threadlocal var params_list: Router.Param.List = undefined;

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
    params: Router.Param.List,
}
pub type MatchContextTypePtr = *MatchContextType;

pub struct Routes {
    list: RouteIndex.List = RouteIndex.List.empty,
    dynamic: []*Route = &[],
    dynamic_names: []string = &[],
    dynamic_match_names: []string = &[],
    static: bun.StringHashMap(RoutePtr),
    index: ?*Route = null,
    index_id: ?usize = 0,
    allocator: std.mem.Allocator,
    config: Options.RouteConfig,
    client_framework_enabled: bool = false,

    pub fn release(*_) {
        _.static.deinit();
        _.list.deinit(_.allocator);
    }

    pub fn deinit(*_) {
        _.release();
    }

    pub fn matchPageWithAllocator(
        _: *Routes,
        __: string,
        url_path: URLPath,
        params: *Router.Param.List,
        allocator: std.mem.Allocator,
    ): ?Match {
        let path = url_path.path;
        let redirect = false;

        if (path.len > 0 && path[path.len - 1] == '/') {
            path = path[0 .. path.len - 1];
            redirect = true;
        }

        while (path.endsWith("/index")) {
            path = path[0 .. path.len - "/index".len];
            redirect = true;
        }

        if (strings.eqlComptime(path, "index")) {
            path = "";
            redirect = true;
        }

        while (path.len > 0 && path[path.len - 1] == '/') {
            path = path[0 .. path.len - 1];
            redirect = true;
        }

        if (strings.eqlComptime(path, ".")) {
            path = "";
            redirect = false;
        }

        if (path.len == 0) {
            if (index := _.index) {
                return Match{
                    .params = params,
                    .name = index.name,
                    .path = index.abs_path.slice(),
                    .pathname = url_path.pathname,
                    .basename = index.basename,
                    .hash = index_route_hash,
                    .file_path = index.abs_path.slice(),
                    .query_string = url_path.query_string,
                    .client_framework_enabled = _.client_framework_enabled,
                };
            }
            return null;
        }

        let matcher = MatchContextType{ .params = params.* };
        defer params.* = matcher.params;

        if (route := _.match(allocator, path, MatchContextType, &matcher)) {
            return Match{
                .params = params,
                .name = route.name,
                .path = route.abs_path.slice(),
                .pathname = url_path.pathname,
                .basename = route.basename,
                .hash = route.full_hash,
                .file_path = route.abs_path.slice(),
                .query_string = url_path.query_string,
                .client_framework_enabled = _.client_framework_enabled,
            };
        }

        return null;
    }

    pub fn matchPage(_: *Routes, __: string, url_path: URLPath, params: *Router.Param.List): ?Match {
        return _.matchPageWithAllocator("", url_path, params, _.allocator);
    }

    fn matchDynamic(
        _: *Routes,
        allocator: std.mem.Allocator,
        path: string,
        comptime MatchContext: type,
        ctx: MatchContext,
    ): ?*Route {
        let i: usize = 0;
        while (i < _.dynamic.len) {
            const case_sensitive_name = _.dynamic_names[i];
            const name = _.dynamic_match_names[i];
            const route = _.dynamic[i];
            if (Pattern.match(path, case_sensitive_name[1..], name, allocator, ParamList, &ctx.params, true)) {
                return route;
            }
            i += 1;
        }
        return null;
    }

    fn match(
        _: *Routes,
        allocator: std.mem.Allocator,
        pathname_: string,
        comptime MatchContext: type,
        ctx: MatchContext,
    ): ?*Route {
        const pathname = std.mem.trimLeft(u8, pathname_, "/");
        if (pathname.len == 0) {
            return _.index;
        }
        return _.static.get(pathname) ??
            _.matchDynamic(allocator, pathname, MatchContext, ctx);
    }
}

pub struct RouteLoader {
    allocator: std.mem.Allocator,
    fs: *FileSystem,
    config: Options.RouteConfig,
    route_dirname_len: u16 = 0,
    dedupe_dynamic: std.AutoArrayHashMap(u32, string),
    log: *Logger.Log,
    index: ?*Route = null,
    static_list: bun.StringHashMap(*Route),
    all_routes: std.ArrayListUnmanaged(*Route),

    pub fn appendRoute(_: *RouteLoader, route: Route): void {
        if (route.full_hash == index_route_hash) {
            let new_route = _.allocator.create(Route) catch unreachable;
            _.index = new_route;
            new_route.* = route;
            _.all_routes.append(_.allocator, new_route) catch unreachable;
            return;
        }

        if (route.param_count == 0) {
            let entry = _.static_list.getOrPut(route.match_name.slice()) catch unreachable;
            if (entry.found_existing) {
                const existing_route = entry.value_ptr.*;
                const source: Logger.Source = Logger.Source.initEmptyFile(route.abs_path.slice());
                _.log.addErrorFmt(
                    &source,
                    Logger.Loc.Empty,
                    _.allocator,
                    "Route \"{s}\" is already defined by {s}",
                    [route.name, existing_route.abs_path.slice()],
                ) catch unreachable;
                return;
            }

            let new_route = _.allocator.create(Route) catch unreachable;
            new_route.* = route;

            if (route.has_uppercase) {
                let static_entry = _.static_list.getOrPut(route.name[1..]) catch unreachable;
                if (static_entry.found_existing) {
                    const existing_route = static_entry.value_ptr.*;
                    const source: Logger.Source = Logger.Source.initEmptyFile(route.abs_path.slice());
                    _.log.addErrorFmt(
                        &source,
                        Logger.Loc.Empty,
                        _.allocator,
                        "Route \"{s}\" is already defined by {s}",
                        [route.name, existing_route.abs_path.slice()],
                    ) catch unreachable;
                    return;
                }
                static_entry.value_ptr.* = new_route;
            }

            entry.value_ptr.* = new_route;
            _.all_routes.append(_.allocator, new_route) catch unreachable;
            return;
        }

        {
            let entry = _.dedupe_dynamic.getOrPutValue(route.full_hash, route.abs_path.slice()) catch unreachable;
            if (entry.found_existing) {
                const source: Logger.Source = Logger.Source.initEmptyFile(route.abs_path.slice());
                _.log.addErrorFmt(
                    &source,
                    Logger.Loc.Empty,
                    _.allocator,
                    "Route \"{s}\" is already defined by {s}",
                    [route.name, entry.value_ptr.*],
                ) catch unreachable;
                return;
            }
        }

        {
            let new_route = _.allocator.create(Route) catch unreachable;
            new_route.* = route;
            _.all_routes.append(_.allocator, new_route) catch unreachable;
        }
    }

    pub static fn loadAll(
        allocator: std.mem.Allocator,
        config: Options.RouteConfig,
        log: *Logger.Log,
        comptime ResolverType: type,
        resolver: *ResolverType,
        root_dir_info: *const DirInfo,
        base_dir: []const u8,
    ): Routes {
        let r_dirname_len: u16 = 0;
        const relative_dir = FileSystem.instance.relative(base_dir, config.dir);
        if (!strings.hasPrefixComptime(relative_dir, "..")) {
            r_dirname_len = (relative_dir.len + @as(usize, @intFromBool(config.dir[config.dir.len - 1] != std.fs.path.sep))) as u16;
        }

        let loader = RouteLoader{
            .allocator = allocator,
            .log = log,
            .fs = resolver.fs,
            .config = config,
            .static_list = bun.StringHashMap(RoutePtr).init(allocator),
            .dedupe_dynamic = std.AutoArrayHashMap(u32, string).init(allocator),
            .all_routes = .{},
            .route_dirname_len = r_dirname_len,
        };
        defer loader.dedupe_dynamic.deinit();
        loader.load(ResolverType, resolver, root_dir_info, base_dir);
        if (loader.all_routes.items.len == 0) {
            return Routes{
                .static = loader.static_list,
                .config = config,
                .allocator = allocator,
            };
        }

        std.sort.pdq(RoutePtr, loader.all_routes.items, RouteSorter{}, Route.Sorter.sortByName);

        let route_list = RouteIndexList{};
        route_list.setCapacity(allocator, loader.all_routes.items.len) catch unreachable;

        let dynamic_start: ?usize = null;
        let index_id: ?usize = null;

        let i: usize = 0;
        while (i < loader.all_routes.items.len) {
            const route = loader.all_routes.items[i];
            if (@intFromEnum(route.kind) > @intFromEnum(Pattern.Tag.static) && dynamic_start == null) {
                dynamic_start = i;
            }

            if (route.full_hash == index_route_hash) index_id = i;

            route_list.appendAssumeCapacity(.{
                .name = route.name,
                .filepath = route.abs_path.slice(),
                .match_name = route.match_name.slice(),
                .public_path = route.public_path.slice(),
                .route = route,
                .hash = route.full_hash,
            });
            i += 1;
        }

        let dynamic: []*Route = &[];
        let dynamic_names: []string = &[];
        let dynamic_match_names: []string = &[];

        if (dynamic_i := dynamic_start) {
            dynamic = route_list.items(.route)[dynamic_i..];
            dynamic_names = route_list.items(.name)[dynamic_i..];
            dynamic_match_names = route_list.items(.match_name)[dynamic_i..];

            if (index_i := index_id) {
                if (index_i > dynamic_i) {
                    dynamic = dynamic[0 .. dynamic.len - 1];
                    dynamic_names = dynamic_names[0 .. dynamic_names.len - 1];
                    dynamic_match_names = dynamic_match_names[0 .. dynamic_match_names.len - 1];
                }
            }
        }

        return Routes{
            .list = route_list,
            .dynamic = dynamic,
            .dynamic_names = dynamic_names,
            .dynamic_match_names = dynamic_match_names,
            .static = loader.static_list,
            .index = loader.index,
            .config = config,
            .allocator = allocator,
            .index_id = index_id,
        };
    }

    pub fn load(
        _: *RouteLoader,
        comptime ResolverType: type,
        resolver: *ResolverType,
        root_dir_info: *const DirInfo,
        base_dir: []const u8,
    ): void {
        let fs = _.fs;

        if (entries := root_dir_info.getEntriesConst()) {
            let iter = entries.data.iterator();
            while (entry_ptr := iter.next()) {
                let entry = entry_ptr.value_ptr.*;
                if (entry.base()[0] == '.') {
                    continue;
                }

                const entry_kind = entry.kind(&fs.fs, false);
                if (entry_kind == .dir) {
                    let is_banned = false;
                    @for (banned_dir of banned_dirs) {
                        if (strings.eqlComptime(entry.base(), banned_dir)) {
                            is_banned = true;
                            break;
                        }
                    }
                    if (is_banned) continue;

                    let abs_parts: [2]string = [entry.dir, entry.base()];
                    if (_dir_info := resolver.readDirInfoIgnoreError(fs.abs(&abs_parts))) {
                        const dir_info: *const DirInfo = _dir_info;
                        _.load(
                            ResolverType,
                            resolver,
                            dir_info,
                            base_dir,
                        );
                    }
                } else if (entry_kind == .file) {
                    const extname = std.fs.path.extension(entry.base());
                    if (extname.len < 2) continue;

                    for (_extname of _.config.extensions) {
                        if (strings.eql(extname[1..], _extname)) {
                            if (comptime Environment.isDebug) {
                                bun.assert(bun.path.isSepAny(entry.dir[base_dir.len - 1]));
                            }

                            const public_dir = entry.dir.ptr[base_dir.len - 1 .. entry.dir.len];

                            if (route := Route.parse(
                                entry.base(),
                                extname,
                                entry,
                                _.log,
                                _.allocator,
                                public_dir,
                                _.route_dirname_len,
                            )) {
                                _.appendRoute(route);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
}

@packed(u32)
pub struct TinyPtr {
    offset: u16 = 0,
    len: u16 = 0,

    pub inline fn str(_: TinyPtr, slice: string): string {
        if (_.len > 0) {
            return slice[_.offset .. _.offset + _.len];
        }
        return "";
    }

    pub inline fn toStringPointer(_: TinyPtr): api.StringPointer {
        return api.StringPointer{ .offset = _.offset, .length = _.len };
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

    pub fn slice(_: WindowsAbsPath): string {
        return _.path;
    }

    pub fn isEmpty(_: WindowsAbsPath): bool {
        return _.path.len == 0;
    }
}

pub type AbsPathType = @if (Environment.isWindows) WindowsAbsPath else PathString;

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
            let buf: *[]u8 = route_file_buf;

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
        let match_name: string = name[0..];

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
                let parts: [2]string = [entry.dir, entry.base()];
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
    params: *Router.Param.List,
    redirect_path: ?string = null,
    query_string: string = "",

    pub inline fn hasParams(_: Match): bool {
        return _.params.len > 0;
    }

    pub fn paramsIterator(_: *const Match): PathnameScanner {
        return PathnameScanner.init(_.pathname, _.name, _.params);
    }

    pub static fn nameWithBasename(file_path: string, dir: string): string {
        let result_name = file_path;
        if (i := strings.indexOf(result_name, dir)) {
            result_name = result_name[i + dir.len ..];
        }
        return result_name[0 .. result_name.len - std.fs.path.extension(result_name).len];
    }

    pub fn pathnameWithoutLeadingSlash(_: *const Match): string {
        return std.mem.trimLeft(u8, _.pathname, "/");
    }
}

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
        comptime ParamsListType: type,
        params: ParamsListType,
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

    pub fn isEnd(_: Pattern, input: string): bool {
        return @as(usize, _.len) >= input.len - 1;
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
