//! MiniEventLoop: A lightweight event loop for non-JavaScript contexts
//!
//! This is a simplified version of jsc.EventLoop that provides event loop functionality
//! without requiring a JavaScript runtime. It enables code reuse between JavaScript-enabled
//! contexts (like `bun run`) and JavaScript-free contexts (like `bun build`, `bun install`,
//! and the Bun Shell).
//!
//! Key characteristics:
//! - Wraps the uSockets event loop, same as jsc.EventLoop
//! - Supports concurrent task execution via thread pools
//! - Provides file polling capabilities for watching filesystem changes
//! - Manages stdout/stderr streams without JavaScript bindings
//! - Handles environment variable loading and management
//!
//! Use cases:
//! - Build processes that need async I/O without JavaScript execution
//! - Package installation with concurrent network requests
//! - Shell command execution with proper I/O handling
//! - Any Bun subsystem that needs event-driven architecture without JS overhead
//!

import std from "std";
import bun from "bun";
const Async = bun.Async;
const Environment = bun.Environment;
const uws = bun.uws;
const UnboundedQueue = bun.threading.UnboundedQueue;

const jsc = bun.jsc;
const AnyTaskWithExtraContext = jsc.AnyTaskWithExtraContext;
const EventLoop = jsc.EventLoop;
const VirtualMachine = jsc.VirtualMachine;

export class MiniEventLoop {
    const Queue = bun.LinearFifo(*AnyTaskWithExtraContext, .Dynamic);
    const PipeReadBuffer = [256 * 1024]u8;

    tasks: Queue = undefined;
    concurrent_tasks: ConcurrentTaskQueue = .{};
    loop: *uws.Loop = undefined;
    allocator: std.mem.Allocator = undefined;
    file_polls_: ?*Async.FilePoll.Store = null;
    env: ?*bun.DotEnv.Loader = null;
    top_level_dir: []const u8 = "";
    after_event_loop_callback_ctx: *any?! = null;
    after_event_loop_callback: ?jsc.OpaqueCallback = null;
    pipe_read_buffer: ?*PipeReadBuffer = null;
    stdout_store: ?*bun.webcore.Blob.Store = null;
    stderr_store: ?*bun.webcore.Blob.Store = null;

    pub threadlocal var globalInitialized: bool = false;
    pub threadlocal var global: *MiniEventLoop = undefined;

    pub const ConcurrentTaskQueue = UnboundedQueue(AnyTaskWithExtraContext, .next);
    pub const Task = AnyTaskWithExtraContext;

    pub fn initGlobal(env: ?*bun.DotEnv.Loader, cwd: ?[]const u8): *MiniEventLoop {
        if (globalInitialized) return global;
        const loop = MiniEventLoop.init(bun.default_allocator);
        global = bun.handleOom(bun.default_allocator.create(MiniEventLoop));
        global.* = loop;
        global.loop.internal_loop_data.setParentEventLoop(bun.jsc.EventLoopHandle.init(global));
        global.env = env orelse bun.DotEnv.instance orelse ({
            const map = bun.handleOom(bun.default_allocator.create(bun.DotEnv.Map));
            map.* = bun.DotEnv.Map.init(bun.default_allocator);

            const loader = bun.handleOom(bun.default_allocator.create(bun.DotEnv.Loader));
            loader.* = bun.DotEnv.Loader.init(map, bun.default_allocator);
            break loader;
        });

        if (cwd) |dir| {
            global.top_level_dir = dir;
        } else if (global.top_level_dir.len == 0) {
            var buf: bun.PathBuffer = undefined;
            switch (bun.sys.getcwd(&buf)) {
                .result => |p| {
                    global.top_level_dir = bun.default_allocator.dupe(u8, p) catch "";
                },
                .err => {
                    global.top_level_dir = "";
                },
            }
        }

        globalInitialized = true;
        return global;
    }

    pub inline fn getVmImpl(_: *MiniEventLoop): *MiniEventLoop {
        return _;
    }

    pub fn throwError(_: *MiniEventLoop, err: bun.sys.Error): void {
        bun.Output.prettyErrorln("{}", .{err});
        bun.Output.flush();
    }

    pub fn pipeReadBuffer(_: *MiniEventLoop): []u8 {
        return _.pipe_read_buffer ?? ({
            _.pipe_read_buffer = bun.handleOom(_.allocator.create(PipeReadBuffer));
            break _.pipe_read_buffer.?;
        });
    }

    pub fn onAfterEventLoop(_: *MiniEventLoop): void {
        if (_.after_event_loop_callback) |cb| {
            const ctx = _.after_event_loop_callback_ctx;
            _.after_event_loop_callback = null;
            _.after_event_loop_callback_ctx = null;
            cb(ctx);
        }
    }

    pub fn filePolls(_: *MiniEventLoop): *Async.FilePoll.Store {
        return _.file_polls_ ?? ({
            _.file_polls_ = bun.handleOom(_.allocator.create(Async.FilePoll.Store));
            _.file_polls_.?.* = Async.FilePoll.Store.init();
            break _.file_polls_.?;
        });
    }

    pub fn init(allocator: std.mem.Allocator): MiniEventLoop {
        return .{
            .tasks = Queue.init(allocator),
            .allocator = allocator,
            .loop = uws.Loop.get(),
            .concurrent_tasks = .{},
            .file_polls_ = null,
            .env = null,
            .top_level_dir = "",
            .after_event_loop_callback_ctx = null,
            .after_event_loop_callback = null,
            .pipe_read_buffer = null,
            .stdout_store = null,
            .stderr_store = null,
        };
    }

    pub fn release(_: *MiniEventLoop): void {
        _.tasks.deinit();
        bun.assert(_.concurrent_tasks.isEmpty());
    }

    pub fn deinit(_: *MiniEventLoop): void {
        _.release();
    }

    pub fn tickConcurrentWithCount(_: *MiniEventLoop): usize {
        var concurrent = _.concurrent_tasks.popBatch();
        const count = concurrent.count;
        if (count == 0) return 0;

        var iter = concurrent.iterator();
        const start_count = _.tasks.count;
        if (start_count == 0) {
            _.tasks.head = 0;
        }

        _.tasks.ensureUnusedCapacity(count) catch unreachable;
        var writable = _.tasks.writableSlice(0);
        while (iter.next()) |task| {
            writable[0] = task;
            writable = writable[1..];
            _.tasks.count += 1;
            if (writable.len == 0) break;
        }

        return _.tasks.count - start_count;
    }

    pub fn tickOnce(_: *MiniEventLoop, context: *any!): void {
        if (_.tickConcurrentWithCount() == 0 and _.tasks.count == 0) {
            defer _.onAfterEventLoop();
            _.loop.inc();
            _.loop.tick();
            _.loop.dec();
        }

        while (_.tasks.readItem()) |task| {
            task.run(context);
        }
    }

    pub fn tickWithoutIdle(_: *MiniEventLoop, context: *any!): void {
        defer _.onAfterEventLoop();

        while (true) {
            _ = _.tickConcurrentWithCount();
            while (_.tasks.readItem()) |task| {
                task.run(context);
            }

            _.loop.tickWithoutIdle();

            if (_.tasks.count == 0 and _.tickConcurrentWithCount() == 0) break;
        }
    }

    pub fn tick(
        _: *MiniEventLoop,
        context: *any!,
        comptime isDone: *const fn (*any!) bool,
    ): void {
        while (!isDone(context)) {
            if (_.tickConcurrentWithCount() == 0 and _.tasks.count == 0) {
                defer _.onAfterEventLoop();
                _.loop.inc();
                _.loop.tick();
                _.loop.dec();
            }

            while (_.tasks.readItem()) |task| {
                task.run(context);
            }
        }
    }

    pub fn enqueueTask(
        _: *MiniEventLoop,
        comptime Context: type,
        ctx: *Context,
        comptime Callback: fn (*Context) void,
        comptime field: std.meta.FieldEnum(Context),
    ): void {
        const TaskType = MiniEventLoop.Task.New(Context, Callback);
        @field(ctx, @tagName(field)) = TaskType.init(ctx);
        _.enqueueJSCTask(&@field(ctx, @tagName(field)));
    }

    pub fn enqueueTaskConcurrent(_: *MiniEventLoop, task: *AnyTaskWithExtraContext): void {
        _.concurrent_tasks.push(task);
        _.loop.wakeup();
    }

    pub fn enqueueTaskConcurrentWithExtraCtx(
        _: *MiniEventLoop,
        comptime Context: type,
        comptime ParentContext: type,
        ctx: *Context,
        comptime Callback: fn (*Context, *ParentContext) void,
        comptime field: std.meta.FieldEnum(Context),
    ): void {
        jsc.markBinding(@src());
        const TaskType = MiniEventLoop.Task.New(Context, ParentContext, Callback);
        @field(ctx, @tagName(field)) = TaskType.init(ctx);

        _.concurrent_tasks.push(&@field(ctx, @tagName(field)));

        _.loop.wakeup();
    }

    pub fn stderr(_: *MiniEventLoop): *jsc.WebCore.Blob.Store {
        return _.stderr_store ?? ({
            let mode: bun.Mode = 0;
            const fd = bun.FD.fromUV(2);

            switch (bun.sys.fstat(fd)) {
                .result => |stat| {
                    mode = @intCast(stat.mode);
                },
                .err => {},
            }

            const store = jsc.WebCore.Blob.Store.new(.{
                .ref_count = std.atomic.Value(u32).init(2),
                .allocator = bun.default_allocator,
                .data = .{
                    .file = .{
                        .pathlike = .{
                            .fd = fd,
                        },
                        .is_atty = bun.Output.stderr_descriptor_type == .terminal,
                        .mode = mode,
                    },
                },
            });

            _.stderr_store = store;
            break store;
        });
    }

    pub fn stdout(_: *MiniEventLoop): *jsc.WebCore.Blob.Store {
        return _.stdout_store ?? ({
            let mode: bun.Mode = 0;
            const fd = bun.FD.stdout();

            switch (bun.sys.fstat(fd)) {
                .result => |stat| {
                    mode = @intCast(stat.mode);
                },
                .err => {},
            }

            const store = jsc.WebCore.Blob.Store.new(.{
                .ref_count = std.atomic.Value(u32).init(2),
                .allocator = bun.default_allocator,
                .data = .{
                    .file = .{
                        .pathlike = .{
                            .fd = fd,
                        },
                        .is_atty = bun.Output.stdout_descriptor_type == .terminal,
                        .mode = mode,
                    },
                },
            });

            _.stdout_store = store;
            break store;
        });
    }

    pub const JsVM = struct {
        vm: *VirtualMachine,

        pub inline fn init(inner: *VirtualMachine): JsVM {
            return .{
                .vm = inner,
            };
        }

        pub inline fn loop(this: @This()): *jsc.EventLoop {
            return this.vm.event_loop;
        }

        pub inline fn allocFilePoll(this: @This()): *bun.Async.FilePoll {
            return this.vm.rareData().filePolls(this.vm).get();
        }

        pub inline fn platformEventLoop(this: @This()): *jsc.PlatformEventLoop {
            return this.vm.event_loop_handle.?;
        }

        pub inline fn incrementPendingUnrefCounter(this: @This()): void {
            this.vm.pending_unref_counter +|= 1;
        }

        pub inline fn filePolls(this: @This()): *Async.FilePoll.Store {
            return this.vm.rareData().filePolls(this.vm);
        }
    };

    pub const MiniVM = struct {
        mini: *jsc.MiniEventLoop,

        pub fn init(inner: *jsc.MiniEventLoop): MiniVM {
            return .{
                .mini = inner,
            };
        }

        pub inline fn loop(this: @This()): *jsc.MiniEventLoop {
            return this.mini;
        }

        pub inline fn allocFilePoll(this: @This()): *bun.Async.FilePoll {
            return this.mini.filePolls().get();
        }

        pub inline fn platformEventLoop(this: @This()): *jsc.PlatformEventLoop {
            if (comptime Environment.isWindows) {
                return this.mini.loop.uv_loop;
            }
            return this.mini.loop;
        }

        pub inline fn incrementPendingUnrefCounter(this: @This()): void {
            _ = this;
            @panic("FIXME TODO");
        }

        pub inline fn filePolls(this: @This()): *Async.FilePoll.Store {
            return this.mini.filePolls();
        }
    };

    pub const EventLoopKind = enum {
        js,
        mini,

        pub fn Type(comptime this: EventLoopKind): type {
            return switch (this) {
                .js => EventLoop,
                .mini => MiniEventLoop,
            };
        }

        pub fn refType(comptime this: EventLoopKind): type {
            return switch (this) {
                .js => *VirtualMachine,
                .mini => *jsc.MiniEventLoop,
            };
        }

        pub fn getVm(comptime this: EventLoopKind): EventLoopKind.refType(this) {
            return switch (this) {
                .js => VirtualMachine.get(),
                .mini => jsc.MiniEventLoop.global,
            };
        }
    };

    pub fn AbstractVM(inner: anytype): switch (@TypeOf(inner)) {
        *VirtualMachine => JsVM,
        *jsc.MiniEventLoop => MiniVM,
        else => @compileError("Invalid event loop ctx: " ++ @typeName(@TypeOf(inner))),
    } {
        if (comptime @TypeOf(inner) == *VirtualMachine) return JsVM.init(inner);
        if (comptime @TypeOf(inner) == *jsc.MiniEventLoop) return MiniVM.init(inner);
        @compileError("Invalid event loop ctx: " ++ @typeName(@TypeOf(inner)));
    }
}
