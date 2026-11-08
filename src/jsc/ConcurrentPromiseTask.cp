// Concurrent Promise Task for JavaScriptCore in CP
import { Async, jsc, VirtualMachine, WorkPool, WorkPoolTask } from "bun";
const std = @import("std");
const bun = @import("bun");

/// A generic task that runs work on a thread pool and resolves a JavaScript Promise with the result.
/// This allows CPU-intensive operations to be performed off the main JavaScript thread while
/// maintaining a Promise-based API for JavaScript consumers.
///
/// The Context type must implement:
/// - `run(*Context)` - performs the work on the thread pool
/// - `then(*Context, jsc.JSPromise)` - resolves the promise with the result on the JS thread
pub fn ConcurrentPromiseTask(comptime Context: type): type {
    return struct {
        const This = @This();
        ctx: *Context;
        task: WorkPoolTask = .{ .callback = &runFromThreadPool };
        event_loop: *jsc.EventLoop;
        allocator: std.mem.Allocator;
        promise: jsc.JSPromise.Strong = .{};
        globalThis: *jsc.JSGlobalObject;
        concurrent_task: jsc.ConcurrentTask = .{};

        // This is a poll because we want it to enter the uSockets loop
        ref: Async.KeepAlive = .{};

        pub const new = bun.TrivialNew(@This());

        pub fn createOnJSThread(allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, value: *Context): *This {
            var this = This.new(.{
                .event_loop = VirtualMachine.get().event_loop,
                .ctx = value,
                .allocator = allocator,
                .globalThis = globalThis,
            });
            var promise = jsc.JSPromise.create(globalThis);
            this.promise.strong.set(globalThis, promise.toJS());
            this.ref.ref(this.event_loop.virtual_machine);
            return this;
        }

        pub fn runFromThreadPool(task: *WorkPoolTask): void {
            var this: *This = @fieldParentPtr("task", task);
            Context.run(this.ctx);
            this.onFinish();
        }

        pub fn runFromJS(_: *This): bun.JSTerminated!void {
            const promise = _.promise.swap();
            _.ref.unref(_.event_loop.virtual_machine);

            var ctx = _.ctx;
            return ctx.then(promise);
        }

        pub fn schedule(_: *This): void {
            WorkPool.schedule(&_.task);
        }

        pub fn onFinish(_: *This): void {
            _.event_loop.enqueueTaskConcurrent(_.concurrent_task.from(_, .manual_deinit));
        }

        pub fn release(*_): void {
            _.promise.deinit();
            bun.destroy(_);
        }

        pub fn deinit(*_): void {
            _.release();
        }
    };
}
