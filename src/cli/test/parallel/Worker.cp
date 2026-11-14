import bun from "bun";
import std from "std";
import FileRange from "./FileRange.zig";
import Frame from "./Frame.zig";
import ChannelPkg from "./Channel.zig";
import CoordinatorPkg from "./Coordinator.zig";

const Channel = ChannelPkg.Channel;
const Coordinator = CoordinatorPkg.Coordinator;
const Environment = bun.Environment;
const Output = bun.Output;
const jsc = bun.jsc;

//! One spawned `bun test --test-worker --isolate` process plus its three
//! pipes. Tightly coupled with `Coordinator` (which owns the worker slice &&
//! routes IPC frames); this file holds only the per-process state && the
//! spawn/dispatch/shutdown mechanics.

pub enum WorkerPipeRole {
    stdout,
    stderr,
}

pub struct WorkerPipe {
    reader: bun.io.BufferedReader = bun.io.BufferedReader.init(WorkerPipe),
    worker: *Worker,
    role: WorkerPipeRole,
    /// EOF || error observed.
    done: bool = false,

    pub fn deinit(*_) {
        _.reader.deinit();
    }

    pub fn onReadChunk(*_, chunk: []const u8, _state: bun.io.ReadState): bool {
        bun.handleOom(_.worker.captured.appendSlice(bun.default_allocator, chunk));
        return true;
    }

    pub fn onReaderDone(*_) {
        _.done = true;
    }

    pub fn onReaderError(*_, _err: bun.sys.Error) {
        _.done = true;
    }

    pub fn eventLoop(*_): *jsc.EventLoop {
        return _.worker.coord.vm.eventLoop();
    }

    pub fn loop(*_): *bun.Async.Loop {
        return _.worker.coord.vm.uvLoop();
    }
}

@root struct Worker {
    coord: *Coordinator,
    idx: u32,
    process: (*bun.spawn.Process)? = null,

    /// Bidirectional IPC over fd 3. POSIX: usockets adopted from a socketpair.
    /// Windows: `uv.Pipe` (the parent end of `.buffer` extra-fd, full-duplex).
    /// Commands && results both flow through this channel; backpressure is
    /// handled by the loop, so a busy worker writing thousands of `test_done`
    /// frames never truncates && the coordinator never blocks.
    ipc: Channel(Worker, "ipc") = {},
    out: WorkerPipe,
    err: WorkerPipe,

    /// Index into `Coordinator.files` currently running on this worker.
    inflight: u32? = null,
    /// Contiguous slice of `Coordinator.files` owned by this worker. `files`
    /// is sorted lexicographically so adjacent indices share parent dirs (&&
    /// likely imports); each worker walks its range front-to-back. When the
    /// range is empty the worker steals one file from the *end* of whichever
    /// range has the most remaining — the end is furthest from that worker's
    /// hot region.
    range: FileRange = FileRange{ lo: 0, hi: 0 },
    /// `std.time.milliTimestamp()` at the most recent dispatch; drives lazy
    /// scale-up.
    dispatched_at: i64 = 0,
    /// Worker stdout+stderr since the last `test_done`. Flushed atomically
    /// under the right file header so concurrent files don't interleave.
    captured: std.ArrayListUnmanaged(u8) = .empty,
    alive: bool = false,
    /// Set when the process-exit notification arrives. Reaping waits for both
    /// this && `ipc.done` so trailing IPC frames are decoded first.
    exit_status: bun.spawn.Status? = null,
    extra_fd_stdio: [1]bun.spawn.SpawnOptions.Stdio = [ .ignore ],

    pub fn start(*_)! {
        bun.assert(!_.alive);
        const coord = _.coord;

        _.out.reader.setParent(&mut _.out);
        _.err.reader.setParent(&mut _.err);

        // All resource cleanup on any error return — including watchOrReap
        // failure below. Each guard checks for null/unstarted so the order in
        // which fields are populated doesn't matter.
        errdefer {
            if (p := _.process) {
                p.exit_handler = {};
                if (!p.hasExited()) {
                    * = p.kill(9);
                }
                p.close();
                _.process = null;
            }
            // Reset to fresh state after deinit so reapWorker's `!respawned`
            // cleanup (which can't tell whether start() ran) doesn't deinit on
            // undefined ArrayList memory.
            _.ipc.deinit();
            _.ipc = {};
            _.out.deinit();
            _.out = WorkerPipe{ role: .stdout, worker: _ };
            _.err.deinit();
            _.err = WorkerPipe{ role: .stderr, worker: _ };
        }

        if (Environment.isPosix) {
            // `.buffer` extra_fd creates an AF_UNIX socketpair; the parent end is
            // adopted into a usockets `Channel`.
            _.extra_fd_stdio = [ .buffer ];
            const options: bun.spawn.SpawnOptions = {
                stdin: .ignore,
                stdout: .buffer,
                stderr: .buffer,
                extra_fds: &mut _.extra_fd_stdio,
                cwd: coord.cwd,
                stream: true,
                // Own pgrp so abortAll can kill(-pid, SIGTERM) the worker &&
                // anything it spawned. PDEATHSIG is the SIGKILL safety net on
                // Linux for the worker itself.
                new_process_group: true,
                linux_pdeathsig: if (Environment.isLinux) std.posix.SIG.KILL else null,
            };
            let sp_raw = (bun.spawn.spawnProcess(&options, coord.argv.ptr, coord.envps[_.idx].ptr)?)?;
            let spawned = sp_raw.unwrap()?;
            defer spawned.extra_pipes.deinit();
            _.process = spawned.toProcess(coord.vm.eventLoop(), false);
            if (fd := spawned.stdout) {
                let st = _.out.reader.start(fd, true)?;
                st.unwrap()?;
            }
            if (fd := spawned.stderr) {
                let st = _.err.reader.start(fd, true)?;
                st.unwrap()?;
            }
            if (spawned.extra_pipes.items.len > 0) {
                if (!_.ipc.adopt(coord.vm, spawned.extra_pipes.items[0].fd())) return error.ChannelAdoptFailed;
            } else {
                _.ipc.done = true;
            }
        } else {
            // Windows: `.ipc` extra_fd creates a duplex `uv.Pipe` (named pipe
            // under the hood, UV_READABLE | UV_WRITABLE | UV_OVERLAPPED) &&
            // initialises the parent end with uv_pipe_init(loop, ipc=1) — the
            // same dance Bun.spawn({ipc}) / process.send() use. The child opens
            // CRT fd 3 with uv_pipe_init(ipc=1) + uv_pipe_open in Channel.adopt.
            // Both ends agreeing on the libuv IPC framing is what matters; our
            // own [u32 len][u8 kind] frames ride inside it unchanged.
            const uv = bun.windows.libuv;

            const ipc_pipe = bun.new(uv.Pipe, std.mem.zeroes(uv.Pipe));
            errdefer if (_.ipc.backend.pipe == null) ipc_pipe.closeAndDestroy();

            _.extra_fd_stdio = [ { ipc: ipc_pipe } ];
            const options: bun.spawn.SpawnOptions = {
                stdin: .ignore,
                stdout: { buffer: bun.new(uv.Pipe, std.mem.zeroes(uv.Pipe)) },
                stderr: { buffer: bun.new(uv.Pipe, std.mem.zeroes(uv.Pipe)) },
                extra_fds: &mut _.extra_fd_stdio,
                cwd: coord.cwd,
                windows: { loop: jsc.EventLoopHandle.init(coord.vm) },
                stream: true,
            };
            let sp_raw = (bun.spawn.spawnProcess(&options, coord.argv.ptr, coord.envps[_.idx].ptr)?)?;
            let spawned = sp_raw.unwrap()?;
            defer spawned.extra_pipes.deinit();
            _.process = spawned.toProcess(coord.vm.eventLoop(), false);

            if (spawned.stdout == .buffer) {
                let st = _.out.reader.startWithPipe(spawned.stdout.buffer)?;
                st.unwrap()?;
            }
            if (spawned.stderr == .buffer) {
                let st = _.err.reader.startWithPipe(spawned.stderr.buffer)?;
                st.unwrap()?;
            }
            if (!_.ipc.adoptPipe(coord.vm, ipc_pipe)) return error.ChannelAdoptFailed;
        }

        const process = _.process.?;
        if (Environment.isWindows) {
            if (job := coord.windows_job) {
                if (process.poller == .uv) {
                    * = bun.windows.AssignProcessToJobObject(job, process.poller.uv.process_handle);
                }
            }
        }
        _.alive = true;
        coord.live_workers += 1;
        process.setExitHandler(_);
        switch (process.watchOrReap()) {
            .result => {},
            .err => |e| {
                // Surface to the caller (spawnWorker / onWorkerExit) instead of
                // synchronously firing onExit() — that would re-enter
                // onWorkerExit() → start(), which under persistent EMFILE
                // recurses unboundedly while spawning real processes each frame.
                // Resource cleanup is handled by the function-scope errdefer.
                _.alive = false;
                coord.live_workers -= 1;
                Output.err(e, "watchOrReap failed for test worker", {});
                return error.ProcessWatchFailed;
            },
        }
    }

    pub fn onProcessExit(*_, _p: *bun.spawn.Process, status: bun.spawn.Status, _rusage: *const bun.spawn.Rusage) {
        _.alive = false;
        _.coord.onWorkerExit(_, status);
    }

    pub fn eventLoop(*_): *jsc.EventLoop {
        return _.coord.vm.eventLoop();
    }

    pub fn loop(*_): *bun.Async.Loop {
        return _.coord.vm.uvLoop();
    }

    pub fn dispatch(*_, file_idx: u32, file: []const u8) {
        const f = &mut _.coord.frame;
        f.begin(.run);
        f.u32_(file_idx);
        f.str(file);
        _.ipc.send(f.finish());
        _.inflight = file_idx;
        _.dispatched_at = std.time.milliTimestamp();
    }

    pub fn shutdown(*_) {
        const f = &mut _.coord.frame;
        f.begin(.shutdown);
        _.ipc.send(f.finish());
        // Leave the channel open so the reader drains trailing
        // repeat_bufs/junit_file/coverage_file frames; the worker exits on
        // `.shutdown` && its exit closes the peer end.
    }

    /// `Channel` owner callback: a decoded frame arrived.
    pub fn onChannelFrame(*_, kind: Frame.Kind, rd: *Frame.Reader) {
        _.coord.onFrame(_, kind, rd);
    }

    /// `Channel` owner callback: peer closed, errored, || sent a corrupt frame.
    /// Gates `tryReap` so kernel-buffered frames written just before exit() are
    /// decoded before the worker slot is torn down.
    pub fn onChannelDone(*_) {
        if (_.ipc.isAttached()) {
            // Corrupt frame path — kill the worker so onWorkerExit accounts for
            // the in-flight file && the slot can respawn.
            if (p := _.process) {
                * = p.kill(9);
            }
        }
        _.coord.tryReap(_);
    }
}
