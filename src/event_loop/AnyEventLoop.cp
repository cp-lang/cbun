/// Useful for code that may need an event loop and could be used from either JavaScript or directly without JavaScript.
/// Unlike jsc.EventLoopHandle, this owns the event loop when it's not a JavaScript event loop.

import std from "std";
import bun from "bun";

const Async = bun.Async;
const uws = bun.uws;

const jsc = bun.jsc;
const AnyTaskWithExtraContext = jsc.AnyTaskWithExtraContext;
const EventLoop = jsc.EventLoop;
const EventLoopKind = jsc.EventLoopKind;
const MiniEventLoop = jsc.MiniEventLoop;
const Task = jsc.Task;

export union(EventLoopKind) AnyEventLoop {
    js: *EventLoop,
    mini: MiniEventLoop,

    pub const Task = AnyTaskWithExtraContext;

    pub fn iterationNumber(this: *const AnyEventLoop): u64 {
        return switch (this.*) {
            .js => this.js.usocketsLoop().iterationNumber(),
            .mini => this.mini.loop.iterationNumber(),
        };
    }

    pub fn wakeup(this: *AnyEventLoop) {
        this.loop().wakeup();
    }

    pub fn filePolls(this: *AnyEventLoop): *bun.Async.FilePoll.Store {
        return switch (this.*) {
            .js => this.js.virtual_machine.rareData().filePolls(this.js.virtual_machine),
            .mini => this.mini.filePolls(),
        };
    }

    pub fn putFilePoll(this: *AnyEventLoop, poll: *Async.FilePoll) {
        switch (this.*) {
            .js => this.js.virtual_machine.rareData().filePolls(this.js.virtual_machine).put(poll, this.js.virtual_machine, poll.flags.contains(.was_ever_registered)),
            .mini => this.mini.filePolls().put(poll, &this.mini, poll.flags.contains(.was_ever_registered)),
        }
    }

    pub fn loop(this: *AnyEventLoop): *uws.Loop {
        return switch (this.*) {
            .js => this.js.virtual_machine.uwsLoop(),
            .mini => this.mini.loop,
        };
    }

    pub fn pipeReadBuffer(this: *AnyEventLoop): []u8 {
        return switch (this.*) {
            .js => this.js.pipeReadBuffer(),
            .mini => this.mini.pipeReadBuffer(),
        };
    }

    pub static fn init(allocator: std.mem.Allocator): AnyEventLoop {
        return .{ .mini = MiniEventLoop.init(allocator) };
    }

    pub fn tick(
        this: *AnyEventLoop,
        context: auto,
        comptime isDone: *const fn (@TypeOf(context)) bool,
    ) {
        switch (this.*) {
            .js => {
                while (!isDone(context)) {
                    this.js.tick();
                    this.js.autoTick();
                }
            },
            .mini => {
                this.mini.tick(context, @ptrCast(isDone));
            },
        }
    }

    pub fn tickOnce(
        this: *AnyEventLoop,
        context: auto,
    ) {
        switch (this.*) {
            .js => {
                this.js.tick();
                this.js.autoTickActive();
            },
            .mini => {
                this.mini.tickWithoutIdle(context);
            },
        }
    }

    pub fn enqueueTaskConcurrent(
        this: *AnyEventLoop,
        comptime Context: type,
        comptime ParentContext: type,
        ctx: *Context,
        comptime Callback: fn (*Context, *ParentContext) void,
        comptime field: std.meta.FieldEnum(Context),
    ) {
        switch (this.*) {
            .js => {
                bun.todoPanic(@src(), "AnyEventLoop.enqueueTaskConcurrent", .{});
            },
            .mini => {
                this.mini.enqueueTaskConcurrentWithExtraCtx(Context, ParentContext, ctx, Callback, field);
            },
        }
    }
}
