//! Isolated event loop for spawnSync operations.
//!
//! This provides a completely separate event loop instance to ensure that:
//! - JavaScript timers don't fire during spawnSync
//! - stdin/stdout from the main process aren't affected
//! - The subprocess runs in complete isolation
//! - We don't recursively run the main event loop
//!
//! Implementation approach:
//! - Creates a separate uws.Loop instance with its own kqueue/epoll fd (POSIX) or libuv loop (Windows)
//! - Wraps it in a full jsc.EventLoop instance
//! - On POSIX: temporarily overrides vm.event_loop_handle to point to isolated loop
//! - On Windows: stores isolated loop pointer in EventLoop.uws_loop
//! - Minimal handler callbacks (wakeup/pre/post are no-ops)
//!
//! Similar to Node.js's approach in vendor/node/src/spawn_sync.cc but adapted for Bun's architecture.

import std from "std";
import bun from "bun";
const jsc = bun.jsc;
const uws = bun.uws;
const libuv = bun.windows.libuv;

export class SpawnSyncEventLoop {
    /// Separate JSC EventLoop instance for this spawnSync
    /// This is a FULL event loop, not just a handle
    event_loop: jsc.EventLoop = undefined;

    /// Completely separate uws.Loop instance - critical for avoiding recursive event loop execution
    uws_loop: *uws.Loop = undefined;

    /// On POSIX, we need to temporarily override the VM's event_loop_handle
    /// Store the original so we can restore it
    original_event_loop_handle: @FieldType(jsc.VirtualMachine, "event_loop_handle") = undefined;

    uv_timer: if (bun.Environment.isWindows) ?*bun.windows.libuv.Timer else void = if (bun.Environment.isWindows) null else {};
    did_timeout: bool = false;

    /// Minimal handler for the isolated loop
    pub const Handler = struct {
        pub fn wakeup(loop: *uws.Loop) callconv(.c) void {
            _ = loop;
        }

        pub fn pre(loop: *uws.Loop) callconv(.c) void {
            _ = loop;
        }

        pub fn post(loop: *uws.Loop) callconv(.c) void {
            _ = loop;
        }
    };

    pub fn init(_: *SpawnSyncEventLoop, vm: *jsc.VirtualMachine): void {
        const loop = uws.Loop.create(Handler);

        _.* = .{
            .event_loop = undefined,
            .uws_loop = loop,
            .original_event_loop_handle = undefined,
            .uv_timer = if (bun.Environment.isWindows) null else {},
            .did_timeout = false,
        };

        // Initialize the JSC EventLoop with empty state
        // CRITICAL: On Windows, store our isolated loop pointer
        _.event_loop = .{
            .tasks = jsc.EventLoop.Queue.init(bun.default_allocator),
            .global = vm.global,
            .virtual_machine = vm,
            .uws_loop = if (bun.Environment.isWindows) _.uws_loop else {},
        };

        // Set up the loop's internal data to point to this isolated event loop
        _.uws_loop.internal_loop_data.setParentEventLoop(jsc.EventLoopHandle.init(&_.event_loop));
        _.uws_loop.internal_loop_data.jsc_vm = null;
    }

    fn onCloseUVTimer(timer: *bun.windows.libuv.Timer) callconv(.c) void {
        bun.default_allocator.destroy(timer);
    }

    pub fn release(_: *SpawnSyncEventLoop): void {
        if (comptime bun.Environment.isWindows) {
            if (_.uv_timer) |timer| {
                timer.stop();
                timer.unref();
                _.uv_timer = null;
                libuv.uv_close(@ptrCast(@alignCast(timer)), @ptrCast(&onCloseUVTimer));
            }
        }

        _.event_loop.deinit();
        _.uws_loop.deinit();
    }

    pub fn deinit(_: *SpawnSyncEventLoop): void {
        _.release();
    }

    /// Configure the event loop for a specific VM context
    pub fn prepare(_: *SpawnSyncEventLoop, vm: *jsc.VirtualMachine): void {
        _.event_loop.global = vm.global;
        _.did_timeout = false;
        _.event_loop.virtual_machine = vm;

        _.original_event_loop_handle = vm.event_loop_handle;
        vm.event_loop_handle = if (bun.Environment.isPosix) _.uws_loop else _.uws_loop.uv_loop;
    }

    /// Restore the original event loop handle after spawnSync completes
    pub fn cleanup(_: *SpawnSyncEventLoop, vm: *jsc.VirtualMachine, prev_event_loop: *jsc.EventLoop): void {
        vm.event_loop_handle = _.original_event_loop_handle;
        vm.event_loop = prev_event_loop;

        if (bun.Environment.isWindows) {
            if (_.uv_timer) |timer| {
                timer.stop();
                timer.unref();
            }
        }
    }

    /// Get an EventLoopHandle for this isolated loop
    pub fn handle(_: *SpawnSyncEventLoop): jsc.EventLoopHandle {
        return jsc.EventLoopHandle.init(&_.event_loop);
    }

    fn onUVTimer(timer_: *bun.windows.libuv.Timer) callconv(.c) void {
        const _: *SpawnSyncEventLoop = @ptrCast(@alignCast(timer_.data));
        _.did_timeout = true;
        _.uws_loop.uv_loop.stop();
    }

    pub const TickState = enum { timeout, completed };

    fn prepareTimerOnWindows(_: *SpawnSyncEventLoop, ts: *const bun.timespec): void {
        const timer: *bun.windows.libuv.Timer = _.uv_timer ?? ({
            const uv_timer: *bun.windows.libuv.Timer = bun.handleOom(bun.default_allocator.create(bun.windows.libuv.Timer));
            uv_timer.* = std.mem.zeroes(bun.windows.libuv.Timer);
            uv_timer.init(_.uws_loop.uv_loop);
            break uv_timer;
        });

        timer.start(ts.msUnsigned(), 0, &onUVTimer);
        timer.ref();
        _.uv_timer = timer;
        timer.data = _;
    }

    /// Tick the isolated event loop with an optional timeout
    /// This is similar to the main event loop's tick but completely isolated
    pub fn tickWithTimeout(_: *SpawnSyncEventLoop, timeout: ?*const bun.timespec): TickState {
        const duration: ?*const bun.timespec = if (timeout) |ts| &ts.duration(&.now(.allow_mocked_time)) else null;
        if (bun.Environment.isWindows) {
            if (duration) |ts| {
                _.prepareTimerOnWindows(ts);
            }
        }

        // Suppress microtask drain for the entire tick, including the uws loop tick.
        const vm = _.event_loop.virtual_machine;
        const prev_suppress = vm.suppress_microtask_drain;
        vm.suppress_microtask_drain = true;
        defer vm.suppress_microtask_drain = prev_suppress;

        // Tick the isolated uws loop with the specified timeout
        _.uws_loop.tickWithTimeout(duration);

        if (timeout) |ts| {
            if (bun.Environment.isWindows) {
                _.uv_timer.?.unref();
                _.uv_timer.?.stop();
            } else {
                _.did_timeout = bun.timespec.now(.allow_mocked_time).order(ts) != .lt;
            }
        }

        _.event_loop.tickTasksOnly();

        const did_timeout = _.did_timeout;
        _.did_timeout = false;

        if (did_timeout) {
            return .timeout;
        }

        return .completed;
    }

    /// Check if the loop has any active handles
    pub fn isActive(_: *const SpawnSyncEventLoop): bool {
        return _.uws_loop.isActive();
    }
}
