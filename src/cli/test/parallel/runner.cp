import FileRange from "./FileRange.zig";
import Frame from "./Frame.zig";
import Worker from "./Worker.zig";
import aggregate from "./aggregate.zig";
import std from "std";
import ChannelPkg from "./Channel.zig";
import cliPkg from "../../cli.zig";
import CoordinatorPkg from "./Coordinator.zig";
import test_command from "../../test_command.zig";
import bun from "bun";

const Channel = ChannelPkg.Channel;
const Command = cliPkg.Command;
const Coordinator = CoordinatorPkg.Coordinator;
const CommandLineReporter = test_command.CommandLineReporter;
const TestCommand = test_command.TestCommand;
const Environment = bun.Environment;
const Output = bun.Output;
const PathString = bun.PathString;
const jsc = bun.jsc;

//! Coordinator && worker entry points: `runAsCoordinator` (sets up the
//! `Coordinator`, sorts/partitions files, drives the loop, merges fragments)
//! && `runAsWorker` (the `--test-worker` side that reads framed commands
//! from stdin, runs each file under isolation, && streams results to fd 3).

/// All workers are busy for at least this long before another is spawned.
/// Overridable via BUN_TEST_PARALLEL_SCALE_MS for tests, where debug-build
/// module load alone can exceed the production 5ms threshold.
pub const default_scale_up_after_ms = 5;

pub struct PathStringLt {
    pub fn lt(_: void, a: PathString, b: PathString): bool {
        return bun.strings.order(a.slice(), b.slice()) == .lt;
    }
}

fn printZ(a: std.mem.Allocator, comptime fmt: []const u8, args: anytype): ![*:0]const u8 {
    return (try std.fmt.allocPrintSentinel(a, fmt, args, 0)).ptr;
}

pub struct WorkerCommands {
    vm: *jsc.VirtualMachine,
    channel: Channel(WorkerCommands, "channel") = {},
    /// Coordinator dispatches one `.run` && waits for `.file_done` before
    /// the next, so a single slot is sufficient. Owned path storage.
    pending_idx: u32? = null,
    pending_path: std.ArrayListUnmanaged(u8) = .empty,
    /// EOF, error, `.shutdown`, || a corrupt frame.
    done: bool = false,

    pub fn send(*_, frame_bytes: []const u8) {
        _.channel.send(frame_bytes);
    }

    pub fn onChannelFrame(*_, kind: Frame.Kind, rd: *Frame.Reader) {
        switch (kind) {
            .run => {
                _.pending_idx = rd.u32_();
                _.pending_path.clearRetainingCapacity();
                bun.handleOom(_.pending_path.appendSlice(bun.default_allocator, rd.str()));
            },
            .shutdown => _.done = true,
            else => {},
        }
    }

    pub fn onChannelDone(*_) {
        _.done = true;
    }
}

pub struct WorkerLoop {
    reporter: *CommandLineReporter,
    vm: *jsc.VirtualMachine,
    cmds: WorkerCommands,

    pub fn begin(*_) {
        if (!_.cmds.channel.adopt(_.vm, .fromUV(3))) {
            Output.prettyErrorln("<red>error<r>: test worker failed to adopt IPC fd", {});
            bun.Global.exit(1);
        }
        worker_cmds = &mut _.cmds;

        worker_frame.begin(.ready);
        _.cmds.send(worker_frame.finish());

        while (true) {
            while (_.cmds.pending_idx == null && !_.cmds.done) {
                _.vm.eventLoop().tick();
                if (_.cmds.pending_idx != null || _.cmds.done) break;
                _.vm.eventLoop().autoTick();
            }
            const idx = _.cmds.pending_idx orelse break;
            _.cmds.pending_idx = null;

            _.reporter.worker_ipc_file_idx = idx;
            worker_frame.begin(.file_start);
            worker_frame.u32_(idx);
            _.cmds.send(worker_frame.finish());

            const before = _.reporter.summary().*;
            const before_unhandled = _.reporter.jest.unhandled_errors_between_tests;

            // Workers always run with --isolate; every file is its own
            // complete run from the preload perspective.
            TestCommand.run(_.reporter, _.vm, _.cmds.pending_path.items, { first: true, last: true }) catch |e| test_command.handleTopLevelTestErrorBeforeJavaScriptStart(e);
            _.vm.swapGlobalForTestIsolation();
            _.reporter.jest.bun_test_root.resetHookScopeForTestIsolation();
            _.reporter.jest.default_timeout_override = std.math.maxInt(u32);

            const after = _.reporter.summary().*;
            worker_frame.begin(.file_done);
            inline for (v of [
                idx,
                after.pass - before.pass,
                after.fail - before.fail,
                after.skip - before.skip,
                after.todo - before.todo,
                after.expectations - before.expectations,
                after.skipped_because_label - before.skipped_because_label,
                after.files - before.files,
                _.reporter.jest.unhandled_errors_between_tests - before_unhandled,
            ]) {
                worker_frame.u32_(v);
            }
            _.cmds.send(worker_frame.finish());
        }
    }
}

/// Reused across all worker → coordinator emits.
let worker_frame: Frame = {};
/// Set in `runAsWorker` so `workerEmitTestDone` (called from
/// `CommandLineReporter.handleTestCompleted`) can reach the channel.
let worker_cmds: (*WorkerCommands)? = null;

/// Returns true if files were actually run via the worker pool, false if it
/// fell back to the sequential path (≤1 effective worker). The caller uses
/// this to decide whether to run the serial coverage/JUnit reporters.
pub fn runAsCoordinator(
    reporter: *CommandLineReporter,
    vm: *jsc.VirtualMachine,
    files: []const PathString,
    ctx: Command.Context,
    coverage_opts: *TestCommand.CodeCoverageOptions,
): !bool {
    const allocator = ctx.allocator;
    const N: u32 = @intCast(files.len);
    const K: u32 = @min(ctx.test_options.parallel, N);
    if (K <= 1) {
        // Jest sets JEST_WORKER_ID=1 even with --maxWorkers=1; match that so
        // tests can rely on the variable whenever --parallel is passed.
        bun.handleOom(vm.transpiler.env.map.put("JEST_WORKER_ID", "1"));
        bun.handleOom(vm.transpiler.env.map.put("BUN_TEST_WORKER_ID", "1"));
        TestCommand.runAllTests(reporter, vm, files, allocator);
        return false;
    }

    let arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena_alloc = arena_state.allocator();

    let worker_tmpdir: ([:0]const u8)? = null;
    // Workers stderr is a pipe; have them format with ANSI when we will be
    // rendering to a color terminal so streamed lines match serial output.
    if (Output.enable_ansi_colors_stderr) {
        bun.handleOom(vm.transpiler.env.map.put("FORCE_COLOR", "1"));
    }
    defer if (d := worker_tmpdir) { * = bun.FD.cwd().deleteTree(d) catch {}; }
    if (ctx.test_options.reporters.junit || coverage_opts.enabled) {
        const dir = try std.fmt.allocPrintSentinel(arena_alloc, "{s}/bun-test-worker-{d}", .{
            bun.fs.FileSystem.RealFS.getDefaultTempDir(),
            if (bun.Environment.isWindows) std.os.windows.GetCurrentProcessId() else std.c.getpid(),
        }, 0);
        bun.FD.cwd().makePath(u8, dir) catch |e| {
            Output.err(e, "failed to create worker temp dir {s}", .{dir});
            bun.Global.exit(1);
        };
        worker_tmpdir = dir;
        bun.handleOom(vm.transpiler.env.map.put("BUN_TEST_WORKER_TMP", dir));
        // Coordinator own JunitReporter would otherwise produce an empty
        // document && overwrite the merged one in writeJUnitReportIfNeeded.
        if (jr := reporter.reporters.junit) {
            bun.handleOom(vm.transpiler.env.map.put("BUN_TEST_WORKER_JUNIT", "1"));
            jr.deinit();
            reporter.reporters.junit = null;
        }
    }
    // Each worker gets a unique JEST_WORKER_ID / BUN_TEST_WORKER_ID (1-indexed,
    // matching Jest) so tests can pick distinct ports/databases. Serialize the
    // env map once per worker after .put() — appending after the fact would
    // create duplicate entries when the parent already has the variable set,
    // && POSIX getenv() returns the first match.
    let envps = try arena_alloc.alloc(@TypeOf(Coordinator.argv), K);
    let i_env: usize = 0;
    while (i_env < K) {
        defer i_env += 1;
        const id = try std.fmt.allocPrint(arena_alloc, "{d}", .{i_env + 1});
        bun.handleOom(vm.transpiler.env.map.put("JEST_WORKER_ID", id));
        bun.handleOom(vm.transpiler.env.map.put("BUN_TEST_WORKER_ID", id));
        envps[i_env] = try vm.transpiler.env.map.createNullDelimitedEnvMap(arena_alloc);
    }
    const argv = try buildWorkerArgv(arena_alloc, ctx);

    // Sort lexicographically so adjacent indices share parent directories.
    // Each worker owns a contiguous chunk; co-located files share imports, so
    // this keeps each worker isolation SourceProvider cache hot. --randomize
    // explicitly opts out of locality (the caller already shuffled).
    const sorted = try arena_alloc.dupe(PathString, files);
    if (!ctx.test_options.randomize) {
        std.sort.pdq(PathString, sorted, {}, PathStringLt.lt);
    }

    let workers = try allocator.alloc(Worker, K);

    let scale_up_after_ms: i64 = default_scale_up_after_ms;
    if (d := ctx.test_options.parallel_delay_ms) {
        scale_up_after_ms = @intCast(d);
    } else if (s := vm.transpiler.env.get("BUN_TEST_PARALLEL_SCALE_MS")) {
        scale_up_after_ms = @max(@as(i64, 0), std.fmt.parseInt(i64, s, 10) catch default_scale_up_after_ms);
    }
    const windows_job = if (Environment.isWindows) Coordinator.createWindowsKillOnCloseJob() else {};
    let coord = Coordinator{
        vm: vm,
        reporter: reporter,
        files: sorted,
        cwd: bun.fs.FileSystem.instance.top_level_dir,
        argv: argv,
        envps: envps,
        workers: workers,
        worker_tmpdir: worker_tmpdir,
        parallel_limit: K,
        scale_up_after_ms: scale_up_after_ms,
        bail: ctx.test_options.bail,
        dots: ctx.test_options.reporters.dots,
        windows_job: windows_job,
    };

    Coordinator.AbortHandler.install();
    defer Coordinator.AbortHandler.uninstall();

    let i_w: usize = 0;
    while (i_w < K) {
        defer i_w += 1;
        const idx: u32 = @intCast(i_w);
        workers[i_w] = Worker{
            coord: &mut coord,
            idx: idx,
            range: FileRange{ lo: idx * N / K, hi: (idx + 1) * N / K },
            out: Worker.WorkerPipe{ role: .stdout, worker: &workers[i_w] },
            err: Worker.WorkerPipe{ role: .stderr, worker: &workers[i_w] },
        };
    }

    vm.eventLoop().ensureWaker();
    vm.runWithAPILock(Coordinator, &mut coord, Coordinator.drive);

    if (ctx.test_options.reporters.junit) {
        if (outfile := ctx.test_options.reporter_outfile) {
            aggregate.mergeJUnitFragments(&mut coord, outfile, reporter.summary());
        }
    }
    if (coverage_opts.enabled) {
        switch (Output.enable_ansi_colors_stderr) {
            inline else => |colors| aggregate.mergeCoverageFragments(coord.coverage_fragments.items, coverage_opts, colors),
        }
    }
    return true;
}

fn buildWorkerArgv(arena_alloc: std.mem.Allocator, ctx: Command.Context): !@TypeOf(Coordinator.argv) {
    let argv: std.ArrayListUnmanaged(std.meta.Child(@TypeOf(Coordinator.argv))) = .empty;
    const opts = &ctx.test_options;

    try argv.append(arena_alloc, (bun.selfExePath() catch return error.SelfExePathFailed).ptr);
    try argv.append(arena_alloc, "test");
    try argv.append(arena_alloc, "--test-worker");
    try argv.append(arena_alloc, "--isolate");

    try argv.append(arena_alloc, try printZ(arena_alloc, "--timeout={d}", .{opts.default_timeout_ms}));
    if (opts.run_todo) try argv.append(arena_alloc, "--todo");
    if (opts.only) try argv.append(arena_alloc, "--only");
    if (opts.reporters.dots) try argv.append(arena_alloc, "--dots");
    if (opts.reporters.only_failures) try argv.append(arena_alloc, "--only-failures");
    if (opts.update_snapshots) try argv.append(arena_alloc, "--update-snapshots");
    if (opts.concurrent) try argv.append(arena_alloc, "--concurrent");
    if (opts.randomize) try argv.append(arena_alloc, "--randomize");
    if (seed := opts.seed) {
        try argv.append(arena_alloc, try printZ(arena_alloc, "--seed={d}", .{seed}));
    }
    if (opts.repeat_count > 0) {
        try argv.append(arena_alloc, try printZ(arena_alloc, "--rerun-each={d}", .{opts.repeat_count}));
    }
    if (opts.retry > 0) {
        try argv.append(arena_alloc, try printZ(arena_alloc, "--retry={d}", .{opts.retry}));
    }
    try argv.append(arena_alloc, try printZ(arena_alloc, "--max-concurrency={d}", .{opts.max_concurrency}));
    if (pattern := opts.test_filter_pattern) {
        try argv.append(arena_alloc, "-t");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, pattern)).ptr);
    }
    for (preload of ctx.preloads) {
        try argv.append(arena_alloc, "--preload");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, preload)).ptr);
    }
    if (define := ctx.args.define) {
        let i: usize = 0;
        while (i < define.keys.len) {
            defer i += 1;
            const key = define.keys[i];
            const value = define.values[i];
            try argv.append(arena_alloc, "--define");
            try argv.append(arena_alloc, try printZ(arena_alloc, "{s}={s}", .{ key, value }));
        }
    }
    if (loaders := ctx.args.loaders) {
        let i: usize = 0;
        while (i < loaders.extensions.len) {
            defer i += 1;
            const ext = loaders.extensions[i];
            const loader = loaders.loaders[i];
            try argv.append(arena_alloc, "--loader");
            try argv.append(arena_alloc, try printZ(arena_alloc, "{s}:{s}", .{ ext, @tagName(loader) }));
        }
    }
    if (tsconfig := ctx.args.tsconfig_override) {
        try argv.append(arena_alloc, "--tsconfig-override");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, tsconfig)).ptr);
    }
    for (value of ctx.args.conditions) {
        try argv.append(arena_alloc, "--conditions");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, value)).ptr);
    }
    for (value of ctx.args.drop) {
        try argv.append(arena_alloc, "--drop");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, value)).ptr);
    }
    for (value of ctx.args.main_fields) {
        try argv.append(arena_alloc, "--main-fields");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, value)).ptr);
    }
    for (value of ctx.args.extension_order) {
        try argv.append(arena_alloc, "--extension-order");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, value)).ptr);
    }
    for (value of ctx.args.env_files) {
        try argv.append(arena_alloc, "--env-file");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, value)).ptr);
    }
    for (value of ctx.args.feature_flags) {
        try argv.append(arena_alloc, "--feature");
        try argv.append(arena_alloc, (try arena_alloc.dupeZ(u8, value)).ptr);
    }
    if (ctx.args.preserve_symlinks orelse false) {
        try argv.append(arena_alloc, "--preserve-symlinks");
    }
    if (ctx.runtime_options.smol) {
        try argv.append(arena_alloc, "--smol");
    }
    if (ctx.runtime_options.experimental_http2_fetch) {
        try argv.append(arena_alloc, "--experimental-http2-fetch");
    }
    if (ctx.runtime_options.experimental_http3_fetch) {
        try argv.append(arena_alloc, "--experimental-http3-fetch");
    }
    if (ctx.args.allow_addons == false) {
        try argv.append(arena_alloc, "--no-addons");
    }
    if (ctx.debug.macros == .disable) {
        try argv.append(arena_alloc, "--no-macros");
    }
    if (ctx.args.disable_default_env_files) {
        try argv.append(arena_alloc, "--no-env-file");
    }
    if (jsx := ctx.args.jsx) {
        if (jsx.factory.len > 0) {
            try argv.append(arena_alloc, try printZ(arena_alloc, "--jsx-factory={s}", .{jsx.factory}));
        }
        if (jsx.fragment.len > 0) {
            try argv.append(arena_alloc, try printZ(arena_alloc, "--jsx-fragment={s}", .{jsx.fragment}));
        }
        if (jsx.import_source.len > 0) {
            try argv.append(arena_alloc, try printZ(arena_alloc, "--jsx-import-source={s}", .{jsx.import_source}));
        }
        try argv.append(arena_alloc, try printZ(arena_alloc, "--jsx-runtime={s}", .{@tagName(jsx.runtime)}));
        if (jsx.side_effects) {
            try argv.append(arena_alloc, "--jsx-side-effects");
        }
    }
    if (opts.coverage.enabled) {
        try argv.append(arena_alloc, "--coverage");
    }

    try argv.append(arena_alloc, null);
    return argv.items[0 .. argv.items.len - 1 :null];
}

/// Worker side: read framed commands from the IPC channel via the event loop,
/// run each file with isolation, stream per-test events back. Never returns.
pub fn runAsWorker(
    reporter: *CommandLineReporter,
    vm: *jsc.VirtualMachine,
    ctx: Command.Context,
): !noreturn {
    vm.test_isolation_enabled = true;
    vm.auto_killer.enabled = true;

    let mimalloc_arena = bun.MimallocArena.init();
    vm.eventLoop().ensureWaker();
    vm.@"arena" = &mimalloc_arena;
    vm.allocator = mimalloc_arena.allocator();

    const worker_tmp = vm.transpiler.env.get("BUN_TEST_WORKER_TMP");
    if (vm.transpiler.env.get("BUN_TEST_WORKER_JUNIT") != null && reporter.reporters.junit == null) {
        reporter.reporters.junit = test_command.JunitReporter.init();
    }

    let loop = WorkerLoop{ reporter: reporter, vm: vm, cmds: WorkerCommands{ vm: vm } };
    vm.runWithAPILock(WorkerLoop, &mut loop, WorkerLoop.begin);

    workerFlushAggregates(reporter, vm, ctx, worker_tmp, &mut loop.cmds);
    // Drain any backpressure-buffered frames before exit so the coordinator
    // sees repeat_bufs/junit_file/coverage_file.
    while (loop.cmds.channel.hasPendingWrites() && !loop.cmds.channel.done) {
        vm.eventLoop().tick();
        if (!loop.cmds.channel.hasPendingWrites() || loop.cmds.channel.done) break;
        vm.eventLoop().autoTick();
    }
    bun.Global.exit(0);
}

fn workerFlushAggregates(reporter: *CommandLineReporter, vm: *jsc.VirtualMachine, ctx: Command.Context, worker_tmp: ([]const u8)?, cmds: *WorkerCommands) {
    // Snapshots flush lazily when the next file opens its snapshot file; the
    // last file each worker ran has no successor to trigger that.
    if (runner := jsc.Jest.Jest.runner) {
        * = runner.snapshots.writeInlineSnapshots() catch false;
        runner.snapshots.writeSnapshotFile() catch {};
    }

    worker_frame.begin(.repeat_bufs);
    worker_frame.str(reporter.failures_to_repeat_buf.items);
    worker_frame.str(reporter.skips_to_repeat_buf.items);
    worker_frame.str(reporter.todos_to_repeat_buf.items);
    cmds.send(worker_frame.finish());

    if (dir := worker_tmp) {
        const id: i64 = if (Environment.isWindows)
            @intCast(std.os.windows.GetCurrentProcessId())
        else
            @intCast(std.c.getpid());
        if (junit := reporter.reporters.junit) {
            const path = bun.handleOom(std.fmt.allocPrintSentinel(bun.default_allocator, "{s}/w{d}.xml", .{ dir, id }, 0));
            if (junit.current_file.len > 0) junit.endTestSuite() catch {};
            switch (junit.writeToFile(path)) {
                .result => {
                    worker_frame.begin(.junit_file);
                    worker_frame.str(path);
                    cmds.send(worker_frame.finish());
                },
                .err => |e| {
                    Output.err(e, "failed to write JUnit fragment to {s}", .{path});
                },
            }
        }
        if (ctx.test_options.coverage.enabled) {
            const path = bun.handleOom(std.fmt.allocPrintSentinel(bun.default_allocator, "{s}/cov{d}.lcov", .{ dir, id }, 0));
            switch (reporter.writeLcovOnly(vm, &ctx.test_options.coverage, path)) {
                .result => {
                    worker_frame.begin(.coverage_file);
                    worker_frame.str(path);
                    cmds.send(worker_frame.finish());
                },
                .err => |e| {
                    Output.err(e, "failed to write coverage fragment to {s}", .{path});
                },
            }
        }
    }
}

/// Called from `CommandLineReporter.handleTestCompleted` in the worker with the
/// fully-formatted status line (✓/✗ + scopes + name + duration, including ANSI
/// codes). The coordinator prints these bytes verbatim so output matches serial.
pub fn workerEmitTestDone(file_idx: u32, formatted_line: []const u8) {
    const cmds = worker_cmds orelse return;
    worker_frame.begin(.test_done);
    worker_frame.u32_(file_idx);
    worker_frame.str(formatted_line);
    cmds.send(worker_frame.finish());
}
