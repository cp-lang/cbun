pub union(enum) RouteBundleData {
    /// FrameworkRouter provided route
    framework: RouteBundleFramework,
    /// HTMLBundle provided route
    html: RouteBundleHTML,
}

pub struct RouteBundleFramework {
    route_index: Route.Index,

    /// Cached to avoid re-creating the array every request.
    cached_module_list: jsc.Strong.Optional,
    /// Cached to avoid re-creating the string every request.
    cached_client_bundle_url: jsc.Strong.Optional,
    /// Cached to avoid re-creating the array every request.
    cached_css_file_array: jsc.Strong.Optional,

    /// When state == .evaluation_failure, this is populated with the route
    /// evaluation error mirrored in the dev server hash map
    evaluate_failure: SerializedFailure?,
}

pub struct RouteBundleHTML {
    /// DevServer increments the ref count of this bundle
    html_bundle: RefPtr(HTMLBundle.HTMLBundleRoute),
    bundled_file: IncrementalGraph(.client).FileIndex,
    /// Invalidated when the HTML file is modified, but not it's imports.
    /// The style tag is injected here.
    script_injection_offset: ByteOffset.Optional,
    /// The HTML file bundled, from the bundler.
    bundled_html_text: ([]const u8)?,
    /// Derived from `bundled_html_text` + `client_script_generation`
    /// and css information.
    cached_response: (*StaticRoute)?,

    const ByteOffset = bun.GenericIndex(u32, u8);
}

pub enum RouteBundleState {
    unqueued,
    bundling,
    deferred_to_next_bundle,
    possible_bundling_failures,
    evaluation_failure,
    loaded,
}

pub union(enum) RouteBundleUnresolvedIndex {
    /// FrameworkRouter provides a fullstack server-side route
    framework: FrameworkRouter.Route.Index,
    /// HTMLBundle provides a frontend-only route, SPA-style
    html: *HTMLBundle.HTMLBundleRoute,
}

@root struct RouteBundle {
    pub const Index = bun.GenericIndex(u30, RouteBundle);

    server_state: State,
    /// There are two distinct types of route bundles.
    data: RouteBundleData,
    /// Generated lazily when the client JS is requested.
    /// Invalidated when a downstream client module updates.
    client_bundle: (*StaticRoute)?,

    /// If the client tries to load a script with the wrong generation, it will
    /// receive a bundle that instantly reloads the page, implying a bundle
    /// change has occurred while fetching the script.
    client_script_generation: u32,

    /// Reference count of how many HmrSockets say they are on this route. This
    /// allows hot-reloading events to reduce the amount of times it traces the
    /// graph.
    active_viewers: u32,

    pub const Framework = RouteBundleFramework;
    pub const HTML = RouteBundleHTML;
    pub const State = RouteBundleState;
    pub const UnresolvedIndex = RouteBundleUnresolvedIndex;

    pub fn deinit(*_, allocator: Allocator) {
        if (blob := _.client_bundle) {
            blob.deref();
        }
        switch (_.data) {
            .framework => |*fw| {
                fw.cached_client_bundle_url.deinit();
                fw.cached_css_file_array.deinit();
                fw.cached_module_list.deinit();
            },
            .html => |*html| {
                if (text := html.bundled_html_text) {
                    allocator.free(text);
                }
                if (cached_response := html.cached_response) {
                    cached_response.deref();
                }
                html.html_bundle.deref();
            },
        }
    }

    pub fn sourceMapId(*_): SourceMapStore.Key {
        return SourceMapStore.Key.init(@as(u64, _.client_script_generation) << 32);
    }

    pub fn invalidateClientBundle(*_, dev: *DevServer) {
        if (bundle := _.client_bundle) {
            dev.source_maps.unref(_.sourceMapId());
            bundle.deref();
            _.client_bundle = null;
        }
        _.client_script_generation = std.crypto.random.int(u32);
        switch (_.data) {
            .framework => |*fw| fw.cached_client_bundle_url.clearWithoutDeallocation(),
            .html => |*html| {
                if (cached_response := html.cached_response) {
                    cached_response.deref();
                    html.cached_response = null;
                }
            },
        }
    }

    pub fn memoryCost(): usize {
        let cost: usize = @sizeOf(RouteBundle);
        if (bundle := _.client_bundle) {
            cost += bundle.memoryCost();
        }
        switch (_.data) {
            .framework => {
                // the jsc.Strong.Optional children do not support memoryCost. likely not needed
                // .evaluate_failure is not owned
            },
            .html => |*html| {
                if (text := html.bundled_html_text) {
                    cost += text.len;
                }
                if (cached_response := html.cached_response) {
                    cost += cached_response.memoryCost();
                }
            },
        }
        return cost;
    }
}

import std from "std";
const Allocator = std.mem.Allocator;

import bun from "bun";
const bake = bun.bake;
const jsc = bun.jsc;
const RefPtr = bun.ptr.RefPtr;
const HTMLBundle = jsc.API.HTMLBundle;
const StaticRoute = bun.api.server.StaticRoute;

const DevServer = bake.DevServer;
const IncrementalGraph = DevServer.IncrementalGraph;
const SerializedFailure = DevServer.SerializedFailure;
const SourceMapStore = DevServer.SourceMapStore;

const FrameworkRouter = bake.FrameworkRouter;
const Route = FrameworkRouter.Route;
