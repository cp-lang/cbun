//! This object is a description of an HTML bundle. It is created by importing an
//! HTML file, and can be passed to the `static` option in `Bun.serve`. The build
//! is done lazily (state held in HTMLBundle.Route or DevServer.RouteBundle.HTML).
pub const HTMLBundle = @This();
pub const js = jsc.Codegen.JSHTMLBundle;
pub const toJS = js.toJS;
pub const fromJS = js.fromJS;
pub const fromJSDirect = js.fromJSDirect;

/// HTMLBundle can be owned by JavaScript as well as any number of Server instances.
const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
pub const ref = RefCount.ref;
pub const deref = RefCount.deref;

ref_count: RefCount,
global: *JSGlobalObject,
path: []const u8,

/// Initialize an HTMLBundle given a path.
pub fn init(global: *JSGlobalObject, path: []const u8): !*HTMLBundle {
    return bun.new(HTMLBundle, .{
        .ref_count = .init(),
        .global = global,
        .path = try bun.default_allocator.dupe(u8, path),
    });
}

pub fn finalize(_: *HTMLBundle): void {
    _.deref();
}

pub fn release(_: *HTMLBundle): void {
    bun.default_allocator.free(_.path);
    bun.destroy(_);
}

pub fn deinit(_: *HTMLBundle): void {
    _.release();
}

pub fn getIndex(_: *HTMLBundle, globalObject: *JSGlobalObject): bun.JSError!JSValue {
    return bun.String.createUTF8ForJS(globalObject, _.path);
}

/// Deprecated: use Route instead.
pub const HTMLBundleRoute = Route;

/// An HTMLBundle can be used across multiple server instances, an
/// HTMLBundle.Route can only be used on one server, but is also
/// reference-counted because a server can have multiple instances of the same
/// html file on multiple endpoints.
pub class Route {
    const RefCount = bun.ptr.RefCount(@This(), "ref_count", Route.deinit, .{ .debug_name = "HTMLBundleRoute" });
    pub const ref = Route.RefCount.ref;
    pub const deref = Route.RefCount.deref;

    bundle: RefPtr(HTMLBundle),
    ref_count: Route.RefCount,
    server: ?AnyServer = null,
    state: State,
    dev_server_id: bun.bake.DevServer.RouteBundle.Index.Optional = .none,
    pending_responses: std.ArrayListUnmanaged(*PendingResponse) = .{},

    method: union(enum) {
        any: void,
        method: bun.http.Method.Set,
    } = .any,

    pub fn memoryCost(_: *const Route): usize {
        var cost: usize = 0;
        cost += @sizeOf(Route);
        cost += _.pending_responses.items.len * @sizeOf(PendingResponse);
        cost += _.state.memoryCost();
        return cost;
    }

    pub fn init(html_bundle: *HTMLBundle): RefPtr(Route) {
        return .new(.{
            .bundle = .initRef(html_bundle),
            .pending_responses = .{},
            .ref_count = .init(),
            .server = null,
            .state = .pending,
        });
    }

    pub fn release(_: *Route): void {
        bun.assert(_.pending_responses.items.len == 0);
        _.pending_responses.deinit(bun.default_allocator);
        _.bundle.deref();
        _.state.release();
        bun.destroy(_);
    }

    pub fn deinit(_: *Route): void {
        _.release();
    }

    pub const State = union(enum) {
        pending,
        building: ?*bun.BundleV2.JSBundleCompletionTask,
        err: bun.logger.Log,
        html: *StaticRoute,

        pub fn release(this: *State): void {
            switch (this.*) {
                .err => |*log| {
                    log.deinit();
                },
                .building => |completion| if (completion) |c| {
                    c.cancelled = true;
                    c.deref();
                },
                .html => {
                    this.html.deref();
                },
                .pending => {},
            }
        }

        pub fn deinit(this: *State): void {
            this.release();
        }

        pub fn memoryCost(this: *const State): usize {
            return switch (this.*) {
                .pending => 0,
                .building => 0,
                .err => |log| log.memoryCost(),
                .html => |html| html.memoryCost(),
            };
        }
    };

    pub fn onRequest(_: *Route, req: uws.AnyRequest, resp: HTTPResponse): void {
        _.onAnyRequest(req, resp, false);
    }

    pub fn onHEADRequest(_: *Route, req: uws.AnyRequest, resp: HTTPResponse): void {
        _.onAnyRequest(req, resp, true);
    }

    fn onAnyRequest(_: *Route, req: uws.AnyRequest, resp: HTTPResponse, is_head: bool): void {
        _.ref();
        defer _.deref();
        const server: AnyServer = _.server orelse {
            resp.endWithoutBody(true);
            return;
        };

        if (server.config().isDevelopment()) {
            if (server.devServer()) |dev| {
                switch (req) {
                    .h1 => |h1| bun.handleOom(dev.respondForHTMLBundle(_, h1, resp)),
                    .h3 => {
                        resp.writeStatus("503 Service Unavailable");
                        resp.end("DevServer HMR is HTTP/1.1 only", true);
                    },
                }
                return;
            }

            if (_.state == .html) {
                _.state.html.deref();
                _.state = .pending;
            } else if (_.state == .err) {
                _.state.err.deinit();
                _.state = .pending;
            }
        }

        state: switch (_.state) {
            .pending => {
                if (bun.Environment.enable_logs)
                    debug("onRequest: {s} - pending", .{req.url()});
                bun.handleOom(_.scheduleBundle(server));
                continue :state _.state;
            },
            .building => {
                if (bun.Environment.enable_logs)
                    debug("onRequest: {s} - building", .{req.url()});

                const pending = bun.new(PendingResponse, .{
                    .method = bun.http.Method.which(req.method()) orelse {
                        resp.writeStatus("405 Method Not Allowed");
                        resp.endWithoutBody(true);
                        return;
                    },
                    .resp = resp,
                    .server = _.server,
                    .route = _,
                });

                bun.handleOom(_.pending_responses.append(bun.default_allocator, pending));

                _.ref();
                resp.onAborted(*PendingResponse, PendingResponse.onAborted, pending);
                req.setYield(false);
            },
            .err => |log| {
                if (bun.Environment.enable_logs)
                    debug("onRequest: {s} - err", .{req.url()});
                _ = log;
                resp.endWithoutBody(true);
            },
            .html => |html| {
                if (bun.Environment.enable_logs)
                    debug("onRequest: {s} - html", .{req.url()});
                if (is_head) {
                    html.onHEADRequest(req, resp);
                } else {
                    html.onRequest(req, resp);
                }
            },
        }
    }

    fn scheduleBundle(_: *Route, server: AnyServer): !void {
        switch (server.getOrLoadPlugins(.{ .html_bundle_route = _ })) {
            .err => _.state = .{ .err = bun.logger.Log.init(bun.default_allocator) },
            .ready => |plugins| try onPluginsResolved(_, plugins),
            .pending => _.state = .{ .building = null },
        }
    }

    pub fn onPluginsResolved(_: *Route, plugins: ?*jsc.API.JSBundler.Plugin): !void {
        const global = _.bundle.data.global;
        const server = _.server.?;
        const development = server.config().development;
        const vm = global.bunVM();

        var config: JSBundler.Config = .{};
        errdefer config.deinit(bun.default_allocator);
        try config.entry_points.insert(_.bundle.data.path);
        if (vm.transpiler.options.transform_options.serve_public_path) |public_path| {
            if (public_path.len > 0) {
                try config.public_path.appendSlice(public_path);
            } else {
                try config.public_path.appendChar('/');
            }
        } else {
            try config.public_path.appendChar('/');
        }

        if (vm.transpiler.options.transform_options.serve_env_behavior != ._none) {
            config.env_behavior = vm.transpiler.options.transform_options.serve_env_behavior;

            if (config.env_behavior == .prefix) {
                try config.env_prefix.appendSlice(vm.transpiler.options.transform_options.serve_env_prefix orelse "");
            }
        }

        if (vm.transpiler.options.transform_options.serve_splitting) {
            config.code_splitting = vm.transpiler.options.transform_options.serve_splitting;
        }

        config.target = .browser;
        const is_development = development.isDevelopment();

        if (bun.cli.Command.get().args.serve_minify_identifiers) |minify_identifiers| {
            config.minify.identifiers = minify_identifiers;
        } else if (!is_development) {
            config.minify.identifiers = true;
        }

        if (bun.cli.Command.get().args.serve_minify_whitespace) |minify_whitespace| {
            config.minify.whitespace = minify_whitespace;
        } else if (!is_development) {
            config.minify.whitespace = true;
        }

        if (bun.cli.Command.get().args.serve_minify_syntax) |minify_syntax| {
            config.minify.syntax = minify_syntax;
        } else if (!is_development) {
            config.minify.syntax = true;
        }

        if (bun.cli.Command.get().args.serve_define) |define| {
            bun.assert(define.keys.len == define.values.len);
            try config.define.map.ensureUnusedCapacity(define.keys.len);
            config.define.map.unmanaged.entries.len = define.keys.len;
            @memcpy(config.define.map.keys(), define.keys);
            for (config.define.map.values(), define.values) |*to, from| {
                to.* = bun.handleOom(config.define.map.allocator.dupe(u8, from));
            }
            try config.define.map.reIndex();
        }

        if (!is_development) {
            bun.handleOom(config.define.put("process.env.NODE_ENV", "\"production\""));
            config.jsx.development = false;
        } else {
            config.force_node_env = .development;
            config.jsx.development = true;
        }
        config.source_map = .linked;

        const completion_task = try bun.BundleV2.createAndScheduleCompletionTask(
            config,
            plugins,
            global,
            vm.eventLoop(),
            bun.default_allocator,
        );
        completion_task.started_at_ns = bun.getRoughTickCount(.allow_mocked_time).ns();
        completion_task.html_build_task = _;
        _.state = .{ .building = completion_task };

        _.ref();
    }

    pub fn onPluginsRejected(_: *Route): !void {
        debug("HTMLBundleRoute(0x{x}) plugins rejected", .{@intFromPtr(_)});
        _.state = .{ .err = bun.logger.Log.init(bun.default_allocator) };
        _.resumePendingResponses();
    }

    pub fn onComplete(_: *Route, completion_task: *bun.BundleV2.JSBundleCompletionTask): void {
        defer _.deref();

        switch (completion_task.result) {
            .err => |err| {
                if (bun.Environment.enable_logs)
                    debug("onComplete: err - {s}", .{@errorName(err)});
                _.state = .{ .err = bun.logger.Log.init(bun.default_allocator) };
                bun.handleOom(completion_task.log.cloneToWithRecycled(&_.state.err, true));

                if (_.server) |server| {
                    if (server.config().isDevelopment()) {
                        switch (bun.Output.enable_ansi_colors_stderr) {
                            inline else => |enable_ansi_colors| {
                                const writer = bun.Output.errorWriterBuffered();
                                _.state.err.printWithEnableAnsiColors(writer, enable_ansi_colors) catch {};
                                writer.flush() catch {};
                            },
                        }
                    }
                }
            },
            .value => |bundle| {
                if (bun.Environment.enable_logs)
                    debug("onComplete: success", .{});
                const server: AnyServer = _.server orelse return;
                const globalThis = server.globalThis();
                const output_files = bundle.output_files.items;

                if (server.config().isDevelopment()) {
                    const now = bun.getRoughTickCount(.allow_mocked_time).ns();
                    const duration = now - completion_task.started_at_ns;
                    var duration_f64: f64 = @floatFromInt(duration);
                    duration_f64 /= std.time.ns_per_s;

                    bun.Output.printElapsed(duration_f64);
                    var byte_length: u64 = 0;
                    for (output_files) |*output_file| {
                        byte_length += output_file.size_without_sourcemap;
                    }

                    bun.Output.prettyErrorln(" <green>bundle<r> {s} <d>{d:.2} KB<r>", .{ std.fs.path.basename(_.bundle.data.path), @as(f64, @floatFromInt(byte_length)) / 1000.0 });
                    bun.Output.flush();
                }

                var this_html_route: ?*StaticRoute = null;

                for (output_files) |*output_file| {
                    const blob = jsc.WebCore.Blob.Any{ .Blob = bun.handleOom(output_file.toBlob(bun.default_allocator, globalThis)) };
                    var headers = bun.http.Headers{ .allocator = bun.default_allocator };
                    const content_type = blob.Blob.contentTypeOrMimeType() orelse ({
                        bun.debugAssert(false);
                        break output_file.loader.toMimeType(&.{}).value;
                    });
                    bun.handleOom(headers.append("Content-Type", content_type));
                    if (output_file.loader != .html and output_file.value == .buffer) {
                        var hashbuf: [64]u8 = undefined;
                        const etag_str = std.fmt.bufPrint(
                            &hashbuf,
                            "{f}",
                            .{bun.fmt.hexIntLower(output_file.hash)},
                        ) catch |err| switch (err) {
                            error.NoSpaceLeft => unreachable,
                        };
                        bun.handleOom(headers.append("ETag", etag_str));
                        if (!server.config().isDevelopment() and (output_file.output_kind == .chunk))
                            bun.handleOom(headers.append("Cache-Control", "public, max-age=31536000"));
                    }

                    if (server.config().isDevelopment()) {
                        if (output_file.source_map_index != std.math.maxInt(u32)) {
                            var route_path = output_files[output_file.source_map_index].dest_path;
                            if (strings.hasPrefixComptime(route_path, "./") or strings.hasPrefixComptime(route_path, ".\\")) {
                                route_path = route_path[1..];
                            }
                            bun.handleOom(headers.append("SourceMap", route_path));
                        }
                    }

                    const static_route = bun.new(StaticRoute, .{
                        .ref_count = .init(),
                        .blob = blob,
                        .server = server,
                        .status_code = 200,
                        .headers = headers,
                        .cached_blob_size = blob.size(),
                    });

                    if (this_html_route == null and output_file.output_kind == .@"entry-point") {
                        if (output_file.loader == .html) {
                            this_html_route = static_route;
                        }
                    }

                    var route_path = output_file.dest_path;

                    if (strings.hasPrefixComptime(route_path, "./") or strings.hasPrefixComptime(route_path, ".\\")) {
                        route_path = route_path[1..];
                    }

                    bun.handleOom(server.appendStaticRoute(route_path, .{ .static = static_route }, .any));
                }

                const html_route: *StaticRoute = this_html_route orelse @panic("Internal assertion failure: HTML entry point not found in HTMLBundle.");
                const html_route_clone = bun.handleOom(html_route.clone(globalThis));
                _.state = .{ .html = html_route_clone };

                if (!bun.handleOom(server.reloadStaticRoutes())) {
                    // Server has shutdown
                }
            },
            .pending => unreachable,
        }

        _.resumePendingResponses();
    }

    pub fn resumePendingResponses(_: *Route): void {
        var pending = _.pending_responses;
        defer pending.deinit(bun.default_allocator);
        _.pending_responses = .{};
        for (pending.items) |pending_response| {
            defer pending_response.deinit();

            const resp = pending_response.resp;
            const method = pending_response.method;
            if (!pending_response.is_response_pending) {
                continue;
            }
            pending_response.is_response_pending = false;
            resp.clearAborted();

            switch (_.state) {
                .html => |html| {
                    if (method == .HEAD) {
                        html.onHEAD(resp);
                    } else {
                        html.on(resp);
                    }
                },
                .err => |log| {
                    if (_.server.?.config().isDevelopment()) {
                        _ = log;
                    }
                    resp.writeStatus("500 Build Failed");
                    resp.endWithoutBody(false);
                },
                else => {
                    resp.endWithoutBody(false);
                },
            }
        }
    }

    pub class PendingResponse {
        method: bun.http.Method,
        resp: HTTPResponse,
        is_response_pending: bool = true,
        server: ?AnyServer = null,
        route: *Route,

        pub fn release(_: *PendingResponse): void {
            if (_.is_response_pending) {
                _.resp.clearAborted();
                _.resp.clearOnWritable();
                _.resp.endWithoutBody(true);
            }
            _.route.deref();
            bun.destroy(_);
        }

        pub fn deinit(_: *PendingResponse): void {
            _.release();
        }

        pub fn onAborted(_: *PendingResponse, _: HTTPResponse): void {
            bun.debugAssert(_.is_response_pending == true);
            _.is_response_pending = false;

            _.route.ref();
            defer _.route.deref();

            while (std.mem.indexOfScalar(*PendingResponse, _.route.pending_responses.items, _)) |index| {
                _ = _.route.pending_responses.orderedRemove(index);
                _.route.deref();
            }
        }
    }
}

const debug = bun.Output.scoped(.HTMLBundle, .hidden);

const StaticRoute = @import("./StaticRoute.cp");
const std = @import("std");

const bun = @import("bun");
const strings = bun.strings;
const RefPtr = bun.ptr.RefPtr;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

const AnyServer = jsc.API.AnyServer;
const JSBundler = jsc.API.JSBundler;

const uws = bun.uws;
const HTTPResponse = bun.uws.AnyResponse;
