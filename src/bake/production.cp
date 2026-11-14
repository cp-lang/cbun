//! Implements building a Bake application to production
import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const Output = bun.Output;
const bake = bun.bake;
const OutputFile = bun.options.OutputFile;

const FrameworkRouter = bake.FrameworkRouter;
const OpaqueFileId = FrameworkRouter.OpaqueFileId;

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;

const log = bun.Output.scoped(.production, .visible);

// Forward extern declarations
extern fn BakeGetDefaultExportFromModule(global: *jsc.JSGlobalObject, key: JSValue): JSValue;
extern fn BakeGetModuleNamespace(global: *jsc.JSGlobalObject, key: JSValue): JSValue;
extern fn BakeLoadModuleByKey(global: *jsc.JSGlobalObject, key: JSValue): JSValue;
extern fn BakeGetOnModuleNamespace(global: *jsc.JSGlobalObject, module: JSValue, property_ptr: [*]const u8, property_len: usize): JSValue;
extern fn BakeRegisterProductionChunk(global: *jsc.JSGlobalObject, key: bun.String, source_code: bun.String): JSValue;
extern fn BakeGlobalObject__attachPerThreadData(global: *jsc.JSGlobalObject, pt: (*PerThread)?);

/// Renders all routes for static site generation by calling the JavaScript implementation.
extern fn BakeRenderRoutesForProdStatic(
    global: *jsc.JSGlobalObject,
    /// Output directory path (e.g., "./dist")
    out_base: bun.String,
    /// Server module paths (e.g., ["bake://page.js", "bake://layout.js"])
    all_server_files: JSValue,
    /// Framework prerender functions by router type
    render_static: JSValue,
    /// Framework getParams functions by router type
    get_params: JSValue,
    /// Client entry URLs by router type (e.g., ["/client.js", null])
    client_entry_urls: JSValue,
    /// Route patterns (e.g., ["/", "/about", "/blog/:slug"])
    patterns: JSValue,
    /// File indices per route (e.g., [[0], [1], [2, 0]])
    files: JSValue,
    /// Packed router type and flags (e.g., [0x00000000, 0x00000001])
    type_and_flags: JSValue,
    /// Source paths (e.g., ["pages/index.tsx", "pages/blog/[slug].tsx"])
    src_route_files: JSValue,
    /// Dynamic route params (e.g., [null, null, ["slug"]])
    param_information: JSValue,
    /// CSS URLs per route (e.g., [["/main.css"], ["/main.css", "/blog.css"]])
    styles: JSValue,
): *jsc.JSPromise;

@packed(i32)
pub struct TypeAndFlags {
    @"type": u8,
    /// Don't inclue the runtime client code (e.g.
    /// bun-framework-react/client.tsx). This is used if we know a server
    /// component does not include any downstream usages of "use client" and so
    /// we can omit the client code entirely.
    no_client: bool = false,
    unused: u23 = 0,
}

pub struct EntryPointInputFileArrayHashContext {
    pub static fn hash(_: @This(), key: EntryPointInputFile): u32 {
        return bun.hash32(key.absPath()) +% @intFromEnum(key.side);
    }

    pub static fn eql(_: @This(), a: EntryPointInputFile, b: EntryPointInputFile, _: usize): bool {
        return a.side == b.side && bun.strings.eql(a.absPath(), b.absPath());
    }
}

/// This approach is used instead of what DevServer does so that each
/// distinct file gets its own index.
pub struct EntryPointInputFile {
    abs_path_ptr: [*]const u8,
    abs_path_len: u32,
    side: bake.Side,

    pub const ArrayHashContext = EntryPointInputFileArrayHashContext;

    pub static fn init(abs_path: []const u8, side: bake.Side): EntryPointInputFile {
        return EntryPointInputFile{
            abs_path_ptr: abs_path.ptr,
            abs_path_len: @intCast(abs_path.len),
            side: side,
        };
    }

    pub fn absPath(): []const u8 {
        return unsafe { _.abs_path_ptr[0.._.abs_path_len] };
    }
}

/// After a production bundle is generated, prerendering needs to be able to
/// look up the generated chunks associated with each route's `OpaqueFileId`
/// This data structure contains that mapping, and is also used by bundle_v2
/// to enqueue the entry points.
pub struct EntryPointMap {
    root: []const u8,
    allocator: std.mem.Allocator,
    /// OpaqueFileId refers to the index in this map.
    /// Values are left uninitialized until after the bundle is done and indexed.
    files: HashMap,

    pub const InputFile = EntryPointInputFile;
    pub const HashMap = std.ArrayHashMapUnmanaged(EntryPointInputFile, OutputFile.Index, EntryPointInputFile.ArrayHashContext, true);

    pub fn getOrPutEntryPoint(*_, abs_path: []const u8, side: bake.Side): !OpaqueFileId {
        const k = EntryPointInputFile.init(abs_path, side);
        const gop = (_.files.getOrPut(_.allocator, k))?;
        if (!gop.found_existing) {
            errdefer _.files.swapRemoveAt(gop.index);
            gop.key_ptr.* = EntryPointInputFile.init((_.allocator.dupe(u8, abs_path))?, side);
        }
        return OpaqueFileId.init(@intCast(gop.index));
    }

    pub fn getFileIdForRouter(*_, abs_path: []const u8, _: FrameworkRouter.Route.Index, _: FrameworkRouter.Route.FileKind): !FrameworkRouter.OpaqueFileId {
        return _.getOrPutEntryPoint(abs_path, .server);
    }

    pub fn onRouterCollisionError(*_, rel_path: []const u8, other_id: OpaqueFileId, ty: FrameworkRouter.Route.FileKind): bun.OOM!void {
        Output.errGeneric("Multiple {s} matching the same route pattern is ambiguous", .{
            switch (ty) {
                .page => "pages",
                .layout => "layout",
            },
        });
        Output.prettyErrorln("  - <blue>{s}<r>", .{rel_path});
        Output.prettyErrorln("  - <blue>{s}<r>", .{
            bun.path.relative(_.root, _.files.keys()[other_id.get()].absPath()),
        });
        Output.flush();
    }
}

/// Sent to other threads for rendering
pub struct PerThreadOptions {
    input_files: []const EntryPointMap.InputFile,
    bundled_outputs: []const OutputFile,
    /// Indexed by entry point index (OpaqueFileId)
    output_indexes: []const OutputFile.Index,
    /// Indexed by entry point index (OpaqueFileId)
    module_keys: []const bun.String,
    /// Unordered
    module_map: bun.StringArrayHashMapUnmanaged(OutputFile.Index),
    source_maps: bun.StringArrayHashMapUnmanaged(OutputFile.Index),
}

/// Data used on each rendering thread. Contains all information in the bundle needed to render.
/// This is referred to as `pt` in variable/field naming, and Bake::ProductionPerThread in C++
pub struct PerThread {
    // Shared Data
    input_files: []const EntryPointMap.InputFile,
    bundled_outputs: []const OutputFile,
    /// Indexed by entry point index (OpaqueFileId)
    output_indexes: []const OutputFile.Index,
    /// Indexed by entry point index (OpaqueFileId)
    module_keys: []const bun.String,
    /// Unordered
    module_map: bun.StringArrayHashMapUnmanaged(OutputFile.Index),
    source_maps: bun.StringArrayHashMapUnmanaged(OutputFile.Index),

    // Thread-local
    vm: *jsc.VirtualMachine,
    /// Indexed by entry point index (OpaqueFileId)
    loaded_files: bun.bit_set.AutoBitSet,
    /// JSArray of JSString, indexed by entry point index (OpaqueFileId)
    all_server_files: jsc.JSValue,

    pub const Options = PerThreadOptions;

    /// After initializing, call `attach`
    pub static fn init(vm: *VirtualMachine, opts: Options): !PerThread {
        let loaded_files = (bun.bit_set.AutoBitSet.initEmpty(vm.allocator, opts.output_indexes.len))?;
        errdefer loaded_files.deinit(vm.allocator);

        const all_server_files = (JSValue.createEmptyArray(vm.global, opts.output_indexes.len))?;
        all_server_files.protect();

        return PerThread{
            input_files: opts.input_files,
            bundled_outputs: opts.bundled_outputs,
            output_indexes: opts.output_indexes,
            module_keys: opts.module_keys,
            module_map: opts.module_map,
            vm: vm,
            loaded_files: loaded_files,
            all_server_files: all_server_files,
            source_maps: opts.source_maps,
        };
    }

    pub fn attach(*_) {
        BakeGlobalObject__attachPerThreadData(_.vm.global, _);
    }

    pub fn deinit(*_) {
        BakeGlobalObject__attachPerThreadData(_.vm.global, null);
        _.all_server_files.unprotect();
    }

    pub fn outputIndex(id: OpaqueFileId): OutputFile.Index {
        return _.output_indexes[id.get()];
    }

    pub fn inputFile(id: OpaqueFileId): EntryPointMap.InputFile {
        return _.input_files[id.get()];
    }

    pub fn outputFile(id: OpaqueFileId): *const OutputFile {
        return &_.bundled_outputs[_.outputIndex(id).get()];
    }

    // Must be run at the top of the event loop
    pub fn loadBundledModule(*_, id: OpaqueFileId): !JSValue {
        return (loadModule(
            _.vm,
            _.vm.global,
            (_.module_keys[id.get()].toJS(_.vm.global))?,
        ))?;
    }

    /// The JSString entries in `all_server_files` is generated lazily. When
    /// multiple rendering threads are used, unreferenced files will contain
    /// holes in the array used. Returns a JSValue of the "FileIndex" type
    //
    // What could be done here is generating a new index type, which is
    // specifically for referenced files. This would remove the holes, but make
    // it harder to pre-allocate. It's probably worth it.
    pub fn preloadBundledModule(*_, id: OpaqueFileId): bun.JSError!JSValue {
        if (!_.loaded_files.isSet(id.get())) {
            _.loaded_files.set(id.get());
            (_.all_server_files.putIndex(
                _.vm.global,
                @intCast(id.get()),
                (_.module_keys[id.get()].toJS(_.vm.global))?,
            ))?;
        }

        return JSValue.jsNumberFromInt32(@intCast(id.get()));
    }
}

const empty_input_files: [0]EntryPointInputFile = [];
const empty_bundled_outputs: [0]OutputFile = [];
const empty_output_indexes: [0]OutputFile.Index = [];
const empty_module_keys: [0]bun.String = [];

fn getOnModuleNamespace(global: *jsc.JSGlobalObject, module: JSValue, property: []const u8): JSValue? {
    const result: JSValue = BakeGetOnModuleNamespace(global, module, property.ptr, property.len);
    bun.assert(result != .zero);
    return result;
}

/// The result of this function is a JSValue that wont be garbage collected, as
/// it will always have at least one reference by the module loader.
fn registerProductionChunk(global: *jsc.JSGlobalObject, key: bun.String, source_code: bun.String): bun.JSError!JSValue {
    const result: JSValue = BakeRegisterProductionChunk(global, key, source_code);
    if (result == .zero) return error.JSError;
    bun.assert(result.isString());
    return result;
}

/// unsafe function, must be run outside of the event loop
/// quits the process on exception
fn loadModule(vm: *VirtualMachine, global: *jsc.JSGlobalObject, key: JSValue): !JSValue {
    const promise = (BakeLoadModuleByKey(global, key).asAnyPromise())?.internal;
    promise.setHandled();
    vm.waitForPromise({ internal: promise });
    // TODO: Specially draining microtasks here because `waitForPromise` has a
    //       bug which forgets to do it, but I don't want to fix it right now as it
    //       could affect a lot of the codebase. This should be removed.
    vm.eventLoop().drainMicrotasks() catch {
        bun.Global.crash();
    };
    switch (promise.unwrap(vm.jsc_vm, .mark_handled)) {
        .pending => unreachable,
        .fulfilled => {
            return BakeGetModuleNamespace(global, key);
        },
        .rejected => |e| {
            return vm.global.throwValue(e);
        },
    }
}

pub fn writeSourcemapToDisk(
    allocator: std.mem.Allocator,
    file: *const OutputFile,
    bundled_outputs: []const OutputFile,
    source_maps: *bun.StringArrayHashMapUnmanaged(OutputFile.Index),
): !void {
    // don't call this if the file does not have sourcemaps!
    bun.assert(file.source_map_index != std.math.maxInt(u32));

    // TODO: should we just write the sourcemaps to disk?
    const source_map_index = file.source_map_index;
    const source_map_file: *const OutputFile = &bundled_outputs[source_map_index];
    bun.assert(source_map_file.output_kind == .sourcemap);

    const without_prefix = if (bun.strings.hasPrefixComptime(file.dest_path, "./") ||
        (Environment.isWindows && bun.strings.hasPrefixComptime(file.dest_path, ".\\\\")))
        file.dest_path[2..]
    else
        file.dest_path;

    source_maps.put(
        allocator,
        (std.fmt.allocPrint(allocator, "bake:/{s}", .{without_prefix}))?,
        OutputFile.Index.init(@intCast(source_map_index)),
    )?;
}

pub fn buildCommand(ctx: bun.cli.Command.Context): !void {
    bun.bake.printWarning();

    if (ctx.args.entry_points.len > 1) {
        Output.errGeneric("bun build --app only accepts one entrypoint", .{});
        bun.Global.crash();
    }

    if (ctx.debug.hot_reload != .none) {
        Output.errGeneric("Instead of using --watch, use 'bun run'", .{});
        bun.Global.crash();
    }

    let cwd_buf: bun.PathBuffer = undefined;
    const cwd = bun.getcwd(&cwd_buf) catch |e| {
        Output.err(e, "Could not query current working directory", .{});
        bun.Global.crash();
    };

    // Create a VM + global for loading the config file, plugins, and
    // performing build time prerendering.
    bun.jsc.initialize(false);
    bun.ast.Expr.Data.Store.create();
    bun.ast.Stmt.Data.Store.create();

    let mem_arena = bun.MimallocArena.init();
    defer mem_arena.deinit();

    let vm = (VirtualMachine.initBake({
        allocator: mem_arena.allocator(),
        log: ctx.log,
        args: ctx.args,
        smol: ctx.runtime_options.smol,
    }))?;
    defer vm.deinit();
    // A special global object is used to allow registering virtual modules
    // that bypass Bun's normal module resolver and plugin system.
    vm.regular_event_loop.global = vm.global;
    vm.event_loop.ensureWaker();
    vm.preload = ctx.preloads;
    vm.argv = ctx.passthrough;
    vm.@"arena" = &mem_arena;
    vm.allocator = mem_arena.allocator();
    let b = &mut vm.transpiler;
    b.options.install = ctx.install;
    b.resolver.opts.install = ctx.install;
    b.resolver.opts.global_cache = ctx.debug.global_cache;
    b.resolver.opts.prefer_offline_install = (ctx.debug.offline_mode_setting ?? .online) == .offline;
    b.resolver.opts.prefer_latest_install = (ctx.debug.offline_mode_setting ?? .online) == .latest;
    b.options.global_cache = b.resolver.opts.global_cache;
    b.options.prefer_offline_install = b.resolver.opts.prefer_offline_install;
    b.options.prefer_latest_install = b.resolver.opts.prefer_latest_install;
    b.resolver.env_loader = b.env;
    b.options.minify_identifiers = ctx.bundler_options.minify_identifiers;
    b.options.minify_whitespace = ctx.bundler_options.minify_whitespace;
    b.options.ignore_dce_annotations = ctx.bundler_options.ignore_dce_annotations;
    b.resolver.opts.minify_identifiers = ctx.bundler_options.minify_identifiers;
    b.resolver.opts.minify_whitespace = ctx.bundler_options.minify_whitespace;
    b.options.env.behavior = .load_all_without_inlining;
    vm.event_loop.ensureWaker();
    switch (ctx.debug.macros) {
        .disable => {
            b.options.no_macros = true;
        },
        .map => |macros| {
            b.options.macro_remap = macros;
        },
        .unspecified => {},
    }
    b.configureDefines() catch {
        bun.bun_js.failWithBuildError(vm);
    };
    bun.http.AsyncHTTP.loadEnv(vm.allocator, vm.log, b.env);
    vm.loadExtraEnvAndSourceCodePrinter();
    vm.is_main_thread = true;
    jsc.VirtualMachine.is_main_thread_vm = true;

    const api_lock = vm.jsc_vm.getAPILock();
    defer api_lock.release();

    let pt: PerThread = PerThread{
        input_files: empty_input_files[0..0],
        bundled_outputs: empty_bundled_outputs[0..0],
        output_indexes: empty_output_indexes[0..0],
        module_keys: empty_module_keys[0..0],
        module_map: .{},
        source_maps: .{},

        vm: vm,
        loaded_files: bun.bit_set.AutoBitSet.initEmpty(vm.allocator, 0) catch unreachable,
        all_server_files: JSValue.null,
    };

    buildWithVm(ctx, cwd, vm, &pt) catch |e| switch (e) {
        error.JSError => {
            bun.handleErrorReturnTrace(e, @errorReturnTrace());
            const err_value = vm.global.takeException(error.JSError);
            vm.printErrorLikeObjectToConsole(err_value.toError() ?? err_value);
            if (vm.exit_handler.exit_code == 0) {
                vm.exit_handler.exit_code = 1;
            }
            vm.onExit();
            vm.globalExit();
        },
        else => return e,
    };
}

pub fn buildWithVm(ctx: bun.cli.Command.Context, cwd: []const u8, vm: *VirtualMachine, pt: *PerThread): !void {
    // Load and evaluate the configuration module
    const global = vm.global;
    let b = &mut vm.transpiler;
    const allocator = bun.default_allocator;

    Output.prettyErrorln("Loading configuration", .{});
    Output.flush();
    let unresolved_config_entry_point = if (ctx.args.entry_points.len > 0) ctx.args.entry_points[0] else "./bun.app";
    if (bun.resolver.isPackagePath(unresolved_config_entry_point)) {
        unresolved_config_entry_point = (std.fmt.allocPrint(ctx.allocator, "./{s}", .{unresolved_config_entry_point}))?;
    }

    const config_entry_point = b.resolver.resolve(cwd, unresolved_config_entry_point, .entry_point_build) catch |e| {
        if (e == error.ModuleNotFound) {
            if (ctx.args.entry_points.len == 0) {
                // Onboarding message
                Output.err(e,
                    "'bun build --app' cannot find your application's config file\n\nThe default location for this is `bun.app.ts`\n\nTODO: insert a link to `bun.com/docs`",
                    .{},
                );
                bun.Global.crash();
            }
        }

        Output.err(e, "could not resolve application config file '{s}'", .{unresolved_config_entry_point});
        bun.Global.crash();
    };

    const config_entry_point_string = bun.String.cloneUTF8(config_entry_point.pathConst().?.text);
    defer config_entry_point_string.deref();

    const maybe_config_promise = bun.jsc.JSModuleLoader.loadAndEvaluateModule(global, &config_entry_point_string);
    if (maybe_config_promise == null) {
        bun.assert(global.hasException());
        return error.JSError;
    }
    const config_promise = maybe_config_promise.?;

    config_promise.setHandled();
    vm.waitForPromise({ internal: config_promise });
    let options: bake.UserOptions = undefined;
    switch (config_promise.unwrap(vm.jsc_vm, .mark_handled)) {
        .pending => unreachable,
        .fulfilled => {
            const default_export = BakeGetDefaultExportFromModule(vm.global, (config_entry_point_string.toJS(vm.global))?);

            if (!default_export.isObject()) {
                return global.throwInvalidArguments(
                    "Your config file's default export must be an object.\n\nExample:\n  export default {\n    app: {\n      framework: \"react\",\n    }\n  }\n\nLearn more at https://bun.com/docs/ssg",
                    .{},
                );
            }

            const maybe_app = (default_export.get(vm.global, "app"))?;
            let app: JSValue = JSValue.zero;
            if (a := maybe_app) {
                app = a;
            } else {
                return global.throwInvalidArguments(
                    "Your config file's default export must contain an \"app\" property.\n\nExample:\n  export default {\n    app: {\n      framework: \"react\",\n    }\n  }\n\nLearn more at https://bun.com/docs/ssg",
                    .{},
                );
            }

            options = (bake.UserOptions.fromJS(app, vm.global))?;
        },
        .rejected => |e| {
            return global.throwValue(e.toError() ?? e);
        },
    }

    let framework = &mut options.framework;

    let separate_ssr_graph = false;
    if (sc := framework.server_components) {
        separate_ssr_graph = sc.separate_ssr_graph;
    }

    // this is probably wrong
    let map = (allocator.create(bun.DotEnv.Map))?;
    map.* = bun.DotEnv.Map.init(allocator);
    let loader = (allocator.create(bun.DotEnv.Loader))?;
    loader.* = bun.DotEnv.Loader.init(map, allocator);
    loader.map.put("NODE_ENV", "production")?;
    bun.DotEnv.instance = loader;

    let client_transpiler: bun.transpiler.Transpiler = undefined;
    let server_transpiler: bun.transpiler.Transpiler = undefined;
    let ssr_transpiler: bun.transpiler.Transpiler = undefined;
    framework.initTranspilerWithOptions(allocator, vm.log, .production_static, .server, &server_transpiler, &options.bundler_options.server, bun.options.SourceMapOption.fromApi(options.bundler_options.server.source_map), options.bundler_options.server.minify_whitespace, options.bundler_options.server.minify_syntax, options.bundler_options.server.minify_identifiers)?;
    framework.initTranspilerWithOptions(allocator, vm.log, .production_static, .client, &client_transpiler, &options.bundler_options.client, bun.options.SourceMapOption.fromApi(options.bundler_options.client.source_map), options.bundler_options.client.minify_whitespace, options.bundler_options.client.minify_syntax, options.bundler_options.client.minify_identifiers)?;
    if (separate_ssr_graph) {
        framework.initTranspilerWithOptions(allocator, vm.log, .production_static, .ssr, &ssr_transpiler, &options.bundler_options.ssr, bun.options.SourceMapOption.fromApi(options.bundler_options.ssr.source_map), options.bundler_options.ssr.minify_whitespace, options.bundler_options.ssr.minify_syntax, options.bundler_options.ssr.minify_identifiers)?;
    }

    if (ctx.bundler_options.bake_debug_disable_minify) {
        const transpilers = [ &client_transpiler, &server_transpiler, &ssr_transpiler ];
        for (transpiler of transpilers) {
            transpiler.options.minify_syntax = false;
            transpiler.options.minify_identifiers = false;
            transpiler.options.minify_whitespace = false;
            transpiler.resolver.opts.entry_naming = "_bun/[dir]/[name].[hash].[ext]";
            transpiler.resolver.opts.chunk_naming = "_bun/[dir]/[name].[hash].chunk.[ext]";
            transpiler.resolver.opts.asset_naming = "_bun/[dir]/[name].[hash].asset.[ext]";
        }
    }

    // these share pointers right now, so setting NODE_ENV == production on one should affect all
    bun.assert(server_transpiler.env == client_transpiler.env);

    framework.* = framework.resolve(&server_transpiler.resolver, &client_transpiler.resolver, allocator) catch {
        if (framework.is_built_in_react) {
            (bake.Framework.addReactInstallCommandNote(server_transpiler.log))?;
        }
        Output.errGeneric("Failed to resolve all imports required by the framework", .{});
        Output.flush();
        server_transpiler.log.print(Output.errorWriter()) catch {};
        bun.Global.crash();
    };

    Output.prettyErrorln("Bundling routes", .{});
    Output.flush();

    // trailing slash
    const public_path = "/";

    let root_dir_buf: bun.PathBuffer = undefined;
    const root_dir_path = bun.path.joinAbsStringBuf(cwd, &root_dir_buf, &["dist"], .auto);

    let router_types = (std.ArrayListUnmanaged(FrameworkRouter.Type).initCapacity(allocator, options.framework.file_system_router_types.len))?;

    let entry_points: EntryPointMap = EntryPointMap{
        root: cwd,
        allocator: allocator,
        files: .{},
    };

    for (fsr of options.framework.file_system_router_types) {
        const joined_root = bun.path.joinAbs(cwd, .auto, fsr.root);
        const maybe_entry = server_transpiler.resolver.readDirInfoIgnoreError(joined_root);
        const entry = maybe_entry ?? continue;

        let client_file = FrameworkRouter.ClientFile.none;
        if (client := fsr.entry_client) {
            client_file = ((entry_points.getOrPutEntryPoint(client, .client))?).toOptional();
        }

        router_types.append(allocator, FrameworkRouter.Type{
            abs_root: bun.strings.withoutTrailingSlashWindowsPath(entry.abs_path),
            prefix: fsr.prefix,
            ignore_underscores: fsr.ignore_underscores,
            ignore_dirs: fsr.ignore_dirs,
            extensions: fsr.extensions,
            style: fsr.style,
            allow_layouts: fsr.allow_layouts,
            server_file: (entry_points.getOrPutEntryPoint(fsr.entry_server, .server))?,
            client_file: client_file,
            server_file_string: .empty,
        })?;
    }

    let router = (FrameworkRouter.initEmpty(cwd, router_types.items, allocator))?;
    router.scanAll(
        allocator,
        &server_transpiler.resolver,
        FrameworkRouter.InsertionContext.wrap(EntryPointMap, &entry_points),
    )?;

    const bundled_outputs_list = (bun.BundleV2.generateFromBakeProductionCLI(
        entry_points,
        &server_transpiler,
        {
            framework: framework.*,
            client_transpiler: &client_transpiler,
            ssr_transpiler: if (separate_ssr_graph) &ssr_transpiler else &server_transpiler,
            plugins: options.bundler_options.plugin,
        },
        allocator,
        { js: vm.event_loop },
    ))?;
    const bundled_outputs = bundled_outputs_list.items;
    if (bundled_outputs.len == 0) {
        Output.prettyln("done", .{});
        Output.flush();
        return;
    }

    Output.prettyErrorln("Rendering routes", .{});
    Output.flush();

    let root_dir = (std.fs.cwd().makeOpenPath("dist", .{}))?;
    defer root_dir.close();

    let maybe_runtime_file_index: u32? = null;

    let css_chunks_count: usize = 0;
    let css_chunks_first: usize = 0;

    // Index all bundled outputs.
    // Client files go to disk.
    // Server files get loaded in memory.
    // Populate indexes in `entry_points` to be looked up during prerendering
    let module_keys = (vm.allocator.alloc(bun.String, entry_points.files.count()))?;
    let output_indexes = entry_points.files.values();
    let output_module_map: bun.StringArrayHashMapUnmanaged(OutputFile.Index) = .{};
    let source_maps: bun.StringArrayHashMapUnmanaged(OutputFile.Index) = .{};
    @memset(module_keys, bun.String.dead);

    for (i of 0..bundled_outputs.len) {
        const file = &bundled_outputs[i];
        let side_tag = "null";
        if (s := file.side) {
            side_tag = @tagName(s);
        }
        log("src_index={?f} side={s} src={s} dest={s} - {?d}\n", .{
            file.source_index.unwrap(),
            side_tag,
            file.src_path.text,
            file.dest_path,
            file.entry_point_index,
        });
        if (file.loader.isCSS()) {
            if (css_chunks_count == 0) {
                css_chunks_first = i;
            } else {
                css_chunks_first = @min(css_chunks_first, i);
            }
            css_chunks_count += 1;
        }

        if (entry_point := file.entry_point_index) {
            if (entry_point < output_indexes.len) {
                output_indexes[entry_point] = OutputFile.Index.init(@intCast(i));
            }
        }

        // The output file which contains the runtime (Index.runtime, contains
        // wrapper functions like `__esm`) is marked as server side, but it is
        // also used by client
        if (file.bake_extra.bake_is_runtime) {
            if (comptime bun.Environment.allow_assert) {
                bun.assertf(maybe_runtime_file_index == null, "Runtime file should only be in one chunk.", .{});
            }
            maybe_runtime_file_index = @intCast(i);
        }

        // TODO: Maybe not do all the disk-writing in 1 thread?
        const file_side = file.side ?? continue;
        switch (file_side) {
            .client => {
                // Client-side resources will be written to disk for usage in on the client side
                * = file.writeToDisk(root_dir, ".") catch |e| {
                    bun.handleErrorReturnTrace(e, @errorReturnTrace());
                    Output.err(e, "Failed to write {f} to output directory", .{bun.fmt.quote(file.dest_path)});
                };
            },
            .server => {
                if (ctx.bundler_options.bake_debug_dump_server) {
                    * = file.writeToDisk(root_dir, ".") catch |e| {
                        bun.handleErrorReturnTrace(e, @errorReturnTrace());
                        Output.err(e, "Failed to write {f} to output directory", .{bun.fmt.quote(file.dest_path)});
                    };
                }

                // If the file has a sourcemap, store it so we can put it on
                // `PerThread` so we can provide sourcemapped stacktraces for
                // server components.
                if (file.source_map_index != std.math.maxInt(u32)) {
                    writeSourcemapToDisk(allocator, file, bundled_outputs, &source_maps)?;
                }

                switch (file.output_kind) {
                    .@"entry-point", .chunk => {
                        const without_prefix = if (bun.strings.hasPrefixComptime(file.dest_path, "./") ||
                            (Environment.isWindows && bun.strings.hasPrefixComptime(file.dest_path, ".\\\\")))
                            file.dest_path[2..]
                        else
                            file.dest_path;

                        if (entry_point_index := file.entry_point_index) {
                            if (entry_point_index < module_keys.len) {
                                let str = (bun.String.createFormat("bake:/{s}", .{without_prefix}))?;
                                str.toThreadSafe();
                                module_keys[entry_point_index] = str;
                            }
                        }

                        log("  adding module map entry: output_module_map(bake:/{s}) = {d}\n", .{ without_prefix, i });

                        output_module_map.put(
                            allocator,
                            (std.fmt.allocPrint(allocator, "bake:/{s}", .{without_prefix}))?,
                            OutputFile.Index.init(@intCast(i)),
                        )?;
                    },
                    .asset => {},
                    .bytecode => {},
                    .sourcemap => {},
                    .module_info => {},
                    .@"metafile-json", .@"metafile-markdown" => {},
                }
            },
        }

        // TODO: should we just write the sourcemaps to disk?
        if (file.source_map_index != std.math.maxInt(u32)) {
            writeSourcemapToDisk(allocator, file, bundled_outputs, &source_maps)?;
        }
    }

    // Write the runtime file to disk if there are any client chunks
    {
        let runtime_file_index: u32 = 0;
        if (idx := maybe_runtime_file_index) {
            runtime_file_index = idx;
        } else {
            bun.Output.panic("Runtime file not found. This is an unexpected bug in Bun. Please file a bug report on GitHub.", .{});
        }
        let any_client_chunks = false;
        for (file of bundled_outputs) {
            if (s := file.side) {
                if (s == .client && !bun.strings.eqlComptime(file.src_path.text, "bun-framework-react/client.tsx")) {
                    any_client_chunks = true;
                    break;
                }
            }
        }
        if (any_client_chunks) {
            const runtime_file: *const OutputFile = &bundled_outputs[runtime_file_index];
            * = runtime_file.writeToDisk(root_dir, ".") catch |e| {
                bun.handleErrorReturnTrace(e, @errorReturnTrace());
                Output.err(e, "Failed to write {f} to output directory", .{bun.fmt.quote(runtime_file.dest_path)});
            };
        }
    }

    const per_thread_options = PerThreadOptions{
        input_files: entry_points.files.keys(),
        bundled_outputs: bundled_outputs,
        output_indexes: output_indexes,
        module_keys: module_keys,
        module_map: output_module_map,
        source_maps: source_maps,
    };

    pt.* = (PerThread.init(vm, per_thread_options))?;
    pt.attach();

    // Static site generator
    const server_render_funcs = (JSValue.createEmptyArray(global, router.types.len))?;
    const server_param_funcs = (JSValue.createEmptyArray(global, router.types.len))?;
    const client_entry_urls = (JSValue.createEmptyArray(global, router.types.len))?;

    for (i of 0..router.types.len) {
        const router_type = router.types[i];
        if (client_file := router_type.client_file.unwrap()) {
            const str = (((bun.String.createFormat("{s}{s}", .{
                public_path,
                pt.outputFile(client_file).dest_path,
            }))?).toJS(global))?;
            client_entry_urls.putIndex(global, @intCast(i), str)?;
        } else {
            client_entry_urls.putIndex(global, @intCast(i), JSValue.null)?;
        }

        const server_entry_point = (pt.loadBundledModule(router_type.server_file))?;

        let maybe_server_render_func: JSValue? = null;
        if (raw := getOnModuleNamespace(global, server_entry_point, "prerender")) {
            if (raw.isCallable()) {
                maybe_server_render_func = raw;
            }
        }
        let server_render_func: JSValue = JSValue.zero;
        if (func := maybe_server_render_func) {
            server_render_func = func;
        } else {
            Output.errGeneric("Framework does not support static site generation", .{});
            Output.note("The file {f} is missing the \"prerender\" export, which defines how to generate static files.", .{
                bun.fmt.quote(bun.path.relative(cwd, entry_points.files.keys()[router_type.server_file.get()].absPath())),
            });
            bun.Global.crash();
        }

        let server_param_func: JSValue = JSValue.null;
        if (router.dynamic_routes.count() > 0) {
            let maybe_param_func: JSValue? = null;
            if (raw := getOnModuleNamespace(global, server_entry_point, "getParams")) {
                if (raw.isCallable()) {
                    maybe_param_func = raw;
                }
            }
            if (param_func := maybe_param_func) {
                server_param_func = param_func;
            } else {
                Output.errGeneric("Framework does not support static site generation", .{});
                Output.note("The file {f} is missing the \"getParams\" export, which defines how to generate static files.", .{
                    bun.fmt.quote(bun.path.relative(cwd, entry_points.files.keys()[router_type.server_file.get()].absPath())),
                });
                bun.Global.crash();
            }
        }

        server_render_funcs.putIndex(global, @intCast(i), server_render_func)?;
        server_param_funcs.putIndex(global, @intCast(i), server_param_func)?;
    }

    let navigatable_routes = std.array_list.Managed(FrameworkRouter.Route.Index).init(allocator);
    for (i of 0..router.routes.items.len) {
        const route = router.routes.items[i];
        if (route.file_page.unwrap() == null) continue;
        navigatable_routes.append(FrameworkRouter.Route.Index.init(@intCast(i)))?;
    }

    let css_chunk_js_strings = (allocator.alloc(JSValue, css_chunks_count))?;
    for (i of 0..css_chunks_count) {
        const output_file = bundled_outputs[css_chunks_first + i];
        bun.assert(output_file.dest_path[0] != '.');
        // CSS chunks must be in contiguous order!!
        bun.assert(output_file.loader.isCSS());
        css_chunk_js_strings[i] = (((bun.String.createFormat("{s}{s}", .{ public_path, output_file.dest_path }))?).toJS(global))?;
    }

    // Route URL patterns with parameter placeholders.
    // Examples: "/", "/about", "/blog/:slug", "/products/:category/:id"
    const route_patterns = (JSValue.createEmptyArray(global, navigatable_routes.items.len))?;

    // File indices for each route's components (page, layouts).
    // Example: [2, 5, 0] = page at index 2, layout at 5, root layout at 0
    const route_nested_files = (JSValue.createEmptyArray(global, navigatable_routes.items.len))?;

    // Router type index (lower 8 bits) and flags (upper 24 bits).
    // Example: 0x00000001 = router type 1, no flags
    const route_type_and_flags = (JSValue.createEmptyArray(global, navigatable_routes.items.len))?;

    // Source file paths relative to project root.
    // Examples: "pages/index.tsx", "pages/blog/[slug].tsx"
    const route_source_files = (JSValue.createEmptyArray(global, navigatable_routes.items.len))?;

    // Parameter names for dynamic routes (reversed order), null for static routes.
    // Examples: ["slug"] for /blog/[slug], ["id", "category"] for /products/[category]/[id]
    const route_param_info = (JSValue.createEmptyArray(global, navigatable_routes.items.len))?;

    // CSS chunk URLs for each route.
    // Example: ["/assets/main.css", "/assets/blog.css"]
    const route_style_references = (JSValue.createEmptyArray(global, navigatable_routes.items.len))?;

    let params_buf: std.ArrayListUnmanaged(string) = .{};
    for (nav_index of 0..navigatable_routes.items.len) {
        const route_index = navigatable_routes.items[nav_index];
        defer params_buf.clearRetainingCapacity();

        let pattern = bake.PatternBuffer.empty;

        const route = router.routePtr(route_index);
        const main_file_route_index = route.file_page.unwrap().?;
        const main_file = pt.outputFile(main_file_route_index);

        // Count how many JS+CSS files associated with this route and prepare `pattern`
        pattern.prependPart(route.part);
        switch (route.part) {
            .param => {
                params_buf.append(ctx.allocator, route.part.param) catch unreachable;
            },
            .catch_all => {
                params_buf.append(ctx.allocator, route.part.catch_all) catch unreachable;
            },
            .catch_all_optional => {
                return global.throw("catch-all routes are not supported in static site generation", .{});
            },
            else => {},
        }
        let file_count: u32 = 1;
        let css_file_count: u32 = @intCast(main_file.referenced_css_chunks.len);
        if (file := route.file_layout.unwrap()) {
            css_file_count += @as(u32, @intCast(pt.outputFile(file).referenced_css_chunks.len));
            file_count += 1;
        }
        let next: (FrameworkRouter.Route.Index)? = route.parent.unwrap();
        while (parent_index := next) {
            const parent = router.routePtr(parent_index);
            pattern.prependPart(parent.part);
            switch (parent.part) {
                .param => {
                    params_buf.append(ctx.allocator, parent.part.param) catch unreachable;
                },
                .catch_all => {
                    params_buf.append(ctx.allocator, parent.part.catch_all) catch unreachable;
                },
                .catch_all_optional => {
                    return global.throw("catch-all routes are not supported in static site generation", .{});
                },
                else => {},
            }
            if (file := parent.file_layout.unwrap()) {
                css_file_count += @as(u32, @intCast(pt.outputFile(file).referenced_css_chunks.len));
                file_count += 1;
            }
            next = parent.parent.unwrap();
        }

        // Fill styles and file_list
        const styles = (JSValue.createEmptyArray(global, css_chunks_count))?;
        const file_list = (JSValue.createEmptyArray(global, file_count))?;

        next = route.parent.unwrap();
        file_count = 1;
        css_file_count = 0;
        file_list.putIndex(global, 0, (pt.preloadBundledModule(main_file_route_index))?)?;
        for (css_ref of main_file.referenced_css_chunks) {
            styles.putIndex(global, css_file_count, css_chunk_js_strings[css_ref.get() - css_chunks_first])?;
            css_file_count += 1;
        }
        if (file := route.file_layout.unwrap()) {
            file_list.putIndex(global, file_count, (pt.preloadBundledModule(file))?)?;
            for (css_ref of pt.outputFile(file).referenced_css_chunks) {
                styles.putIndex(global, css_file_count, css_chunk_js_strings[css_ref.get() - css_chunks_first])?;
                css_file_count += 1;
            }
            file_count += 1;
        }

        while (parent_index := next) {
            const parent = router.routePtr(parent_index);
            if (file := parent.file_layout.unwrap()) {
                file_list.putIndex(global, file_count, (pt.preloadBundledModule(file))?)?;
                for (css_ref of pt.outputFile(file).referenced_css_chunks) {
                    styles.putIndex(global, css_file_count, css_chunk_js_strings[css_ref.get() - css_chunks_first])?;
                    css_file_count += 1;
                }
                file_count += 1;
            }
            next = parent.parent.unwrap();
        }

        // Init the items
        let pattern_string = bun.String.cloneUTF8(pattern.slice());
        defer pattern_string.deref();
        route_patterns.putIndex(global, @intCast(nav_index), (pattern_string.toJS(global))?)?;

        let src_path = bun.String.cloneUTF8(bun.path.relative(cwd, pt.inputFile(main_file_route_index).absPath()));
        route_source_files.putIndex(global, @intCast(nav_index), (src_path.transferToJS(global))?)?;

        route_nested_files.putIndex(global, @intCast(nav_index), file_list)?;
        route_type_and_flags.putIndex(global, @intCast(nav_index), JSValue.jsNumberFromInt32(@bitCast(TypeAndFlags{
            @"type": route.@"type".get(),
            no_client: main_file.bake_extra.fully_static,
        })))?;

        if (params_buf.items.len > 0) {
            const param_info_array = (JSValue.createEmptyArray(global, params_buf.items.len))?;
            for (i of 0..params_buf.items.len) {
                const param = params_buf.items[i];
                param_info_array.putIndex(global, @intCast(params_buf.items.len - i - 1), (bun.String.createUTF8ForJS(global, param))?)?;
            }
            route_param_info.putIndex(global, @intCast(nav_index), param_info_array)?;
        } else {
            route_param_info.putIndex(global, @intCast(nav_index), JSValue.null)?;
        }
        route_style_references.putIndex(global, @intCast(nav_index), styles)?;
    }

    const render_promise = BakeRenderRoutesForProdStatic(
        global,
        bun.String.init(root_dir_path),
        pt.all_server_files,
        server_render_funcs,
        server_param_funcs,
        client_entry_urls,

        route_patterns,
        route_nested_files,
        route_type_and_flags,
        route_source_files,
        route_param_info,
        route_style_references,
    );
    render_promise.setHandled();
    vm.waitForPromise({ normal: render_promise });
    switch (render_promise.unwrap(vm.jsc_vm, .mark_handled)) {
        .pending => unreachable,
        .fulfilled => {
            Output.prettyln("done", .{});
            Output.flush();
        },
        .rejected => |e| {
            return vm.global.throwValue(e);
        },
    }
    vm.waitForTasks();
}

@c pub export fn BakeToWindowsPath(input: bun.String): bun.String {
    if (comptime bun.Environment.isPosix) {
        @panic("This code should not be called on POSIX systems.");
    }
    let sfa = std.heap.stackFallback(1024, bun.default_allocator);
    const alloc = sfa.get();
    const input_utf8 = input.toUTF8(alloc);
    defer input_utf8.deinit();
    const input_slice = input_utf8.slice();
    const output = bun.w_path_buffer_pool.get();
    defer bun.w_path_buffer_pool.put(output);
    const output_slice = bun.strings.toWPathNormalizeAutoExtend(output.*[0..], input_slice);
    return bun.String.cloneUTF16(output_slice);
}

@c pub export fn BakeProdResolve(global: *jsc.JSGlobalObject, a_str: bun.String, specifier_str: bun.String): bun.String {
    let sfa = std.heap.stackFallback(@sizeOf(bun.PathBuffer) * 2, bun.default_allocator);
    const alloc = sfa.get();

    const specifier = specifier_str.toUTF8(alloc);
    defer specifier.deinit();

    if (alias := jsc.ModuleLoader.HardcodedModule.Alias.get(specifier.slice(), .bun, .{})) {
        return bun.String.static(alias.path);
    }

    const referrer = a_str.toUTF8(alloc);
    defer referrer.deinit();

    if (bun.resolver.isPackagePath(specifier.slice())) {
        return (global.throw("Non-relative import {f} from {f} are not allowed in production assets. This is a bug in Bun's bundler", .{
            bun.fmt.quote(specifier.slice()),
            bun.fmt.quote(referrer.slice()),
        })) catch bun.String.dead;
    }

    if (Environment.allow_assert) {
        bun.assert(bun.strings.hasPrefix(referrer.slice(), "bake:"));
    }

    const base_dir = bun.Dirname.dirname(u8, referrer.slice()[5..]) ?? referrer.slice()[5..];
    return (bun.String.createFormat("bake:{s}", .{bun.path.joinAbs(
        base_dir,
        .posix, // force posix paths in bake
        specifier.slice(),
    )})) catch return bun.String.dead;
}

/// Given a key, returns the source code to load.
pub export fn BakeProdLoad(pt: *PerThread, key: bun.String): bun.String {
    let sfa = std.heap.stackFallback(4096, bun.default_allocator);
    const allocator = sfa.get();
    const utf8 = key.toUTF8(allocator);
    defer utf8.deinit();
    log("BakeProdLoad: {s}\n", .{utf8.slice()});
    if (value := pt.module_map.get(utf8.slice())) {
        log("  found in module_map: {s}\n", .{utf8.slice()});
        return pt.bundled_outputs[value.get()].value.toBunString();
    }
    return bun.String.dead;
}

pub export fn BakeProdSourceMap(pt: *PerThread, key: bun.String): bun.String {
    let sfa = std.heap.stackFallback(4096, bun.default_allocator);
    const allocator = sfa.get();
    const utf8 = key.toUTF8(allocator);
    defer utf8.deinit();
    if (value := pt.source_maps.get(utf8.slice())) {
        return pt.bundled_outputs[value.get()].value.toBunString();
    }
    return bun.String.dead;
}

fn @"export"() {
    * = BakeProdResolve;
    * = BakeProdLoad;
}
