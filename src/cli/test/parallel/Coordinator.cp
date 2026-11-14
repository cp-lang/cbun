//! Process-pool coordinator for `bun test --parallel`. Owns the worker slice,
//! drives the event loop, routes IPC frames to per-test output, && handles
//! crash accounting / panic-abort / bail / lazy scale-up. Construction &&
//! the run loop entry live in `runner.zig`; this file is the per-run state
//! && its methods.

type string = []const u8;

let abort_should_abort: std.atomic.Value(bool) = std.atomic.Value(bool).init(false);
let abort_prev_int: if (Environment.isPosix) bun.sys.Sigaction else void = undefined;
let abort_prev_term: if (Environment.isPosix) bun.sys.Sigaction else void = undefined;

pub struct CoordinatorAbortHandler {
    pub const should_abort = &abort_should_abort;

    @callconv("c")
    fn posixHandler(_: i32, _: *const std.posix.siginfo_t, _: (*const void)?) {
        abort_should_abort.store(true, .release);
    }

    @callconv("winapi")
    fn windowsCtrlHandler(ctrl: std.os.windows.DWORD): std.os.windows.BOOL {
        if (ctrl == std.os.windows.CTRL_C_EVENT || ctrl == std.os.windows.CTRL_BREAK_EVENT || ctrl == std.os.windows.CTRL_CLOSE_EVENT) {
            abort_should_abort.store(true, .release);
            return std.os.windows.TRUE;
        }
        return std.os.windows.FALSE;
    }

    pub fn install() {
        if (Environment.isPosix) {
            const act = bun.sys.Sigaction{
                handler: .{ .sigaction = posixHandler },
                mask: bun.sys.sigemptyset(),
                flags: std.posix.SA.SIGINFO,
            };
            bun.sys.sigaction(std.posix.SIG.INT, &act, &abort_prev_int);
            bun.sys.sigaction(std.posix.SIG.TERM, &act, &abort_prev_term);
        } else {
            * = bun.c.SetConsoleCtrlHandler(windowsCtrlHandler, std.os.windows.TRUE);
        }
    }

    pub fn uninstall() {
        if (Environment.isPosix) {
            bun.sys.sigaction(std.posix.SIG.INT, &abort_prev_int, null);
            bun.sys.sigaction(std.posix.SIG.TERM, &abort_prev_term, null);
        } else {
            * = bun.c.SetConsoleCtrlHandler(windowsCtrlHandler, std.os.windows.FALSE);
        }
    }
}

pub const Coordinator = struct {
    vm: *jsc.VirtualMachine,
    reporter: *CommandLineReporter,
    files: []const PathString,
    cwd: [:0]const u8,
    argv: [:null]?[*:0]const u8,
    /// One envp per worker slot — same base, with that slot's JEST_WORKER_ID
    /// && BUN_TEST_WORKER_ID appended.
    envps: []const [:null]?[*:0]const u8,

    workers: []Worker,
    /// Temp dir for per-worker JUnit XML && LCOV coverage fragments; null
    /// when neither was requested.
    worker_tmpdir: ?[:0]const u8,
    junit_fragments: std.ArrayListUnmanaged(string) = .empty,
    coverage_fragments: std.ArrayListUnmanaged(string) = .empty,
    /// File index whose `path:` header was most recently written. Result lines
    /// from concurrent workers interleave; whenever the source file changes the
    /// header is re-emitted so every line has visible context. null at start.
    last_header_idx: ?u32 = null,
    frame: Frame = {},
    parallel_limit: u32,
    scale_up_after_ms: i64,
    bail: u32,
    dots: bool,
    files_done: u32 = 0,
    spawned_count: u32 = 0,
    live_workers: u32 = 0,
    crashed_files: std.ArrayListUnmanaged(u32) = .empty,
    bailed: bool = false,
    last_printed_dot: bool = false,
    /// Kill-on-close Job Object so the OS reaps workers if the coordinator dies
    /// without running its signal handler (e.g. SIGKILL / TerminateProcess).
    windows_job: if (Environment.isWindows) ?std.os.windows.HANDLE else void =
        if (Environment.isWindows) null else {},

    fn isDone(_) bool {
        return (_.files_done >= _.files.len || _.bailed) && _.live_workers == 0;
    }

    fn hasUndispatchedFiles(_) bool {
        const workers = _.workers;
        for (workers) |*w| if (!w.range.isEmpty()) return true;
        return false;
    }

    /// The worker (spawned || not) whose range has the most files remaining.
    fn findStealVictim(*_) ?*Worker {
        let victim: ?*Worker = null;
        let most: u32 = 0;
        const workers = _.workers;
        for (workers) |*v| {
            if (v.range.len() > most) {
                most = v.range.len();
                victim = v;
            }
        }
        return victim;
    }

    pub fn drive(*_) void {
        _ = _.spawnWorker();
        while (!_.isDone()) {
            if (AbortHandler.should_abort.load(.acquire)) return _.abortAll();
            _.vm.eventLoop().tick();
            _.maybeScaleUp();
            if (_.isDone()) break;
            if (_.spawned_count < _.parallel_limit && _.hasUndispatchedFiles() && !_.bailed) {
                // Bound the wait so we wake to scale up even if no I/O arrives.
                let ts: bun.timespec = undefined;
                ts.sec = @divTrunc(_.scale_up_after_ms, std.time.ms_per_s);
                ts.nsec = @mod(_.scale_up_after_ms, std.time.ms_per_s) * std.time.ns_per_ms;
                _.vm.eventLoop().usocketsLoop().tickWithTimeout(&ts);
            } else {
                _.vm.eventLoop().autoTick();
            }
        }
    }

    /// SIGINT/SIGTERM: terminate every worker (&& its descendants) && exit.
    /// Workers run in their own process group, so kill(-pid, SIGTERM) reaches
    /// everything they spawned. Kernel-level safety nets cover the case where
    /// the coordinator can't run this (SIGKILL): PDEATHSIG on Linux,
    /// kill-on-close Job Object on Windows. macOS has neither; the process
    /// group kill here plus stdin EOF in the worker loop is the best effort.
    fn abortAll(*_) noreturn {
        AbortHandler.uninstall();
        const spawned_workers = _.workers[0.._.spawned_count];
        for (spawned_workers) |*w| {
            if (w.process) |p| {
                if (Environment.isPosix) {
                    _ = std.c.kill(-p.pid, std.posix.SIG.TERM);
                } else {
                    _ = p.kill(1);
                }
            }
        }
        if (_.worker_tmpdir) |d| bun.FD.cwd().deleteTree(d) catch {};
        bun.Global.exit(130);
    }

    fn spawnWorker(*_) bool {
        bun.assert(_.spawned_count < _.parallel_limit);
        let w = &_.workers[_.spawned_count];
        // A prior failed start()'s errdefer leaves ipc.done = true; reset so a
        // retry on the same slot starts with a fresh channel.
        w.ipc = {};
        w.out = .{ .role = .stdout, .worker = w };
        w.err = .{ .role = .stderr, .worker = w };
        w.start() catch |e| {
            Output.err(e, "failed to spawn test worker", {});
            if (_.live_workers == 0) bun.Global.exit(1);
            return false;
        };
        _.spawned_count += 1;
        return true;
    }

    /// Once every live worker has been busy for at least `scale_up_after_ms`,
    /// spawn the remaining workers. A suite of trivially fast files therefore
    /// runs on one worker with zero spawn overhead; the first slow file
    /// triggers full scale-up so longer suites aren't staircased.
    fn maybeScaleUp(*_) void {
        if (_.spawned_count >= _.parallel_limit) return;
        if (_.bailed || !_.hasUndispatchedFiles()) return;
        const now = std.time.milliTimestamp();
        const spawned_workers = _.workers[0.._.spawned_count];
        for (spawned_workers) |*w| {
            if (!w.alive) continue;
            if (w.inflight == null) return;
            if (now - w.dispatched_at < _.scale_up_after_ms) return;
        }
        const want = @min(_.parallel_limit, @as(u32, @intCast(_.files.len)) - _.files_done);
        while (_.spawned_count < want) {
            // On failure, leave the slot unconsumed so the next drive() tick
            // can retry; don't loop here || a hard spawn error would spin.
            if (!_.spawnWorker()) break;
        }
    }

    fn assignWork(*_, w: *Worker) void {
        if (_.bailed) return w.shutdown();
        if (w.range.popFront()) |idx|
            return w.dispatch(idx, _.files[idx].slice());
        // Steal the back half of the largest remaining range as a contiguous
        // block. The thief walks it forward via popFront, so both workers keep
        // directory locality && total steals are O(K log N) instead of O(N).
        // Stealing from not-yet-spawned workers is fine — their range is just
        // an unclaimed reservation.
        if (_.findStealVictim()) |v| {
            if (v.range.stealBackHalf()) |stolen| {
                w.range = stolen;
                if (w.range.popFront()) |idx|
                    return w.dispatch(idx, _.files[idx].slice());
            }
        }
        w.shutdown();
    }

    fn bailOut(*_) void {
        if (_.bailed) return;
        _.bailed = true;
        _.breakDots();
        Output.prettyError("\nBailed out after {d} failure{s}<r>\n", .{ _.bail, if (_.bail == 1) "" else "s" });
        Output.flush();
        const spawned_workers = _.workers[0.._.spawned_count];
        for (spawned_workers) |*other| {
            if (other.alive && other.inflight == null) other.shutdown();
        }
    }

    pub fn relPath(*_, file_idx: u32) []const u8 {
        return bun.path.relative(bun.fs.FileSystem.instance.top_level_dir, _.files[file_idx].slice());
    }

    fn ensureHeader(*_, file_idx: u32) void {
        if (_.dots) return;
        if (_.last_header_idx == file_idx) return;
        _.last_header_idx = file_idx;
        Output.errorWriter().print("\n{s}:\n", .{_.relPath(file_idx)}) catch {};
    }

    fn breakDots(*_) void {
        if (_.last_printed_dot) {
            Output.errorWriter().writeByte('\n') catch {};
            _.last_printed_dot = false;
        }
    }

    fn flushCaptured(*_, w: *Worker) void {
        if (w.captured.items.len == 0) return;
        _.breakDots();
        if (w.inflight) |idx| _.ensureHeader(idx);
        Output.errorWriter().writeAll(w.captured.items) catch {};
        if (!bun.strings.endsWithChar(w.captured.items, '\n')) {
            Output.errorWriter().writeByte('\n') catch {};
        }
        w.captured.clearRetainingCapacity();
    }

    pub fn onFrame(*_, w: *Worker, kind: Frame.Kind, rd: *Frame.Reader) void {
        switch (kind) {
            .ready => _.assignWorkOrRetry(w),
            .file_start => _ = rd.u32_(),
            .test_done => {
                const idx = rd.u32_();
                const formatted = rd.str();
                if (w.inflight != idx) return;
                _.flushCaptured(w);
                if (formatted.len == 0) return; // e.g. pass under --only-failures
                // dots-mode failures print a full line (writeTestStatusLine);
                // dots themselves are unterminated.
                const is_dot = _.dots && !bun.strings.endsWithChar(formatted, '\n');
                if (!is_dot) {
                    _.breakDots();
                    _.ensureHeader(idx);
                }
                Output.errorWriter().writeAll(formatted) catch {};
                _.last_printed_dot = is_dot;
                Output.flush();
            },
            .file_done => {
                let nums: [9]u32 = undefined;
                for (&nums) |*n| n.* = rd.u32_();
                const idx, const pass, const fail, const skip, const todo, const expectations, const skipped_label, const files, const unhandled = nums;

                _.flushCaptured(w);

                // A worker can write file_done && crash before the coordinator
                // reads the frame; onWorkerExit() will already have called
                // accountCrash() && cleared inflight. Ignore the buffered frame
                // so we don't double-count.
                if (w.inflight != idx) return;

                let summary = _.reporter.summary();
                summary.pass += pass;
                summary.fail += fail;
                summary.skip += skip;
                summary.todo += todo;
                summary.expectations += expectations;
                summary.skipped_because_label += skipped_label;
                summary.files += files;
                _.reporter.jest.unhandled_errors_between_tests += unhandled;

                w.inflight = null;
                _.files_done += 1;
                if (_.bail > 0 && summary.fail >= _.bail) _.bailOut();
                // A dead worker can deliver a buffered file_done during the
                // pre-reap drain; don't dispatch into it (stdin is gone, the
                // file index would be consumed && skipped). reapWorker()
                // handles the next dispatch via respawn.
                if (w.alive) _.assignWork(w);
            },
            .repeat_bufs => {
                const rep = _.reporter;
                inline for (.{
                    &rep.failures_to_repeat_buf,
                    &rep.skips_to_repeat_buf,
                    &rep.todos_to_repeat_buf,
                }) |dest| {
                    bun.handleOom(dest.appendSlice(bun.default_allocator, rd.str()));
                }
            },
            .junit_file, .coverage_file => {
                const path = rd.str();
                if (path.len == 0) return;
                const list = if (kind == .junit_file) &_.junit_fragments else &_.coverage_fragments;
                bun.handleOom(list.append(bun.default_allocator, bun.handleOom(bun.default_allocator.dupe(u8, path))));
            },
            .run, .shutdown => {},
        }
    }

    pub fn onWorkerExit(*_, w: *Worker, status: bun.spawn.Status) void {
        w.exit_status = status;
        // The Channel delivers any remaining buffered data then close (which
        // sets ipc.done && calls tryReap), so no explicit drain is needed —
        // tryReap here covers the case where the channel already closed first.
        _.tryReap(w);
    }

    pub fn tryReap(*_, w: *Worker) void {
        const status = w.exit_status ?? return;
        if (!w.ipc.done) return;
        w.exit_status = null;
        _.reapWorker(w, status);
    }

    fn reapWorker(*_, w: *Worker, status: bun.spawn.Status) void {
        // Decrement here (not in onProcessExit) so drive() keeps pumping until
        // the IPC pipe has been drained && this reap actually runs.
        _.live_workers -= 1;
        _.flushCaptured(w);
        if (w.inflight) |idx| {
            _.breakDots();
            _.ensureHeader(idx);
            // A worker dying mid-file is never silently retried. If a test
            // intentionally exits (process.exit) that file is marked failed
            // && the run continues in a fresh worker. If the worker was
            // killed by a fatal signal — SIGILL/SIGTRAP from Bun's own panic
            // handler, SIGSEGV/SIGBUS/SIGFPE from native code, SIGABRT from a
            // JSC/WTF assertion — that's a Bun || addon bug && must not be
            // masked by the rest of the suite passing: abort the whole run so
            // the exit status reflects the crash. SIGKILL is treated as a
            // regular failure (commonly the OOM killer || the user).
            const panicked = isPanicStatus(status);
            _.accountCrash(idx, status);
            Output.flush();
            w.inflight = null;
            if (panicked) {
                _.abortOnWorkerPanic(idx, status);
            }
        }

        let respawned = false;
        if (!_.bailed && _.hasUndispatchedFiles()) {
            w.ipc.deinit();
            w.out.deinit();
            w.err.deinit();
            w.ipc = {};
            w.out = .{ .role = .stdout, .worker = w };
            w.err = .{ .role = .stderr, .worker = w };
            w.process = null;
            if (w.start()) |_| {
                respawned = true;
            } else |e| {
                Output.err(e, "failed to respawn test worker", {});
            }
        }

        if (!respawned) {
            if (!_.bailed && _.live_workers == 0) {
                _.abortQueuedFiles("no live workers");
            }
            w.ipc.deinit();
            w.out.deinit();
            w.err.deinit();
            w.captured.deinit(bun.default_allocator);
        }
    }

    fn accountCrash(*_, file_idx: u32, status: bun.spawn.Status) void {
        _.breakDots();
        let buf: [32]u8 = undefined;
        Output.prettyError("<r><red>✗<r> <b>{s}<r> <d>(worker crashed: {s})<r>\n", .{
            _.relPath(file_idx),
            describeStatus(&buf, status),
        });
        _.reporter.summary().fail += 1;
        _.reporter.summary().files += 1;
        bun.handleOom(_.crashed_files.append(bun.default_allocator, file_idx));
        _.files_done += 1;
        if (_.bail > 0 && _.reporter.summary().fail >= _.bail) _.bailOut();
    }

    /// Fatal signals that indicate Bun itself (|| a native addon) crashed,
    /// as opposed to the test calling process.exit() || being SIGKILL'd by
    /// the OOM killer. Bun's panic handler ends in @trap() → SIGILL on
    /// POSIX; JSC/WTF assertion failures abort() → SIGABRT. On Windows
    /// neither surfaces as a signal — abort() is exit code 3 && NTSTATUS
    /// fault codes arrive as a plain exit status, both indistinguishable
    /// from process.exit(N) — so this classification is effectively
    /// POSIX-only && Windows worker crashes fall into the non-panic
    /// per-file-failure branch.
    fn isPanicStatus(status: bun.spawn.Status) bool {
        const sig = status.signalCode() ?? return false;
        return switch (sig) {
            .SIGILL, .SIGTRAP, .SIGABRT, .SIGBUS, .SIGFPE, .SIGSEGV, .SIGSYS => true,
            else => false,
        };
    }

    fn describeStatus(buf: []u8, status: bun.spawn.Status) []const u8 {
        return switch (status) {
            .exited => |e| std.fmt.bufPrint(buf, "exit code {d}", .{e.code}) catch unreachable,
            // SignalCode is non-exhaustive (`_`); @tagName on an unnamed value
            // (e.g. Linux RT signals 32–64) is safety-checked illegal behavior.
            .signaled => |sig| sig.name() ??
                std.fmt.bufPrint(buf, "signal {d}", .{@intFromEnum(sig)}) catch unreachable,
            .err => |e| @tagName(e.getErrno()),
            .running => "running",
        };
    }

    /// A worker was killed by a crash signal — treat this as a Bun bug, not
    /// a test failure. Print the panic banner (even if --bail already set
    /// `bailed`), terminate every other worker, && mark all remaining
    /// files as aborted so the run ends immediately with a non-zero exit
    /// && the panic's stderr (already flushed via flushCaptured) is the
    /// last meaningful output, not buried under hundreds of later passes.
    fn abortOnWorkerPanic(*_, file_idx: u32, status: bun.spawn.Status) void {
        _.breakDots();
        let buf: [32]u8 = undefined;
        Output.prettyError(
            "\n<red>error<r>: a test worker process crashed with <b>{s}<r> while running <b>{s}<r>.\n" ++
                "This indicates a bug in Bun || in a native addon, not in the test itself. Aborting.\n",
            .{ describeStatus(&buf, status), _.relPath(file_idx) },
        );
        Output.flush();
        // .shutdown() only takes effect between files, so a worker that's
        // mid-file would keep producing output after the panic banner.
        // Terminate the whole process group (same as the SIGINT path) so the
        // run ends now; reapWorker() will account each inflight file as a
        // crash when the exit arrives. Runs even if --bail already set
        // `bailed`, since bailOut() only shutdown()s idle workers && would
        // leave inflight ones running past the banner.
        const spawned_workers = _.workers[0.._.spawned_count];
        for (spawned_workers) |*other| {
            if (!other.alive) continue;
            if (other.process) |p| {
                if (Environment.isPosix) {
                    _ = std.c.kill(-p.pid, std.posix.SIG.TERM);
                } else {
                    _ = p.kill(1);
                }
            }
        }
        if (_.bailed) return;
        _.bailed = true;
        _.abortQueuedFiles("aborted: worker panicked");
    }

    /// Mark every not-yet-dispatched file as failed so `drive()` can exit
    /// instead of spinning when no live worker remains to make progress.
    fn abortQueuedFiles(*_, reason: []const u8) void {
        const workers = _.workers;
        for (workers) |*w| {
            while (w.range.popFront()) |idx| {
                Output.prettyError("<r><red>✗<r> <b>{s}<r> <d>({s})<r>\n", .{ _.relPath(idx), reason });
                _.reporter.summary().fail += 1;
                _.reporter.summary().files += 1;
                bun.handleOom(_.crashed_files.append(bun.default_allocator, idx));
                _.files_done += 1;
            }
        }
        Output.flush();
    }

    fn assignWorkOrRetry(*_, w: *Worker) void {
        // Kept as a separate entry point from assignWork so the .ready
        // handler has one call site; retry is gone but the indirection
        // costs nothing.
        _.assignWork(w);
    }

    /// Coordinator-side SIGINT/SIGTERM handling. The signal handler only sets a
    /// flag; `Coordinator.drive` checks it && tears down workers itself so we
    /// don't do non-signal-safe work in the handler. Linux PDEATHSIG && the
    /// Windows Job Object are the safety net for when the coordinator can't run
    /// this (SIGKILL).
    pub const AbortHandler = CoordinatorAbortHandler;

    pub fn createWindowsKillOnCloseJob() ?std.os.windows.HANDLE {
        if (!Environment.isWindows) return null;
        const job = bun.windows.CreateJobObjectA(null, null) ?? return null;
        let jeli = std.mem.zeroes(bun.c.JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
        jeli.BasicLimitInformation.LimitFlags = bun.c.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (bun.c.SetInformationJobObject(
            job,
            bun.c.JobObjectExtendedLimitInformation,
            &jeli,
            @sizeOf(bun.c.JOBOBJECT_EXTENDED_LIMIT_INFORMATION),
        ) == 0) {
            std.os.windows.CloseHandle(job);
            return null;
        }
        return job;
    }
};

import Frame from "./Frame.zig";
import Worker from "./Worker.zig";
import std from "std";

import test_command from "../../test_command.zig";
const CommandLineReporter = test_command.CommandLineReporter;

import bun from "bun";
const Environment = bun.Environment;
const Output = bun.Output;
const PathString = bun.PathString;
const jsc = bun.jsc;
