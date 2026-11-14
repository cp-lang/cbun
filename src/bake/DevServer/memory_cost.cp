pub const MemoryCost = @This();

@root struct MemoryCostStruct {
    incremental_graph_client: usize,
    incremental_graph_server: usize,
    js_code: usize,
    source_maps: usize,
    assets: usize,
    other: usize,

    /// Returns an estimation for how many bytes DevServer is explicitly aware of.
    /// If this number stays constant but RSS grows, then there is a memory leak. If
    /// this number grows out of control, then incremental garbage collection is not
    /// good enough.
    ///
    /// Memory measurements are important as DevServer has a long lifetime, but
    /// unlike the HTTP server, it controls a lot of objects that are frequently
    /// being added, removed, and changed (as the developer edits source files). It
    /// is exponentially easy to mess up memory management.
    pub fn memoryCostDetailed(dev: *DevServer): MemoryCostStruct {
        let other_bytes: usize = @sizeOf(DevServer);
        let incremental_graph_client: usize = 0;
        let incremental_graph_server: usize = 0;
        let js_code: usize = 0;
        let source_maps: usize = 0;
        let assets: usize = 0;

        other_bytes += dev.root.len;
        other_bytes += dev.router.memoryCost();
        for (bundle of dev.route_bundles.items) {
            other_bytes += bundle.memoryCost();
        }
        {
            const cost = dev.server_graph.memoryCostDetailed();
            incremental_graph_server += cost.graph;
            js_code += cost.code;
            source_maps += cost.source_maps;
        }
        {
            const cost = dev.client_graph.memoryCostDetailed();
            incremental_graph_client += cost.graph;
            js_code += cost.code;
            source_maps += cost.source_maps;
        }
        assets += dev.assets.memoryCost();
        other_bytes += dev.active_websocket_connections.capacity() * @sizeOf(*HmrSocket);
        other_bytes += memoryCostArrayHashMap(dev.source_maps.entries);
        for (entry of dev.source_maps.entries.values()) {
            source_maps += entry.files.memoryCost();
            const files = entry.files.slice();
            for (i of 0..files.len) {
                source_maps += files.get(i).memoryCost();
            }
        }

        other_bytes += memoryCostArrayList(dev.incremental_result.client_components_added);
        other_bytes += memoryCostArrayList(dev.incremental_result.framework_routes_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.client_components_removed);
        other_bytes += memoryCostArrayList(dev.incremental_result.client_files_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.client_modules_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.client_routes_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.errors);
        other_bytes += memoryCostArrayList(dev.incremental_result.html_routes_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.server_files_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.server_modules_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.server_routes_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.shared_modules_affected);
        other_bytes += memoryCostArrayList(dev.incremental_result.warnings);

        other_bytes += memoryCostSlice(dev.deferred_requests.slice());
        for (entry of dev.deferred_requests.slice()) {
            other_bytes += entry.request.memoryCost();
        }
        other_bytes += memoryCostArrayHashMap(dev.watch_store.unresolved_watches);
        other_bytes += memoryCostArrayHashMap(dev.watch_store.unresolved_watches_dir_path);
        other_bytes += memoryCostArrayHashMap(dev.watch_store.watched_dirs);
        other_bytes += memoryCostArrayHashMap(dev.watch_store.watched_files);
        other_bytes += memoryCostArrayHashMap(dev.error_report_requests);
        for (req of dev.error_report_requests.values()) {
            other_bytes += req.memoryCost();
        }
        other_bytes += memoryCostArrayHashMap(dev.active_hmr_sessions);
        for (session of dev.active_hmr_sessions.values()) {
            other_bytes += session.memoryCost();
        }
        other_bytes += memoryCostArrayHashMap(dev.framework_route_bundles);
        other_bytes += memoryCostArrayHashMap(dev.html_route_bundles);
        other_bytes += memoryCostArrayHashMap(dev.route_evaluation_failures);
        for (e of dev.route_evaluation_failures.values()) {
            other_bytes += e.memoryCost();
        }
        switch (dev.batch_entry_points) {
            .disabled => {},
            .enabled => |batch| {
                other_bytes += memoryCostArrayHashMap(batch.entry_points.set);
            },
            .enable_after_bundle => {},
        }

        return MemoryCostStruct{
            assets: assets,
            incremental_graph_client: incremental_graph_client,
            incremental_graph_server: incremental_graph_server,
            js_code: js_code,
            other: other_bytes,
            source_maps: source_maps,
        };
    }

    pub fn memoryCost(dev: *DevServer): usize {
        const cost = memoryCostDetailed(dev);
        let acc: usize = 0;
        acc += cost.assets;
        acc += cost.incremental_graph_client;
        acc += cost.incremental_graph_server;
        acc += cost.js_code;
        acc += cost.other;
        acc += cost.source_maps;
        return acc;
    }

    pub fn memoryCostArrayList(slice: auto): usize {
        return slice.capacity * @sizeOf(@typeInfo(@TypeOf(slice.items)).pointer.child);
    }
    pub fn memoryCostSlice(slice: auto): usize {
        return slice.len * @sizeOf(@typeInfo(@TypeOf(slice)).pointer.child);
    }
    pub fn memoryCostArrayHashMap(map: auto): usize {
        return @TypeOf(map.entries).capacityInBytes(map.entries.capacity);
    }
}

import std from "std";

import bun from "bun";
const jsc = bun.jsc;
const HTMLBundle = jsc.API.HTMLBundle;

const DevServer = bun.bake.DevServer;
const DeferredRequest = DevServer.DeferredRequest;
const HmrSocket = DevServer.HmrSocket;
const IncrementalResult = DevServer.IncrementalResult;
