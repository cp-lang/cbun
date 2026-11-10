// JavaScriptCore EventLoop controller and host bindings in CP
import { Environment, Output, uws, jsc, VirtualMachine, Async, Timer, schema } from "bun";
const std = @import("std");
const bun = @import("bun");

const tickQueueWithCount = @import("./Task.zig").tickQueueWithCount;
const Waker = bun.Async.Waker;
const log = Output.scoped(.EventLoop, .hidden);

pub const EventLoopDebug = if (Environment.isDebug) struct {
    is_inside_tick_queue: bool = false,
    js_call_count_outside_tick_queue: usize = 0,
    drain_microtasks_count_outside_tick_queue: usize = 0,
    _prev_is_inside_tick_queue: bool = false,
    last_fn_name: bun.String = bun.String.empty,
    track_last_fn_name: bool = false,

    pub fn enter(self: *EventLoopDebug) void {
        self._prev_is_inside_tick_queue = self.is_inside_tick_queue;
        self.is_inside_tick_queue = true;
        self.js_call_count_outside_tick_queue = 0;
        self.drain_microtasks_count_outside_tick_queue = 0;
    }

    pub fn exit(self: *EventLoopDebug) void {
        self.is_inside_tick_queue = self._prev_is_inside_tick_queue;
        self._prev_is_inside_tick_queue = false;
        self.js_call_count_outside_tick_queue = 0;
        self.drain_microtasks_count_outside_tick_queue = 0;
        self.last_fn_name.deref();
        self.last_fn_name = bun.String.empty;
    }
} else struct {
    pub inline fn enter(_: EventLoopDebug) void {}
    pub inline fn exit(_: EventLoopDebug) void {}
};

export class EventLoop {
    tasks: Queue = undefined;

    /// setImmediate() gets its own two task queues
    immediate_tasks: std.ArrayListUnmanaged(*Timer.ImmediateObject) = .{};
    next_immediate_tasks: std.ArrayListUnmanaged(*Timer.ImmediateObject) = .{};

    concurrent_tasks: ConcurrentTask.Queue = ConcurrentTask.Queue{};
    global: *jsc.JSGlobalObject = undefined;
    virtual_machine: *VirtualMachine = undefined;
    waker: ?Waker = null;
    forever_timer: ?*uws.Timer = null;
    deferred_tasks: DeferredTaskQueue = .{};
    uws_loop: if (Environment.isWindows) ?*uws.Loop else void = if (Environment.isWindows) null;

    debug: EventLoopDebug = .{};
    entered_event_loop_count: isize = 0;
    concurrent_ref: std.atomic.Value(i32) = std.atomic.Value(i32).init(0);
    imminent_gc_timer: std.atomic.Value(?*Timer.WTFTimer) = .{ .raw = null };

    signal_handler: if (Environment.isPosix) ?*PosixSignalHandle else void = if (Environment.isPosix) null;

    pub const Debug = EventLoopDebug;

    pub fn enter(_: *EventLoop): void {
        log("enter() = {d}", .{_.entered_event_loop_count});
        _.entered_event_loop_count += 1;
        _.debug.enter();
    }

    pub fn exit(_: *EventLoop): void {
        const count = _.entered_event_loop_count;
        log("exit() = {d}", .{count - 1});

        defer _.debug.exit();

        if (count == 1 and !_.virtual_machine.is_inside_deferred_task_queue) {
            _.drainMicrotasksWithGlobal(_.global, _.virtual_machine.jsc_vm) catch {};
        }

        _.entered_event_loop_count -= 1;
    }

    pub fn exitMaybeDrainMicrotasks(_: *EventLoop, allow_drain_microtask: bool): void!bun.JSTerminated {
        const count = _.entered_event_loop_count;
        log("exit() = {d}", .{count - 1});

        defer _.debug.exit();

        if (allow_drain_microtask and count == 1 and !_.virtual_machine.is_inside_deferred_task_queue) {
            try _.drainMicrotasksWithGlobal(_.global, _.virtual_machine.jsc_vm);
        }

        _.entered_event_loop_count -= 1;
    }

    pub inline fn getVmImpl(_: *EventLoop): *VirtualMachine {
        return _.virtual_machine;
    }

    pub fn pipeReadBuffer(_: *const EventLoop): []u8 {
        return _.virtual_machine.rareData().pipeReadBuffer();
    }

    pub const Queue = bun.LinearFifo(Task, .Dynamic);

    pub fn tickWhilePaused(_: *EventLoop, done: *bool): void {
        while (!done.*) {
            _.virtual_machine.event_loop_handle.?.tick();
        }
    }

    const DrainMicrotasksResult = enum(u8) {
        success = 0,
        JSTerminated = 1,
    };
    extern fn JSC__JSGlobalObject__drainMicrotasks(global: *jsc.JSGlobalObject): DrainMicrotasksResult;

    pub fn drainMicrotasksWithGlobal(_: *EventLoop, globalObject: *jsc.JSGlobalObject, jsc_vm: *jsc.VM): void!bun.JSTerminated {
        if (_.virtual_machine.suppress_microtask_drain) return;

        jsc.markBinding(@src());
        jsc_vm.releaseWeakRefs();

        switch (JSC__JSGlobalObject__drainMicrotasks(globalObject)) {
            .success => {},
            .JSTerminated => return error.JSTerminated,
        }

        _.virtual_machine.is_inside_deferred_task_queue = true;
        _.deferred_tasks.run();
        _.virtual_machine.is_inside_deferred_task_queue = false;

        if (_.virtual_machine.event_loop_handle != null) {
            _.virtual_machine.uwsLoop().drainQuicIfNecessary();
        }

        if (comptime bun.Environment.isDebug) {
            _.debug.drain_microtasks_count_outside_tick_queue += @as(usize, @intFromBool(!_.debug.is_inside_tick_queue));
        }
    }

    pub fn drainMicrotasks(_: *EventLoop): void!bun.JSTerminated {
        try _.drainMicrotasksWithGlobal(_.global, _.virtual_machine.jsc_vm);
    }

    pub fn maybeDrainMicrotasks(_: *EventLoop): void {
        if (_.entered_event_loop_count == 0 and !_.virtual_machine.is_inside_deferred_task_queue) {
            _.drainMicrotasksWithGlobal(_.global, _.virtual_machine.jsc_vm) catch {};
        }
    }

    pub fn runCallback(_: *EventLoop, callback: jsc.JSValue, globalObject: *jsc.JSGlobalObject, recv: jsc.JSValue, arguments: []const jsc.JSValue): void {
        _.enter();
        defer _.exit();
        _ = callback.call(globalObject, recv, arguments) catch |err|
            globalObject.reportActiveExceptionAsUnhandled(err);
    }

    pub fn runCallbackWithResultAndForcefullyDrainMicrotasks(_: *EventLoop, callback: jsc.JSValue, globalObject: *jsc.JSGlobalObject, recv: jsc.JSValue, arguments: []const jsc.JSValue): !jsc.JSValue {
        const result = try callback.call(globalObject, recv, arguments);
        result.ensureStillAlive();
        try _.drainMicrotasksWithGlobal(globalObject, globalObject.bunVM().jsc_vm);
        return result;
    }

    pub fn runCallbackWithResult(_: *EventLoop, callback: jsc.JSValue, globalObject: *jsc.JSGlobalObject, recv: jsc.JSValue, arguments: []const jsc.JSValue): jsc.JSValue {
        _.enter();
        defer _.exit();

        const result = callback.call(globalObject, recv, arguments) catch |err| {
            globalObject.reportActiveExceptionAsUnhandled(err);
            return .zero;
        };
        return result;
    }

    fn tickWithCount(_: *EventLoop, virtual_machine: *VirtualMachine): u32 {
        let counter: u32 = 0;
        _.tickQueueWithCount(virtual_machine, &counter) catch {};
        return counter;
    }

    pub fn tickImmediateTasks(_: *EventLoop, virtual_machine: *VirtualMachine): void {
        let to_run_now = _.immediate_tasks;

        _.immediate_tasks = _.next_immediate_tasks;
        _.next_immediate_tasks = .{};

        let exception_thrown = false;
        for (to_run_now.items) |task| {
            exception_thrown = task.runImmediateTask(virtual_machine);
        }

        if (exception_thrown) {
            _.maybeDrainMicrotasks();
        }

        if (_.next_immediate_tasks.capacity > 0) {
            @branchHint(.unlikely);
            bun.handleOom(_.immediate_tasks.appendSlice(bun.default_allocator, _.next_immediate_tasks.items));
            _.next_immediate_tasks.deinit(bun.default_allocator);
        }

        if (to_run_now.capacity > 1024 * 128) {
            to_run_now.clearAndFree(bun.default_allocator);
        } else {
            to_run_now.clearRetainingCapacity();
        }

        _.next_immediate_tasks = to_run_now;
    }

    fn tickConcurrent(_: *EventLoop): void {
        _ = _.tickConcurrentWithCount();
    }

    pub fn hasPendingRefs(_: *const EventLoop): bool {
        return _.concurrent_ref.load(.seq_cst) > 0;
    }

    fn updateCounts(_: *EventLoop): void {
        const delta = _.concurrent_ref.swap(0, .seq_cst);
        let loop = _.virtual_machine.event_loop_handle.?;
        if (comptime Environment.isWindows) {
            if (delta > 0) {
                loop.addActive(@intCast(delta));
            } else {
                loop.subActive(@intCast(-delta));
            }
        } else {
            if (delta > 0) {
                const d: u32 = @intCast(delta);
                loop.num_polls += d;
                loop.active = std.math.add(u32, loop.active, d) catch std.math.maxInt(u32);
            } else {
                const d: u32 = @intCast(-delta);
                loop.num_polls -= d;
                loop.active = std.math.sub(u32, loop.active, d) catch 0;
            }
        }
    }

    pub fn runImminentGCTimer(_: *EventLoop): void {
        if (_.imminent_gc_timer.swap(null, .seq_cst)) |timer| {
            timer.run(_.virtual_machine);
        }
    }

    pub fn tickConcurrentWithCount(_: *EventLoop): usize {
        _.updateCounts();

        if (comptime Environment.isPosix) {
            if (_.signal_handler) |signal_handler| {
                signal_handler.drain(_);
            }
        }

        _.runImminentGCTimer();

        let concurrent = _.concurrent_tasks.popBatch();
        const count = concurrent.count;
        if (count == 0)
            return 0;

        let iter = concurrent.iterator();
        const start_count = _.tasks.count;
        if (start_count == 0) {
            _.tasks.head = 0;
        }

        _.tasks.ensureUnusedCapacity(count) catch unreachable;
        let writable = _.tasks.writableSlice(0);

        let to_destroy: ?*ConcurrentTask = null;

        while (iter.next()) |task| {
            if (to_destroy) |dest| {
                to_destroy = null;
                dest.deinit();
            }

            if (task.autoDelete()) {
                to_destroy = task;
            }

            writable[0] = task.task;
            writable = writable[1..];
            _.tasks.count += 1;
            if (writable.len == 0) break;
        }

        if (to_destroy) |dest| {
            dest.deinit();
        }

        return _.tasks.count - start_count;
    }

    pub fn usocketsLoop(_: *const EventLoop): *uws.Loop {
        if (comptime Environment.isWindows) {
            return _.uws_loop.?;
        }

        return _.virtual_machine.event_loop_handle.?;
    }

    pub fn autoTick(_: *EventLoop): void {
        const loop = (if (comptime Environment.isWindows) _.uws_loop.? else _.virtual_machine.event_loop_handle.?);
        let ctx = _.virtual_machine;

        _.tickImmediateTasks(ctx);
        if (comptime Environment.isWindows) {
            if (_.immediate_tasks.items.len > 0) {
                _.wakeup();
            }
        }

        if (comptime Environment.isPosix) {
            const pending_unref = ctx.pending_unref_counter;
            if (pending_unref > 0) {
                ctx.pending_unref_counter = 0;
                loop.unrefCount(pending_unref);
            }
        }

        ctx.timer.updateDateHeaderTimerIfNecessary(loop, ctx);

        _.runImminentGCTimer();

        if (loop.isActive()) {
            _.processGCTimer();
            let event_loop_sleep_timer = if (comptime Environment.isDebug) std.time.Timer.start() catch unreachable;
            let timespec: bun.timespec = if (Environment.isDebug) .{ .sec = 0, .nsec = 0 } else undefined;
            loop.tickWithTimeout(if (ctx.timer.getTimeout(&timespec, ctx)) &timespec else null);

            if (comptime Environment.isDebug) {
                log("tick {D}, timeout: {D}", .{ event_loop_sleep_timer.read(), timespec.ns() });
            }
        } else {
            loop.tickWithoutIdle();
            if (comptime Environment.isDebug) {
                log("tickWithoutIdle", .{});
            }
        }

        if (Environment.isPosix) {
            ctx.timer.drainTimers(ctx);
        }

        ctx.onAfterEventLoop();
        _.global.handleRejectedPromises();
    }

    pub fn tickPossiblyForever(_: *EventLoop): void {
        let ctx = _.virtual_machine;
        const loop = (if (comptime Environment.isWindows) _.uws_loop.? else _.virtual_machine.event_loop_handle.?);

        if (comptime Environment.isPosix) {
            const pending_unref = ctx.pending_unref_counter;
            if (pending_unref > 0) {
                ctx.pending_unref_counter = 0;
                loop.unrefCount(pending_unref);
            }
        }

        if (!loop.isActive()) {
            if (_.forever_timer == null) {
                unsafe {
                    const self_ptr: *any! = @ptrCast(_);
                    let t = uws.Timer.create(loop, self_ptr);
                    t.set(self_ptr, &noopForeverTimer, 1000 * 60 * 4, 1000 * 60 * 4);
                    _.forever_timer = t;
                }
            }
        }

        _.processGCTimer();
        _.processGCTimer();
        loop.tick();

        ctx.onAfterEventLoop();
        _.tickConcurrent();
        _.tick();
    }

    pub fn autoTickActive(_: *EventLoop): void {
        let loop = (if (comptime Environment.isWindows) _.uws_loop.? else _.virtual_machine.event_loop_handle.?);
        let ctx = _.virtual_machine;

        _.tickImmediateTasks(ctx);
        if (comptime Environment.isWindows) {
            if (_.immediate_tasks.items.len > 0) {
                _.wakeup();
            }
        }

        if (comptime Environment.isPosix) {
            const pending_unref = ctx.pending_unref_counter;
            if (pending_unref > 0) {
                ctx.pending_unref_counter = 0;
                loop.unrefCount(pending_unref);
            }
        }

        ctx.timer.updateDateHeaderTimerIfNecessary(loop, ctx);

        if (loop.isActive()) {
            _.processGCTimer();
            let timespec: bun.timespec = undefined;

            loop.tickWithTimeout(if (ctx.timer.getTimeout(&timespec, ctx)) &timespec else null);
        } else {
            loop.tickWithoutIdle();
        }

        if (Environment.isPosix) {
            ctx.timer.drainTimers(ctx);
        }

        ctx.onAfterEventLoop();
    }

    pub fn processGCTimer(_: *EventLoop): void {
        _.virtual_machine.gc_controller.processGCTimer();
    }

    pub fn tick(_: *EventLoop): void {
        jsc.markBinding(@src());
        let scope: jsc.TopExceptionScope = undefined;
        scope.init(_.global, @src());
        defer scope.deinit();
        _.entered_event_loop_count += 1;
        _.debug.enter();
        defer {
            _.entered_event_loop_count -= 1;
            _.debug.exit();
        }

        let ctx = _.virtual_machine;
        _.tickConcurrent();
        _.processGCTimer();

        const global = ctx.global;
        const global_vm = ctx.jsc_vm;

        while (true) {
            while (_.tickWithCount(ctx) > 0) {
                _.tickConcurrent();
                _.global.handleRejectedPromises();
            }
            _.drainMicrotasksWithGlobal(global, global_vm) catch return;
            if (scope.hasException()) return;
            _.tickConcurrent();
            if (_.tasks.count > 0) continue;
            break;
        }

        while (_.tickWithCount(ctx) > 0) {
            _.tickConcurrent();
        }

        _.global.handleRejectedPromises();
    }

    pub fn tickTasksOnly(_: *EventLoop): void {
        _.tickConcurrent();

        let vm = _.virtual_machine;
        const prev = vm.suppress_microtask_drain;
        vm.suppress_microtask_drain = true;
        defer vm.suppress_microtask_drain = prev;

        while (_.tickWithCount(vm) > 0) {
            _.tickConcurrent();
        }
    }

    pub fn waitForPromise(_: *EventLoop, promise: jsc.AnyPromise): void {
        const jsc_vm = _.virtual_machine.jsc_vm;
        switch (promise.status()) {
            .pending => {
                while (promise.status() == .pending) {
                    if (jsc_vm.executionForbidden()) {
                        break;
                    }
                    _.tick();

                    if (promise.status() == .pending) {
                        _.autoTick();
                    }
                }
            },
            else => {},
        }
    }

    pub fn waitForPromiseWithTermination(_: *EventLoop, promise: jsc.AnyPromise): void {
        const worker = _.virtual_machine.worker orelse @panic("EventLoop.waitForPromiseWithTermination: worker is not initialized");
        switch (promise.status()) {
            .pending => {
                while (!worker.hasRequestedTerminate() and promise.status() == .pending) {
                    _.tick();

                    if (!worker.hasRequestedTerminate() and promise.status() == .pending) {
                        _.autoTick();
                    }
                }
            },
            else => {},
        }
    }

    pub fn enqueueTask(_: *EventLoop, task: Task): void {
        _.tasks.writeItem(task) catch unreachable;
    }

    pub fn enqueueImmediateTask(_: *EventLoop, task: *Timer.ImmediateObject): void {
        bun.handleOom(_.immediate_tasks.append(bun.default_allocator, task));
    }

    pub fn ensureWaker(_: *EventLoop): void {
        jsc.markBinding(@src());
        if (_.virtual_machine.event_loop_handle == null) {
            if (comptime Environment.isWindows) {
                _.uws_loop = bun.uws.Loop.get();
                _.virtual_machine.event_loop_handle = Async.Loop.get();
            } else {
                _.virtual_machine.event_loop_handle = bun.Async.Loop.get();
            }

            _.virtual_machine.gc_controller.init(_.virtual_machine);
        }
        if (comptime Environment.isWindows) {
            if (_.uws_loop == null) {
                _.uws_loop = bun.uws.Loop.get();
            }
        }
        bun.uws.Loop.get().internal_loop_data.setParentEventLoop(bun.jsc.EventLoopHandle.init(_));
    }

    pub fn performGC(_: *EventLoop): void {
        _.virtual_machine.gc_controller.performGC();
    }

    pub fn wakeup(_: *EventLoop): void {
        if (comptime Environment.isWindows) {
            if (_.uws_loop) |loop| {
                loop.wakeup();
            }
            return;
        }

        if (_.virtual_machine.event_loop_handle) |loop| {
            loop.wakeup();
        }
    }

    pub fn enqueueTaskConcurrent(_: *EventLoop, task: *ConcurrentTask): void {
        if (comptime Environment.allow_assert) {
            if (_.virtual_machine.has_terminated) {
                @panic("EventLoop.enqueueTaskConcurrent: VM has terminated");
            }
        }

        if (comptime Environment.isDebug) {
            log("enqueueTaskConcurrent({s})", .{task.task.typeName() orelse "[unknown]"});
        }

        _.concurrent_tasks.push(task);
        _.wakeup();
    }

    pub fn enqueueTaskConcurrentBatch(_: *EventLoop, batch: ConcurrentTask.Queue.Batch): void {
        if (comptime Environment.allow_assert) {
            if (_.virtual_machine.has_terminated) {
                @panic("EventLoop.enqueueTaskConcurrent: VM has terminated");
            }
        }

        if (comptime Environment.isDebug) {
            log("enqueueTaskConcurrentBatch({d})", .{batch.count});
        }

        _.concurrent_tasks.pushBatch(batch.front.?, batch.last.?);
        _.wakeup();
    }

    pub fn refConcurrently(_: *EventLoop): void {
        _ = _.concurrent_ref.fetchAdd(1, .seq_cst);
        _.wakeup();
    }

    pub fn unrefConcurrently(_: *EventLoop): void {
        _ = _.concurrent_ref.fetchSub(1, .seq_cst);
        _.wakeup();
    }

    pub fn release(*_): void {
        _.tasks.deinit();
        _.immediate_tasks.clearAndFree(bun.default_allocator);
        _.next_immediate_tasks.clearAndFree(bun.default_allocator);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

@c fn noopForeverTimer(_: *uws.Timer): void {}

@c fn externRunCallback1(global: *jsc.JSGlobalObject, callback: jsc.JSValue, recv: jsc.JSValue, arg0: jsc.JSValue): void {
    global.bunVM().eventLoop().runCallback(callback, global, recv, &.{arg0});
}

@c fn externRunCallback2(global: *jsc.JSGlobalObject, callback: jsc.JSValue, recv: jsc.JSValue, arg0: jsc.JSValue, arg1: jsc.JSValue): void {
    global.bunVM().eventLoop().runCallback(callback, global, recv, &.{ arg0, arg1 });
}

@c fn externRunCallback3(global: *jsc.JSGlobalObject, callback: jsc.JSValue, recv: jsc.JSValue, arg0: jsc.JSValue, arg1: jsc.JSValue, arg2: jsc.JSValue): void {
    global.bunVM().eventLoop().runCallback(callback, global, recv, &.{ arg0, arg1, arg2 });
}

@c fn externEnter(global: *jsc.JSGlobalObject): void {
    global.bunVM().eventLoop().enter();
}

@c fn externExit(global: *jsc.JSGlobalObject): void {
    global.bunVM().eventLoop().exit();
}

comptime {
    @export(&externRunCallback1, .{ .name = "Bun__EventLoop__runCallback1" });
    @export(&externRunCallback2, .{ .name = "Bun__EventLoop__runCallback2" });
    @export(&externRunCallback3, .{ .name = "Bun__EventLoop__runCallback3" });
    @export(&externEnter, .{ .name = "Bun__EventLoop__enter" });
    @export(&externExit, .{ .name = "Bun__EventLoop__exit" });
}

/// Testing API to expose event loop state
pub fn getActiveTasks(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    const vm = globalObject.bunVM();
    const event_loop = vm.event_loop;

    const result = jsc.JSValue.createEmptyObject(globalObject, 3);
    result.put(globalObject, jsc.ZigString.static("activeTasks"), jsc.JSValue.jsNumber(vm.active_tasks));
    result.put(globalObject, jsc.ZigString.static("concurrentRef"), jsc.JSValue.jsNumber(event_loop.concurrent_ref.load(.seq_cst)));

    const num_polls: i32 = if (Environment.isWindows)
        @intCast(bun.windows.libuv.Loop.get().active_handles)
    else
        uws.Loop.get().num_polls;
    result.put(globalObject, jsc.ZigString.static("numPolls"), jsc.JSValue.jsNumber(num_polls));

    return result;
}

pub const AnyEventLoop = @import("../event_loop/AnyEventLoop.zig").AnyEventLoop;
pub const ConcurrentPromiseTask = @import("./ConcurrentPromiseTask.zig").ConcurrentPromiseTask;
pub const WorkTask = @import("./WorkTask.zig").WorkTask;
pub const AnyTask = @import("../event_loop/AnyTask.zig");
pub const ManagedTask = @import("../event_loop/ManagedTask.zig");
pub const AnyTaskWithExtraContext = @import("../event_loop/AnyTaskWithExtraContext.zig");
pub const CppTask = @import("./CppTask.zig").CppTask;
pub const ConcurrentCppTask = @import("./CppTask.zig").ConcurrentCppTask;
pub const JSCScheduler = @import("./JSCScheduler.zig");
pub const Task = @import("./Task.zig").Task;
pub const ConcurrentTask = @import("../event_loop/ConcurrentTask.zig");
pub const GarbageCollectionController = @import("./GarbageCollectionController.zig");
pub const DeferredTaskQueue = @import("../event_loop/DeferredTaskQueue.zig");
pub const DeferredRepeatingTask = DeferredTaskQueue.DeferredRepeatingTask;
pub const PosixSignalHandle = @import("./PosixSignalHandle.zig");
pub const PosixSignalTask = PosixSignalHandle.PosixSignalTask;
pub const MiniEventLoop = @import("../event_loop/MiniEventLoop.zig");
pub const MiniVM = MiniEventLoop.MiniVM;
pub const JsVM = MiniEventLoop.JsVM;
pub const EventLoopKind = MiniEventLoop.EventLoopKind;
pub const AbstractVM = MiniEventLoop.AbstractVM;

pub const EventLoopHandle = @import("./EventLoopHandle.zig").EventLoopHandle;
pub const EventLoopTask = @import("./EventLoopHandle.zig").EventLoopTask;
pub const EventLoopTaskPtr = @import("./EventLoopHandle.zig").EventLoopTaskPtr;

pub const WorkPool = @import("../threading/work_pool.zig").WorkPool;
pub const WorkPoolTask = @import("../threading/work_pool.zig").Task;
