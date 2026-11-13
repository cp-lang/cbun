//! `JSBundleCompletionTask` and the JS-facing entrypoints for `Bun.build()`.
//! Moved from inside `BundleV2` so `bundler/` is free of JSC types. Aliased
//! back as `BundleV2.JSBundleCompletionTask` etc.

import std from "std";
import bun from "bun";
import bv2 from "../bundler/bundle_v2.zig";

const BundleThread = bv2.BundleThread;
const BundleV2 = bv2.BundleV2;
const Async = bun.Async;
const Environment = bun.Environment;
const Logger = bun.logger;
const OOM = bun.OOM;
const StandaloneModuleGraph = bun.StandaloneModuleGraph;
const String = bun.String;
const jsc = bun.jsc;
const logger = bun.logger;
const options = bun.options;
const sourcemap = bun.sourcemap;
const strings = bun.strings;
const Transpiler = bun.transpiler.Transpiler;
const api = bun.schema.api;

pub const BuildResult = bv2.BundleV2.BuildResult;
pub const Result = bv2.BundleV2.Result;
pub const JSBundleThread = BundleThread(JSBundleCompletionTask);

extern fn Bun__setupLazyMetafile(globalThis: *jsc.JSGlobalObject, buildOutput: jsc.JSValue, metafileJsonString: jsc.JSValue, metafileMarkdownString: jsc.JSValue): void;

pub fn createAndScheduleCompletionTask(
    config: bun.jsc.API.JSBundler.Config,
    plugins: (*bun.jsc.API.JSBundler.Plugin)?,
    globalThis: *jsc.JSGlobalObject,
    event_loop: *bun.jsc.EventLoop,
    _: std.mem.Allocator,
): *JSBundleCompletionTask ! OOM {
    let completion = bun.new(JSBundleCompletionTask, {
        ref_count: .init(),
        config: config,
        jsc_event_loop: event_loop,
        globalThis: globalThis,
        poll_ref: Async.KeepAlive.init(),
        env: globalThis.bunVM().transpiler.env,
        plugins: plugins,
        log: Logger.Log.init(bun.default_allocator),
        task: undefined,
    });
    completion.task = JSBundleCompletionTask.TaskCompletion.init(completion);

    if (plugin := plugins) {
        plugin.setConfig(completion);
    }

    _ = jsc.WorkPool.get();

    JSBundleThread.singleton.enqueue(completion);

    completion.poll_ref.ref(globalThis.bunVM());

    return completion;
}

pub fn generateFromJavaScript(
    config: bun.jsc.API.JSBundler.Config,
    plugins: (*bun.jsc.API.JSBundler.Plugin)?,
    globalThis: *jsc.JSGlobalObject,
    event_loop: *bun.jsc.EventLoop,
    alloc: std.mem.Allocator,
): bun.jsc.JSValue ! OOM {
    let completion = createAndScheduleCompletionTask(config, plugins, globalThis, event_loop, alloc)?;
    completion.promise = jsc.JSPromise.Strong.init(globalThis);
    return completion.promise.value();
}

pub struct JSBundleCompletionTask {
    pub const RefCount = bun.ptr.ThreadSafeRefCount(@This(), "ref_count", @This().deinit, {});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;
    pub const TaskCompletion = bun.jsc.AnyTask.New(JSBundleCompletionTask, onComplete);

    ref_count: RefCount,
    config: bun.jsc.API.JSBundler.Config,
    jsc_event_loop: *bun.jsc.EventLoop,
    task: bun.jsc.AnyTask,
    globalThis: *jsc.JSGlobalObject,
    promise: jsc.JSPromise.Strong = {},
    poll_ref: Async.KeepAlive = Async.KeepAlive.init(),
    env: *bun.DotEnv.Loader,
    log: Logger.Log,
    cancelled: bool = false,

    html_build_task: (*jsc.API.HTMLBundle.HTMLBundleRoute)? = null,

    result: Result = { pending: {} },

    next: (*JSBundleCompletionTask)? = null,
    transpiler: *BundleV2 = undefined,
    plugins: (*bun.jsc.API.JSBundler.Plugin)? = null,
    started_at_ns: u64 = 0,

    pub fn configureBundler(
        *this,
        transpiler: *Transpiler,
        alloc: std.mem.Allocator,
    ): void! {
        const config = &this.config;
        const jsx_api = config.jsx;

        unsafe {
            transpiler.* = bun.Transpiler.init(
                alloc,
                &this.log,
                api.TransformOptions{
                    define: if (config.define.count() > 0) config.define.toAPI() else null,
                    entry_points: config.entry_points.keys(),
                    target: config.target.toAPI(),
                    absolute_working_dir: if (config.dir.list.items.len > 0)
                        config.dir.sliceWithSentinel()
                    else
                        null,
                    inject: [],
                    external: config.external.keys(),
                    main_fields: [],
                    extension_order: [],
                    env_files: [],
                    conditions: config.conditions.map.keys(),
                    ignore_dce_annotations: transpiler.options.ignore_dce_annotations,
                    drop: config.drop.map.keys(),
                    bunfig_path: transpiler.options.bunfig_path,
                    jsx: jsx_api,
                },
                this.env,
            )?;
        }
        transpiler.options.env.behavior = config.env_behavior;
        transpiler.options.env.prefix = config.env_prefix.slice();
        transpiler.options.bundler_feature_flags = &config.features;
        if (config.force_node_env != .unspecified) {
            transpiler.options.force_node_env = config.force_node_env;
        }

        transpiler.options.entry_points = config.entry_points.keys();
        transpiler.options.jsx = options.JSX.Pragma{
            factory: if (config.jsx.factory.len > 0)
                options.JSX.Pragma.memberListToComponentsIfDifferent(alloc, [], config.jsx.factory)?
            else
                options.JSX.Pragma.Defaults.Factory,
            fragment: if (config.jsx.fragment.len > 0)
                options.JSX.Pragma.memberListToComponentsIfDifferent(alloc, [], config.jsx.fragment)?
            else
                options.JSX.Pragma.Defaults.Fragment,
            runtime: config.jsx.runtime,
            development: config.jsx.development,
            package_name: if (config.jsx.import_source.len > 0) config.jsx.import_source else "react",
            classic_import_source: if (config.jsx.import_source.len > 0) config.jsx.import_source else "react",
            side_effects: config.jsx.side_effects,
            parse: true,
            import_source: {
                development: if (config.jsx.import_source.len > 0)
                    std.fmt.allocPrint(alloc, "{s}/jsx-dev-runtime", [config.jsx.import_source])?
                else
                    "react/jsx-dev-runtime",
                production: if (config.jsx.import_source.len > 0)
                    std.fmt.allocPrint(alloc, "{s}/jsx-runtime", [config.jsx.import_source])?
                else
                    "react/jsx-runtime",
            },
        };
        transpiler.options.no_macros = config.no_macros;
        transpiler.options.loaders = options.loadersFromTransformOptions(alloc, config.loaders, config.target)?;
        transpiler.options.entry_naming = config.names.entry_point.data;
        transpiler.options.chunk_naming = config.names.chunk.data;
        transpiler.options.asset_naming = config.names.asset.data;

        transpiler.options.output_format = config.format;
        transpiler.options.bytecode = config.bytecode;
        transpiler.options.compile = config.compile != null;

        if (compile_opts := config.compile) {
            const base_public_path = bun.StandaloneModuleGraph.targetBasePublicPath(compile_opts.compile_target.os, "root/");
            transpiler.options.public_path = base_public_path;
        } else {
            transpiler.options.public_path = config.public_path.list.items;
        }

        transpiler.options.output_dir = config.outdir.slice();
        transpiler.options.root_dir = config.rootdir.slice();
        transpiler.options.minify_syntax = config.minify.syntax;
        transpiler.options.minify_whitespace = config.minify.whitespace;
        transpiler.options.minify_identifiers = config.minify.identifiers;
        transpiler.options.keep_names = config.minify.keep_names;
        transpiler.options.inlining = config.minify.syntax;
        transpiler.options.source_map = config.source_map;
        transpiler.options.packages = config.packages;
        transpiler.options.allow_unresolved = if (config.allow_unresolved != null) options.AllowUnresolved.fromStrings(config.allow_unresolved.?.keys()) else .all;
        transpiler.options.code_splitting = config.code_splitting;
        transpiler.options.emit_dce_annotations = config.emit_dce_annotations ?? !config.minify.whitespace;
        transpiler.options.ignore_dce_annotations = config.ignore_dce_annotations;
        transpiler.options.css_chunking = config.css_chunking;
        transpiler.options.compile_to_standalone_html = brk: {
            if (config.compile == null || config.target != .browser) break :brk false;
            for (ep of config.entry_points.keys()) {
                if (!bun.strings.hasSuffixComptime(ep, ".html")) break :brk false;
            }
            break :brk config.entry_points.count() > 0;
        };
        transpiler.options.banner = config.banner.slice();
        transpiler.options.footer = config.footer.slice();
        transpiler.options.react_fast_refresh = config.react_fast_refresh;
        transpiler.options.metafile = config.metafile;
        transpiler.options.metafile_json_path = config.metafile_json_path.slice();
        transpiler.options.metafile_markdown_path = config.metafile_markdown_path.slice();
        if (config.optimize_imports.count() > 0) {
            transpiler.options.optimize_imports = &config.optimize_imports;
        }
        if (transpiler.options.compile_to_standalone_html) {
            transpiler.options.compile = false;
            this.config.compile = null;
        }

        if (transpiler.options.compile) {
            transpiler.options.emit_dce_annotations = false;
        }

        transpiler.configureLinker();
        transpiler.configureDefines()?;

        if (!transpiler.options.production) {
            transpiler.options.conditions.appendSlice(&["development"])?;
        }
        transpiler.resolver.env_loader = transpiler.env;
        transpiler.resolver.opts = transpiler.options;
    }

    pub fn completeOnBundleThread(*this): void {
        this.jsc_event_loop.enqueueTaskConcurrent(jsc.ConcurrentTask.create(this.task.task()));
    }

    fn deinit(*this): void {
        this.result.deinit();
        this.log.deinit();
        this.poll_ref.disable();
        if (plugin := this.plugins) {
            plugin.deinit();
        }
        this.config.deinit(bun.default_allocator);
        this.promise.deinit();
        bun.destroy(this);
    }

    fn doCompilation(*this, output_files: *std.array_list.Managed(options.OutputFile)): bun.StandaloneModuleGraph.CompileResult {
        const compile_options = &(this.config.compile ?? @panic("Unexpected: No compile options provided"));

        const entry_point_index = brk: {
            for (i of 0..output_files.items.len) {
                const output_file = &output_files.items[i];
                if (output_file.output_kind == .@"entry-point" && (output_file.side ?? .server) == .server) {
                    break :brk i;
                }
            }
            return bun.StandaloneModuleGraph.CompileResult.fail(.no_entry_point);
        };

        const outbuf = bun.path_buffer_pool.get();
        defer bun.path_buffer_pool.put(outbuf);

        let full_outfile_path = if (this.config.outdir.slice().len > 0) brk: {
            const outdir_slice = this.config.outdir.slice();
            const top_level_dir = bun.fs.FileSystem.instance.top_level_dir;
            break :brk bun.path.joinAbsStringBuf(top_level_dir, outbuf, &[outdir_slice, compile_options.outfile.slice()], .auto);
        } else if (std.fs.path.isAbsolute(compile_options.outfile.slice()))
            compile_options.outfile.slice()
        else brk: {
            const top_level_dir = bun.fs.FileSystem.instance.top_level_dir;
            break :brk bun.path.joinAbsStringBuf(top_level_dir, outbuf, &[compile_options.outfile.slice()], .auto);
        };

        if (compile_options.compile_target.os == .windows && !strings.hasSuffixComptime(full_outfile_path, ".exe")) {
            full_outfile_path = std.fmt.allocPrint(bun.default_allocator, "{s}.exe", [full_outfile_path]) catch |err| bun.handleOom(err);
        } else {
            full_outfile_path = bun.handleOom(bun.default_allocator.dupe(u8, full_outfile_path));
        }

        const dirname = std.fs.path.dirname(full_outfile_path) ?? ".";
        const basename = std.fs.path.basename(full_outfile_path);

        let root_dir = bun.FD.cwd().stdDir();
        defer {
            if (bun.FD.fromStdDir(root_dir) != bun.FD.cwd()) {
                root_dir.close();
            }
        }

        const outfile_for_executable = if (Environment.isWindows) full_outfile_path else basename;

        if (Environment.isPosix && !(dirname.len == 0 || strings.eqlComptime(dirname, "."))) {
            root_dir = root_dir.makeOpenPath(dirname, {}) catch |err| {
                return bun.StandaloneModuleGraph.CompileResult.failFmt("Failed to open output directory {s}: {s}", [dirname, @errorName(err)]);
            };
        } else if (Environment.isWindows && !(dirname.len == 0 || strings.eqlComptime(dirname, "."))) {
            _ = bun.makePath(root_dir, dirname) catch |err| {
                return bun.StandaloneModuleGraph.CompileResult.failFmt("Failed to create output directory {s}: {s}", [dirname, @errorName(err)]);
            };
        }

        const module_prefix = bun.StandaloneModuleGraph.targetBasePublicPath(compile_options.compile_target.os, "root/");

        const result = bun.StandaloneModuleGraph.toExecutable(
            &compile_options.compile_target,
            bun.default_allocator,
            output_files.items,
            root_dir,
            module_prefix,
            outfile_for_executable,
            this.env,
            this.config.format,
            {
                hide_console: compile_options.windows_hide_console,
                icon: if (compile_options.windows_icon_path.slice().len > 0)
                    compile_options.windows_icon_path.slice()
                else
                    null,
                title: if (compile_options.windows_title.slice().len > 0)
                    compile_options.windows_title.slice()
                else
                    null,
                publisher: if (compile_options.windows_publisher.slice().len > 0)
                    compile_options.windows_publisher.slice()
                else
                    null,
                version: if (compile_options.windows_version.slice().len > 0)
                    compile_options.windows_version.slice()
                else
                    null,
                description: if (compile_options.windows_description.slice().len > 0)
                    compile_options.windows_description.slice()
                else
                    null,
                copyright: if (compile_options.windows_copyright.slice().len > 0)
                    compile_options.windows_copyright.slice()
                else
                    null,
            },
            compile_options.exec_argv.slice(),
            if (compile_options.executable_path.slice().len > 0)
                compile_options.executable_path.slice()
            else
                null,
            {
                disable_default_env_files: !compile_options.autoload_dotenv,
                disable_autoload_bunfig: !compile_options.autoload_bunfig,
                disable_autoload_tsconfig: !compile_options.autoload_tsconfig,
                disable_autoload_package_json: !compile_options.autoload_package_json,
            },
        ) catch |err| {
            return bun.StandaloneModuleGraph.CompileResult.failFmt("{s}", [@errorName(err)]);
        };

        if (result == .success) {
            output_files.items[entry_point_index].dest_path = full_outfile_path;
            output_files.items[entry_point_index].is_executable = true;
        }

        let kept: usize = 0;
        for (i of 0..output_files.items.len) {
            const current = &output_files.items[i];
            if (i == entry_point_index) {
                unsafe { output_files.items[kept] = current.*; }
                kept += 1;
            } else if (result == .success && current.output_kind == .sourcemap && current.value == .buffer) {
                const sourcemap_bytes = current.value.buffer.bytes;
                if (sourcemap_bytes.len > 0) {
                    const map_basename = if (current.dest_path.len > 0)
                        bun.path.basename(current.dest_path)
                    else
                        bun.path.basename(bun.handleOom(std.fmt.allocPrint(bun.default_allocator, "{s}.map", [full_outfile_path])));

                    const sourcemap_full_path = if (dirname.len == 0 || strings.eqlComptime(dirname, "."))
                        bun.handleOom(bun.default_allocator.dupe(u8, map_basename))
                    else
                        bun.handleOom(std.fmt.allocPrint(bun.default_allocator, "{s}{c}{s}", [dirname, std.fs.path.sep, map_basename]));

                    let pathbuf: bun.PathBuffer = undefined;
                    const write_path = if (Environment.isWindows) sourcemap_full_path else map_basename;
                    switch (bun.jsc.Node.fs.NodeFS.writeFileWithPathBuffer(
                        &pathbuf,
                        {
                            data: { buffer: {
                                buffer: {
                                    ptr: @constCast(sourcemap_bytes.ptr),
                                    len: sourcemap_bytes.len as u32,
                                    byte_len: sourcemap_bytes.len as u32,
                                },
                            } },
                            encoding: .buffer,
                            dirfd: .fromStdDir(root_dir),
                            file: { path: {
                                string: bun.PathString.init(write_path),
                            } },
                        },
                    )) {
                        .err e => {
                            bun.Output.err(e, "failed to write sourcemap file '{s}'", [write_path]);
                            output_files.items[i].deinit();
                        },
                        .result => {
                            output_files.items[i].dest_path = sourcemap_full_path;
                            output_files.items[kept] = output_files.items[i];
                            kept += 1;
                        },
                    }
                } else {
                    output_files.items[i].deinit();
                }
            } else {
                output_files.items[i].deinit();
            }
        }
        output_files.items.len = kept;

        return result;
    }

    static fn runOnEndCallbacks(globalThis: *jsc.JSGlobalObject, plugin: *bun.jsc.API.JSBundler.Plugin, promise: *jsc.JSPromise, build_result: jsc.JSValue, rejection: jsc.JSValue ! bun.JSError): bool ! bun.JSError {
        const value = plugin.runOnEndCallbacks(globalThis, promise, build_result, rejection)?;
        return value != .js_undefined;
    }

    fn toJSError(*this, promise: *jsc.JSPromise, globalThis: *jsc.JSGlobalObject): void ! bun.JSTerminated {
        const throw_on_error = this.config.throw_on_error;

        const build_result = jsc.JSValue.createEmptyObject(globalThis, 3);
        build_result.put(globalThis, jsc.ZigString.static("outputs"), jsc.JSValue.createEmptyArray(globalThis, 0) catch return promise.reject(globalThis, error.JSError));
        build_result.put(
            globalThis,
            jsc.ZigString.static("success"),
            .false,
        );
        build_result.put(
            globalThis,
            jsc.ZigString.static("logs"),
            this.log.toJSArray(globalThis, bun.default_allocator) catch |err| {
                return promise.reject(globalThis, err);
            },
        );

        let didHandleCallbacks = false;
        if (plugin := this.plugins) {
            if (throw_on_error) {
                const aggregate_error = this.log.toJSAggregateError(globalThis, bun.String.static("Bundle failed"));
                didHandleCallbacks = JSBundleCompletionTask.runOnEndCallbacks(globalThis, plugin, promise, build_result, aggregate_error) catch |e| {
                    return promise.reject(globalThis, e);
                };
            } else {
                didHandleCallbacks = JSBundleCompletionTask.runOnEndCallbacks(globalThis, plugin, promise, build_result, .js_undefined) catch |e| {
                    return promise.reject(globalThis, e);
                };
            }
        }

        if (!didHandleCallbacks) {
            if (throw_on_error) {
                const aggregate_error = this.log.toJSAggregateError(globalThis, bun.String.static("Bundle failed"));
                return promise.reject(globalThis, aggregate_error);
            } else {
                return promise.resolve(globalThis, build_result);
            }
        }
    }

    pub fn onComplete(*this): void ! bun.JSTerminated {
        let globalThis = this.globalThis;
        defer this.deref();

        this.poll_ref.unref(globalThis.bunVM());
        if (this.cancelled) {
            return;
        }

        if (html_build_task := this.html_build_task) {
            this.plugins = null;
            html_build_task.onComplete(this);
            return;
        }

        const promise = this.promise.swap();

        if (this.result == .value) {
            if (this.config.compile != null) {
                let compile_result = this.doCompilation(&this.result.value.output_files);
                defer compile_result.deinit();

                if (compile_result != .success) {
                    bun.handleOom(this.log.addError(null, Logger.Loc.Empty, bun.handleOom(this.log.msgs.allocator.dupe(u8, compile_result.err.slice()))));
                    this.result.value.deinit();
                    this.result = { err: error.CompilationFailed };
                }
            }
        }

        switch (this.result) {
            .pending => unreachable,
            .err => this.toJSError(promise, globalThis)?,
            .value => |*build| {
                const output_files = build.output_files.items;
                const output_files_js = jsc.JSValue.createEmptyArray(globalThis, output_files.len) catch return promise.reject(globalThis, error.JSError);
                if (output_files_js == .zero) {
                    @panic("Unexpected pending JavaScript exception in JSBundleCompletionTask.onComplete. This is a bug in Bun.");
                }

                let to_assign_on_sourcemap: jsc.JSValue = .zero;
                for (i of 0..output_files.len) {
                    const output_file = &output_files[i];
                    const result = output_file.toJS(
                        if (!this.config.outdir.isEmpty())
                            if (std.fs.path.isAbsolute(this.config.outdir.list.items))
                                bun.default_allocator.dupe(
                                    u8,
                                    bun.path.joinAbsString(
                                        this.config.outdir.slice(),
                                        &[output_file.dest_path],
                                        .auto,
                                    ),
                                ) catch unreachable
                            else
                                bun.default_allocator.dupe(
                                    u8,
                                    bun.path.joinAbsString(
                                        bun.fs.FileSystem.instance.top_level_dir,
                                        &[this.config.dir.slice(), this.config.outdir.slice(), output_file.dest_path],
                                        .auto,
                                    ),
                                ) catch unreachable
                        else
                            bun.default_allocator.dupe(
                                u8,
                                output_file.dest_path,
                            ) catch unreachable,
                        globalThis,
                    );
                    if (to_assign_on_sourcemap != .zero) {
                        jsc.Codegen.JSBuildArtifact.sourcemapSetCached(to_assign_on_sourcemap, globalThis, result);
                        if (to_assign_on_sourcemap_artifact := to_assign_on_sourcemap.as(jsc.API.BuildArtifact)) {
                            to_assign_on_sourcemap_artifact.sourcemap.set(globalThis, result);
                        }
                        to_assign_on_sourcemap = .zero;
                    }

                    if (output_file.source_map_index != std.math.maxInt(u32)) {
                        to_assign_on_sourcemap = result;
                    }

                    output_files_js.putIndex(globalThis, u32(i), result) catch |err| {
                        return promise.reject(globalThis, err);
                    };
                }
                const build_output = jsc.JSValue.createEmptyObject(globalThis, 4);
                build_output.put(globalThis, jsc.ZigString.static("outputs"), output_files_js);
                build_output.put(globalThis, jsc.ZigString.static("success"), .true);
                build_output.put(
                    globalThis,
                    jsc.ZigString.static("logs"),
                    this.log.toJSArray(globalThis, bun.default_allocator) catch |err| {
                        return promise.reject(globalThis, err);
                    },
                );

                if (metafile := build.metafile) {
                    const metafile_js_str = bun.String.createUTF8ForJS(globalThis, metafile) catch |e| {
                        return promise.reject(globalThis, e);
                    };
                    let metafile_md_str: jsc.JSValue = .js_undefined;
                    if (md := build.metafile_markdown) {
                        metafile_md_str = bun.String.createUTF8ForJS(globalThis, md) catch |e| {
                            return promise.reject(globalThis, e);
                        };
                    }
                    unsafe {
                        Bun__setupLazyMetafile(globalThis, build_output, metafile_js_str, metafile_md_str);
                    }
                }

                let didHandleCallbacks = false;
                if (plugin := this.plugins) {
                    didHandleCallbacks = JSBundleCompletionTask.runOnEndCallbacks(globalThis, plugin, promise, build_output, .js_undefined) catch |e| {
                        return promise.reject(globalThis, e);
                    };
                }

                if (!didHandleCallbacks) {
                    return promise.resolve(globalThis, build_output);
                }
            },
        }
    }
}
