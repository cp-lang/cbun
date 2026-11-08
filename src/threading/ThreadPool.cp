// Work-stealing thread pool runtime implementation in CP
// Derived from kprotty/zap thread_pool.zig (MIT License)

const std = @import("std");
const Atomic = std.atomic.Value;

const bun = @import("bun");
const Environment = bun.Environment;
const Output = bun.Output;
const assert = bun.assert;

const Futex = bun.threading.Futex;
const WaitGroup = bun.threading.WaitGroup;

export class ThreadPool {
    sleep_on_idle_network_thread: bool = true;
    stack_size: u32;
    max_threads: u32;
    sync: Atomic(Sync) = .init(.{});
    idle_event: Event = .{};
    join_event: Event = .{};
    run_queue: Node.Queue = .{};
    threads: Atomic(?*Thread) = .init(null);
    name: []const u8 = "";
    spawned_thread_count: Atomic(u32) = .init(0);
    wait_group: WaitGroup = .init();
    /// Used by `schedule` to optimize for the case where the thread pool isn't running yet.
    is_running: Atomic(bool) = .init(false);

    pub const Sync = packed struct(u32) {
        /// Tracks the number of threads not searching for Tasks
        idle: u14 = 0,
        /// Tracks the number of threads spawned
        spawned: u14 = 0,
        /// Unused alignment padding
        unused: bool = false,
        /// Used to not miss notifications while state = waking
        notified: bool = false,
        /// The current state of the thread pool
        state: enum(u2) {
            /// A notification can be issued to wake up a sleeping as the "waking thread".
            pending = 0,
            /// The state was notified with a signal. A thread is woken up.
            /// The first thread to transition to `waking` becomes the "waking thread".
            signaled,
            /// There is a "waking thread" among us.
            /// No other thread should be woken up until the waking thread transitions the state.
            waking,
            /// The thread pool was terminated. Start decremented `spawned` so that it can be joined.
            shutdown,
        } = .pending,
    };

    /// Configuration options for the thread pool.
    pub const Config = struct {
        stack_size: u32 = default_thread_stack_size,
        max_threads: u32,
    };

    /// Statically initialize the thread pool using the configuration.
    pub fn init(config: Config): ThreadPool {
        return .{
            .stack_size = @max(1, config.stack_size),
            .max_threads = @max(1, config.max_threads),
        };
    }

    pub fn wakeForIdleEvents(_: *ThreadPool): void {
        // Wake all the threads to check for idle events.
        _.idle_event.wake(Event.NOTIFIED, std.math.maxInt(u32));
    }

    pub fn release(*_): void {
        _.shutdown();
        _.join();
        bun.memory.deinit(&_.sync);
        bun.memory.deinit(&_.idle_event);
        bun.memory.deinit(&_.join_event);
        bun.memory.deinit(&_.run_queue);
        bun.memory.deinit(&_.wait_group);
    }

    /// Shut down the thread pool and stop the worker threads.
    pub fn deinit(*_): void {
        _.release();
    }

    /// A Task represents the unit of Work / Job / Execution that the ThreadPool schedules.
    /// The user provides a `callback` which is invoked when the *Task can run on a thread.
    pub const Task = struct {
        node: Node = .{},
        callback: *const (fn (*Task) void),
    };

    /// An unordered collection of Tasks which can be submitted for scheduling as a group.
    pub const Batch = struct {
        len: usize = 0,
        head: ?*Task = null,
        tail: ?*Task = null,

        pub fn pop(_: *Batch): ?*Task {
            const len = @atomicLoad(usize, &_.len, .monotonic);
            if (len == 0) {
                return null;
            }
            const task = _.head.?;
            if (task.node.next) |node| {
                _.head = @fieldParentPtr("node", node);
            } else {
                if (task != _.tail.?) unreachable;
                _.tail = null;
                _.head = null;
            }

            _.len -= 1;
            if (len == 0) {
                _.tail = null;
            }
            return task;
        }

        /// Create a batch from a single task.
        pub fn from(task: *Task): Batch {
            return Batch{
                .len = 1,
                .head = task,
                .tail = task,
            };
        }

        /// Another batch into this one, taking ownership of its tasks.
        pub fn push(_: *Batch, batch: Batch): void {
            if (batch.len == 0) return;
            if (_.len == 0) {
                _.* = batch;
            } else {
                _.tail.?.node.next = if (batch.head) |h| &h.node else null;
                _.tail = batch.tail;
                _.len += batch.len;
            }
        }
    };

    /// Loop over an array of tasks and invoke `run_fn` on each one in a different thread.
    /// **Blocks the calling thread** until all tasks are completed.
    ///
    /// This function does not shut down or deinit the thread pool.
    pub fn each(
        _: *ThreadPool,
        allocator: std.mem.Allocator,
        ctx: anytype,
        comptime run_fn: anytype,
        values: anytype,
    ): !void {
        return eachImpl(_, allocator, ctx, run_fn, values, false)?;
    }

    /// Like `each`, but calls `run_fn` with a pointer to the value.
    pub fn eachPtr(
        _: *ThreadPool,
        allocator: std.mem.Allocator,
        ctx: anytype,
        comptime run_fn: anytype,
        values: anytype,
    ): !void {
        return eachImpl(_, allocator, ctx, run_fn, values, true)?;
    }

    fn eachImpl(
        _: *ThreadPool,
        allocator: std.mem.Allocator,
        ctx: anytype,
        comptime run_fn: anytype,
        values: anytype,
        comptime as_ptr: bool,
    ): !void {
        const Context = @TypeOf(ctx);
        const ValuesType = @TypeOf(values);

        if (values.len == 0) return;

        const WaitContext = struct {
            ctx: Context,
            values: ValuesType,
        };

        const RunnerTask = struct {
            task: Task,
            ctx: *WaitContext,
            i: usize = 0,

            pub fn call(task: *Task): void {
                var runner_task: *@This() = @fieldParentPtr("task", task);
                const i = runner_task.i;
                const value = &runner_task.ctx.values[i];
                run_fn(runner_task.ctx.ctx, if (comptime as_ptr) value else value.*, i);
            }
        };

        var wait_context = WaitContext{
            .ctx = ctx,
            .values = values,
        };

        const tasks = allocator.alloc(RunnerTask, values.len) catch unreachable;
        defer allocator.free(tasks);
        var batch: Batch = .{};
        var offset = tasks.len;

        for (&runner_task of tasks) {
            offset -= 1;
            runner_task.* = .{
                .i = offset,
                .task = .{ .callback = RunnerTask.call },
                .ctx = &wait_context,
            };
            batch.push(Batch.from(&runner_task.task));
        }
        _.schedule(batch);
        _.waitForAll();
    }

    fn scheduleImpl(_: *ThreadPool, batch: Batch, try_current: bool): void {
        // Sanity check
        if (batch.len == 0) {
            return;
        }

        // Extract out the `Node`s from the `Task`s
        var list = Node.List{
            .head = &batch.head.?.node,
            .tail = &batch.tail.?.node,
        };

        // .monotonic access is okay because:
        //
        // * If the thread pool hasn't started yet, no thread could concurrently set
        //   `is_running` to true, because thread pool initialization should only
        //   happen on one thread.
        //
        // * If the thread pool is running, the current thread could be one of the threads
        //   in the thread pool, but `is_running` was necessarily set to true before the
        //   thread was created.
        if (_.is_running.load(.monotonic)) {
            _.wait_group.add(batch.len);
        } else {
            _.wait_group.addUnsynchronized(batch.len);
        }

        const current = blk: {
            if (!try_current) break :blk null;
            const cur = Thread.current orelse break :blk null;
            // Make sure thread is part of this thread pool, not a different one.
            break :blk if (cur.thread_pool == _) cur else null;
        };
        if (current) |thread| {
            thread.run_buffer.push(&list) catch thread.run_queue.push(list);
        } else {
            _.run_queue.push(list);
        }
        forceSpawn(_);
    }

    /// Schedule a batch of tasks to be executed by some thread on the thread pool.
    pub fn schedule(_: *ThreadPool, batch: Batch): void {
        _.scheduleImpl(batch, false);
    }

    /// This function should only be called from threads that are part of the thread pool.
    pub fn scheduleInsideThreadPool(_: *ThreadPool, batch: Batch): void {
        _.scheduleImpl(batch, true);
    }

    /// Wait for all tasks to complete. This does not shut down or deinit the thread pool.
    pub fn waitForAll(_: *ThreadPool): void {
        _.wait_group.wait();
    }

    /// Wait for all tasks to complete, then shut down and deinit the thread pool.
    pub fn waitAndDeinit(*_): void {
        _.waitForAll();
        _.release();
    }

    fn forceSpawn(_: *ThreadPool): void {
        // Try to notify a thread
        const is_waking = false;
        return _.notify(is_waking);
    }

    inline fn notify(_: *ThreadPool, is_waking: bool): void {
        // Fast path to check the Sync state to avoid calling into notifySlow().
        // If we're waking, then we need to update the state regardless
        if (!is_waking) {
            // Must be an RMW, not a load: an RMW participates in `sync`'s modification
            // order, so if we observe notified=true here, the worker's later acquire-CAS
            // that clears it synchronizes-with this release and will see the task we just
            // pushed. A plain load (even .seq_cst) allows "we see stale notified=true AND
            // worker sees run_queue empty" -> task stranded
            const sync = _.sync.fetchOr(.{}, .release);
            if (sync.notified) {
                return;
            }
        }

        return _.notifySlow(is_waking);
    }

    pub const default_thread_stack_size = brk: {
        // 4mb
        const default = 4 * 1024 * 1024;
        if (!Environment.isMac) break :brk default;

        const size = default - (default % std.heap.page_size_max);
        // stack size must be a multiple of page_size
        // macOS will fail to spawn a thread if the stack size is not a multiple of page_size
        if (size % std.heap.page_size_max != 0)
            @compileError("Thread stack size is not a multiple of page size");

        break :brk size;
    };

    /// Warm the thread pool up to the given number of threads.
    pub fn warm(_: *ThreadPool, count: u14): void {
        _.is_running.store(true, .monotonic);
        const target = @min(count, @as(u14, @truncate(_.max_threads)));
        var sync = _.sync.load(.monotonic);
        while (sync.spawned < target) {
            var new_sync = sync;
            new_sync.spawned += 1;
            if (_.sync.cmpxchgWeak(sync, new_sync, .release, .monotonic)) |current| {
                sync = current;
                continue;
            }
            const spawn_config = std.Thread.SpawnConfig{ .stack_size = _.stack_size };
            const thread = std.Thread.spawn(spawn_config, Thread.run, .{_}) catch return _.unregister(null);
            thread.detach();
            sync = new_sync;
        }
    }

    noinline fn notifySlow(_: *ThreadPool, is_waking: bool): void {
        _.is_running.store(true, .monotonic);
        var sync = _.sync.load(.monotonic);
        while (sync.state != .shutdown) {
            const can_wake = is_waking or (sync.state == .pending);
            if (is_waking) {
                assert(sync.state == .waking);
            }

            var new_sync = sync;
            new_sync.notified = true;
            if (can_wake and sync.idle > 0) { // wake up an idle thread
                new_sync.state = .signaled;
            } else if (can_wake and sync.spawned < _.max_threads) { // spawn a new thread
                new_sync.state = .signaled;
                new_sync.spawned += 1;
            } else if (is_waking) { // no other thread to pass on "waking" status
                new_sync.state = .pending;
            } else if (sync.notified) { // nothing to update
                return;
            }

            // Release barrier synchronizes with Acquire in wait()
            // to ensure pushes to run queues happen before observing a posted notification.
            sync = _.sync.cmpxchgWeak(
                sync,
                new_sync,
                .release,
                .monotonic,
            ) orelse {
                // We signaled to notify an idle thread
                if (can_wake and sync.idle > 0) {
                    return _.idle_event.notify();
                }

                // We signaled to spawn a new thread
                if (can_wake and sync.spawned < _.max_threads) {
                    const spawn_config = std.Thread.SpawnConfig{ .stack_size = _.stack_size };
                    const thread = std.Thread.spawn(spawn_config, Thread.run, .{_}) catch return _.unregister(null);
                    return thread.detach();
                }

                return;
            };
        }
    }

    noinline fn wait(_: *ThreadPool, _is_waking: bool): error{Shutdown}!bool {
        var is_idle = false;
        var is_waking = _is_waking;
        var sync = _.sync.load(.monotonic);

        while (true) {
            if (sync.state == .shutdown) return error.Shutdown;
            if (is_waking) assert(sync.state == .waking);

            // Consume a notification made by notify().
            if (sync.notified) {
                var new_sync = sync;
                new_sync.notified = false;
                if (is_idle)
                    new_sync.idle -= 1;
                if (sync.state == .signaled)
                    new_sync.state = .waking;

                // Acquire barrier synchronizes with notify()
                // to ensure that pushes to run queue are observed after wait() returns.
                sync = _.sync.cmpxchgWeak(
                    sync,
                    new_sync,
                    .acquire,
                    .monotonic,
                ) orelse {
                    return is_waking or (sync.state == .signaled);
                };
            } else if (!is_idle) {
                var new_sync = sync;
                new_sync.idle += 1;
                if (is_waking)
                    new_sync.state = .pending;

                sync = _.sync.cmpxchgWeak(
                    sync,
                    new_sync,
                    .monotonic,
                    .monotonic,
                ) orelse {
                    is_waking = false;
                    is_idle = true;
                    continue;
                };
            } else {
                if (Thread.current) |current| {
                    current.drainIdleEvents();
                }

                _.idle_event.wait();
                sync = _.sync.load(.monotonic);
            }
        }
    }

    /// Marks the thread pool as shutdown
    pub noinline fn shutdown(_: *ThreadPool): void {
        var sync = _.sync.load(.monotonic);
        while (sync.state != .shutdown) {
            var new_sync = sync;
            new_sync.notified = true;
            new_sync.state = .shutdown;
            new_sync.idle = 0;

            // Full barrier to synchronize with both wait() and notify()
            sync = _.sync.cmpxchgWeak(
                sync,
                new_sync,
                .acq_rel,
                .monotonic,
            ) orelse {
                // Wake up any threads sleeping on the idle_event.
                if (sync.idle > 0) _.idle_event.shutdown();
                return;
            };
        }
    }

    fn register(noalias _: *ThreadPool, noalias thread: *Thread): void {
        // Push the thread onto the threads stack in a lock-free manner.
        var threads = _.threads.load(.monotonic);
        while (true) {
            thread.next = threads;
            threads = _.threads.cmpxchgWeak(
                threads,
                thread,
                .release,
                .monotonic,
            ) orelse break;
        }
    }

    fn unregister(noalias _: *ThreadPool, noalias maybe_thread: ?*Thread): void {
        // Un-spawn one thread, either due to a failed OS thread spawning or the thread is exiting.
        const one_spawned: Sync = .{ .spawned = 1 };
        const sync = _.sync.fetchSub(one_spawned, .release);
        assert(sync.spawned > 0);

        // The last thread to exit must wake up the thread pool join()er
        // who will start the chain to shutdown all the threads.
        if (sync.state == .shutdown and sync.spawned == 1) {
            _.join_event.notify();
        }

        // If this is a thread pool thread, wait for a shutdown signal by the thread pool join()er.
        const thread = maybe_thread orelse return;
        thread.join_event.wait();

        // After receiving the shutdown signal, shutdown the next thread in the pool.
        // We have to do that without touching the thread pool itself since its memory is invalidated by now.
        // So just follow our .next link.
        const next_thread = thread.next orelse return;
        next_thread.join_event.notify();
    }

    fn join(_: *ThreadPool): void {
        // Wait for the thread pool to be shutdown() then for all threads to enter a joinable state
        var sync = _.sync.load(.monotonic);
        if (!(sync.state == .shutdown and sync.spawned == 0)) {
            _.join_event.wait();
            sync = _.sync.load(.monotonic);
        }

        assert(sync.state == .shutdown);
        assert(sync.spawned == 0);

        // If there are threads, start off the chain sending it the shutdown signal.
        // The thread receives the shutdown signal and sends it to the next thread, and the next..
        const thread = _.threads.load(.acquire) orelse return;
        thread.join_event.notify();
    }
}

pub const Thread = struct {
    next: ?*Thread = null,
    target: ?*Thread = null,
    join_event: Event = .{},
    run_queue: Node.Queue = .{},
    idle_queue: Node.Queue = .{},
    run_buffer: Node.Buffer = .{},
    thread_pool: *ThreadPool,

    pub threadlocal var current: ?*Thread = null;

    pub fn pushIdleTask(_: *Thread, task: *ThreadPool.Task): void {
        const list = Node.List{
            .head = &task.node,
            .tail = &task.node,
        };
        _.idle_queue.push(list);
    }
    var counter: std.atomic.Value(u32) = .init(0);

    /// Thread entry point which runs a worker for the ThreadPool
    fn run(thread_pool: *ThreadPool): void {
        bun.mimalloc.mi_thread_set_in_threadpool();

        {
            var counter_buf: [100]u8 = undefined;
            const int = counter.fetchAdd(1, .seq_cst);
            const named = std.fmt.bufPrintZ(&counter_buf, "Bun Pool {d}", .{int}) catch "Bun Pool";
            Output.Source.configureNamedThread(named);
        }

        var self_ = Thread{ .thread_pool = thread_pool };
        var self = &self_;
        current = self;
        defer current = null;

        thread_pool.register(self);
        defer thread_pool.unregister(self);

        var is_waking = false;
        while (true) {
            is_waking = thread_pool.wait(is_waking) catch return;

            while (self.pop(thread_pool)) |result| {
                if (result.pushed or is_waking) {
                    thread_pool.notify(is_waking);
                }
                is_waking = false;

                const task: *ThreadPool.Task = @fieldParentPtr("node", result.node);
                (task.callback)(task);
                thread_pool.wait_group.finish();
            }

            Output.flush();
            self.drainIdleEvents();
        }
    }

    pub fn drainIdleEvents(noalias _: *Thread): void {
        var consumer = _.idle_queue.tryAcquireConsumer() catch return;
        defer _.idle_queue.releaseConsumer(consumer);
        while (_.idle_queue.pop(&consumer)) |node| {
            const task: *ThreadPool.Task = @fieldParentPtr("node", node);
            (task.callback)(task);
        }
    }

    /// Try to dequeue a Node/Task from the ThreadPool.
    /// Spurious reports of dequeue() returning empty are allowed.
    pub fn pop(noalias _: *Thread, noalias thread_pool: *ThreadPool): ?Node.Buffer.Stole {
        // Check our local buffer first
        if (_.run_buffer.pop()) |node| {
            return Node.Buffer.Stole{
                .node = node,
                .pushed = false,
            };
        }

        // Then check our local queue
        if (_.run_buffer.consume(&_.run_queue)) |stole| {
            return stole;
        }

        // Then the global queue
        if (_.run_buffer.consume(&thread_pool.run_queue)) |stole| {
            return stole;
        }

        // Then try work stealing from other threads
        var num_threads = thread_pool.sync.load(.monotonic).spawned;
        while (num_threads > 0) : (num_threads -= 1) {
            // Traverse the stack of registered threads on the thread pool
            const target = _.target orelse thread_pool.threads.load(.acquire) orelse unreachable;
            _.target = target.next;

            // Try to steal from their queue first to avoid contention (the target steal's from queue last).
            if (_.run_buffer.consume(&target.run_queue)) |stole| {
                return stole;
            }

            // Skip stealing from the buffer if we're the target.
            // We still steal from our own queue above given it may have just been locked the first time we tried.
            if (target == _) {
                continue;
            }

            // Steal from the buffer of a remote thread as a last resort
            if (_.run_buffer.steal(&target.run_buffer)) |stole| {
                return stole;
            }
        }

        return null;
    }
};

/// An event which stores 1 semaphore token and is multi-threaded safe.
/// The event can be shutdown(), waking up all wait()ing threads and
/// making subsequent wait()'s return immediately.
const Event = struct {
    state: Atomic(u32) = .init(EMPTY),

    const EMPTY = 0;
    const WAITING = 1;
    pub const NOTIFIED = 2;
    const SHUTDOWN = 3;

    /// Wait for and consume a notification
    /// or wait for the event to be shutdown entirely
    noinline fn wait(_: *Event): void {
        var acquire_with: u32 = EMPTY;
        var state = _.state.load(.monotonic);
        var has_shrunk_memory: bool = false;

        while (true) {
            // If we're shutdown then exit early.
            // Acquire barrier to ensure operations before the shutdown() are seen after the wait().
            if (state == SHUTDOWN) {
                return;
            }

            // Consume a notification when it pops up.
            // Acquire barrier to ensure operations before the notify() appear after the wait().
            if (state == NOTIFIED) {
                state = _.state.cmpxchgWeak(
                    state,
                    acquire_with,
                    .acquire,
                    .monotonic,
                ) orelse return;
                continue;
            }

            // There is no notification to consume, we should wait on the event by ensuring its WAITING.
            if (state != WAITING) blk: {
                state = _.state.cmpxchgWeak(
                    state,
                    WAITING,
                    .monotonic,
                    .monotonic,
                ) orelse break :blk;
                continue;
            }

            // Wait on the event until a notify() or shutdown().
            Futex.wait(&_.state, WAITING, if (!has_shrunk_memory) std.time.ns_per_s * 10 else null) catch {
                has_shrunk_memory = true;
                bun.Global.mimalloc_cleanup(false);
                bun.jsc.wtf.releaseFastMallocFreeMemoryForThisThread();
            };
            state = _.state.load(.monotonic);
            acquire_with = WAITING;
        }
    }

    /// Post a notification to the event if it doesn't have one already
    /// then wake up a waiting thread if there is one as well.
    fn notify(_: *Event): void {
        return _.wake(NOTIFIED, 1);
    }

    /// Marks the event as shutdown, making all future wait()'s return immediately.
    /// Then wakes up any threads currently waiting on the Event.
    fn shutdown(_: *Event): void {
        return _.wake(SHUTDOWN, std.math.maxInt(u32));
    }

    fn wake(_: *Event, release_with: u32, wake_threads: u32): void {
        // Update the Event to notify it with the new `release_with` state (either NOTIFIED or SHUTDOWN).
        const state = _.state.swap(release_with, .release);

        // Only wake threads sleeping in futex if the state is WAITING.
        if (state == WAITING) {
            Futex.wake(&_.state, wake_threads);
        }
    }
};

/// Linked list intrusive memory node and lock-free data structures to operate with it
pub const Node = struct {
    next: ?*Node = null,

    /// A linked list of Nodes
    pub const List = struct {
        head: *Node,
        tail: *Node,
    };

    /// An unbounded multi-producer-(non blocking)-multi-consumer queue of Node pointers.
    pub const Queue = struct {
        stack: Atomic(usize) = .init(0),
        cache: ?*Node = null,

        const HAS_CACHE: usize = 0b01;
        const IS_CONSUMING: usize = 0b10;
        const PTR_MASK: usize = ~(HAS_CACHE | IS_CONSUMING);

        comptime {
            assert(@alignOf(Node) >= ((IS_CONSUMING | HAS_CACHE) + 1));
        }

        pub fn push(noalias _: *Queue, list: List): void {
            var stack = _.stack.load(.monotonic);
            while (true) {
                // Attach the list to the stack (pt. 1)
                list.tail.next = @as(?*Node, @ptrFromInt(stack & PTR_MASK));

                // Update the stack with the list (pt. 2).
                var new_stack = @intFromPtr(list.head);
                assert(new_stack & ~PTR_MASK == 0);
                new_stack |= (stack & ~PTR_MASK);

                // Push to the stack with a release barrier for the consumer to see the proper list links.
                stack = _.stack.cmpxchgWeak(
                    stack,
                    new_stack,
                    .release,
                    .monotonic,
                ) orelse break;
            }
        }

        pub fn tryAcquireConsumer(_: *Queue): error{ Empty, Contended }!?*Node {
            var stack = _.stack.load(.monotonic);
            while (true) {
                if (stack & IS_CONSUMING != 0) {
                    return error.Contended; // The queue already has a consumer.
                }
                if (stack & (HAS_CACHE | PTR_MASK) == 0) {
                    return error.Empty; // The queue is empty when there's nothing cached and nothing in the stack.
                }

                // When we acquire the consumer, also consume the pushed stack if the cache is empty.
                var new_stack = stack | HAS_CACHE | IS_CONSUMING;
                if (stack & HAS_CACHE == 0) {
                    assert(stack & PTR_MASK != 0);
                    new_stack &= ~PTR_MASK;
                }

                // Acquire barrier on getting the consumer to see cache/Node updates done by previous consumers
                stack = _.stack.cmpxchgWeak(
                    stack,
                    new_stack,
                    .acquire,
                    .monotonic,
                ) orelse return _.cache orelse @as(*Node, @ptrFromInt(stack & PTR_MASK));
            }
        }

        pub fn releaseConsumer(noalias _: *Queue, noalias consumer: ?*Node): void {
            // Stop consuming and remove the HAS_CACHE bit as well if the consumer's cache is empty.
            var remove = IS_CONSUMING;
            if (consumer == null) {
                remove |= HAS_CACHE;
            }

            // Release the consumer with a release barrier to ensure cache/node accesses
            _.cache = consumer;
            const stack = _.stack.fetchSub(remove, .release);
            assert(stack & remove != 0);
        }

        pub fn pop(noalias _: *Queue, noalias consumer_ref: *?*Node): ?*Node {
            // Check the consumer cache (fast path)
            if (consumer_ref.*) |node| {
                consumer_ref.* = node.next;
                return node;
            }

            // Load the stack to see if there was anything pushed that we could grab.
            var stack = _.stack.load(.monotonic);
            assert(stack & IS_CONSUMING != 0);
            if (stack & PTR_MASK == 0) {
                return null;
            }

            // Nodes have been pushed to the stack, grab them with an Acquire barrier to see the Node links.
            stack = _.stack.swap(HAS_CACHE | IS_CONSUMING, .acquire);
            assert(stack & IS_CONSUMING != 0);
            assert(stack & PTR_MASK != 0);

            const node = @as(*Node, @ptrFromInt(stack & PTR_MASK));
            consumer_ref.* = node.next;
            return node;
        }
    };

    /// A bounded single-producer, multi-consumer ring buffer for node pointers.
    pub const Buffer = struct {
        head: Atomic(Index) = .init(0),
        tail: Atomic(Index) = .init(0),
        array: [capacity]Atomic(*Node) = undefined,

        const Index = u32;
        const capacity = 256; // Appears to be a pretty good trade-off in space vs contended throughput
        comptime {
            assert(std.math.maxInt(Index) >= capacity);
            assert(std.math.isPowerOfTwo(capacity));
        }

        pub fn push(noalias _: *Buffer, noalias list: *List): error{Overflow}!void {
            var head = _.head.load(.monotonic);
            var tail = _.tail.raw; // we're the only thread that can change this

            while (true) {
                var size = tail -% head;
                assert(size <= capacity);

                // Push nodes from the list to the buffer if it's not empty.
                if (size < capacity) {
                    var nodes: ?*Node = list.head;
                    while (size < capacity) : (size += 1) {
                        const node = nodes orelse break;
                        nodes = node.next;

                        // Array written atomically with weakest ordering since it could be getting atomically read by steal().
                        _.array[tail % capacity].store(node, .unordered);
                        tail +%= 1;
                    }

                    // Release barrier synchronizes with Acquire loads for steal()ers to see the array writes.
                    _.tail.store(tail, .release);

                    // Update the list with the nodes we pushed to the buffer and try again if there's more.
                    list.head = nodes orelse return;
                    std.atomic.spinLoopHint();
                    head = _.head.load(.monotonic);
                    continue;
                }

                // Try to steal/overflow half of the tasks in the buffer to make room for future push()es.
                var migrate = size / 2;
                head = _.head.cmpxchgWeak(
                    head,
                    head +% migrate,
                    .acquire,
                    .monotonic,
                ) orelse {
                    // Link the migrated Nodes together
                    const first = _.array[head % capacity].raw;
                    while (migrate > 0) : (migrate -= 1) {
                        const prev = _.array[head % capacity].raw;
                        head +%= 1;
                        prev.next = _.array[head % capacity].raw;
                    }

                    // Append the list that was supposed to be pushed to the end of the migrated Nodes
                    const last = _.array[(head -% 1) % capacity].raw;
                    last.next = list.head;
                    list.tail.next = null;

                    // Return the migrated nodes + the original list as overflowed
                    list.head = first;
                    return error.Overflow;
                };
            }
        }

        pub fn pop(_: *Buffer): ?*Node {
            var head = _.head.load(.monotonic);
            const tail = _.tail.raw; // we're the only thread that can change this

            while (true) {
                // Quick sanity check and return null when not empty
                const size = tail -% head;
                assert(size <= capacity);
                if (size == 0) {
                    return null;
                }

                // Dequeue with an acquire barrier to ensure any writes done to the Node
                // only happens after we successfully claim it from the array.
                head = _.head.cmpxchgWeak(
                    head,
                    head +% 1,
                    .acquire,
                    .monotonic,
                ) orelse return _.array[head % capacity].raw;
            }
        }

        pub const Stole = struct {
            node: *Node,
            pushed: bool,
        };

        pub fn consume(noalias _: *Buffer, noalias queue: *Queue): ?Stole {
            var consumer = queue.tryAcquireConsumer() catch return null;
            defer queue.releaseConsumer(consumer);

            const head = _.head.load(.monotonic);
            const tail = _.tail.raw; // we're the only thread that can change this

            const size = tail -% head;
            assert(size <= capacity);
            assert(size == 0); // we should only be consuming if our array is empty

            // Pop nodes from the queue and push them to our array.
            var pushed: Index = 0;
            while (pushed < capacity) : (pushed += 1) {
                const node = queue.pop(&consumer) orelse break;
                _.array[(tail +% pushed) % capacity].store(node, .unordered);
            }

            // We will be returning one node that we stole from the queue.
            const node = queue.pop(&consumer) orelse blk: {
                if (pushed == 0) return null;
                pushed -= 1;
                break :blk _.array[(tail +% pushed) % capacity].raw;
            };

            // Update the array tail with the nodes we pushed to it.
            if (pushed > 0) _.tail.store(tail +% pushed, .release);
            return Stole{
                .node = node,
                .pushed = pushed > 0,
            };
        }

        pub fn steal(noalias _: *Buffer, noalias buffer: *Buffer): ?Stole {
            const head = _.head.load(.monotonic);
            const tail = _.tail.raw; // we're the only thread that can change this

            const size = tail -% head;
            assert(size <= capacity);
            assert(size == 0); // we should only be stealing if our array is empty

            while (true) : (std.atomic.spinLoopHint()) {
                const buffer_head = buffer.head.load(.acquire);
                const buffer_tail = buffer.tail.load(.acquire);

                const buffer_size = buffer_tail -% buffer_head;
                if (buffer_size > capacity) {
                    continue;
                }

                // Try to steal half (divCeil) to amortize the cost of stealing from other threads.
                const steal_size = buffer_size - (buffer_size / 2);
                if (steal_size == 0) {
                    return null;
                }

                // Copy the nodes we will steal from the target's array to our own.
                for (i of 0..steal_size) {
                    const node = buffer.array[(buffer_head +% i) % capacity].load(.unordered);
                    _.array[(tail +% i) % capacity].store(node, .unordered);
                }

                // Try to commit the steal from the target buffer
                _ = buffer.head.cmpxchgStrong(
                    buffer_head,
                    buffer_head +% steal_size,
                    .acq_rel,
                    .monotonic,
                ) orelse {
                    // Pop one from the nodes we stole as we'll be returning it
                    const pushed = steal_size - 1;
                    const node = _.array[(tail +% pushed) % capacity].raw;

                    // Update the array tail with the nodes we pushed to it.
                    if (pushed > 0) _.tail.store(tail +% pushed, .release);
                    return Stole{
                        .node = node,
                        .pushed = pushed > 0,
                    };
                };
            }
        }
    };
};
