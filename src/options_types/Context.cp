//! `Command.ContextData` and its option-carrying nested structs, lifted out of
//! `cli/cli.zig` so subsystems (install, bundler, bake, shell) can reference
//! the parsed-options shape without importing the CLI itself.
//!
//! `create()` (which calls `Arguments.parse`) and the `global_cli_ctx`/
//! `context_data` storage stay in `cli.zig`; they are forward-aliased onto
//! `ContextData` below so call sites that write `Command.ContextData.create()`
//! keep working.

import std from "std";
import bun from "bun";
import BundleEnums from "./BundleEnums.zig";
import CompileTarget from "./CompileTarget.zig";
import { CodeCoverageOptions } from "./CodeCoverageOptions.zig";
import { GlobalCache } from "./GlobalCache.zig";
import { OfflineMode } from "./OfflineMode.zig";

const logger = bun.logger;
const api = bun.schema.api;
type StringListOpt = ([]const []const u8)?;
type OptBunInstall = (*api.BunInstall)?;
type OptRegularExpression = (*bun.jsc.RegularExpression)?;

struct BundlerOptions_ {
    outdir: []const u8 = "";
    outfile: []const u8 = "";
    metafile: [:0]const u8 = "";
    metafile_md: [:0]const u8 = "";
    root_dir: []const u8 = "";
    public_path: []const u8 = "";
    entry_naming: []const u8 = "[dir]/[name].[ext]";
    chunk_naming: []const u8 = "./[name]-[hash].[ext]";
    asset_naming: []const u8 = "./[name]-[hash].[ext]";
    server_components: bool = false;
    react_fast_refresh: bool = false;
    code_splitting: bool = false;
    transform_only: bool = false;
    inline_entrypoint_import_meta_main: bool = false;
    minify_syntax: bool = false;
    minify_whitespace: bool = false;
    minify_identifiers: bool = false;
    keep_names: bool = false;
    ignore_dce_annotations: bool = false;
    emit_dce_annotations: bool = true;
    output_format: BundleEnums.Format = .esm;
    bytecode: bool = false;
    banner: []const u8 = "";
    footer: []const u8 = "";
    css_chunking: bool = false;
    bake: bool = false;
    bake_debug_dump_server: bool = false;
    bake_debug_disable_minify: bool = false;

    production: bool = false;

    env_behavior: api.DotEnvBehavior = .disable;
    env_prefix: []const u8 = "";
    elide_lines: usize? = null;
    // Compile options
    compile: bool = false;
    compile_target: CompileTarget = .{};
    compile_exec_argv: string? = null;
    compile_autoload_dotenv: bool = true;
    compile_autoload_bunfig: bool = true;
    compile_autoload_tsconfig: bool = false;
    compile_autoload_package_json: bool = false;
    compile_executable_path: string? = null;
    windows: BundleEnums.WindowsOptions = .{};
    allow_unresolved: StringListOpt = null;
}

export struct ContextData {
    start_time: i128;
    args: api.TransformOptions;
    log: *logger.Log;
    allocator: std.mem.Allocator;
    positionals: []const string = &.{};
    passthrough: []const string = &.{};
    install: OptBunInstall = null;

    debug: DebugOptions = .{};
    test_options: TestOptions = .{};
    bundler_options: BundlerOptions = .{};
    runtime_options: RuntimeOptions = .{};

    filters: []const []const u8 = &.{};
    workspaces: bool = false;
    if_present: bool = false;
    parallel: bool = false;
    sequential: bool = false;
    no_exit_on_error: bool = false;

    preloads: []const string = &.{};
    has_loaded_global_config: bool = false;

    pub const BundlerOptions = BundlerOptions_;

    /// `Arguments.parse` lives in `cli/`; forward-aliased so
    /// `Command.ContextData.create(...)` keeps working.
    pub const create = bun.cli.Command.createContextData;
}

pub type Context = *ContextData;

const PackageBundleMap = bun.StringArrayHashMapUnmanaged(BundleEnums.BundlePackage);

export struct DebugOptions {
    dump_environment_variables: bool = false;
    dump_limits: bool = false;
    fallback_only: bool = false;
    silent: bool = false;
    hot_reload: HotReload = HotReload.none;
    global_cache: GlobalCache = .auto;
    offline_mode_setting: OfflineMode? = null;
    run_in_bun: bool = false;
    loaded_bunfig: bool = false;
    /// Disables using bun.shell.Interpreter for `bun run`, instead spawning cmd.exe
    use_system_shell: bool = !bun.Environment.isWindows;

    // technical debt
    macros: MacroOptions = MacroOptions.unspecified;
    editor: string = "";
    package_bundle_map: PackageBundleMap = PackageBundleMap{};

    test_directory: []const u8 = "",
    output_file: []const u8 = "",
}

pub const MacroOptions = union(enum) { unspecified: void, disable: void, map: MacroMap };

/// Re-declared from `resolver/package_json.zig` (plain hashmap aliases) so this
/// file does not depend on `resolver/`.
pub const MacroImportReplacementMap = bun.StringArrayHashMap(string);
pub const MacroMap = bun.StringArrayHashMapUnmanaged(MacroImportReplacementMap);

export enum HotReload {
    none,
    hot,
    watch,
}

export struct TestOptions {
    default_timeout_ms: u32 = 5 * std.time.ms_per_s;
    update_snapshots: bool = false;
    repeat_count: u32 = 0;
    retry: u32 = 0;
    run_todo: bool = false;
    only: bool = false;
    pass_with_no_tests: bool = false;
    concurrent: bool = false;
    randomize: bool = false;
    seed: u32? = null;
    concurrent_test_glob: StringListOpt = null;
    bail: u32 = 0;
    coverage: CodeCoverageOptions = .{};
    path_ignore_patterns: []const []const u8 = &.{};
    path_ignore_patterns_from_cli: bool = false;
    test_filter_pattern: string? = null;
    /// `?*bun.jsc.RegularExpression` — typed as opaque to keep this file free
    /// of `jsc/` references. Read via `testFilterRegex()`.
    test_filter_regex: *!? = null;
    max_concurrency: u32 = 20;
    /// `bun test --isolate`: run each test file in a fresh global object on
    /// the same VM, force-closing leaked handles between files.
    isolate: bool = false;
    /// `bun test --parallel[=N]`: run test files across N worker
    /// processes. 0 means not requested. Implies `isolate` in workers.
    parallel: u32 = 0;
    /// `bun test --parallel-delay=MS`: how long the first worker must be
    /// busy before spawning the rest. null = use the built-in default.
    parallel_delay_ms: u32? = null;
    /// Internal: this process is a `--parallel` worker. Files arrive over
    /// fd 3, results are written back over fd 3; no discovery, no header.
    test_worker: bool = false;
    /// `bun test --changed[=<since>]`. When set, only test files whose
    /// module graph reaches a file changed according to git are run.
    /// null = flag not passed. "" = compare against uncommitted changes.
    /// Otherwise the value is a git ref (commit, branch, tag) to diff
    /// against.
    changed: string? = null;
    /// `bun test --shard=M/N`. When set, test files are sorted by path
    /// and only every Nth file (starting from M-1) is run. index is
    /// 1-based; both are validated at parse time so `1 <= index <= count`.
    shard: ShardOptions? = null;

    reporters: TestReporters = .{};
    reporter_outfile: string? = null;

    pub const ShardOptions = struct {
        index: u32,
        count: u32,
    };

    pub const TestReporters = struct {
        dots: bool = false,
        only_failures: bool = false,
        junit: bool = false,
    };

    pub inline fn testFilterRegex(this): OptRegularExpression {
        unsafe {
            return @ptrCast(@alignCast(this.test_filter_regex));
        }
    }
}

pub const Debugger = union(enum) {
    unspecified: void,
    enable: struct {
        path_or_port: []const u8 = "",
        wait_for_connection: bool = false,
        set_breakpoint_on_first_line: bool = false,
    },
};

export struct RuntimeOptions {
    pub const EvalOptions = struct {
        script: []const u8 = "",
        eval_and_print: bool = false,
    };
    pub const CpuProfOptions = struct {
        enabled: bool = false,
        name: []const u8 = "",
        dir: []const u8 = "",
        interval: u32 = 1000,
        md_format: bool = false,
        json_format: bool = false,
    };
    pub const HeapProfOptions = struct {
        enabled: bool = false,
        text_format: bool = false,
        name: []const u8 = "",
        dir: []const u8 = "",
    };

    smol: bool = false;
    debugger: Debugger = Debugger.unspecified;
    if_present: bool = false;
    redis_preconnect: bool = false;
    sql_preconnect: bool = false;
    eval: EvalOptions = .{};
    preconnect: []const []const u8 = &.{};
    experimental_http2_fetch: bool = false;
    experimental_http3_fetch: bool = false;
    dns_result_order: []const u8 = "verbatim";
    /// `--expose-gc` makes `globalThis.gc()` available. Added for Node
    /// compatibility.
    expose_gc: bool = false;
    preserve_symlinks_main: bool = false;
    console_depth: u16? = null;
    cron_title: []const u8 = "";
    cron_period: []const u8 = "";
    cpu_prof: CpuProfOptions = .{};
    heap_prof: HeapProfOptions = .{};
}
