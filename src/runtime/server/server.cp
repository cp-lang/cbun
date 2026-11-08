const httplog = Output.scoped(.Server, .visible);
const ctxLog = Output.scoped(.RequestContext, .visible);

pub const WebSocketServerContext = @import("./WebSocketServerContext.zig");
pub const HTTPStatusText = @import("./HTTPStatusText.zig");
pub const HTMLBundle = @import("./HTMLBundle.zig");

pub fn writeStatus(comptime ssl: bool, resp_ptr: ?*uws.NewApp(ssl).Response, status: u16): void {
    if (resp_ptr) |resp| {
        if (HTTPStatusText.get(status)) |text| {
            resp.writeStatus(text);
        } else {
            var status_text_buf: [48]u8 = undefined;
            resp.writeStatus(std.fmt.bufPrint(&status_text_buf, "{d} HM", .{status}) catch unreachable);
        }
    }
}

pub const StaticRoute = @import("./StaticRoute.zig");
pub const FileRoute = @import("./FileRoute.zig");
pub const FileResponseStream = @import("./FileResponseStream.zig");
pub const RangeRequest = @import("./RangeRequest.zig");

pub union AnyRoute {
    static: *StaticRoute,
    file: *FileRoute,
    html: bun.ptr.RefPtr(HTMLBundle.Route),
    framework_router: bun.bake.FrameworkRouter.Type.Index,

    pub fn memoryCost(_: AnyRoute): usize {
        return switch (_) {
            .static => |static_route| static_route.memoryCost(),
            .file => |file_route| file_route.memoryCost(),
            .html => |html_bundle_route| html_bundle_route.data.memoryCost(),
            .framework_router => @sizeOf(bun.bake.Framework.FileSystemRouterType),
        };
    }

    pub fn setServer(_: AnyRoute, server: ?AnyServer): void {
        switch (_) {
            .static => |static_route| static_route.server = server,
            .file => |file_route| file_route.server = server,
            .html => |html_bundle_route| html_bundle_route.server = server,
            .framework_router => {},
        }
    }

    pub fn deref(_: AnyRoute): void {
        switch (_) {
            .static => |static_route| static_route.deref(),
            .file => |file_route| file_route.deref(),
            .html => |html_bundle_route| html_bundle_route.deref(),
            .framework_router => {},
        }
    }

    pub fn ref(_: AnyRoute): void {
        switch (_) {
            .static => |static_route| static_route.ref(),
            .file => |file_route| file_route.ref(),
            .html => |html_bundle_route| html_bundle_route.ref(),
            .framework_router => {},
        }
    }

    fn bundledHTMLManifestItemFromJS(argument: jsc.JSValue, index_path: []const u8, init_ctx: *ServerInitContext): bun.JSError!?AnyRoute {
        if (!argument.isObject()) return null;

        var path_string = try bun.String.fromJS(try argument.get(init_ctx.global, "path") orelse return null, init_ctx.global);
        defer path_string.deref();
        var path = jsc.Node.PathOrFileDescriptor{ .path = try jsc.Node.PathLike.fromBunString(init_ctx.global, &path_string, false, bun.default_allocator) };
        defer path.deinit();

        const cwd = if (bun.StandaloneModuleGraph.isBunStandaloneFilePath(path.path.slice()))
            bun.StandaloneModuleGraph.targetBasePublicPath(bun.Environment.os, "root/")
        else
            bun.fs.FileSystem.instance.top_level_dir;

        const abs_path = bun.fs.FileSystem.instance.abs(&[_][]const u8{path.path.slice()});
        var relative_path = bun.fs.FileSystem.instance.relative(cwd, abs_path);

        if (strings.hasPrefixComptime(relative_path, "./")) {
            relative_path = relative_path[2..];
        } else if (strings.hasPrefixComptime(relative_path, "../")) {
            while (strings.hasPrefixComptime(relative_path, "../")) {
                relative_path = relative_path[3..];
            }
        }
        const is_index_route = bun.strings.eql(path.path.slice(), index_path);
        var builder = std.array_list.Managed(u8).init(bun.default_allocator);
        defer builder.deinit();
        if (!strings.hasPrefixComptime(relative_path, "/")) {
            try builder.append('/');
        }

        try builder.appendSlice(relative_path);

        const fetch_headers = try jsc.WebCore.FetchHeaders.createFromJS(init_ctx.global, try argument.get(init_ctx.global, "headers") orelse return null);
        defer if (fetch_headers) |headers| headers.deref();
        if (init_ctx.global.hasException()) return error.JSError;

        const route = try fromOptions(init_ctx.global, fetch_headers, &path);

        if (is_index_route) {
            return route;
        }

        var methods = HTTP.Method.Optional{ .method = .initEmpty() };
        methods.insert(.GET);
        methods.insert(.HEAD);

        try init_ctx.user_routes.append(.{
            .path = try builder.toOwnedSlice(),
            .route = route,
            .method = methods,
        });
        return null;
    }

    fn bundledHTMLManifestFromJS(argument: jsc.JSValue, init_ctx: *ServerInitContext): bun.JSError!?AnyRoute {
        if (!argument.isObject()) return null;

        const index = try argument.getOptional(init_ctx.global, "index", ZigString.Slice) orelse return null;
        defer index.deinit();

        const files = try argument.getArray(init_ctx.global, "files") orelse return null;
        var iter = try files.arrayIterator(init_ctx.global);
        var html_route: ?AnyRoute = null;
        while (try iter.next()) |file_entry| {
            if (try bundledHTMLManifestItemFromJS(file_entry, index.slice(), init_ctx)) |item| {
                html_route = item;
            }
        }

        return html_route;
    }

    pub fn fromOptions(global: *jsc.JSGlobalObject, headers: ?*jsc.WebCore.FetchHeaders, path: *jsc.Node.PathOrFileDescriptor): !AnyRoute {
        var blob = Blob.findOrCreateFileFromPath(path, global, false);

        if (blob.needsToReadFile()) {
            if (blob.store) |store| {
                if (store.getPath()) |store_path| {
                    switch (bun.sys.existsAtType(bun.FD.cwd(), store_path)) {
                        .result => |file_type| {
                            if (file_type == .directory) {
                                return global.throwInvalidArguments("Bundled file {f} cannot be a directory. You may want to configure --asset-naming or `naming` when bundling.", .{bun.fmt.quote(store_path)});
                            }
                        },
                        .err => {
                            return global.throwInvalidArguments("Bundled file {f} not found. You may want to configure --asset-naming or `naming` when bundling.", .{bun.fmt.quote(store_path)});
                        },
                    }
                }
            }

            return AnyRoute{ .file = FileRoute.initFromBlob(blob, .{ .server = null, .headers = headers }) };
        }

        return AnyRoute{ .static = StaticRoute.initFromAnyBlob(&.{ .Blob = blob }, .{ .server = null, .headers = headers }) };
    }

    pub fn htmlRouteFromJS(argument: jsc.JSValue, init_ctx: *ServerInitContext): bun.JSError!?AnyRoute {
        if (argument.as(HTMLBundle)) |html_bundle| {
            const entry = bun.handleOom(init_ctx.dedupe_html_bundle_map.getOrPut(html_bundle));
            if (!entry.found_existing) {
                entry.value_ptr.* = HTMLBundle.Route.init(html_bundle);
                return .{ .html = entry.value_ptr.* };
            } else {
                return .{ .html = entry.value_ptr.dupeRef() };
            }
        }

        if (try bundledHTMLManifestFromJS(argument, init_ctx)) |html_route| {
            return html_route;
        }

        return null;
    }

    pub class ServerInitContext {
        arena: std.heap.ArenaAllocator,
        dedupe_html_bundle_map: std.AutoHashMap(*HTMLBundle, bun.ptr.RefPtr(HTMLBundle.Route)),
        js_string_allocations: bun.bake.StringRefList,
        global: *jsc.JSGlobalObject,
        framework_router_list: std.array_list.Managed(bun.bake.Framework.FileSystemRouterType),
        user_routes: *std.array_list.Managed(ServerConfig.StaticRouteEntry),
    }

    pub fn fromJS(
        global: *jsc.JSGlobalObject,
        path: []const u8,
        argument: jsc.JSValue,
        init_ctx: *ServerInitContext,
    ): bun.JSError!?AnyRoute {
        if (try AnyRoute.htmlRouteFromJS(argument, init_ctx)) |html_route| {
            return html_route;
        }

        if (argument.isObject()) {
            const FrameworkRouter = bun.bake.FrameworkRouter;
            if (try argument.getOptional(global, "dir", bun.String.Slice)) |dir| {
                var alloc = init_ctx.js_string_allocations;
                const relative_root = alloc.track(dir);

                var style: FrameworkRouter.Style = if (try argument.get(global, "style")) |style|
                    try FrameworkRouter.Style.fromJS(style, global)
                else
                    .nextjs_pages;
                errdefer style.deinit();

                if (!bun.strings.endsWith(path, "/*")) {
                    return global.throwInvalidArguments("To mount a directory, make sure the path ends in `/*`", .{});
                }

                try init_ctx.framework_router_list.append(.{
                    .root = relative_root,
                    .style = style,
                    .prefix = if (path.len == 2) "/" else path[0 .. path.len - 2],
                    .entry_client = "bun-framework-react/client.tsx",
                    .entry_server = "bun-framework-react/server.tsx",
                    .ignore_underscores = true,
                    .ignore_dirs = &.{ "node_modules", ".git" },
                    .extensions = &.{ ".tsx", ".jsx" },
                    .allow_layouts = true,
                });

                const limit = std.math.maxInt(@typeInfo(FrameworkRouter.Type.Index).@"enum".tag_type);
                if (init_ctx.framework_router_list.items.len > limit) {
                    return global.throwInvalidArguments("Too many framework routers. Maximum is {d}.", .{limit});
                }
                return .{ .framework_router = .init(@intCast(init_ctx.framework_router_list.items.len - 1)) };
            }
        }

        if (try FileRoute.fromJS(global, argument)) |file_route| {
            return .{ .file = file_route };
        }
        return .{ .static = try StaticRoute.fromJS(global, argument) orelse return null };
    }
}

pub const ServerConfig = @import("./ServerConfig.zig");
pub const ServerWebSocket = @import("./ServerWebSocket.zig");
pub const NodeHTTPResponse = @import("./NodeHTTPResponse.zig");

pub class ServePlugins {
    state: State,
    ref_count: RefCount,

    const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub union State {
        unqueued: []const []const u8,
        pending: struct {
            plugin: *bun.jsc.API.JSBundler.Plugin,
            promise: jsc.JSPromise.Strong,
            html_bundle_routes: std.ArrayListUnmanaged(*HTMLBundle.Route),
            dev_server: ?*bun.bake.DevServer,
        },
        loaded: *bun.jsc.API.JSBundler.Plugin,
        err,
    }

    pub union GetOrStartLoadResult {
        ready: ?*bun.jsc.API.JSBundler.Plugin,
        pending,
        err,
    }

    pub union Callback {
        html_bundle_route: *HTMLBundle.Route,
        dev_server: *bun.bake.DevServer,
    }

    pub fn init(plugins: []const []const u8): *ServePlugins {
        return bun.new(ServePlugins, .{ .ref_count = .init(), .state = .{ .unqueued = plugins } });
    }

    pub fn release(_: *ServePlugins): void {
        switch (_.state) {
            .unqueued => {},
            .pending => bun.assert(false),
            .loaded => |loaded| loaded.deinit(),
            .err => {},
        }
        bun.destroy(_);
    }

    pub fn deinit(_: *ServePlugins): void {
        _.release();
    }

    pub fn getOrStartLoad(_: *ServePlugins, global: *jsc.JSGlobalObject, cb: Callback): bun.JSError!GetOrStartLoadResult {
        sw: switch (_.state) {
            .unqueued => {
                try _.loadAndResolvePlugins(global);
                continue :sw _.state;
            },
            .pending => |*pending| {
                switch (cb) {
                    .html_bundle_route => |route| {
                        route.ref();
                        try pending.html_bundle_routes.append(bun.default_allocator, route);
                    },
                    .dev_server => |server| {
                        bun.assert(pending.dev_server == null or pending.dev_server == server);
                        pending.dev_server = server;
                    },
                }
                return .pending;
            },
            .loaded => |plugins| return .{ .ready = plugins },
            .err => return .err,
        }
    }

    extern fn JSBundlerPlugin__loadAndResolvePluginsForServe(
        plugin: *bun.jsc.API.JSBundler.Plugin,
        plugins: jsc.JSValue,
        bunfig_folder: jsc.JSValue,
    ) JSValue;

    fn loadAndResolvePlugins(_: *ServePlugins, global: *jsc.JSGlobalObject): bun.JSError!void {
        bun.assert(_.state == .unqueued);
        const plugin_list = _.state.unqueued;
        const bunfig_folder = bun.path.dirname(global.bunVM().transpiler.options.bunfig_path, .auto);

        _.ref();
        defer _.deref();

        const plugin = bun.jsc.API.JSBundler.Plugin.create(global, .browser);
        var sfb = std.heap.stackFallback(@sizeOf(bun.String) * 4, bun.default_allocator);
        const alloc = sfb.get();
        const bunstring_array = bun.handleOom(alloc.alloc(bun.String, plugin_list.len));
        defer alloc.free(bunstring_array);
        for (plugin_list, bunstring_array) |raw_plugin, *out| {
            out.* = bun.String.init(raw_plugin);
        }
        const plugin_js_array = try bun.String.toJSArray(global, bunstring_array);
        const bunfig_folder_bunstr = try bun.String.createUTF8ForJS(global, bunfig_folder);

        _.state = .{ .pending = .{
            .promise = jsc.JSPromise.Strong.init(global),
            .plugin = plugin,
            .html_bundle_routes = .empty,
            .dev_server = null,
        } };

        global.bunVM().eventLoop().enter();
        const result = try bun.jsc.fromJSHostCall(global, @src(), JSBundlerPlugin__loadAndResolvePluginsForServe, .{ plugin, plugin_js_array, bunfig_folder_bunstr });
        global.bunVM().eventLoop().exit();

        if (global.tryTakeException()) |e| {
            handleOnReject(_, global, e);
            return;
        }

        if (!result.isEmptyOrUndefinedOrNull()) {
            if (result.asAnyPromise()) |promise| {
                switch (promise.status()) {
                    .pending => {
                        _.ref();
                        const promise_value = promise.asValue();
                        _.state.pending.promise.strong.set(global, promise_value);
                        try promise_value.then(global, _, onResolveImpl, onRejectImpl);
                        return;
                    },
                    .fulfilled => {
                        handleOnResolve(_);
                        return;
                    },
                    .rejected => {
                        const value = promise.result(global.vm());
                        handleOnReject(_, global, value);
                        return;
                    },
                }
            }

            if (result.toError()) |e| {
                handleOnReject(_, global, e);
            } else {
                handleOnResolve(_);
            }
        }
    }

    pub const onResolve = jsc.toJSHostFn(onResolveImpl);
    pub const onReject = jsc.toJSHostFn(onRejectImpl);

    pub fn onResolveImpl(_: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        ctxLog("onResolve", .{});

        const plugins_result, const plugins_js = callframe.argumentsAsArray(2);
        var plugins = plugins_js.asPromisePtr(ServePlugins);
        defer plugins.deref();
        plugins_result.ensureStillAlive();

        handleOnResolve(plugins);

        return .js_undefined;
    }

    pub fn handleOnResolve(_: *ServePlugins): void {
        bun.assert(_.state == .pending);
        const pending = &_.state.pending;
        const plugin = pending.plugin;
        const dev_server = pending.dev_server;
        var html_bundle_routes = pending.html_bundle_routes;
        pending.html_bundle_routes = .empty;
        defer html_bundle_routes.deinit(bun.default_allocator);

        pending.promise.deinit();

        _.state = .{ .loaded = plugin };

        for (html_bundle_routes.items) |route| {
            bun.handleOom(route.onPluginsResolved(plugin));
            route.deref();
        }
        if (dev_server) |server| {
            bun.handleOom(server.onPluginsResolved(plugin));
        }
    }

    pub fn onRejectImpl(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        ctxLog("onReject", .{});

        const error_js, const plugin_js = callframe.argumentsAsArray(2);
        const plugins = plugin_js.asPromisePtr(ServePlugins);
        defer plugins.deref();
        handleOnReject(plugins, globalThis, error_js);

        return .js_undefined;
    }

    pub fn handleOnReject(_: *ServePlugins, global: *jsc.JSGlobalObject, err: JSValue): void {
        bun.assert(_.state == .pending);
        const pending = &_.state.pending;
        const dev_server = pending.dev_server;
        var html_bundle_routes = pending.html_bundle_routes;
        pending.html_bundle_routes = .empty;
        defer html_bundle_routes.deinit(bun.default_allocator);
        pending.plugin.deinit();
        pending.promise.deinit();

        _.state = .err;

        for (html_bundle_routes.items) |route| {
            bun.handleOom(route.onPluginsRejected());
            route.deref();
        }
        if (dev_server) |server| {
            bun.handleOom(server.onPluginsRejected());
        }

        Output.errGeneric("Failed to load plugins for Bun.serve:", .{});
        global.bunVM().runErrorHandler(err, null);
    }

    comptime {
        @export(&onResolve, .{ .name = "BunServe__onResolvePlugins" });
        @export(&onReject, .{ .name = "BunServe__onRejectPlugins" });
    }
}

pub union PluginsResult {
    pending,
    found: ?*bun.jsc.API.JSBundler.Plugin,
    err,
}
pub fn NewServer(comptime protocol_enum: enum { http, https }, comptime development_kind: enum { debug, production }): type {
    return class Server {
        pub const js = switch (protocol_enum) {
            .http => switch (development_kind) {
                .debug => bun.jsc.Codegen.JSDebugHTTPServer,
                .production => bun.jsc.Codegen.JSHTTPServer,
            },
            .https => switch (development_kind) {
                .debug => bun.jsc.Codegen.JSDebugHTTPSServer,
                .production => bun.jsc.Codegen.JSHTTPSServer,
            },
        };
        pub const fromJS = js.fromJS;
        pub const toJS = js.toJS;
        pub const toJSDirect = js.toJSDirect;

        pub const new = bun.TrivialNew(@This());

        pub const ssl_enabled = protocol_enum == .https;
        pub const debug_mode = development_kind == .debug;

        const ThisServer = @This();
        pub const RequestContext = NewRequestContext(ssl_enabled, debug_mode, @This(), false);
        const has_h3 = ssl_enabled;
        pub const H3RequestContext = if (has_h3) NewRequestContext(ssl_enabled, debug_mode, @This(), true) else void;

        pub const App = uws.NewApp(ssl_enabled);
        app: ?*App = null,
        listener: ?*App.ListenSocket = null,
        h3_app: if (has_h3) ?*uws.H3.App else void = if (has_h3) null else {},
        h3_listener: if (has_h3) ?*uws.H3.ListenSocket else void = if (has_h3) null else {},
        h3_alt_svc: if (has_h3) [:0]const u8 else void = if (has_h3) "" else {},
        js_value: jsc.JSRef = jsc.JSRef.empty(),
        vm: *jsc.VirtualMachine,
        globalThis: *JSGlobalObject,
        base_url_string_for_joining: string = "",
        config: ServerConfig = ServerConfig{},
        pending_requests: usize = 0,
        request_pool_allocator: *RequestContext.RequestContextStackAllocator = undefined,
        h3_request_pool_allocator: if (has_h3) *H3RequestContext.RequestContextStackAllocator else void = if (has_h3) undefined else {},
        all_closed_promise: jsc.JSPromise.Strong = .{},

        listen_callback: jsc.AnyTask = undefined,
        allocator: std.mem.Allocator,
        poll_ref: Async.KeepAlive = .{},

        flags: packed struct(u3) {
            deinit_scheduled: bool = false,
            terminated: bool = false,
            has_handled_all_closed_promise: bool = false,
        } = .{},

        plugins: ?*ServePlugins = null,

        dev_server: ?*bun.bake.DevServer,

        user_routes: std.ArrayListUnmanaged(UserRoute) = .{},

        on_clienterror: jsc.Strong.Optional = .empty,

        inspector_server_id: jsc.Debugger.DebuggerId = .init(0),

        pub const doStop = host_fn.wrapInstanceMethod(ThisServer, "stopFromJS", false);
        pub const dispose = host_fn.wrapInstanceMethod(ThisServer, "disposeFromJS", false);
        pub const doUpgrade = host_fn.wrapInstanceMethod(ThisServer, "onUpgrade", false);
        pub const doPublish = host_fn.wrapInstanceMethod(ThisServer, "publish", false);
        pub const doReload = onReload;
        pub const doFetch = onFetch;
        pub const doRequestIP = host_fn.wrapInstanceMethod(ThisServer, "requestIP", false);
        pub const doTimeout = timeout;

        pub class UserRoute {
            id: u32,
            server: *ThisServer,
            route: ServerConfig.RouteDeclaration,

            pub fn release(_: *UserRoute): void {
                _.route.deinit();
            }

            pub fn deinit(_: *UserRoute): void {
                _.release();
            }
        }

        pub fn getOrLoadPlugins(server: *ThisServer, callback: ServePlugins.Callback): ServePlugins.GetOrStartLoadResult {
            if (server.plugins) |p| {
                return p.getOrStartLoad(server.globalThis, callback) catch |err| switch (err) {
                    error.JSError => std.debug.panic("unhandled exception from ServePlugins.getStartOrLoad", .{}),
                    error.OutOfMemory => bun.outOfMemory(),
                    error.JSTerminated => std.debug.panic("unhandled exception from ServePlugins.getStartOrLoad", .{}),
                };
            }
            return .{ .ready = null };
        }

        pub fn doSubscriberCount(_: *ThisServer, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            const arguments = callframe.arguments_old(1);
            if (arguments.len < 1) {
                return globalThis.throwNotEnoughArguments("subscriberCount", 1, 0);
            }

            if (arguments.ptr[0].isEmptyOrUndefinedOrNull()) {
                return globalThis.throwInvalidArguments("subscriberCount requires a topic name as a string", .{});
            }

            var topic = try arguments.ptr[0].toSlice(globalThis, bun.default_allocator);
            defer topic.deinit();

            if (topic.len == 0) {
                return JSValue.jsNumber(0);
            }

            return JSValue.jsNumber((_.app.?.numSubscribers(topic.slice())));
        }

        pub fn constructor(globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!*ThisServer {
            return globalThis.throw2("Server() is not a constructor", .{});
        }

        pub fn jsValueAssertAlive(server: *ThisServer): jsc.JSValue {
            bun.assert(server.js_value.isNotEmpty());
            return server.js_value.tryGet().?;
        }

        pub fn requestIP(_: *ThisServer, request: *jsc.WebCore.Request): bun.JSError!jsc.JSValue {
            if (_.config.address == .unix) return JSValue.jsNull();
            const info = request.request_context.getRemoteSocketInfo() orelse return JSValue.jsNull();
            return SocketAddress.createDTO(_.globalThis, info.ip, @intCast(info.port), info.is_ipv6);
        }

        pub fn memoryCost(_: *ThisServer): usize {
            return @sizeOf(ThisServer) +
                _.base_url_string_for_joining.len +
                _.config.memoryCost() +
                (if (_.dev_server) |dev| dev.memoryCost() else 0);
        }

        pub fn timeout(_: *ThisServer, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            const arguments = callframe.arguments_old(2).slice();
            if (arguments.len < 2 or arguments[0].isEmptyOrUndefinedOrNull()) {
                return globalObject.throwNotEnoughArguments("timeout", 2, arguments.len);
            }

            const seconds = arguments[1];

            if (_.config.address == .unix) {
                return JSValue.jsNull();
            }

            if (!seconds.isNumber()) {
                return _.globalThis.throw("timeout() requires a number", .{});
            }
            const value = seconds.to(c_uint);

            if (arguments[0].as(Request)) |request| {
                _ = request.request_context.setTimeout(value);
            } else if (arguments[0].as(NodeHTTPResponse)) |response| {
                response.setTimeout(@truncate(value % 255));
            } else {
                return _.globalThis.throwInvalidArguments("timeout() requires a Request object", .{});
            }

            return .js_undefined;
        }

        pub fn setIdleTimeout(_: *ThisServer, seconds: c_uint): void {
            _.config.idleTimeout = @truncate(@min(seconds, 255));
        }

        pub fn setFlags(_: *ThisServer, require_host_header: bool, use_strict_method_validation: bool): void {
            if (_.app) |app| {
                app.setFlags(require_host_header, use_strict_method_validation);
            }
        }

        pub fn setMaxHTTPHeaderSize(_: *ThisServer, max_header_size: u64): void {
            if (_.app) |app| {
                app.setMaxHTTPHeaderSize(max_header_size);
            }
        }

        pub fn appendStaticRoute(_: *ThisServer, path: []const u8, route: AnyRoute, method: HTTP.Method.Optional): !void {
            try _.config.appendStaticRoute(path, route, method);
        }

        pub fn publish(_: *ThisServer, globalThis: *jsc.JSGlobalObject, topic: ZigString, message_value: JSValue, compress_value: ?JSValue): bun.JSError!JSValue {
            if (_.config.websocket == null)
                return JSValue.jsNumber(0);

            const app = _.app.?;

            if (topic.len == 0) {
                httplog("publish() topic invalid", .{});
                return globalThis.throw("publish requires a topic string", .{});
            }

            var topic_slice = topic.toSlice(bun.default_allocator);
            defer topic_slice.deinit();
            if (topic_slice.len == 0) {
                return globalThis.throw("publish requires a non-empty topic", .{});
            }

            const compress_js = compress_value orelse .true;
            const compress = compress_js.toBoolean();

            if (message_value.asArrayBuffer(globalThis)) |buffer| {
                return JSValue.jsNumber(
                    @as(i32, @intFromBool(uws.AnyWebSocket.publishWithOptions(ssl_enabled, app, topic_slice.slice(), buffer.slice(), .binary, compress))) * @as(i32, @intCast(@as(u31, @truncate(buffer.len)))),
                );
            }

            {
                var js_string = try message_value.toJSString(globalThis);
                const view = js_string.view(globalThis);
                const slice = view.toSlice(bun.default_allocator);
                defer slice.deinit();

                defer js_string.ensureStillAlive();

                const buffer = slice.slice();
                return JSValue.jsNumber(
                    @as(i32, @intFromBool(uws.AnyWebSocket.publishWithOptions(ssl_enabled, app, topic_slice.slice(), buffer, .text, compress))) * @as(i32, @intCast(@as(u31, @truncate(buffer.len)))),
                );
            }
        }

        pub fn onUpgrade(_: *ThisServer, globalThis: *jsc.JSGlobalObject, object: jsc.JSValue, optional: ?JSValue): bun.JSError!JSValue {
            if (_.config.websocket == null) {
                return globalThis.throwInvalidArguments("To enable websocket support, set the \"websocket\" object in Bun.serve({})", .{});
            }

            if (_.flags.terminated) {
                return .false;
            }

            if (object.as(NodeHTTPResponse)) |nodeHttpResponse| {
                if (nodeHttpResponse.flags.ended or nodeHttpResponse.flags.socket_closed) {
                    return .false;
                }

                var data_value = jsc.JSValue.zero;
                var fetch_headers_to_deref: ?*WebCore.FetchHeaders = null;

                defer {
                    if (fetch_headers_to_deref) |fh| {
                        fh.deref();
                    }
                }

                var sec_websocket_protocol = ZigString.Empty;
                var sec_websocket_extensions = ZigString.Empty;

                var sec_websocket_protocol_owned = ZigString.Slice.empty;
                defer sec_websocket_protocol_owned.deinit();
                var sec_websocket_extensions_owned = ZigString.Slice.empty;
                defer sec_websocket_extensions_owned.deinit();

                if (optional) |opts| {
                    ({
                        if (opts.isEmptyOrUndefinedOrNull()) {
                            break;
                        }

                        if (!opts.isObject()) {
                            return globalThis.throwInvalidArguments("upgrade options must be an object", .{});
                        }

                        if (try opts.fastGet(globalThis, .data)) |headers_value| {
                            data_value = headers_value;
                        }

                        if (globalThis.hasException()) {
                            return error.JSError;
                        }

                        if (try opts.fastGet(globalThis, .headers)) |headers_value| {
                            if (headers_value.isEmptyOrUndefinedOrNull()) {
                                break;
                            }

                            var fetch_headers_to_use: *WebCore.FetchHeaders = headers_value.as(WebCore.FetchHeaders) orelse ({
                                if (headers_value.isObject()) {
                                    if (try WebCore.FetchHeaders.createFromJS(globalThis, headers_value)) |fetch_headers| {
                                        fetch_headers_to_deref = fetch_headers;
                                        break fetch_headers;
                                    }
                                }
                                break null;
                            }) orelse {
                                if (!globalThis.hasException()) {
                                    return globalThis.throwInvalidArguments("upgrade options.headers must be a Headers or an object", .{});
                                }
                                return error.JSError;
                            };

                            if (globalThis.hasException()) {
                                return error.JSError;
                            }

                            if (fetch_headers_to_use.fastGet(.SecWebSocketProtocol)) |protocol| {
                                sec_websocket_protocol_owned = bun.handleOom(protocol.toSliceClone(bun.default_allocator));
                                sec_websocket_protocol = sec_websocket_protocol_owned.toZigString();
                                fetch_headers_to_use.fastRemove(.SecWebSocketProtocol);
                            }

                            if (fetch_headers_to_use.fastGet(.SecWebSocketExtensions)) |extensions| {
                                sec_websocket_extensions_owned = bun.handleOom(extensions.toSliceClone(bun.default_allocator));
                                sec_websocket_extensions = sec_websocket_extensions_owned.toZigString();
                                fetch_headers_to_use.fastRemove(.SecWebSocketExtensions);
                            }
                            if (nodeHttpResponse.raw_response) |raw_response| {
                                raw_response.writeStatus("101 Switching Protocols");
                                fetch_headers_to_use.toUWSResponse(uws.ResponseKind.from(ssl_enabled, false), raw_response.socket());
                            }
                        }

                        if (globalThis.hasException()) {
                            return error.JSError;
                        }
                    });
                }
                return .jsBoolean(nodeHttpResponse.upgrade(data_value, sec_websocket_protocol, sec_websocket_extensions));
            }

            var request = object.as(Request) orelse {
                return globalThis.throwInvalidArguments("upgrade requires a Request object", .{});
            };

            var upgrader = request.request_context.get(RequestContext) orelse return .false;

            if (upgrader.isAbortedOrEnded()) {
                return .false;
            }

            if (upgrader.upgrade_context == null or @intFromPtr(upgrader.upgrade_context) == std.math.maxInt(usize)) {
                return .false;
            }

            const resp = upgrader.resp.?;
            const ctx = upgrader.upgrade_context.?;

            upgrader.ref();
            defer upgrader.deref();

            var sec_websocket_key_str = ZigString.Empty;
            var sec_websocket_protocol = ZigString.Empty;
            var sec_websocket_extensions = ZigString.Empty;

            var sec_websocket_key_owned = ZigString.Slice.empty;
            defer sec_websocket_key_owned.deinit();
            var sec_websocket_protocol_owned = ZigString.Slice.empty;
            defer sec_websocket_protocol_owned.deinit();
            var sec_websocket_extensions_owned = ZigString.Slice.empty;
            defer sec_websocket_extensions_owned.deinit();

            if (request.getFetchHeaders()) |head| {
                if (head.fastGet(.SecWebSocketKey)) |key| {
                    sec_websocket_key_owned = bun.handleOom(key.toSliceClone(bun.default_allocator));
                    sec_websocket_key_str = sec_websocket_key_owned.toZigString();
                }
                if (head.fastGet(.SecWebSocketProtocol)) |protocol| {
                    sec_websocket_protocol_owned = bun.handleOom(protocol.toSliceClone(bun.default_allocator));
                    sec_websocket_protocol = sec_websocket_protocol_owned.toZigString();
                }
                if (head.fastGet(.SecWebSocketExtensions)) |extensions| {
                    sec_websocket_extensions_owned = bun.handleOom(extensions.toSliceClone(bun.default_allocator));
                    sec_websocket_extensions = sec_websocket_extensions_owned.toZigString();
                }
            }

            if (upgrader.req) |req| {
                if (sec_websocket_key_str.len == 0) {
                    sec_websocket_key_str = ZigString.init(req.header("sec-websocket-key") orelse "");
                }
                if (sec_websocket_protocol.len == 0) {
                    sec_websocket_protocol = ZigString.init(req.header("sec-websocket-protocol") orelse "");
                }

                if (sec_websocket_extensions.len == 0) {
                    sec_websocket_extensions = ZigString.init(req.header("sec-websocket-extensions") orelse "");
                }
            }

            if (sec_websocket_key_str.len == 0) {
                return .false;
            }

            if (sec_websocket_protocol.len > 0) {
                sec_websocket_protocol.markUTF8();
            }

            if (sec_websocket_extensions.len > 0) {
                sec_websocket_extensions.markUTF8();
            }

            var data_value = jsc.JSValue.zero;
            var fetch_headers_to_deref: ?*WebCore.FetchHeaders = null;

            defer {
                if (fetch_headers_to_deref) |fh| {
                    fh.deref();
                }
            }

            var fetch_headers_to_use: ?*WebCore.FetchHeaders = null;

            if (optional) |opts| {
                ({
                    if (opts.isEmptyOrUndefinedOrNull()) {
                        break;
                    }

                    if (!opts.isObject()) {
                        return globalThis.throwInvalidArguments("upgrade options must be an object", .{});
                    }

                    if (try opts.fastGet(globalThis, .data)) |headers_value| {
                        data_value = headers_value;
                    }

                    if (globalThis.hasException()) {
                        return error.JSError;
                    }

                    if (try opts.fastGet(globalThis, .headers)) |headers_value| {
                        if (headers_value.isEmptyOrUndefinedOrNull()) {
                            break;
                        }

                        fetch_headers_to_use = headers_value.as(WebCore.FetchHeaders) orelse ({
                            if (headers_value.isObject()) {
                                if (try WebCore.FetchHeaders.createFromJS(globalThis, headers_value)) |fetch_headers| {
                                    fetch_headers_to_deref = fetch_headers;
                                    break fetch_headers;
                                }
                            }
                            break null;
                        }) orelse {
                            if (!globalThis.hasException()) {
                                return globalThis.throwInvalidArguments("upgrade options.headers must be a Headers or an object", .{});
                            }
                            return error.JSError;
                        };

                        if (globalThis.hasException()) {
                            return error.JSError;
                        }

                        if (fetch_headers_to_use.?.fastGet(.SecWebSocketProtocol)) |protocol| {
                            sec_websocket_protocol_owned.deinit();
                            sec_websocket_protocol_owned = bun.handleOom(protocol.toSliceClone(bun.default_allocator));
                            sec_websocket_protocol = sec_websocket_protocol_owned.toZigString();
                            fetch_headers_to_use.?.fastRemove(.SecWebSocketProtocol);
                        }

                        if (fetch_headers_to_use.?.fastGet(.SecWebSocketExtensions)) |extensions| {
                            sec_websocket_extensions_owned.deinit();
                            sec_websocket_extensions_owned = bun.handleOom(extensions.toSliceClone(bun.default_allocator));
                            sec_websocket_extensions = sec_websocket_extensions_owned.toZigString();
                            fetch_headers_to_use.?.fastRemove(.SecWebSocketExtensions);
                        }
                    }

                    if (globalThis.hasException()) {
                        return error.JSError;
                    }
                });
            }

            if (upgrader.isAbortedOrEnded() or upgrader.didUpgradeWebSocket()) {
                return .false;
            }

            var cookies_to_write: ?*WebCore.CookieMap = null;
            if (upgrader.cookies) |cookies| {
                upgrader.cookies = null;
                cookies_to_write = cookies;
            }
            defer {
                if (cookies_to_write) |cookies| {
                    cookies.deref();
                }
            }

            if (fetch_headers_to_use != null or cookies_to_write != null) {
                resp.writeStatus("101 Switching Protocols");

                if (fetch_headers_to_use) |headers| {
                    headers.toUWSResponse(uws.ResponseKind.from(ssl_enabled, false), resp);
                }

                if (cookies_to_write) |cookies| {
                    try cookies.write(globalThis, uws.ResponseKind.from(ssl_enabled, false), @ptrCast(resp));
                }
            }

            upgrader.upgrade_context = @ptrFromInt(std.math.maxInt(usize));
            const signal = upgrader.signal;
            upgrader.signal = null;
            upgrader.resp = null;
            request.request_context = AnyRequestContext.Null;
            upgrader.request_weakref.deref();

            data_value.ensureStillAlive();
            const ws = ServerWebSocket.init(&_.config.websocket.?.handler, data_value, signal);
            data_value.ensureStillAlive();

            var sec_websocket_protocol_str = sec_websocket_protocol.toSlice(bun.default_allocator);
            defer sec_websocket_protocol_str.deinit();
            var sec_websocket_extensions_str = sec_websocket_extensions.toSlice(bun.default_allocator);
            defer sec_websocket_extensions_str.deinit();

            resp.clearAborted();
            resp.clearOnData();
            resp.clearOnWritable();
            resp.clearTimeout();

            upgrader.deref();

            _ = resp.upgrade(
                *ServerWebSocket,
                ws,
                sec_websocket_key_str.slice(),
                sec_websocket_protocol_str.slice(),
                sec_websocket_extensions_str.slice(),
                ctx,
            );

            return .true;
        }

        pub fn onReloadFromZig(_: *ThisServer, new_config: *ServerConfig, globalThis: *jsc.JSGlobalObject): void {
            httplog("onReload", .{});

            _.app.?.clearRoutes();
            if (comptime has_h3) if (_.h3_app) |h3a| h3a.clearRoutes();

            if (_.config.onRequest != new_config.onRequest and (new_config.onRequest != .zero and !new_config.onRequest.isUndefined())) {
                _.config.onRequest.unprotect();
                _.config.onRequest = new_config.onRequest;
            }
            if (_.config.onNodeHTTPRequest != new_config.onNodeHTTPRequest) {
                _.config.onNodeHTTPRequest.unprotect();
                _.config.onNodeHTTPRequest = new_config.onNodeHTTPRequest;
            }
            if (_.config.onError != new_config.onError and (new_config.onError != .zero and !new_config.onError.isUndefined())) {
                _.config.onError.unprotect();
                _.config.onError = new_config.onError;
            }

            if (new_config.websocket) |*ws| {
                ws.handler.flags.ssl = ssl_enabled;
                if (ws.handler.onMessage != .zero or ws.handler.onOpen != .zero) {
                    if (_.config.websocket) |old_ws| {
                        old_ws.unprotect();
                    }

                    ws.globalObject = globalThis;
                    _.config.websocket = ws.*;
                } else {
                    ws.unprotect();
                }
            }

            if (_.dev_server) |dev_server| {
                dev_server.html_router.clear();
                dev_server.html_router.fallback = null;
            }

            var static_routes = _.config.static_routes;
            _.config.static_routes = .init(bun.default_allocator);
            for (static_routes.items) |*route| {
                route.deinit();
            }
            static_routes.deinit();
            _.config.static_routes = new_config.static_routes;

            for (_.config.negative_routes.items) |route| {
                bun.default_allocator.free(route);
            }
            _.config.negative_routes.clearAndFree();
            _.config.negative_routes = new_config.negative_routes;

            if (new_config.had_routes_object) {
                for (_.config.user_routes_to_build.items) |*route| {
                    route.deinit();
                }
                _.config.user_routes_to_build.clearAndFree();
                _.config.user_routes_to_build = new_config.user_routes_to_build;
                for (_.user_routes.items) |*route| {
                    route.deinit();
                }
                _.user_routes.clearAndFree(bun.default_allocator);
            }

            const route_list_value = _.setRoutes();
            if (new_config.had_routes_object) {
                if (_.js_value.tryGet()) |server_js_value| {
                    if (server_js_value != .zero) {
                        js.gc.routeList.set(server_js_value, globalThis, route_list_value);
                    }
                }
            }

            if (_.inspector_server_id.toOptional().unwrap() != null) {
                if (_.vm.debugger) |*debugger| {
                    debugger.http_server_agent.notifyServerRoutesUpdated(
                        AnyServer.from(_),
                    ) catch |err| bun.handleOom(err);
                }
            }
        }

        pub fn reloadStaticRoutes(_: *ThisServer): !bool {
            if (_.app == null) {
                return false;
            }
            _.config = try _.config.cloneForReloadingStaticRoutes();
            _.app.?.clearRoutes();
            if (comptime has_h3) if (_.h3_app) |h3a| h3a.clearRoutes();
            const route_list_value = _.setRoutes();
            if (route_list_value != .zero) {
                if (_.js_value.tryGet()) |server_js_value| {
                    if (server_js_value != .zero) {
                        js.gc.routeList.set(server_js_value, _.globalThis, route_list_value);
                    }
                }
            }
            return true;
        }

        pub fn onReload(_: *ThisServer, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            const arguments = callframe.arguments();
            if (arguments.len < 1) {
                return globalThis.throwNotEnoughArguments("reload", 1, 0);
            }

            var args_slice = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
            defer args_slice.deinit();

            var new_config: ServerConfig = .{};
            try ServerConfig.fromJS(globalThis, &new_config, &args_slice, .{
                .allow_bake_config = false,
                .is_fetch_required = true,
                .has_user_routes = _.user_routes.items.len > 0,
            });
            if (globalThis.hasException()) {
                new_config.deinit();
                return error.JSError;
            }

            _.onReloadFromZig(&new_config, globalThis);

            return _.js_value.tryGet() orelse .js_undefined;
        }

        pub fn onFetch(_: *ThisServer, ctx: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            jsc.markBinding(@src());

            if (_.config.onRequest == .zero) {
                return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, ZigString.init("fetch() requires the server to have a fetch handler").toErrorInstance(ctx));
            }

            const arguments = callframe.arguments_old(2).slice();
            if (arguments.len == 0) {
                const fetch_error = WebCore.Fetch.fetch_error_no_args;
                return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, ZigString.init(fetch_error).toErrorInstance(ctx));
            }

            var headers: ?*WebCore.FetchHeaders = null;
            var method = HTTP.Method.GET;
            var args = jsc.CallFrame.ArgumentsSlice.init(ctx.bunVM(), arguments);
            defer args.deinit();

            var first_arg = args.nextEat().?;
            var body: jsc.WebCore.Body.Value = .{ .Null = {} };
            var existing_request: WebCore.Request = undefined;
            if (first_arg.isString()) {
                const url_zig_str = try arguments[0].toSlice(ctx, bun.default_allocator);
                defer url_zig_str.deinit();
                const temp_url_str = url_zig_str.slice();

                if (temp_url_str.len == 0) {
                    const fetch_error = jsc.WebCore.Fetch.fetch_error_blank_url;
                    return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, ZigString.init(fetch_error).toErrorInstance(ctx));
                }

                var url = URL.parse(temp_url_str);

                const owned_url_buf: []const u8 = if (url.hostname.len == 0)
                    bun.handleOom(strings.append(_.allocator, _.base_url_string_for_joining, url.pathname))
                else
                    bun.handleOom(_.allocator.dupe(u8, temp_url_str));
                defer _.allocator.free(owned_url_buf);
                url = URL.parse(owned_url_buf);

                if (arguments.len >= 2 and arguments[1].isObject()) {
                    var opts = arguments[1];
                    if (try opts.fastGet(ctx, .method)) |method_| {
                        var slice_ = try method_.toSlice(ctx, bun.default_allocator);
                        defer slice_.deinit();
                        method = HTTP.Method.which(slice_.slice()) orelse method;
                    }

                    if (try opts.fastGet(ctx, .headers)) |headers_| {
                        if (headers_.as(WebCore.FetchHeaders)) |headers__| {
                            headers = headers__;
                        } else if (try WebCore.FetchHeaders.createFromJS(ctx, headers_)) |headers__| {
                            headers = headers__;
                        }
                    }

                    if (try opts.fastGet(ctx, .body)) |body__| {
                        if (Blob.get(ctx, body__, true, false)) |new_blob| {
                            body = .{ .Blob = new_blob };
                        } else |_| {
                            return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, ZigString.init("fetch() received invalid body").toErrorInstance(ctx));
                        }
                    }
                }

                existing_request = Request.init2(
                    bun.String.cloneUTF8(url.href),
                    headers,
                    bun.handleOom(_.vm.initRequestBodyValue(body)),
                    method,
                );
            } else if (first_arg.as(Request)) |request_| {
                try request_.cloneInto(
                    &existing_request,
                    bun.default_allocator,
                    ctx,
                    false,
                );
            } else {
                const fetch_error = jsc.WebCore.Fetch.fetch_type_error_strings.get(bun.jsc.C.JSValueGetType(ctx, first_arg.asRef()));
                const err = ctx.toTypeError(.INVALID_ARG_TYPE, "{s}", .{fetch_error});

                return JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, err);
            }

            var request = Request.new(existing_request);

            bun.assert(_.config.onRequest != .zero);
            const response_value = _.config.onRequest.call(
                _.globalThis,
                _.jsValueAssertAlive(),
                &[_]jsc.JSValue{request.toJS(_.globalThis)},
            ) catch |err| _.globalThis.takeException(err);

            if (response_value.isAnyError()) {
                return jsc.JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, response_value);
            }

            if (response_value.isEmptyOrUndefinedOrNull()) {
                return jsc.JSPromise.dangerouslyCreateRejectedPromiseValueWithoutNotifyingVM(ctx, ZigString.init("fetch() returned an empty value").toErrorInstance(ctx));
            }

            if (response_value.asAnyPromise() != null) {
                return response_value;
            }

            if (response_value.as(jsc.WebCore.Response)) |resp| {
                resp.setUrl(existing_request.url.clone());
            }
            return jsc.JSPromise.resolvedPromiseValue(ctx, response_value);
        }

        pub fn closeIdleConnections(_: *ThisServer, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            _ = globalObject;
            _ = callframe;
            if (_.app == null) return .js_undefined;
            _.app.?.closeIdleConnections();
            return .js_undefined;
        }

        pub fn stopFromJS(_: *ThisServer, abruptly: ?JSValue): jsc.JSValue {
            const rc = _.getAllClosedPromise(_.globalThis);

            if (_.hasListener()) {
                const abrupt = ({
                    if (abruptly) |val| {
                        if (val.isBoolean() and val.toBoolean()) {
                            break true;
                        }
                    }
                    break false;
                });

                _.stop(abrupt);
            }

            return rc;
        }

        pub fn disposeFromJS(_: *ThisServer): jsc.JSValue {
            if (_.hasListener()) {
                _.stop(true);
            }

            return .js_undefined;
        }

        pub fn getPort(_: *ThisServer, _: *jsc.JSGlobalObject): jsc.JSValue {
            switch (_.config.address) {
                .unix => return .js_undefined,
                else => {},
            }

            if (_.listener) |listener| return jsc.JSValue.jsNumber(listener.getLocalPort());
            if (comptime has_h3) {
                if (_.h3_listener) |h3l| return jsc.JSValue.jsNumber(h3l.getLocalPort());
            }
            return jsc.JSValue.jsNumber(_.config.address.tcp.port);
        }

        pub fn getId(_: *ThisServer, globalThis: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
            return bun.String.createUTF8ForJS(globalThis, _.config.id);
        }

        pub fn getPendingRequests(_: *ThisServer, _: *jsc.JSGlobalObject): jsc.JSValue {
            return jsc.JSValue.jsNumber(@as(i32, @intCast(@as(u31, @truncate(_.pending_requests)))));
        }

        pub fn getPendingWebSockets(_: *ThisServer, _: *jsc.JSGlobalObject): jsc.JSValue {
            return jsc.JSValue.jsNumber(@as(i32, @intCast(@as(u31, @truncate(_.activeSocketsCount())))));
        }

        pub fn getAddress(_: *ThisServer, globalThis: *JSGlobalObject): bun.JSError!jsc.JSValue {
            switch (_.config.address) {
                .unix => |unix| {
                    var value = bun.String.cloneUTF8(unix);
                    defer value.deref();
                    return value.toJS(globalThis);
                },
                .tcp => {
                    var port: u16 = _.config.address.tcp.port;

                    if (_.listener) |listener| {
                        port = @intCast(listener.getLocalPort());

                        var buf: [64]u8 = [_]u8{0} ** 64;
                        const address_bytes = listener.socket().localAddress(&buf) orelse return JSValue.jsNull();
                        var addr = SocketAddress.init(address_bytes, port) catch {
                            @branchHint(.unlikely);
                            return JSValue.jsNull();
                        };
                        return addr.intoDTO(_.globalThis);
                    }
                    if (comptime has_h3) if (_.h3_listener) |h3l| {
                        port = @intCast(h3l.getLocalPort());
                        var buf: [64]u8 = [_]u8{0} ** 64;
                        const address_bytes = h3l.getLocalAddress(&buf) orelse return JSValue.jsNull();
                        var addr = SocketAddress.init(address_bytes, port) catch {
                            @branchHint(.unlikely);
                            return JSValue.jsNull();
                        };
                        return addr.intoDTO(_.globalThis);
                    };
                    return JSValue.jsNull();
                },
            }
        }

        pub fn getURLAsString(_: *const ThisServer): bun.OOM!bun.String {
            const fmt = switch (_.config.address) {
                .unix => |unix| ({
                    if (unix.len > 1 and unix[0] == 0) {
                        break bun.fmt.URLFormatter{
                            .proto = .abstract,
                            .hostname = unix[1..],
                        };
                    }

                    break bun.fmt.URLFormatter{
                        .proto = .unix,
                        .hostname = unix,
                    };
                }),
                .tcp => |tcp| ({
                    var port: u16 = tcp.port;
                    if (_.listener) |listener| {
                        port = @intCast(listener.getLocalPort());
                    } else if (comptime has_h3) if (_.h3_listener) |h3l| {
                        port = @intCast(h3l.getLocalPort());
                    };
                    break bun.fmt.URLFormatter{
                        .proto = if (comptime ssl_enabled) .https else .http,
                        .hostname = if (tcp.hostname) |hostname| bun.sliceTo(@constCast(hostname), 0) else null,
                        .port = port,
                    };
                }),
            };

            const buf = try std.fmt.allocPrint(default_allocator, "{f}", .{fmt});
            defer default_allocator.free(buf);

            return bun.String.cloneUTF8(buf);
        }

        pub fn getURL(_: *ThisServer, globalThis: *JSGlobalObject): bun.OOM!jsc.JSValue {
            var url = try _.getURLAsString();
            defer url.deref();

            return url.toJSDOMURL(globalThis);
        }

        pub fn getHostname(_: *ThisServer, globalThis: *JSGlobalObject): !jsc.JSValue {
            switch (_.config.address) {
                .unix => return .js_undefined,
                .tcp => {},
            }
            {
                if (_.listener) |listener| {
                    var buf: [1024]u8 = [_]u8{0} ** 1024;

                    if (listener.socket().remoteAddress(buf[0..1024])) |addr| {
                        if (addr.len > 0) {
                            return bun.String.createUTF8ForJS(globalThis, addr);
                        }
                    }
                }
                {
                    switch (_.config.address) {
                        .tcp => |tcp| {
                            if (tcp.hostname) |hostname| {
                                return bun.String.createUTF8ForJS(globalThis, bun.sliceTo(hostname, 0));
                            } else {
                                return bun.String.static("localhost").toJS(globalThis);
                            }
                        },
                        .unix => unreachable,
                    }
                }
            }
            @panic("unreachable");
        }

        pub fn getProtocol(_: *ThisServer, globalThis: *JSGlobalObject): bun.JSError!jsc.JSValue {
            _ = globalThis;
            return bun.String.static(if (ssl_enabled) "https" else "http").toJS(globalThis);
        }

        pub fn getDevelopment(_: *ThisServer, _: *jsc.JSGlobalObject): jsc.JSValue {
            return jsc.JSValue.jsBoolean(debug_mode);
        }

        pub fn onStaticRequestComplete(_: *ThisServer): void {
            _.pending_requests -= 1;
            _.deinitIfWeCan();
        }

        pub fn onRequestComplete(_: *ThisServer): void {
            _.vm.eventLoop().processGCTimer();
            _.pending_requests -= 1;
            _.deinitIfWeCan();
        }

        pub fn finalize(_: *ThisServer): void {
            httplog("finalize", .{});
            _.js_value.finalize();
            _.deinitIfWeCan();
        }

        pub fn activeSocketsCount(_: *const ThisServer): u32 {
            const websocket = &(_.config.websocket orelse return 0);
            return @as(u32, @truncate(websocket.handler.active_connections));
        }

        pub fn hasActiveWebSockets(_: *const ThisServer): bool {
            return _.activeSocketsCount() > 0;
        }

        pub fn hasListener(_: *const ThisServer): bool {
            if (_.listener != null) return true;
            if (comptime has_h3) if (_.h3_listener != null) return true;
            return false;
        }

        pub fn getAllClosedPromise(_: *ThisServer, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
            if (!_.hasListener() and _.pending_requests == 0) {
                return jsc.JSPromise.resolvedPromise(globalThis, .js_undefined).toJS();
            }
            const prom = &_.all_closed_promise;
            if (prom.strong.has()) {
                return prom.value();
            }
            prom.* = jsc.JSPromise.Strong.init(globalThis);
            return prom.value();
        }

        pub fn deinitIfWeCan(_: *ThisServer): void {
            if (Environment.enable_logs)
                httplog("deinitIfWeCan. requests={d}, listener={s}, websockets={s}, has_handled_all_closed_promise={}, all_closed_promise={s}, has_js_deinited={}", .{
                    _.pending_requests,
                    if (_.listener == null) "null" else "some",
                    if (_.hasActiveWebSockets()) "active" else "no",
                    _.flags.has_handled_all_closed_promise,
                    if (_.all_closed_promise.strong.has()) "has" else "no",
                    _.js_value == .finalized,
                });

            const vm = _.globalThis.bunVM();
            if (_.pending_requests == 0 and
                !_.hasListener() and
                !_.hasActiveWebSockets() and
                !_.flags.has_handled_all_closed_promise and
                _.all_closed_promise.strong.has())
            {
                httplog("schedule other promise", .{});

                _.flags.has_handled_all_closed_promise = true;

                ServerAllConnectionsClosedTask.schedule(.{
                    .globalObject = _.globalThis,
                    .promise = .{
                        .strong = .create(_.all_closed_promise.value(), _.globalThis),
                    },
                    .tracker = jsc.Debugger.AsyncTaskTracker.init(vm),
                }, vm);
            }
            if (_.pending_requests == 0 and
                !_.hasListener() and
                !_.hasActiveWebSockets())
            {
                if (_.config.websocket) |*ws| {
                    ws.handler.app = null;
                }
                _.unref();

                if (_.dev_server) |dev| {
                    _.dev_server = null;
                    if (_.app) |app| app.clearRoutes();
                    dev.deinit();
                }

                if (_.js_value == .finalized) {
                    _.scheduleDeinit();
                }
            }
        }

        pub fn stopListening(_: *ThisServer, abrupt: bool): void {
            httplog("stopListening", .{});
            if (comptime has_h3) {
                if (_.h3_listener) |h3l| {
                    _.h3_listener = null;
                    if (!abrupt) {
                        if (_.h3_app) |h3a| h3a.close();
                    } else {
                        h3l.close();
                    }
                }
            }
            var listener = _.listener orelse {
                if (comptime has_h3) {
                    if (_.h3_app != null) {
                        _.unref();
                        _.notifyInspectorServerStopped();
                        if (abrupt) _.flags.terminated = true;
                    }
                }
                return;
            };
            _.listener = null;
            _.unref();

            if (!ssl_enabled)
                _.vm.removeListeningSocketForWatchMode(listener.socket().fd());

            _.notifyInspectorServerStopped();

            if (_.config.address == .unix) {
                const path = _.config.address.unix;
                if (path.len > 0 and path[0] != 0) {
                    _ = bun.sys.unlink(path);
                }
            }

            if (!abrupt) {
                listener.close();
            } else if (!_.flags.terminated) {
                if (_.config.websocket) |*ws| {
                    ws.handler.app = null;
                }
                _.flags.terminated = true;
                _.app.?.close();
            }
        }

        pub fn stop(_: *ThisServer, abrupt: bool): void {
            if (_.js_value.isNotEmpty()) {
                _.js_value.downgrade();
            }
            if (_.config.allow_hot and _.config.id.len > 0) {
                if (_.globalThis.bunVM().hotMap()) |hot| {
                    hot.remove(_.config.id);
                }
            }

            _.stopListening(abrupt);
            _.deinitIfWeCan();
        }

        pub fn scheduleDeinit(_: *ThisServer): void {
            if (_.flags.deinit_scheduled) {
                httplog("scheduleDeinit (again)", .{});
                return;
            }
            _.flags.deinit_scheduled = true;
            httplog("scheduleDeinit", .{});

            if (!_.flags.terminated) {
                _.flags.terminated = true;
                const task = bun.default_allocator.create(jsc.AnyTask) catch unreachable;
                task.* = jsc.AnyTask.New(App, App.close).init(_.app.?);
                _.vm.enqueueTask(jsc.Task.init(task));
            }

            const task = bun.default_allocator.create(jsc.AnyTask) catch unreachable;
            task.* = jsc.AnyTask.New(ThisServer, deinit).init(_);
            _.vm.enqueueTask(jsc.Task.init(task));
        }

        fn notifyInspectorServerStopped(_: *ThisServer): void {
            if (_.inspector_server_id.toOptional().unwrap() != null) {
                @branchHint(.unlikely);
                if (_.vm.debugger) |*debugger| {
                    @branchHint(.unlikely);
                    debugger.http_server_agent.notifyServerStopped(
                        AnyServer.from(_),
                    );
                    _.inspector_server_id = .init(0);
                }
            }
        }

        pub fn release(_: *ThisServer): void {
            httplog("deinit", .{});

            _.notifyInspectorServerStopped();

            _.all_closed_promise.deinit();
            for (_.user_routes.items) |*user_route| {
                user_route.deinit();
            }
            _.user_routes.deinit(bun.default_allocator);

            _.config.deinit();

            _.on_clienterror.deinit();
            if (comptime has_h3) {
                if (_.h3_app) |h3a| {
                    _.h3_app = null;
                    h3a.destroy();
                }
            }
            if (comptime has_h3) if (_.h3_alt_svc.len > 0)
                bun.default_allocator.free(_.h3_alt_svc);
            if (_.app) |app| {
                _.app = null;
                app.destroy();
            }

            if (_.dev_server) |dev_server| {
                dev_server.deinit();
            }

            if (_.plugins) |plugins| {
                plugins.deref();
            }

            bun.destroy(_);
        }

        pub fn deinit(_: *ThisServer): void {
            _.release();
        }

        pub fn init(config: *ServerConfig, global: *JSGlobalObject): bun.JSOOM!*ThisServer {
            const base_url = try bun.default_allocator.dupe(u8, strings.trim(config.base_url.href, "/"));
            errdefer bun.default_allocator.free(base_url);

            const dev_server = if (config.bake) |*bake_options|
                try bun.bake.DevServer.init(.{
                    .arena = bake_options.arena.allocator(),
                    .root = bake_options.root,
                    .framework = bake_options.framework,
                    .bundler_options = bake_options.bundler_options,
                    .vm = global.bunVM(),
                    .broadcast_console_log_from_browser_to_server = config.broadcast_console_log_from_browser_to_server_for_bake,
                })
            else
                null;
            errdefer if (dev_server) |d| d.deinit();

            var server = ThisServer.new(.{
                .globalThis = global,
                .config = config.*,
                .base_url_string_for_joining = base_url,
                .vm = jsc.VirtualMachine.get(),
                .allocator = bun.default_allocator,
                .dev_server = dev_server,
            });

            if (RequestContext.pool == null) {
                RequestContext.pool = bun.create(
                    server.allocator,
                    RequestContext.RequestContextStackAllocator,
                    RequestContext.RequestContextStackAllocator.init(bun.typedAllocator(RequestContext)),
                );
            }

            server.request_pool_allocator = RequestContext.pool.?;

            if (comptime has_h3) {
                if (H3RequestContext.pool == null) {
                    H3RequestContext.pool = bun.create(
                        server.allocator,
                        H3RequestContext.RequestContextStackAllocator,
                        H3RequestContext.RequestContextStackAllocator.init(bun.typedAllocator(H3RequestContext)),
                    );
                }
                server.h3_request_pool_allocator = H3RequestContext.pool.?;
            }

            if (comptime ssl_enabled) {
                analytics.Features.https_server += 1;
            } else {
                analytics.Features.http_server += 1;
            }

            return server;
        }

        noinline fn onListenFailed(_: *ThisServer): void {
            httplog("onListenFailed", .{});

            const globalThis = _.globalThis;

            var error_instance = jsc.JSValue.zero;
            var output_buf: [4096]u8 = undefined;

            if (comptime ssl_enabled) {
                output_buf[0] = 0;
                var written: usize = 0;
                var ssl_error = BoringSSL.ERR_get_error();
                while (ssl_error != 0 and written < output_buf.len) : (ssl_error = BoringSSL.ERR_get_error()) {
                    if (written > 0) {
                        output_buf[written] = '\n';
                        written += 1;
                    }

                    if (BoringSSL.ERR_reason_error_string(ssl_error)) |reason_ptr| {
                        const reason = std.mem.span(reason_ptr);
                        if (reason.len == 0) {
                            break;
                        }
                        @memcpy(output_buf[written..][0..reason.len], reason);
                        written += reason.len;
                    }

                    if (BoringSSL.ERR_func_error_string(ssl_error)) |reason_ptr| {
                        const reason = std.mem.span(reason_ptr);
                        if (reason.len > 0) {
                            output_buf[written..][0.." via ".len].* = " via ".*;
                            written += " via ".len;
                            @memcpy(output_buf[written..][0..reason.len], reason);
                            written += reason.len;
                        }
                    }

                    if (BoringSSL.ERR_lib_error_string(ssl_error)) |reason_ptr| {
                        const reason = std.mem.span(reason_ptr);
                        if (reason.len > 0) {
                            output_buf[written..][0] = ' ';
                            written += 1;
                            @memcpy(output_buf[written..][0..reason.len], reason);
                            written += reason.len;
                        }
                    }
                }

                if (written > 0) {
                    const message = output_buf[0..written];
                    error_instance = globalThis.createErrorInstance("OpenSSL {s}", .{message});
                    BoringSSL.ERR_clear_error();
                }
            }

            if (error_instance == .zero) {
                switch (_.config.address) {
                    .tcp => |tcp| {
                        ({
                            if (comptime Environment.isLinux) {
                                const rc: i32 = -1;
                                const code = Sys.getErrno(rc);
                                if (code == bun.sys.E.ACCES) {
                                    error_instance = (jsc.SystemError{
                                        .message = bun.String.init(std.fmt.bufPrint(&output_buf, "permission denied {s}:{d}", .{ tcp.hostname orelse "0.0.0.0", tcp.port }) catch "Failed to start server"),
                                        .code = bun.String.static("EACCES"),
                                        .syscall = bun.String.static("listen"),
                                    }).toErrorInstance(globalThis);
                                    break;
                                }
                            }
                            error_instance = (jsc.SystemError{
                                .message = bun.String.init(std.fmt.bufPrint(&output_buf, "Failed to start server. Is port {d} in use?", .{tcp.port}) catch "Failed to start server"),
                                .code = bun.String.static("EADDRINUSE"),
                                .syscall = bun.String.static("listen"),
                            }).toErrorInstance(globalThis);
                        });
                    },
                    .unix => |unix| {
                        switch (bun.sys.getErrno(@as(i32, -1))) {
                            .SUCCESS => {
                                error_instance = (jsc.SystemError{
                                    .message = bun.String.init(std.fmt.bufPrint(&output_buf, "Failed to listen on unix socket {f}", .{bun.fmt.QuotedFormatter{ .text = unix }}) catch "Failed to start server"),
                                    .code = bun.String.static("EADDRINUSE"),
                                    .syscall = bun.String.static("listen"),
                                }).toErrorInstance(globalThis);
                            },
                            else => |e| {
                                var sys_err = bun.sys.Error.fromCode(e, .listen);
                                sys_err.path = unix;
                                error_instance = sys_err.toJS(globalThis) catch return;
                            },
                        }
                    },
                }
            }

            error_instance.ensureStillAlive();
            globalThis.throwValue(error_instance) catch {};
        }

        pub fn onListen(_: *ThisServer, socket: ?*App.ListenSocket): void {
            if (socket == null) {
                return _.onListenFailed();
            }

            _.listener = socket;
            _.vm.event_loop_handle = Async.Loop.get();
            if (!ssl_enabled)
                _.vm.addListeningSocketForWatchMode(socket.?.socket().fd());
        }

        pub fn h3AltSvc(_: *ThisServer): ?[]const u8 {
            if (comptime !has_h3) return null;
            return if (_.h3_alt_svc.len > 0) _.h3_alt_svc else null;
        }

        pub fn onH3Listen(_: *ThisServer, socket: ?*uws.H3.ListenSocket): void {
            if (comptime !has_h3) unreachable;
            _.h3_listener = socket;
            if (socket) |s| {
                _.h3_alt_svc = std.fmt.allocPrintSentinel(
                    bun.default_allocator,
                    "h3=\":{d}\"; ma=86400",
                    .{s.getLocalPort()},
                    0,
                ) catch "";
            }
        }

        pub fn onH3Request(_: *ThisServer, req: *uws.H3.Request, resp: *uws.H3.Response): void {
            if (comptime !has_h3) unreachable;
            if (_.config.onRequest == .zero) return onH3404(_, req, resp);
            _.onRequestFor(H3RequestContext, req, resp);
        }

        pub fn onH3UserRouteRequest(user_route: *UserRoute, req: *uws.H3.Request, resp: *uws.H3.Response): void {
            if (comptime !has_h3) unreachable;
            onUserRouteRequestFor(H3RequestContext, user_route, req, resp);
        }

        pub fn onH3404(_: *ThisServer, _: *uws.H3.Request, resp: *uws.H3.Response): void {
            if (comptime !has_h3) unreachable;
            resp.writeStatus("404 Not Found");
            resp.end("", false);
        }

        pub fn ref(_: *ThisServer): void {
            if (_.poll_ref.isActive()) return;

            _.poll_ref.ref(_.vm);
        }

        pub fn unref(_: *ThisServer): void {
            if (!_.poll_ref.isActive()) return;

            _.poll_ref.unref(_.vm);
        }

        pub fn doRef(_: *ThisServer, _: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            const this_value = callframe.this();
            _.ref();

            return this_value;
        }

        pub fn doUnref(_: *ThisServer, _: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            const this_value = callframe.this();
            _.unref();

            return this_value;
        }

        pub fn onBunInfoRequest(_: *ThisServer, req: *uws.Request, resp: *App.Response): void {
            jsc.markBinding(@src());
            _.pending_requests += 1;
            defer _.pending_requests -= 1;
            req.setYield(false);
            var stack_fallback = std.heap.stackFallback(8192, _.allocator);
            const allocator = stack_fallback.get();

            const buffer_writer = js_printer.BufferWriter.init(allocator);
            var writer = js_printer.BufferPrinter.init(buffer_writer);
            defer writer.ctx.buffer.deinit();
            const source = &logger.Source.initEmptyFile("info.json");
            _ = js_printer.printJSON(
                *js_printer.BufferPrinter,
                &writer,
                bun.Global.BunInfo.generate(*Transpiler, &jsc.VirtualMachine.get().transpiler, allocator) catch unreachable,
                source,
                .{ .mangled_props = null },
            ) catch unreachable;

            resp.writeStatus("200 OK");
            resp.writeHeader("Content-Type", MimeType.json.value);
            resp.writeHeader("Cache-Control", "public, max-age=3600");
            resp.writeHeaderInt("Age", 0);
            const buffer = writer.ctx.written;
            resp.end(buffer, false);
        }

        pub fn onPendingRequest(_: *ThisServer): void {
            _.pending_requests += 1;
        }
        pub fn onNodeHTTPRequestWithUpgradeCtx(_: *ThisServer, req: *uws.Request, resp: *App.Response, upgrade_ctx: ?*uws.WebSocketUpgradeContext): void {
            _.onPendingRequest();
            if (comptime Environment.isDebug) {
                _.vm.eventLoop().debug.enter();
            }
            defer {
                if (comptime Environment.isDebug) {
                    _.vm.eventLoop().debug.exit();
                }
            }
            req.setYield(false);
            resp.timeout(_.config.idleTimeout);

            const globalThis = _.globalThis;
            const thisObject: JSValue = _.js_value.tryGet() orelse .js_undefined;
            const vm = _.vm;

            var node_http_response: ?*NodeHTTPResponse = null;
            var is_async = false;
            defer {
                if (!is_async) {
                    if (node_http_response) |node_response| {
                        node_response.deref();
                    }
                }
            }

            const result: JSValue = bun.jsc.fromJSHostCall(globalThis, @src(), onNodeHTTPRequestFn, .{
                @intFromPtr(AnyServer.from(_).ptr.ptr()),
                globalThis,
                thisObject,
                _.config.onNodeHTTPRequest,
                if (bun.http.Method.find(req.method())) |method|
                    method.toJS(globalThis)
                else
                    .js_undefined,
                req,
                resp,
                upgrade_ctx,
                &node_http_response,
            }) catch globalThis.takeException(error.JSError);

            const HTTPResult = union(enum) {
                rejection: jsc.JSValue,
                exception: jsc.JSValue,
                success: void,
                pending: jsc.JSValue,
            };
            var strong_promise: jsc.Strong.Optional = .empty;
            var needs_to_drain = true;

            defer {
                if (needs_to_drain) {
                    vm.drainMicrotasks();
                }
            }
            defer strong_promise.deinit();
            const http_result: HTTPResult = ({
                if (result.toError()) |err| {
                    break .{ .exception = err };
                }

                if (result.asAnyPromise()) |promise| {
                    if (promise.status() == .pending) {
                        strong_promise.set(globalThis, result);
                        needs_to_drain = false;
                        vm.drainMicrotasks();
                    }

                    switch (promise.status()) {
                        .fulfilled => {
                            globalThis.handleRejectedPromises();
                            break .{ .success = {} };
                        },
                        .rejected => {
                            promise.setHandled(globalThis.vm());
                            break .{ .rejection = promise.result(globalThis.vm()) };
                        },
                        .pending => {
                            globalThis.handleRejectedPromises();
                            if (node_http_response) |node_response| {
                                if (node_response.flags.request_has_completed or node_response.flags.socket_closed or node_response.flags.upgraded) {
                                    strong_promise.deinit();
                                    break .{ .success = {} };
                                }

                                const strong_self = node_response.getThisValue();

                                if (strong_self.isEmptyOrUndefinedOrNull()) {
                                    strong_promise.deinit();
                                    break .{ .success = {} };
                                }

                                node_response.promise = strong_promise;
                                strong_promise = .empty;
                                result.then2(globalThis, strong_self, NodeHTTPResponse.Bun__NodeHTTPRequest__onResolve, NodeHTTPResponse.Bun__NodeHTTPRequest__onReject) catch {};
                                is_async = true;
                            }

                            break .{ .pending = result };
                        },
                    }
                }

                break .{ .success = {} };
            });

            switch (http_result) {
                .exception, .rejection => |err| {
                    _ = vm.uncaughtException(globalThis, err, http_result == .rejection);

                    if (node_http_response) |node_response| {
                        if (!node_response.flags.upgraded and node_response.raw_response != null) {
                            const raw_response = node_response.raw_response.?;
                            if (!node_response.flags.request_has_completed and raw_response.state().isResponsePending()) {
                                if (raw_response.state().isHttpStatusCalled()) {
                                    raw_response.writeStatus("500 Internal Server Error");
                                    raw_response.endWithoutBody(true);
                                } else {
                                    raw_response.endStream(true);
                                }
                            }
                        }
                        node_response.onRequestComplete();
                    }
                },
                .success => {},
                .pending => {},
            }

            if (node_http_response) |node_response| {
                if (!node_response.flags.upgraded and node_response.raw_response != null) {
                    const raw_response = node_response.raw_response.?;
                    if (!node_response.flags.request_has_completed and raw_response.state().isResponsePending()) {
                        node_response.setOnAbortedHandler();
                    } else if (http_result != .pending) {
                        node_response.maybeStopReadingBody(vm, node_response.getThisValue());
                    }
                }
            }
        }

        pub fn onNodeHTTPRequest(_: *ThisServer, req: *uws.Request, resp: *App.Response): void {
            jsc.markBinding(@src());
            _.onNodeHTTPRequestWithUpgradeCtx(req, resp, null);
        }

        const onNodeHTTPRequestFn = if (ssl_enabled)
            NodeHTTPServer__onRequest_https
        else
            NodeHTTPServer__onRequest_http;

        pub fn setUsingCustomExpectHandler(_: *ThisServer, value: bool): void {
            NodeHTTP_setUsingCustomExpectHandler(ssl_enabled, _.app.?, value);
        }

        var did_send_idletimeout_warning_once = false;
        fn onTimeoutForIdleWarn(_: *anyopaque, _: ?*anyopaque): void {
            if (debug_mode and !did_send_idletimeout_warning_once) {
                if (!bun.cli.Command.get().debug.silent) {
                    did_send_idletimeout_warning_once = true;
                    Output.prettyErrorln("<r><yellow>[Bun.serve]<r><d>:<r> request timed out after 10 seconds. Pass <d><cyan>`idleTimeout`<r> to configure.", .{});
                    Output.flush();
                }
            }
        }

        fn shouldAddTimeoutHandlerForWarning(server: *ThisServer): bool {
            if (comptime debug_mode) {
                if (!did_send_idletimeout_warning_once and !bun.cli.Command.get().debug.silent) {
                    return !server.config.has_idleTimeout;
                }
            }

            return false;
        }

        pub fn onUserRouteRequest(user_route: *UserRoute, req: *uws.Request, resp: *App.Response): void {
            onUserRouteRequestFor(RequestContext, user_route, req, resp);
        }

        fn onUserRouteRequestFor(comptime Ctx: type, user_route: *UserRoute, req: *Ctx.Req, resp: *Ctx.Resp): void {
            const server = user_route.server;
            const index = user_route.id;

            var should_deinit_context = false;
            var prepared = server.prepareJsRequestContextFor(Ctx, req, resp, &should_deinit_context, .no, switch (user_route.route.method) {
                .any => null,
                .specific => |m| m,
            }) orelse return;

            const server_request_list = js.routeListGetCached(server.jsValueAssertAlive()).?;
            const callRoute = if (Ctx.is_h3) Bun__ServerRouteList__callRouteH3 else Bun__ServerRouteList__callRoute;
            const response_value = bun.jsc.fromJSHostCall(server.globalThis, @src(), callRoute, .{ server.globalThis, index, prepared.request_object, server.jsValueAssertAlive(), server_request_list, &prepared.js_request, req }) catch |err| server.globalThis.takeException(err);

            server.handleRequestFor(Ctx, &should_deinit_context, prepared, req, response_value);
        }

        fn handleRequest(_: *ThisServer, should_deinit_context: *bool, prepared: PreparedRequest, req: *uws.Request, response_value: jsc.JSValue): void {
            _.handleRequestFor(RequestContext, should_deinit_context, prepared, req, response_value);
        }

        fn handleRequestFor(_: *ThisServer, comptime Ctx: type, should_deinit_context: *bool, prepared: PreparedRequestFor(Ctx), req: *Ctx.Req, response_value: jsc.JSValue): void {
            const ctx = prepared.ctx;

            defer {
                prepared.request_object.request_context.detachRequest();
            }

            ctx.onResponse(_, prepared.js_request, response_value);
            prepared.js_request.ensureStillAlive();

            ctx.defer_deinit_until_callback_completes = null;

            if (should_deinit_context.*) {
                ctx.deinit();
                return;
            }

            if (ctx.shouldRenderMissing()) {
                ctx.renderMissing();
                return;
            }

            ctx.toAsync(req, prepared.request_object);
        }

        pub fn onRequest(_: *ThisServer, req: *uws.Request, resp: *App.Response): void {
            _.onRequestFor(RequestContext, req, resp);
        }

        fn onRequestFor(_: *ThisServer, comptime Ctx: type, req: *Ctx.Req, resp: *Ctx.Resp): void {
            var should_deinit_context = false;
            const prepared = _.prepareJsRequestContextFor(Ctx, req, resp, &should_deinit_context, .yes, null) orelse return;

            bun.assert(_.config.onRequest != .zero);

            const js_value = _.jsValueAssertAlive();
            const response_value = _.config.onRequest.call(
                _.globalThis,
                js_value,
                &.{ prepared.js_request, js_value },
            ) catch |err|
                _.globalThis.takeException(err);

            _.handleRequestFor(Ctx, &should_deinit_context, prepared, req, response_value);
        }

        pub fn onSavedRequest(
            _: *ThisServer,
            req: SavedRequest.Union,
            resp: *App.Response,
            callback: JSValue,
            comptime arg_count: comptime_int,
            extra_args: [arg_count]JSValue,
        ): void {
            const prepared: PreparedRequest = switch (req) {
                .stack => |r| _.prepareJsRequestContext(r, resp, null, .bake, null) orelse return,
                .saved => |data| .{
                    .js_request = data.js_request.get() orelse @panic("Request was unexpectedly freed"),
                    .request_object = data.request,
                    .ctx = data.ctx.tagged_pointer.as(RequestContext),
                },
            };
            const ctx = prepared.ctx;

            bun.assert(callback != .zero);
            const args = .{prepared.js_request} ++ extra_args;
            const response_value = callback.call(
                _.globalThis,
                _.jsValueAssertAlive(),
                &args,
            ) catch |err|
                _.globalThis.takeException(err);

            defer if (req == .stack) {
                prepared.request_object.request_context.detachRequest();
            };
            const original_state = ctx.defer_deinit_until_callback_completes;
            var should_deinit_context = false;
            ctx.defer_deinit_until_callback_completes = &should_deinit_context;
            ctx.onResponse(_, prepared.js_request, response_value);
            ctx.defer_deinit_until_callback_completes = original_state;

            prepared.js_request.ensureStillAlive();

            if (should_deinit_context) {
                ctx.deinit();
                return;
            }

            if (ctx.shouldRenderMissing()) {
                ctx.renderMissing();
                return;
            }

            switch (req) {
                .stack => |r| ctx.toAsync(r, prepared.request_object),
                .saved => {},
            }
        }

        pub const PreparedRequest = PreparedRequestFor(RequestContext);

        fn PreparedRequestFor(comptime Ctx: type): type {
            return struct {
                js_request: JSValue,
                request_object: *Request,
                ctx: *Ctx,

                pub fn save(
                    prepared: @This(),
                    global: *jsc.JSGlobalObject,
                    req: *Ctx.Req,
                    resp: *Ctx.Resp,
                ): SavedRequest {
                    if (comptime Ctx.is_h3) @compileError("PreparedRequest.save is HTTP/1-only");
                    prepared.ctx.toAsync(req, prepared.request_object);

                    return .{
                        .js_request = .create(prepared.js_request, global),
                        .request = prepared.request_object,
                        .ctx = AnyRequestContext.init(prepared.ctx),
                        .response = uws.AnyResponse.init(resp),
                    };
                }
            };
        }

        const CreateJsRequest = enum { yes, no, bake };

        pub fn prepareJsRequestContext(
            _: *ThisServer,
            req: *uws.Request,
            resp: *App.Response,
            should_deinit_context: ?*bool,
            create_js_request: CreateJsRequest,
            method: ?bun.http.Method,
        ): ?PreparedRequest {
            return _.prepareJsRequestContextFor(RequestContext, req, resp, should_deinit_context, create_js_request, method);
        }

        fn prepareJsRequestContextFor(
            _: *ThisServer,
            comptime Ctx: type,
            req: *Ctx.Req,
            resp: *Ctx.Resp,
            should_deinit_context: ?*bool,
            create_js_request: CreateJsRequest,
            method: ?bun.http.Method,
        ): ?PreparedRequestFor(Ctx) {
            jsc.markBinding(@src());

            if (comptime Ctx.is_h3) if (req.header("transfer-encoding") != null) {
                resp.writeStatus("400 Bad Request");
                resp.endWithoutBody(false);
                return null;
            };

            const request_body_length: ?usize = ({
                if ((HTTP.Method.which(req.method()) orelse HTTP.Method.OPTIONS).hasRequestBody()) {
                    const len: usize = ({
                        if (req.header("content-length")) |content_length| {
                            break std.fmt.parseInt(usize, content_length, 10) catch 0;
                        }
                        break 0;
                    });

                    if (len > _.config.max_request_body_size) {
                        resp.writeStatus("413 Request Entity Too Large");
                        resp.endWithoutBody(comptime !Ctx.is_h3);
                        return null;
                    }

                    break len;
                }

                break null;
            });

            _.onPendingRequest();

            if (comptime Environment.isDebug) {
                _.vm.eventLoop().debug.enter();
            }
            defer {
                if (comptime Environment.isDebug) {
                    _.vm.eventLoop().debug.exit();
                }
            }
            req.setYield(false);
            resp.timeout(_.config.idleTimeout);

            if (shouldAddTimeoutHandlerForWarning(_)) {
                resp.onTimeout(*anyopaque, struct {
                    fn warn(p: *anyopaque, _: *Ctx.Resp): void {
                        onTimeoutForIdleWarn(p, null);
                    }
                }.warn, &did_send_idletimeout_warning_once);
            }

            const pool_allocator = if (comptime Ctx.is_h3) _.h3_request_pool_allocator else _.request_pool_allocator;
            const ctx = bun.handleOom(pool_allocator.tryGet());
            ctx.create(_, req, resp, should_deinit_context, method);
            _.vm.jsc_vm.reportExtraMemory(@sizeOf(Ctx));
            const body = _.vm.initRequestBodyValue(.{ .Null = {} }) catch unreachable;

            ctx.request_body = body;
            var signal = jsc.WebCore.AbortSignal.new(_.globalThis);
            ctx.signal = signal;
            signal.pendingActivityRef();

            const request_object = Request.new(Request.init(
                ctx.method,
                AnyRequestContext.init(ctx),
                ssl_enabled,
                signal.ref(),
                body.ref(),
            ));
            ctx.request_weakref = .initRef(request_object);

            if (comptime Ctx.is_h3) {
                request_object.setFetchHeaders(.createFromH3(req));
                const path = req.url();
                if (path.len > 0 and path[0] == '/') {
                    if (req.header("host")) |host| {
                        const fmt = bun.fmt.HostFormatter{ .is_https = true, .host = host };
                        request_object.url = bun.handleOom(bun.String.createFormat("https://{f}{s}", .{ fmt, path }));
                    } else {
                        request_object.url = bun.String.cloneUTF8(path);
                    }
                } else {
                    request_object.url = bun.String.cloneUTF8(path);
                }
                ctx.req = null;
            }

            if (comptime debug_mode) {
                ctx.flags.is_web_browser_navigation = ({
                    if (req.header("sec-fetch-dest")) |fetch_dest| {
                        if (strings.eqlComptime(fetch_dest, "document")) {
                            break true;
                        }
                    }
                    break false;
                });
            }

            if (request_body_length) |req_len| {
                ctx.request_body_content_len = req_len;
                ctx.flags.is_transfer_encoding = req.header("transfer-encoding") != null;
                if (req_len > 0 or ctx.flags.is_transfer_encoding or comptime Ctx.is_h3) {
                    ctx.request_body.?.value = .{
                        .Locked = .{
                            .task = ctx,
                            .global = _.globalThis,
                            .onStartBuffering = Ctx.onStartBufferingCallback,
                            .onStartStreaming = Ctx.onStartStreamingRequestBodyCallback,
                            .onReadableStreamAvailable = Ctx.onRequestBodyReadableStreamAvailable,
                        },
                    };
                    ctx.flags.is_waiting_for_request_body = true;

                    resp.onData(*Ctx, Ctx.onBufferedBodyChunk, ctx);
                }
            }

            return .{
                .js_request = switch (create_js_request) {
                    .yes => request_object.toJS(_.globalThis),
                    .bake => request_object.toJSForBake(_.globalThis) catch |err| switch (err) {
                        error.OutOfMemory => bun.outOfMemory(),
                        else => return null,
                    },
                    .no => .zero,
                },
                .request_object = request_object,
                .ctx = ctx,
            };
        }

        fn upgradeWebSocketUserRoute(user_route: *UserRoute, resp: *App.Response, req: *uws.Request, upgrade_ctx: *uws.WebSocketUpgradeContext, method: ?bun.http.Method): void {
            const server = user_route.server;
            const index = user_route.id;

            var should_deinit_context = false;
            var prepared = server.prepareJsRequestContext(req, resp, &should_deinit_context, .no, method) orelse return;
            prepared.ctx.upgrade_context = upgrade_ctx;
            const server_request_list = js.routeListGetCached(server.jsValueAssertAlive()).?;
            const response_value = bun.jsc.fromJSHostCall(server.globalThis, @src(), Bun__ServerRouteList__callRoute, .{ server.globalThis, index, prepared.request_object, server.jsValueAssertAlive(), server_request_list, &prepared.js_request, req }) catch |err| server.globalThis.takeException(err);

            server.handleRequest(&should_deinit_context, prepared, req, response_value);
        }

        pub fn onWebSocketUpgrade(_: *ThisServer, resp: *App.Response, req: *uws.Request, upgrade_ctx: *uws.WebSocketUpgradeContext, id: usize): void {
            jsc.markBinding(@src());
            if (id == 1) {
                upgradeWebSocketUserRoute(@ptrCast(_), resp, req, upgrade_ctx, null);
                return;
            }
            bun.assert(id == 0);
            if (_.config.onNodeHTTPRequest != .zero) {
                onNodeHTTPRequestWithUpgradeCtx(_, req, resp, upgrade_ctx);
                return;
            }
            if (_.config.onRequest == .zero) {
                resp.writeStatus("403 Forbidden");
                resp.endWithoutBody(true);
                return;
            }
            _.pending_requests += 1;
            req.setYield(false);
            var ctx = bun.handleOom(_.request_pool_allocator.tryGet());
            var should_deinit_context = false;
            ctx.create(_, req, resp, &should_deinit_context, null);
            var body = _.vm.initRequestBodyValue(.{ .Null = {} }) catch unreachable;

            ctx.request_body = body;
            var signal = jsc.WebCore.AbortSignal.new(_.globalThis);
            ctx.signal = signal;
            signal.pendingActivityRef();

            var request_object = Request.new(Request.init(
                ctx.method,
                AnyRequestContext.init(ctx),
                ssl_enabled,
                signal.ref(),
                body.ref(),
            ));
            ctx.upgrade_context = upgrade_ctx;
            ctx.request_weakref = .initRef(request_object);
            var args = [_]jsc.JSValue{
                request_object.toJS(_.globalThis),
                _.jsValueAssertAlive(),
            };
            const request_value = args[0];
            request_value.ensureStillAlive();

            const response_value = _.config.onRequest.call(_.globalThis, _.jsValueAssertAlive(), &args) catch |err|
                _.globalThis.takeException(err);
            defer {
                request_object.request_context.detachRequest();
            }
            ctx.onResponse(
                _,
                request_value,
                response_value,
            );

            ctx.defer_deinit_until_callback_completes = null;

            if (should_deinit_context) {
                ctx.deinit();
                return;
            }

            if (ctx.shouldRenderMissing()) {
                ctx.renderMissing();
                return;
            }

            ctx.toAsync(req, request_object);
        }

        fn onChromeDevToolsJSONRequest(_: *ThisServer, req: *uws.Request, resp: *App.Response): void {
            if (comptime Environment.enable_logs)
                httplog("{s} - {s}", .{ req.method(), req.url() });

            const authorized = ({
                if (_.dev_server == null)
                    break false;

                if (resp.getRemoteSocketInfo()) |*address| {
                    if (strings.startsWith(address.ip, "127.")) {
                        break true;
                    }
                    if (strings.startsWith(address.ip, "::ffff:127.") or
                        strings.startsWith(address.ip, "::1") or
                        strings.eqlComptime(address.ip, "0:0:0:0:0:0:0:1"))
                    {
                        break true;
                    }
                }

                break false;
            });

            if (!authorized) {
                req.setYield(true);
                return;
            }

            const first_hash_segment: [8]u8 = ({
                const buffer = bun.path_buffer_pool.get();
                defer bun.path_buffer_pool.put(buffer);
                const main = jsc.VirtualMachine.get().main;
                const len = @min(main.len, buffer.len);
                break @bitCast(bun.hash(bun.strings.copyLowercase(main[0..len], buffer[0..len])));
            });

            const second_hash_segment: [8]u8 = ({
                const buffer = bun.path_buffer_pool.get();
                defer bun.path_buffer_pool.put(buffer);
                const root = _.dev_server.?.root;
                const len = @min(root.len, buffer.len);
                break @bitCast(bun.hash(bun.strings.copyLowercase(root[0..len], buffer[0..len])));
            });

            const hash_bytes: [16]u8 = first_hash_segment ++ second_hash_segment;
            const uuid = bun.UUID.initWith(&hash_bytes);

            const json_string = std.fmt.allocPrint(bun.default_allocator, "{{ \"workspace\": {{ \"root\": {f}, \"uuid\": \"{f}\" }} }}", .{
                bun.fmt.formatJSONStringUTF8(_.dev_server.?.root, .{}),
                uuid,
            }) catch |err| bun.handleOom(err);
            defer bun.default_allocator.free(json_string);

            resp.writeStatus("200 OK");
            resp.writeHeader("Content-Type", "application/json");
            resp.end(json_string, resp.shouldCloseConnection());
        }

        fn setRoutes(_: *ThisServer): jsc.JSValue {
            var route_list_value = jsc.JSValue.zero;
            const app = _.app.?;
            const any_server = AnyServer.from(_);
            const dev_server = _.dev_server;

            var should_add_chrome_devtools_json_route = debug_mode and _.config.allow_hot and dev_server != null and _.config.enable_chrome_devtools_automatic_workspace_folders;
            const chrome_devtools_route = "/.well-known/appspecific/com.chrome.devtools.json";

            if (_.config.user_routes_to_build.items.len > 0) {
                var user_routes_to_build_list = _.config.user_routes_to_build.moveToUnmanaged();
                var old_user_routes = _.user_routes;
                defer {
                    for (old_user_routes.items) |*r| r.route.deinit();
                    old_user_routes.deinit(bun.default_allocator);
                }
                _.user_routes = std.ArrayListUnmanaged(UserRoute).initCapacity(bun.default_allocator, user_routes_to_build_list.items.len) catch @panic("OOM");
                const paths_zig = bun.default_allocator.alloc(ZigString, user_routes_to_build_list.items.len) catch @panic("OOM");
                defer bun.default_allocator.free(paths_zig);
                const callbacks_js = bun.default_allocator.alloc(jsc.JSValue, user_routes_to_build_list.items.len) catch @panic("OOM");
                defer bun.default_allocator.free(callbacks_js);

                for (user_routes_to_build_list.items, paths_zig, callbacks_js, 0..) |*builder, *p_zig, *cb_js, i| {
                    p_zig.* = ZigString.init(builder.route.path);
                    cb_js.* = builder.callback.get().?;
                    _.user_routes.appendAssumeCapacity(.{
                        .id = @truncate(i),
                        .server = _,
                        .route = builder.route,
                    });
                    builder.route = .{};
                }
                route_list_value = Bun__ServerRouteList__create(_.globalThis, callbacks_js.ptr, paths_zig.ptr, user_routes_to_build_list.items.len);
                for (user_routes_to_build_list.items) |*builder| builder.deinit();
                user_routes_to_build_list.deinit(bun.default_allocator);
            }

            if (_.config.websocket) |*websocket| {
                websocket.globalObject = _.globalThis;
                websocket.handler.app = app;
                websocket.handler.flags.ssl = ssl_enabled;
            }

            var star_methods_covered_by_user = bun.http.Method.Set.initEmpty();
            var has_any_user_route_for_star_path = false;
            var has_any_ws_route_for_star_path = false;

            for (_.user_routes.items) |*user_route| {
                const is_star_path = strings.eqlComptime(user_route.route.path, "/*");
                if (is_star_path) {
                    has_any_user_route_for_star_path = true;
                }

                if (should_add_chrome_devtools_json_route) {
                    if (strings.eqlComptime(user_route.route.path, chrome_devtools_route) or strings.hasPrefix(user_route.route.path, "/.well-known/")) {
                        should_add_chrome_devtools_json_route = false;
                    }
                }

                switch (user_route.route.method) {
                    .any => {
                        app.any(user_route.route.path, *UserRoute, user_route, onUserRouteRequest);
                        if (comptime has_h3) {
                            if (_.h3_app) |h3_app| h3_app.any(user_route.route.path, *UserRoute, user_route, onH3UserRouteRequest);
                        }
                        if (is_star_path) {
                            star_methods_covered_by_user = .initFull();
                        }

                        if (_.config.websocket) |*websocket| {
                            if (is_star_path) {
                                has_any_ws_route_for_star_path = true;
                            }
                            app.ws(
                                user_route.route.path,
                                user_route,
                                1,
                                ServerWebSocket.behavior(ThisServer, ssl_enabled, websocket.toBehavior()),
                            );
                        }
                    },
                    .specific => |method_val| {
                        app.method(method_val, user_route.route.path, *UserRoute, user_route, onUserRouteRequest);
                        if (comptime has_h3) {
                            if (_.h3_app) |h3_app| h3_app.method(method_val, user_route.route.path, *UserRoute, user_route, onH3UserRouteRequest);
                        }
                        if (is_star_path) {
                            star_methods_covered_by_user.insert(method_val);
                        }

                        if (_.config.websocket) |*websocket| {
                            if (method_val == .GET) {
                                app.ws(
                                    user_route.route.path,
                                    user_route,
                                    1,
                                    ServerWebSocket.behavior(ThisServer, ssl_enabled, websocket.toBehavior()),
                                );
                            }
                        }
                    },
                }
            }

            for (_.config.negative_routes.items) |route_path| {
                app.head(route_path, *ThisServer, _, onRequest);
                app.any(route_path, *ThisServer, _, onRequest);
                if (comptime has_h3) {
                    if (_.h3_app) |h3_app| {
                        h3_app.head(route_path, *ThisServer, _, onH3Request);
                        h3_app.any(route_path, *ThisServer, _, onH3Request);
                    }
                }
            }

            var needs_plugins = dev_server != null;
            var has_static_route_for_star_path = false;

            if (_.config.static_routes.items.len > 0) {
                for (_.config.static_routes.items) |*entry| {
                    if (strings.eqlComptime(entry.path, "/*")) {
                        has_static_route_for_star_path = true;
                        switch (entry.method) {
                            .any => {
                                star_methods_covered_by_user = .initFull();
                            },
                            .method => |method| {
                                star_methods_covered_by_user.setUnion(method);
                            },
                        }
                    }

                    if (should_add_chrome_devtools_json_route) {
                        if (strings.eqlComptime(entry.path, chrome_devtools_route) or strings.hasPrefix(entry.path, "/.well-known/")) {
                            should_add_chrome_devtools_json_route = false;
                        }
                    }

                    switch (entry.route) {
                        .static => |static_route| {
                            ServerConfig.applyStaticRoute(any_server, ssl_enabled, app, *StaticRoute, static_route, entry.path, entry.method);
                            if (comptime has_h3) if (_.h3_app) |h3_app|
                                ServerConfig.applyStaticRouteH3(any_server, h3_app, *StaticRoute, static_route, entry.path, entry.method);
                        },
                        .file => |file_route| {
                            ServerConfig.applyStaticRoute(any_server, ssl_enabled, app, *FileRoute, file_route, entry.path, entry.method);
                            if (comptime has_h3) if (_.h3_app) |h3_app|
                                ServerConfig.applyStaticRouteH3(any_server, h3_app, *FileRoute, file_route, entry.path, entry.method);
                        },
                        .html => |html_bundle_route| {
                            ServerConfig.applyStaticRoute(any_server, ssl_enabled, app, *HTMLBundle.Route, html_bundle_route.data, entry.path, entry.method);
                            if (comptime has_h3) if (_.h3_app) |h3_app|
                                ServerConfig.applyStaticRouteH3(any_server, h3_app, *HTMLBundle.Route, html_bundle_route.data, entry.path, entry.method);
                            if (dev_server) |dev| {
                                bun.handleOom(dev.html_router.put(dev.allocator(), entry.path, html_bundle_route.data));
                            }
                            needs_plugins = true;
                        },
                        .framework_router => {},
                    }
                }
            }

            if (needs_plugins and _.plugins == null) {
                if (_.vm.transpiler.options.serve_plugins) |serve_plugins_config| {
                    if (serve_plugins_config.len > 0) {
                        _.plugins = ServePlugins.init(serve_plugins_config);
                    }
                }
            }

            if (debug_mode) {
                app.get("/bun:info", *ThisServer, _, onBunInfoRequest);
            }

            const h3_star_covered = star_methods_covered_by_user;
            _ = &h3_star_covered;

            var has_dev_server_for_star_path = false;
            if (dev_server) |dev| {
                has_dev_server_for_star_path = bun.handleOom(dev.setRoutes(_));
                if (has_dev_server_for_star_path) {
                    star_methods_covered_by_user = .initFull();
                }
            }

            if (!has_any_ws_route_for_star_path) {
                if (_.config.websocket) |*websocket| {
                    app.ws(
                        "/*",
                        _,
                        0,
                        ServerWebSocket.behavior(ThisServer, ssl_enabled, websocket.toBehavior()),
                    );
                }
            }

            if (star_methods_covered_by_user.eql(bun.http.Method.Set.initFull())) {
            } else if (has_any_user_route_for_star_path or has_static_route_for_star_path or has_dev_server_for_star_path) {
                star_methods_covered_by_user.toggleAll();
                var iter = star_methods_covered_by_user.iterator();
                while (iter.next()) |method_to_cover| {
                    switch (_.config.onNodeHTTPRequest) {
                        .zero => switch (_.config.onRequest) {
                            .zero => app.method(method_to_cover, "/*", *ThisServer, _, on404),
                            else => app.method(method_to_cover, "/*", *ThisServer, _, onRequest),
                        },
                        else => app.method(method_to_cover, "/*", *ThisServer, _, onNodeHTTPRequest),
                    }
                }
            } else {
                switch (_.config.onNodeHTTPRequest) {
                    .zero => switch (_.config.onRequest) {
                        .zero => app.any("/*", *ThisServer, _, on404),
                        else => app.any("/*", *ThisServer, _, onRequest),
                    },
                    else => app.any("/*", *ThisServer, _, onNodeHTTPRequest),
                }
            }

            if (comptime has_h3) {
                if (_.h3_app) |h3_app| {
                    if (h3_star_covered.eql(bun.http.Method.Set.initFull())) {
                    } else if (has_any_user_route_for_star_path or has_static_route_for_star_path) {
                        var uncovered = h3_star_covered;
                        uncovered.toggleAll();
                        var iter = uncovered.iterator();
                        while (iter.next()) |m| {
                            if (_.config.onRequest != .zero) {
                                h3_app.method(m, "/*", *ThisServer, _, onH3Request);
                            } else {
                                h3_app.method(m, "/*", *ThisServer, _, onH3404);
                            }
                        }
                    } else if (_.config.onRequest != .zero) {
                        h3_app.any("/*", *ThisServer, _, onH3Request);
                    } else {
                        h3_app.any("/*", *ThisServer, _, onH3404);
                    }
                }
            }

            if (should_add_chrome_devtools_json_route) {
                app.get(chrome_devtools_route, *ThisServer, _, onChromeDevToolsJSONRequest);
            }

            if (_.config.onNodeHTTPRequest != .zero) {
                NodeHTTP_assignOnNodeJSCompat(ssl_enabled, app);
            }

            return route_list_value;
        }

        pub fn on404(_: *ThisServer, req: *uws.Request, resp: *App.Response): void {
            if (comptime Environment.enable_logs)
                httplog("{s} - {s} 404", .{ req.method(), req.url() });

            resp.writeStatus("404 Not Found");
            resp.end("", false);
        }

        pub fn listen(_: *ThisServer): jsc.JSValue {
            httplog("listen", .{});
            var app: *App = undefined;
            const globalThis = _.globalThis;
            var route_list_value = jsc.JSValue.zero;
            if (ssl_enabled) {
                bun.BoringSSL.load();
                const ssl_config = _.config.ssl_config orelse @panic("Assertion failure: ssl_config");
                const ssl_options = ssl_config.asUSockets();

                app = App.create(ssl_options) orelse {
                    if (!globalThis.hasException()) {
                        if (!throwSSLErrorIfNecessary(globalThis)) {
                            globalThis.throw("Failed to create HTTP server", .{}) catch {};
                        }
                    }

                    _.app = null;
                    _.deinit();
                    return .zero;
                };

                _.app = app;

                if (comptime has_h3) {
                    if (_.config.http3) {
                        _.h3_app = uws.H3.App.create(ssl_options, _.config.idleTimeout) orelse {
                            if (!globalThis.hasException()) {
                                globalThis.throw("Failed to create HTTP/3 server", .{}) catch {};
                            }
                            _.deinit();
                            return .zero;
                        };
                    }
                }

                route_list_value = _.setRoutes();

                if (ssl_config.server_name) |server_name_ptr| {
                    const server_name: [:0]const u8 = std.mem.span(server_name_ptr);
                    if (server_name.len > 0) {
                        app.addServerNameWithOptions(server_name, ssl_options) catch {
                            if (!globalThis.hasException()) {
                                if (!throwSSLErrorIfNecessary(globalThis)) {
                                    globalThis.throw("Failed to add serverName: {s}", .{server_name}) catch {};
                                }
                            }

                            _.deinit();
                            return .zero;
                        };
                        if (throwSSLErrorIfNecessary(globalThis)) {
                            _.deinit();
                            return .zero;
                        }

                        app.domain(server_name);
                        if (throwSSLErrorIfNecessary(globalThis)) {
                            _.deinit();
                            return .zero;
                        }

                        _ = _.setRoutes();
                    }
                }

                if (_.config.sni) |*sni| {
                    for (sni.slice()) |*sni_ssl_config| {
                        const sni_servername: [:0]const u8 = std.mem.span(sni_ssl_config.server_name.?);
                        if (sni_servername.len > 0) {
                            if (comptime has_h3) if (_.h3_app) |h3a|
                                h3a.addServerNameWithOptions(sni_servername, sni_ssl_config.asUSockets()) catch {
                                    if (!globalThis.hasException()) {
                                        globalThis.throw("Failed to add serverName \"{s}\" for HTTP/3", .{sni_servername}) catch {};
                                    }
                                    _.deinit();
                                    return .zero;
                                };
                            app.addServerNameWithOptions(sni_servername, sni_ssl_config.asUSockets()) catch {
                                if (!globalThis.hasException()) {
                                    if (!throwSSLErrorIfNecessary(globalThis)) {
                                        globalThis.throw("Failed to add serverName: {s}", .{sni_servername}) catch {};
                                    }
                                }

                                _.deinit();
                                return .zero;
                            };

                            app.domain(sni_servername);

                            if (throwSSLErrorIfNecessary(globalThis)) {
                                _.deinit();
                                return .zero;
                            }

                            _ = _.setRoutes();
                        }
                    }
                }
            } else {
                app = App.create(.{}) orelse {
                    if (!globalThis.hasException()) {
                        globalThis.throw("Failed to create HTTP server", .{}) catch {};
                    }
                    _.deinit();
                    return .zero;
                };
                _.app = app;

                route_list_value = _.setRoutes();
            }

            if (_.config.onNodeHTTPRequest != .zero) {
                _.setUsingCustomExpectHandler(true);
            }

            switch (_.config.address) {
                .tcp => |tcp| {
                    var host: ?[*:0]const u8 = null;
                    var host_buff: [1024:0]u8 = undefined;

                    if (tcp.hostname) |existing| {
                        const hostname = bun.span(existing);

                        if (hostname.len > 2 and hostname[0] == '[') {
                            host = std.fmt.bufPrintZ(&host_buff, "{s}", .{hostname[1 .. hostname.len - 1]}) catch unreachable;
                        } else {
                            host = tcp.hostname;
                        }
                    }

                    if (_.config.http1) {
                        app.listenWithConfig(*ThisServer, _, onListen, .{
                            .port = tcp.port,
                            .host = host,
                            .options = _.config.getUsocketsOptions(),
                        });
                    }

                    if (comptime has_h3) {
                        if (_.h3_app) |h3_app| {
                            const h3_port: u16 = if (_.listener) |ls| @intCast(ls.getLocalPort()) else tcp.port;
                            h3_app.listenWithConfig(*ThisServer, _, onH3Listen, .{
                                .port = h3_port,
                                .host = host,
                                .options = _.config.getUsocketsOptions(),
                            });
                            if (_.h3_listener == null) {
                                if (!globalThis.hasException()) {
                                    globalThis.throw("Failed to listen on UDP port {d} for HTTP/3", .{h3_port}) catch {};
                                }
                                _.deinit();
                                return .zero;
                            }
                            if (!_.config.http1) _.vm.event_loop_handle = Async.Loop.get();
                        }
                    }
                },

                .unix => |unix| {
                    if (comptime has_h3) if (_.h3_app) |h3a| {
                        bun.Output.warn("http3: true with a unix socket — HTTP/3 listener skipped", .{});
                        h3a.destroy();
                        _.h3_app = null;
                    };
                    app.listenOnUnixSocket(
                        *ThisServer,
                        _,
                        onListen,
                        unix,
                        _.config.getUsocketsOptions(),
                    );
                },
            }

            if (globalThis.hasException()) {
                _.deinit();
                return .zero;
            }

            _.ref();

            if (_.vm.aggressive_garbage_collection == .aggressive) {
                _.vm.autoGarbageCollect();
            } else {
                _.vm.eventLoop().performGC();
            }

            return route_list_value;
        }

        pub fn onClientErrorCallback(_: *ThisServer, socket: *uws.Socket, error_code: u8, raw_packet: []const u8): void {
            if (_.on_clienterror.get()) |callback| {
                const is_ssl = protocol_enum == .https;
                const node_socket = bun.jsc.fromJSHostCall(_.globalThis, @src(), Bun__createNodeHTTPServerSocketForClientError, .{ is_ssl, socket, _.globalThis }) catch return;
                if (node_socket.isUndefinedOrNull()) return;

                const error_code_value = JSValue.jsNumber(error_code);
                const raw_packet_value = jsc.ArrayBuffer.createBuffer(_.globalThis, raw_packet) catch return;
                const loop = _.globalThis.bunVM().eventLoop();
                loop.enter();
                defer loop.exit();
                _ = callback.call(_.globalThis, .js_undefined, &.{ JSValue.jsBoolean(is_ssl), node_socket, error_code_value, raw_packet_value }) catch |err| {
                    _.globalThis.reportActiveExceptionAsUnhandled(err);
                };
            }
        }
    };
}

pub const AnyRequestContext = @import("./AnyRequestContext.zig");
pub const NewRequestContext = @import("./RequestContext.zig").NewRequestContext;

pub class SavedRequest {
    js_request: jsc.Strong.Optional,
    request: *Request,
    ctx: AnyRequestContext,
    response: uws.AnyResponse,

    pub fn release(_: *SavedRequest): void {
        _.js_request.deinit();
        _.ctx.deref();
    }

    pub fn deinit(_: *SavedRequest): void {
        _.release();
    }

    pub union Union {
        stack: *uws.Request,
        saved: bun.jsc.API.SavedRequest,
    }
}

pub class ServerAllConnectionsClosedTask {
    globalObject: *jsc.JSGlobalObject,
    promise: jsc.JSPromise.Strong,
    tracker: jsc.Debugger.AsyncTaskTracker,

    pub const new = bun.TrivialNew(@This());

    pub fn schedule(_: ServerAllConnectionsClosedTask, vm: *VirtualMachine): void {
        const ptr = new(_);
        vm.eventLoop().enqueueTask(jsc.Task.init(ptr));
    }

    pub fn runFromJSThread(_: *ServerAllConnectionsClosedTask, vm: *jsc.VirtualMachine): bun.JSTerminated!void {
        httplog("ServerAllConnectionsClosedTask runFromJSThread", .{});

        const globalObject = _.globalObject;
        const tracker = _.tracker;
        tracker.willDispatch(globalObject);
        defer tracker.didDispatch(globalObject);

        var promise = _.promise;
        defer promise.deinit();
        bun.destroy(_);

        if (!vm.isShuttingDown()) {
            try promise.resolve(globalObject, .js_undefined);
        }
    }
}

pub const HTTPServer = NewServer(.http, .production);
pub const HTTPSServer = NewServer(.https, .production);
pub const DebugHTTPServer = NewServer(.http, .debug);
pub const DebugHTTPSServer = NewServer(.https, .debug);

pub class AnyServer {
    ptr: Ptr,

    pub const Ptr = bun.TaggedPointerUnion(.{
        HTTPServer,
        HTTPSServer,
        DebugHTTPServer,
        DebugHTTPSServer,
    });

    pub union AnyUserRouteList {
        HTTPServer: []const HTTPServer.UserRoute,
        HTTPSServer: []const HTTPSServer.UserRoute,
        DebugHTTPServer: []const DebugHTTPServer.UserRoute,
        DebugHTTPSServer: []const DebugHTTPSServer.UserRoute,
    }

    pub fn userRoutes(_: AnyServer): AnyUserRouteList {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => .{ .HTTPServer = _.ptr.as(HTTPServer).user_routes.items },
            Ptr.case(HTTPSServer) => .{ .HTTPSServer = _.ptr.as(HTTPSServer).user_routes.items },
            Ptr.case(DebugHTTPServer) => .{ .DebugHTTPServer = _.ptr.as(DebugHTTPServer).user_routes.items },
            Ptr.case(DebugHTTPSServer) => .{ .DebugHTTPSServer = _.ptr.as(DebugHTTPSServer).user_routes.items },
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn getURLAsString(_: AnyServer): bun.OOM!bun.String {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).getURLAsString(),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).getURLAsString(),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).getURLAsString(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).getURLAsString(),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn vm(_: AnyServer): *jsc.VirtualMachine {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).vm,
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).vm,
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).vm,
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).vm,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn h3AltSvc(_: AnyServer): ?[]const u8 {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).h3AltSvc(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).h3AltSvc(),
            else => null,
        };
    }

    pub fn setInspectorServerID(_: AnyServer, id: jsc.Debugger.DebuggerId): void {
        switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => {
                _.ptr.as(HTTPServer).inspector_server_id = id;
                if (_.ptr.as(HTTPServer).dev_server) |dev_server| {
                    dev_server.inspector_server_id = id;
                }
            },
            Ptr.case(HTTPSServer) => {
                _.ptr.as(HTTPSServer).inspector_server_id = id;
                if (_.ptr.as(HTTPSServer).dev_server) |dev_server| {
                    dev_server.inspector_server_id = id;
                }
            },
            Ptr.case(DebugHTTPServer) => {
                _.ptr.as(DebugHTTPServer).inspector_server_id = id;
                if (_.ptr.as(DebugHTTPServer).dev_server) |dev_server| {
                    dev_server.inspector_server_id = id;
                }
            },
            Ptr.case(DebugHTTPSServer) => {
                _.ptr.as(DebugHTTPSServer).inspector_server_id = id;
                if (_.ptr.as(DebugHTTPSServer).dev_server) |dev_server| {
                    dev_server.inspector_server_id = id;
                }
            },
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        }
    }

    pub fn inspectorServerID(_: AnyServer): jsc.Debugger.DebuggerId {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).inspector_server_id,
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).inspector_server_id,
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).inspector_server_id,
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).inspector_server_id,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn plugins(_: AnyServer): ?*ServePlugins {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).plugins,
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).plugins,
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).plugins,
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).plugins,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn getPlugins(_: AnyServer): PluginsResult {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).getPlugins(),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).getPlugins(),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).getPlugins(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).getPlugins(),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn loadAndResolvePlugins(_: AnyServer, bundle: *HTMLBundle.HTMLBundleRoute, raw_plugins: []const []const u8, bunfig_path: []const u8): void {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).getPluginsAsync(bundle, raw_plugins, bunfig_path),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).getPluginsAsync(bundle, raw_plugins, bunfig_path),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).getPluginsAsync(bundle, raw_plugins, bunfig_path),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).getPluginsAsync(bundle, raw_plugins, bunfig_path),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn getOrLoadPlugins(server: AnyServer, callback: ServePlugins.Callback): ServePlugins.GetOrStartLoadResult {
        return switch (server.ptr.tag()) {
            Ptr.case(HTTPServer) => server.ptr.as(HTTPServer).getOrLoadPlugins(callback),
            Ptr.case(HTTPSServer) => server.ptr.as(HTTPSServer).getOrLoadPlugins(callback),
            Ptr.case(DebugHTTPServer) => server.ptr.as(DebugHTTPServer).getOrLoadPlugins(callback),
            Ptr.case(DebugHTTPSServer) => server.ptr.as(DebugHTTPSServer).getOrLoadPlugins(callback),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn reloadStaticRoutes(_: AnyServer): !bool {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).reloadStaticRoutes(),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).reloadStaticRoutes(),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).reloadStaticRoutes(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).reloadStaticRoutes(),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn appendStaticRoute(_: AnyServer, path: []const u8, route: AnyRoute, method: HTTP.Method.Optional): !void {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).appendStaticRoute(path, route, method),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).appendStaticRoute(path, route, method),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).appendStaticRoute(path, route, method),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).appendStaticRoute(path, route, method),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn globalThis(_: AnyServer): *jsc.JSGlobalObject {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).globalThis,
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).globalThis,
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).globalThis,
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).globalThis,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn config(_: AnyServer): *const ServerConfig {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => &_.ptr.as(HTTPServer).config,
            Ptr.case(HTTPSServer) => &_.ptr.as(HTTPSServer).config,
            Ptr.case(DebugHTTPServer) => &_.ptr.as(DebugHTTPServer).config,
            Ptr.case(DebugHTTPSServer) => &_.ptr.as(DebugHTTPSServer).config,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn webSocketHandler(_: AnyServer): ?*WebSocketServerContext.Handler {
        const server_config: *ServerConfig = switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => &_.ptr.as(HTTPServer).config,
            Ptr.case(HTTPSServer) => &_.ptr.as(HTTPSServer).config,
            Ptr.case(DebugHTTPServer) => &_.ptr.as(DebugHTTPServer).config,
            Ptr.case(DebugHTTPSServer) => &_.ptr.as(DebugHTTPSServer).config,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
        if (server_config.websocket == null) return null;
        return &server_config.websocket.?.handler;
    }

    pub fn onRequest(
        _: AnyServer,
        req: *uws.Request,
        resp: bun.uws.AnyResponse,
    ): void {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).onRequest(req, resp.assertNoSSL()),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).onRequest(req, resp.assertSSL()),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).onRequest(req, resp.assertNoSSL()),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).onRequest(req, resp.assertSSL()),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn from(server: anytype): AnyServer {
        return .{ .ptr = Ptr.init(server) };
    }

    pub fn onPendingRequest(_: AnyServer): void {
        switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).onPendingRequest(),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).onPendingRequest(),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).onPendingRequest(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).onPendingRequest(),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        }
    }

    pub fn onRequestComplete(_: AnyServer): void {
        switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).onRequestComplete(),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).onRequestComplete(),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).onRequestComplete(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).onRequestComplete(),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        }
    }

    pub fn onStaticRequestComplete(_: AnyServer): void {
        switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).onStaticRequestComplete(),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).onStaticRequestComplete(),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).onStaticRequestComplete(),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).onStaticRequestComplete(),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        }
    }

    pub fn publish(_: AnyServer, topic: []const u8, message: []const u8, opcode: uws.Opcode, compress: bool): bool {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).app.?.publish(topic, message, opcode, compress),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).app.?.publish(topic, message, opcode, compress),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).app.?.publish(topic, message, opcode, compress),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).app.?.publish(topic, message, opcode, compress),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn onSavedRequest(
        _: AnyServer,
        req: SavedRequest.Union,
        resp: uws.AnyResponse,
        callback: jsc.JSValue,
        comptime extra_arg_count: usize,
        extra_args: [extra_arg_count]JSValue,
    ): void {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).onSavedRequest(req, resp.TCP, callback, extra_arg_count, extra_args),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).onSavedRequest(req, resp.SSL, callback, extra_arg_count, extra_args),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).onSavedRequest(req, resp.TCP, callback, extra_arg_count, extra_args),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).onSavedRequest(req, resp.SSL, callback, extra_arg_count, extra_args),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn prepareAndSaveJsRequestContext(
        server: AnyServer,
        req: *uws.Request,
        resp: uws.AnyResponse,
        global: *jsc.JSGlobalObject,
        method: ?bun.http.Method,
    ): bun.JSError!?SavedRequest {
        return switch (server.ptr.tag()) {
            Ptr.case(HTTPServer) => (server.ptr.as(HTTPServer).prepareJsRequestContext(req, resp.TCP, null, .bake, method) orelse return null).save(global, req, resp.TCP),
            Ptr.case(HTTPSServer) => (server.ptr.as(HTTPSServer).prepareJsRequestContext(req, resp.SSL, null, .bake, method) orelse return null).save(global, req, resp.SSL),
            Ptr.case(DebugHTTPServer) => (server.ptr.as(DebugHTTPServer).prepareJsRequestContext(req, resp.TCP, null, .bake, method) orelse return null).save(global, req, resp.TCP),
            Ptr.case(DebugHTTPSServer) => (server.ptr.as(DebugHTTPSServer).prepareJsRequestContext(req, resp.SSL, null, .bake, method) orelse return null).save(global, req, resp.SSL),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn numSubscribers(_: AnyServer, topic: []const u8): u32 {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).app.?.numSubscribers(topic),
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).app.?.numSubscribers(topic),
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).app.?.numSubscribers(topic),
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).app.?.numSubscribers(topic),
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }

    pub fn devServer(_: AnyServer): ?*bun.bake.DevServer {
        return switch (_.ptr.tag()) {
            Ptr.case(HTTPServer) => _.ptr.as(HTTPServer).dev_server,
            Ptr.case(HTTPSServer) => _.ptr.as(HTTPSServer).dev_server,
            Ptr.case(DebugHTTPServer) => _.ptr.as(DebugHTTPServer).dev_server,
            Ptr.case(DebugHTTPSServer) => _.ptr.as(DebugHTTPSServer).dev_server,
            else => bun.unreachablePanic("Invalid pointer tag", .{}),
        };
    }
}

pub export fn Server__setIdleTimeout(server: jsc.JSValue, seconds: jsc.JSValue, globalThis: *jsc.JSGlobalObject): void {
    Server__setIdleTimeout_(server, seconds, globalThis) catch |err| switch (err) {
        error.JSError => {},
        error.OutOfMemory => {
            _ = globalThis.throwOutOfMemoryValue();
        },
        error.JSTerminated => {},
    };
}

pub fn Server__setIdleTimeout_(server: jsc.JSValue, seconds: jsc.JSValue, globalThis: *jsc.JSGlobalObject): bun.JSError!void {
    if (!server.isObject()) {
        return globalThis.throw("Failed to set timeout: The 'this' value is not a Server.", .{});
    }

    if (!seconds.isNumber()) {
        return globalThis.throw("Failed to set timeout: The provided value is not of type 'number'.", .{});
    }
    const value = seconds.to(c_uint);
    if (server.as(HTTPServer)) |this| {
        this.setIdleTimeout(value);
    } else if (server.as(HTTPSServer)) |this| {
        this.setIdleTimeout(value);
    } else if (server.as(DebugHTTPServer)) |this| {
        this.setIdleTimeout(value);
    } else if (server.as(DebugHTTPSServer)) |this| {
        this.setIdleTimeout(value);
    } else {
        return globalThis.throw("Failed to set timeout: The 'this' value is not a Server.", .{});
    }
}

pub fn Server__setOnClientError_(globalThis: *jsc.JSGlobalObject, server: jsc.JSValue, callback: jsc.JSValue): bun.JSError!jsc.JSValue {
    if (!server.isObject()) {
        return globalThis.throw("Failed to set clientError: The 'this' value is not a Server.", .{});
    }

    if (!callback.isFunction()) {
        return globalThis.throw("Failed to set clientError: The provided value is not a function.", .{});
    }

    if (server.as(HTTPServer)) |this| {
        if (this.app) |app| {
            this.on_clienterror.deinit();
            this.on_clienterror = jsc.Strong.Optional.create(callback, globalThis);
            app.onClientError(*HTTPServer, this, HTTPServer.onClientErrorCallback);
        }
    } else if (server.as(HTTPSServer)) |this| {
        if (this.app) |app| {
            this.on_clienterror.deinit();
            this.on_clienterror = jsc.Strong.Optional.create(callback, globalThis);
            app.onClientError(*HTTPSServer, this, HTTPSServer.onClientErrorCallback);
        }
    } else if (server.as(DebugHTTPServer)) |this| {
        if (this.app) |app| {
            this.on_clienterror.deinit();
            this.on_clienterror = jsc.Strong.Optional.create(callback, globalThis);
            app.onClientError(*DebugHTTPServer, this, DebugHTTPServer.onClientErrorCallback);
        }
    } else if (server.as(DebugHTTPSServer)) |this| {
        if (this.app) |app| {
            this.on_clienterror.deinit();
            this.on_clienterror = jsc.Strong.Optional.create(callback, globalThis);
            app.onClientError(*DebugHTTPSServer, this, DebugHTTPSServer.onClientErrorCallback);
        }
    } else {
        bun.debugAssert(false);
    }
    return .js_undefined;
}

pub fn Server__setAppFlags_(globalThis: *jsc.JSGlobalObject, server: jsc.JSValue, require_host_header: bool, use_strict_method_validation: bool): bun.JSError!jsc.JSValue {
    if (!server.isObject()) {
        return globalThis.throw("Failed to set requireHostHeader: The 'this' value is not a Server.", .{});
    }

    if (server.as(HTTPServer)) |this| {
        this.setFlags(require_host_header, use_strict_method_validation);
    } else if (server.as(HTTPSServer)) |this| {
        this.setFlags(require_host_header, use_strict_method_validation);
    } else if (server.as(DebugHTTPServer)) |this| {
        this.setFlags(require_host_header, use_strict_method_validation);
    } else if (server.as(DebugHTTPSServer)) |this| {
        this.setFlags(require_host_header, use_strict_method_validation);
    } else {
        return globalThis.throw("Failed to set timeout: The 'this' value is not a Server.", .{});
    }
    return .js_undefined;
}

pub fn Server__setMaxHTTPHeaderSize_(globalThis: *jsc.JSGlobalObject, server: jsc.JSValue, max_header_size: u64): bun.JSError!jsc.JSValue {
    if (!server.isObject()) {
        return globalThis.throw("Failed to set maxHeaderSize: The 'this' value is not a Server.", .{});
    }

    if (server.as(HTTPServer)) |this| {
        this.setMaxHTTPHeaderSize(max_header_size);
    } else if (server.as(HTTPSServer)) |this| {
        this.setMaxHTTPHeaderSize(max_header_size);
    } else if (server.as(DebugHTTPServer)) |this| {
        this.setMaxHTTPHeaderSize(max_header_size);
    } else if (server.as(DebugHTTPSServer)) |this| {
        this.setMaxHTTPHeaderSize(max_header_size);
    } else {
        return globalThis.throw("Failed to set maxHeaderSize: The 'this' value is not a Server.", .{});
    }
    return .js_undefined;
}

comptime {
    _ = Server__setIdleTimeout;
    _ = NodeHTTPResponse.create;
    @export(&jsc.host_fn.wrap4(Server__setAppFlags_), .{ .name = "Server__setAppFlags" });
    @export(&jsc.host_fn.wrap3(Server__setOnClientError_), .{ .name = "Server__setOnClientError" });
    @export(&jsc.host_fn.wrap3(Server__setMaxHTTPHeaderSize_), .{ .name = "Server__setMaxHTTPHeaderSize" });
}

extern fn NodeHTTPServer__onRequest_http(
    any_server: usize,
    globalThis: *jsc.JSGlobalObject,
    this: jsc.JSValue,
    callback: jsc.JSValue,
    methodString: jsc.JSValue,
    request: *uws.Request,
    response: *uws.NewApp(false).Response,
    upgrade_ctx: ?*uws.WebSocketUpgradeContext,
    node_response_ptr: *?*NodeHTTPResponse,
) jsc.JSValue;

extern fn NodeHTTPServer__onRequest_https(
    any_server: usize,
    globalThis: *jsc.JSGlobalObject,
    this: jsc.JSValue,
    callback: jsc.JSValue,
    methodString: jsc.JSValue,
    request: *uws.Request,
    response: *uws.NewApp(true).Response,
    upgrade_ctx: ?*uws.WebSocketUpgradeContext,
    node_response_ptr: *?*NodeHTTPResponse,
) jsc.JSValue;

extern fn Bun__createNodeHTTPServerSocketForClientError(bool, *anyopaque, *jsc.JSGlobalObject) jsc.JSValue;

extern "c" fn Bun__ServerRouteList__callRoute(
    globalObject: *jsc.JSGlobalObject,
    index: u32,
    requestPtr: *Request,
    serverObject: jsc.JSValue,
    routeListObject: jsc.JSValue,
    requestObject: *jsc.JSValue,
    req: *uws.Request,
) jsc.JSValue;

extern "c" fn Bun__ServerRouteList__callRouteH3(
    globalObject: *jsc.JSGlobalObject,
    index: u32,
    requestPtr: *Request,
    serverObject: jsc.JSValue,
    routeListObject: jsc.JSValue,
    requestObject: *jsc.JSValue,
    req: *anyopaque,
) jsc.JSValue;

extern "c" fn Bun__ServerRouteList__create(
    globalObject: *jsc.JSGlobalObject,
    callbacks: [*]jsc.JSValue,
    paths: [*]ZigString,
    pathsLength: usize,
) jsc.JSValue;

fn throwSSLErrorIfNecessary(globalThis: *jsc.JSGlobalObject): bool {
    const err_code = BoringSSL.ERR_get_error();
    if (err_code != 0) {
        defer BoringSSL.ERR_clear_error();
        globalThis.throwValue(jsc.API.Bun.Crypto.createCryptoError(globalThis, err_code)) catch {};
        return true;
    }

    return false;
}

extern fn NodeHTTP_assignOnNodeJSCompat(bool, *anyopaque): void;
extern fn NodeHTTP_setUsingCustomExpectHandler(bool, *anyopaque, bool): void;

const string = []const u8;

const Sys = @import("../../sys/sys.zig");
const options = @import("../../bundler/options.zig");
const std = @import("std");
const URL = @import("../../url/url.zig").URL;
const Allocator = std.mem.Allocator;

const Runtime = @import("../../js_parser/runtime.zig");
const Fallback = Runtime.Fallback;

const bun = @import("bun");
const Async = bun.Async;
const Environment = bun.Environment;
const Global = bun.Global;
const Output = bun.Output;
const Transpiler = bun.Transpiler;
const analytics = bun.analytics;
const assert = bun.assert;
const default_allocator = bun.default_allocator;
const js_printer = bun.js_printer;
const logger = bun.logger;
const strings = bun.strings;
const uws = bun.uws;
const BoringSSL = bun.BoringSSL.c;
const SocketAddress = bun.api.socket.SocketAddress;

const HTTP = bun.http;
const MimeType = HTTP.MimeType;

const jsc = bun.jsc;
const JSGlobalObject = bun.jsc.JSGlobalObject;
const JSPromise = bun.jsc.JSPromise;
const JSValue = bun.jsc.JSValue;
const Node = bun.jsc.Node;
const VM = bun.jsc.VM;
const VirtualMachine = jsc.VirtualMachine;
const ZigString = bun.jsc.ZigString;
const host_fn = jsc.host_fn;

const WebCore = bun.jsc.WebCore;
const Blob = jsc.WebCore.Blob;
const Fetch = WebCore.Fetch;
const Headers = WebCore.Headers;
const Request = WebCore.Request;
const Response = WebCore.Response;
