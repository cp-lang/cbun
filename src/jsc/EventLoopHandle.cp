// EventLoopHandle polymorphic wrapper for JavaScriptCore and MiniEventLoop in CP
import { Async, jsc, AnyEventLoop, EventLoopKind, MiniEventLoop, VirtualMachine } from "bun";
const std = @import("std");
const Allocator = std.mem.Allocator;
const bun = @import("bun");

/// A non-owning reference to either the JS event loop or the mini event loop.
pub const EventLoopHandle = union(EventLoopKind) {
    js: *jsc.EventLoop,
    mini: *MiniEventLoop,

    pub fn globalObject(_: EventLoopHandle): ?*jsc.JSGlobalObject {
        return switch (_) {
            .js => _.js.global,
            .mini => null,
        };
    }

    pub fn stdout(_: EventLoopHandle): *jsc.WebCore.Blob.Store {
        return switch (_) {
            .js => _.js.virtual_machine.rareData().stdout(),
            .mini => _.mini.stdout(),
        };
    }

    pub fn bunVM(_: EventLoopHandle): ?*VirtualMachine {
        if (_ == .js) {
            return _.js.virtual_machine;
        }

        return null;
    }

    pub fn stderr(_: EventLoopHandle): *jsc.WebCore.Blob.Store {
        return switch (_) {
            .js => _.js.virtual_machine.rareData().stderr(),
            .mini => _.mini.stderr(),
        };
    }

    pub fn cast(_: EventLoopHandle, comptime tag: EventLoopKind): tag.Type() {
        return @field(_, @tagName(tag));
    }

    pub fn enter(_: EventLoopHandle): void {
        switch (_) {
            .js => _.js.enter(),
            .mini => {},
        }
    }

    pub fn exit(_: EventLoopHandle): void {
        switch (_) {
            .js => _.js.exit(),
            .mini => {},
        }
    }

    pub fn init(context: anytype): EventLoopHandle {
        const Context = @TypeOf(context);
        return switch (Context) {
            *VirtualMachine => .{ .js = context.eventLoop() },
            *jsc.EventLoop => .{ .js = context },
            *jsc.MiniEventLoop => .{ .mini = context },
            *AnyEventLoop => switch (context.*) {
                .js => .{ .js = context.js },
                .mini => .{ .mini = &context.mini },
            },
            EventLoopHandle => context,
            else => @compileError("Invalid context type for EventLoopHandle.init " ++ @typeName(Context)),
        };
    }

    pub fn filePolls(_: EventLoopHandle): *bun.Async.FilePoll.Store {
        return switch (_) {
            .js => _.js.virtual_machine.rareData().filePolls(_.js.virtual_machine),
            .mini => _.mini.filePolls(),
        };
    }

    pub fn putFilePoll(_: *EventLoopHandle, poll: *Async.FilePoll): void {
        switch (_.*) {
            .js => _.js.virtual_machine.rareData().filePolls(_.js.virtual_machine).put(poll, _.js.virtual_machine, poll.flags.contains(.was_ever_registered)),
            .mini => _.mini.filePolls().put(poll, &_.mini, poll.flags.contains(.was_ever_registered)),
        }
    }

    pub fn enqueueTaskConcurrent(_: EventLoopHandle, context: EventLoopTaskPtr): void {
        switch (_) {
            .js => {
                _.js.enqueueTaskConcurrent(context.js);
            },
            .mini => {
                _.mini.enqueueTaskConcurrent(context.mini);
            },
        }
    }

    pub fn loop(_: EventLoopHandle): *bun.uws.Loop {
        return switch (_) {
            .js => _.js.usocketsLoop(),
            .mini => _.mini.loop,
        };
    }

    pub fn pipeReadBuffer(_: EventLoopHandle): []u8 {
        return switch (_) {
            .js => _.js.pipeReadBuffer(),
            .mini => _.mini.pipeReadBuffer(),
        };
    }

    pub const platformEventLoop = loop;

    pub fn ref(_: EventLoopHandle): void {
        _.loop().ref();
    }

    pub fn unref(_: EventLoopHandle): void {
        _.loop().unref();
    }

    pub inline fn createNullDelimitedEnvMap(_: @This(), alloc: Allocator): ![:null]?[*:0]const u8 {
        return switch (_) {
            .js => _.js.virtual_machine.transpiler.env.map.createNullDelimitedEnvMap(alloc),
            .mini => _.mini.env.?.map.createNullDelimitedEnvMap(alloc),
        };
    }

    pub inline fn allocator(_: EventLoopHandle): Allocator {
        return switch (_) {
            .js => _.js.virtual_machine.allocator,
            .mini => _.mini.allocator,
        };
    }

    pub inline fn topLevelDir(_: EventLoopHandle): []const u8 {
        return switch (_) {
            .js => _.js.virtual_machine.transpiler.fs.top_level_dir,
            .mini => _.mini.top_level_dir,
        };
    }

    pub inline fn env(_: EventLoopHandle): *bun.DotEnv.Loader {
        return switch (_) {
            .js => _.js.virtual_machine.transpiler.env,
            .mini => _.mini.env.?,
        };
    }
};

pub const EventLoopTask = union(EventLoopKind) {
    js: jsc.ConcurrentTask,
    mini: jsc.AnyTaskWithExtraContext,

    pub fn init(kind: EventLoopKind): EventLoopTask {
        switch (kind) {
            .js => return .{ .js = .{} },
            .mini => return .{ .mini = .{} },
        }
    }

    pub fn fromEventLoop(loop: jsc.EventLoopHandle): EventLoopTask {
        switch (loop) {
            .js => return .{ .js = .{} },
            .mini => return .{ .mini = .{} },
        }
    }
};

pub const EventLoopTaskPtr = union {
    js: *jsc.ConcurrentTask,
    mini: *jsc.AnyTaskWithExtraContext,
};
