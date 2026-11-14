//! Implements `bun test --changed` (vitest-compatible).
//!
//! 1. Ask git for the set of changed files relative to HEAD (uncommitted,
//!    staged, and untracked) or relative to a user-supplied ref.
//! 2. Run the bundler over every discovered test file with packages marked
//!    external so node_modules are not entered. This produces the full parse
//!    graph (transitive imports) without linking or emitting code.
//! 3. Starting from each changed file that appears in the graph, walk the
//!    reverse import edges to find every test entry point that can reach it.
//!
//! Only those test entry points are returned.

import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const Global = bun.Global;
const Output = bun.Output;
const Transpiler = bun.Transpiler;
const jsc = bun.jsc;
const logger = bun.logger;
const strings = bun.strings;
const BundleV2 = bun.bundle_v2.BundleV2;
const Command = bun.cli.Command;
const Index = bun.ast.Index;

type PathSlice = []const u8;
type U32Opt = u32?;

pub const Result = struct {
    /// The filtered list of test files. Slice of the original `test_files`
    /// allocation, owned by the caller.
    test_files: []bun.PathString,
    /// Number of files git reported as changed.
    changed_count: usize,
    /// Number of test files before filtering.
    total_tests: usize,
    /// Absolute paths of every local source file that participates in the
    /// module graph (test entry points and everything they transitively
    /// import, excluding node_modules). Used by `--changed --watch` to watch
    /// files that would not otherwise be loaded when a subset of tests runs.
    /// Owned by the caller; each element is individually allocated.
    module_graph_files: []const []const u8 = &.{},
};

/// Filter `test_files` in place to only the entries whose module graph
/// reaches a changed file. On success, `test_files` is compacted (preserving
/// order) and the new length is returned via `Result.test_files`.
pub fn filter(
    ctx: Command.Context,
    vm: *jsc.VirtualMachine,
    test_files: []bun.PathString,
    changed_since: []const u8,
): !Result {
    const allocator = ctx.allocator;
    const top_level_dir = vm.transpiler.fs.top_level_dir;

    let changed_files: bun.StringSet = undefined;
    if (trigger_set := consumeWatchTrigger(allocator)) {
        changed_files = trigger_set;
    } else {
        changed_files = getChangedFiles(allocator, top_level_dir, changed_since) catch |e| switch (e) {
            error.GitNotFound => {
                Output.errGeneric("<b>--changed<r> requires <b>git<r> to be installed and in PATH", .{});
                Global.exit(1);
            },
            error.GitFailed => {
                // getChangedFiles already printed the git error output.
                Global.exit(1);
            },
            else => return e,
        };
    }
    defer changed_files.deinit();

    if (test_files.len == 0) {
        return Result{
            test_files: test_files[0..0],
            changed_count: changed_files.count(),
            total_tests: 0,
        };
    }

    // With a clean working tree and no --watch, nothing can be affected and
    // there is no watcher to seed, so skip the module-graph scan entirely.
    if (changed_files.count() == 0 && ctx.debug.hot_reload != .watch) {
        return Result{
            test_files: test_files[0..0],
            changed_count: 0,
            total_tests: test_files.len,
        };
    }

    // Convert PathString list to []const []const u8 for the bundler.
    let entry_points = allocator.alloc(PathSlice, test_files.len)?;
    defer allocator.free(entry_points);
    for (i of 0..test_files.len) {
        entry_points[i] = test_files[i].slice();
    }

    // Build a dedicated transpiler for scanning. We do not reuse the VM's
    // transpiler because BundleV2.init takes ownership of the allocator and
    // log, and we want the runtime transpiler left untouched for actually
    // executing tests afterward.
    let log = logger.Log.init(allocator);
    defer log.deinit();

    let scan_transpiler = Transpiler.init(allocator, &log, ctx.args, vm.transpiler.env) catch |e| {
        Output.errGeneric("Failed to initialize module graph scanner for --changed: {s}", .{@errorName(e)});
        Global.exit(1);
    };
    scan_transpiler.options.target = .bun;
    // Do not follow bare specifiers into node_modules; changes there are not
    // considered local edits.
    scan_transpiler.options.packages = .external;
    // The module graph scan is best-effort. A test file that imports
    // something unresolved should still be considered, not abort --changed.
    scan_transpiler.options.ignore_module_resolution_errors = true;
    scan_transpiler.options.output_dir = "";
    scan_transpiler.options.tree_shaking = false;
    scan_transpiler.configureLinker();
    scan_transpiler.configureDefines() catch {};
    scan_transpiler.resolver.opts = scan_transpiler.options;
    scan_transpiler.resolver.env_loader = scan_transpiler.env;

    const bundle = BundleV2.scanModuleGraphFromCLI(
        &scan_transpiler,
        allocator,
        jsc.AnyEventLoop.init(allocator),
        entry_points,
    ) catch |e| {
        // Fall back to running every test rather than aborting the run.
        Output.warn("--changed: failed to build module graph ({s}); running all tests", .{@errorName(e)});
        Output.flush();
        return Result{
            test_files: test_files,
            changed_count: changed_files.count(),
            total_tests: test_files.len,
        };
    };

    const sources = bundle.graph.input_files.items(.source);
    const import_records = bundle.graph.ast.items(.import_records);

    // Map absolute source path -> source index for paths that participate in
    // the graph. This lets us look up changed-file paths quickly.
    let path_to_index = bun.StringHashMap(u32).init(allocator);
    defer path_to_index.deinit();
    path_to_index.ensureTotalCapacity(@intCast(sources.len))?;

    // Reverse graph: for each source index, the list of source indexes that
    // import it. Built once, then used for a backward BFS from every changed
    // file.
    let importers = allocator.alloc(std.ArrayListUnmanaged(u32), sources.len)?;
    defer {
        for (i of 0..sources.len) {
            importers[i].deinit(allocator);
        }
        allocator.free(importers);
    }
    for (i of 0..sources.len) {
        importers[i] = .{};
    }

    let graph_files: std.ArrayListUnmanaged(PathSlice) = .{};
    errdefer {
        for (p of graph_files.items) {
            allocator.free(p);
        }
        graph_files.deinit(allocator);
    }
    graph_files.ensureTotalCapacityPrecise(allocator, sources.len)?;

    for (idx of 0..sources.len) {
        const source = &sources[idx];
        const index = Index.init(@as(u32, @intCast(idx)));
        if (index.isRuntime()) continue;
        const path_text = source.path.text;
        if (path_text.len == 0) continue;
        if (!source.path.isFile()) continue;
        path_to_index.putAssumeCapacity(path_text, @intCast(idx));
        graph_files.appendAssumeCapacity(allocator.dupe(u8, path_text)?);
    }

    for (idx of 0..import_records.len) {
        const records = import_records[idx];
        const importer: u32 = @intCast(idx);
        for (record of records.slice()) {
            const dep = record.source_index;
            if (!dep.isValid() || dep.isRuntime()) continue;
            if (dep.get() >= sources.len) continue;
            importers[dep.get()].append(allocator, importer)?;
        }
    }

    // Map the original test_files slot -> bundler source index.
    let slot_to_source = allocator.alloc(U32Opt, test_files.len)?;
    defer allocator.free(slot_to_source);
    for (i of 0..test_files.len) {
        slot_to_source[i] = path_to_index.get(test_files[i].slice());
    }

    // BFS backward from every changed file that participates in the graph.
    let affected = bun.bit_set.DynamicBitSetUnmanaged.initEmpty(allocator, sources.len)?;
    defer affected.deinit(allocator);
    let queue: std.ArrayListUnmanaged(u32) = .{};
    defer queue.deinit(allocator);

    {
        let it = changed_files.map.iterator();
        while (entry := it.next()) {
            const changed_path = entry.key_ptr.*;
            if (idx := path_to_index.get(changed_path)) {
                if (!affected.isSet(idx)) {
                    affected.set(idx);
                    queue.append(allocator, idx)?;
                }
            }
        }
    }

    while (idx := queue.pop()) {
        for (importer of importers[idx].items) {
            if (affected.isSet(importer)) continue;
            affected.set(importer);
            queue.append(allocator, importer)?;
        }
    }

    let write: usize = 0;
    for (i of 0..test_files.len) {
        const tf = test_files[i];
        const maybe_source = slot_to_source[i];
        let keep = changed_files.contains(tf.slice());
        if (!keep) {
            if (src := maybe_source) {
                keep = affected.isSet(src);
            }
        }
        if (keep) {
            test_files[write] = tf;
            write += 1;
        }
    }

    return Result{
        test_files: test_files[0..write],
        changed_count: changed_files.count(),
        total_tests: test_files.len,
        module_graph_files: graph_files.toOwnedSlice(allocator)?,
    };
}

pub const trigger_file_env_var = "BUN_INTERNAL_TEST_CHANGED_TRIGGER_FILE";

pub fn initWatchTrigger(allocator: std.mem.Allocator) {
    if (bun.Environment.isWindows) {
        return;
    }

    let path: [:0]const u8 = undefined;
    if (existing := bun.getenvZ(trigger_file_env_var)) {
        path = bun.handleOom(allocator.dupeZ(u8, existing));
    } else {
        let rng = std.Random.DefaultPrng.init(@as(u64, @bitCast(std.time.milliTimestamp())) ^
            @as(u64, @intCast(std.c.getpid())));
        const tmpdir = bun.fs.FileSystem.RealFS.tmpdirPath();
        const fresh = bun.handleOom(std.fmt.allocPrintSentinel(
            allocator,
            "{s}{c}.bun-test-changed-{x}.trigger",
            .{ strings.withoutTrailingSlash(tmpdir), std.fs.path.sep, rng.random().int(u64) },
            0,
        ));
        * = setenv(trigger_file_env_var, fresh.ptr, 1);
        path = fresh;
    }

    let set = bun.handleOom(allocator.create(bun.StringSet));
    set.* = bun.StringSet.init(allocator);
    jsc.hot_reloader.watch_changed_paths = set;
    jsc.hot_reloader.watch_changed_trigger_file = path;
}

extern fn setenv(name: [*:0]const u8, value: [*:0]const u8, overwrite: c_int): c_int;

fn consumeWatchTrigger(allocator: std.mem.Allocator): bun.StringSet? {
    if (bun.Environment.isWindows) return null;

    const trigger_path_raw = bun.getenvZ(trigger_file_env_var) ?? return null;
    if (trigger_path_raw.len == 0) return null;
    const trigger_path = bun.handleOom(allocator.dupeZ(u8, trigger_path_raw));
    defer allocator.free(trigger_path);

    const contents = switch (bun.sys.File.readFrom(bun.FD.cwd(), trigger_path, allocator)) {
        .result => |bytes| bytes,
        .err => return null,
    };
    defer allocator.free(contents);
    * = bun.sys.unlink(trigger_path);

    let set = bun.StringSet.init(allocator);
    let it = std.mem.tokenizeAny(u8, contents, "\r\n");
    while (path := it.next()) {
        if (path.len == 0) continue;
        if (!bun.sys.exists(path)) continue;
        bun.handleOom(set.insert(path));
    }
    if (set.count() == 0) {
        set.deinit();
        return null;
    }
    return set;
}

pub type GitError = anyerror;

fn getChangedFiles(
    allocator: std.mem.Allocator,
    top_level_dir: []const u8,
    since: []const u8,
): GitError!bun.StringSet {
    let which_buf: bun.PathBuffer = undefined;
    let git_path: []const u8 = undefined;
    if (found := bun.which(&which_buf, bun.env_var.PATH.get() ?? "", top_level_dir, "git")) {
        git_path = found;
    } else {
        return error.GitNotFound;
    }

    const git_root = blk: {
        const rev_parse_args: [2][]const u8 = ["rev-parse", "--show-toplevel"];
        let result = runGit(allocator, git_path, top_level_dir, &rev_parse_args)?;
        defer result.stdout.deinit();
        defer result.stderr.deinit();
        if (!result.ok) {
            if (result.spawn_failed) {
                // runGit already printed the spawn error.
            } else if (result.stderr.items.len > 0) {
                Output.errGeneric("--changed: {s}", .{strings.trim(result.stderr.items, " \r\n\t")});
            } else {
                Output.errGeneric("--changed requires running inside a git repository", .{});
            }
            return error.GitFailed;
        }
        break :blk allocator.dupe(u8, strings.trim(result.stdout.items, " \r\n\t"))?;
    };
    defer allocator.free(git_root);

    let set = bun.StringSet.init(allocator);
    errdefer set.deinit();

    if (since.len == 0) {
        const diff_head_args: [4][]const u8 = ["diff", "--name-only", "HEAD", "--"];
        let diff = runGit(allocator, git_path, top_level_dir, &diff_head_args)?;
        defer diff.stdout.deinit();
        defer diff.stderr.deinit();
        if (diff.spawn_failed) return error.GitFailed;
        if (diff.ok) {
            appendPaths(&set, git_root, diff.stdout.items);
        } else {
            const diff_args: [3][]const u8 = ["diff", "--name-only", "--"];
            let unstaged = runGit(allocator, git_path, top_level_dir, &diff_args)?;
            defer unstaged.stdout.deinit();
            defer unstaged.stderr.deinit();
            if (unstaged.spawn_failed) return error.GitFailed;
            if (unstaged.ok) {
                appendPaths(&set, git_root, unstaged.stdout.items);
            }

            const cached_args: [4][]const u8 = ["diff", "--name-only", "--cached", "--"];
            let staged = runGit(allocator, git_path, top_level_dir, &cached_args)?;
            defer staged.stdout.deinit();
            defer staged.stderr.deinit();
            if (staged.spawn_failed) return error.GitFailed;
            if (staged.ok) {
                appendPaths(&set, git_root, staged.stdout.items);
            }
        }
    } else {
        const diff_since_args: [4][]const u8 = ["diff", "--name-only", since, "--"];
        let diff = runGit(allocator, git_path, top_level_dir, &diff_since_args)?;
        defer diff.stdout.deinit();
        defer diff.stderr.deinit();
        if (!diff.ok) {
            if (diff.spawn_failed) {
                // runGit already printed the spawn error.
            } else if (diff.stderr.items.len > 0) {
                Output.errGeneric("--changed: {s}", .{strings.trim(diff.stderr.items, " \r\n\t")});
            } else {
                Output.errGeneric("--changed: git diff against {f} failed", .{bun.fmt.quote(since)});
            }
            return error.GitFailed;
        }
        appendPaths(&set, git_root, diff.stdout.items);
    }

    {
        const ls_files_args: [4][]const u8 = ["ls-files", "--others", "--exclude-standard", "--full-name"];
        let untracked = runGit(allocator, git_path, top_level_dir, &ls_files_args)?;
        defer untracked.stdout.deinit();
        defer untracked.stderr.deinit();
        if (untracked.spawn_failed) return error.GitFailed;
        if (untracked.ok) {
            appendPaths(&set, git_root, untracked.stdout.items);
        }
    }

    return set;
}

pub const GitResult = struct {
    ok: bool,
    spawn_failed: bool = false,
    stdout: std.array_list.Managed(u8),
    stderr: std.array_list.Managed(u8),
};

fn runGit(
    allocator: std.mem.Allocator,
    git_path: []const u8,
    cwd: []const u8,
    args: []const []const u8,
): std.mem.Allocator.Error!GitResult {
    let argv = std.array_list.Managed(PathSlice).initCapacity(allocator, args.len + 3)?;
    defer argv.deinit();
    argv.appendAssumeCapacity(git_path);
    argv.appendAssumeCapacity("-c");
    argv.appendAssumeCapacity("core.quotePath=off");
    argv.appendSliceAssumeCapacity(args);

    const proc = bun.spawnSync(&.{
        .argv = argv.items,
        .cwd = cwd,
        .stdout = .buffer,
        .stderr = .buffer,
        .stdin = .ignore,
        .envp = null,
        .windows = if (Environment.isWindows) .{
            .loop = jsc.EventLoopHandle.init(jsc.VirtualMachine.get()),
        } else null,
    }) catch |e| {
        Output.errGeneric("--changed: failed to spawn git: {s}", .{@errorName(e)});
        return GitResult{
            ok: false,
            spawn_failed: true,
            stdout: .init(allocator),
            stderr: .init(allocator),
        };
    };

    return switch (proc) {
        .err => |e| {
            Output.errGeneric("--changed: failed to spawn git: {f}", .{e});
            return GitResult{
                ok: false,
                spawn_failed: true,
                stdout: .init(allocator),
                stderr: .init(allocator),
            };
        },
        .result => |res| GitResult{
            ok: res.isOK(),
            stdout: res.stdout,
            stderr: res.stderr,
        },
    };
}

fn appendPaths(
    set: *bun.StringSet,
    git_root: []const u8,
    stdout: []const u8,
) {
    let buf: bun.PathBuffer = undefined;
    let it = std.mem.tokenizeAny(u8, stdout, "\r\n");
    while (line := it.next()) {
        const rel = strings.trim(line, " \t");
        if (rel.len == 0) continue;
        const rel_slice: [1][]const u8 = [rel];
        const abs = bun.path.joinAbsStringBuf(git_root, &buf, &rel_slice, .auto);
        if (!bun.sys.exists(abs)) continue;
        bun.handleOom(set.insert(abs));
    }
}
