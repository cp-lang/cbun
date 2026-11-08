//! This is a slower wrapper around a function pointer.
//! Prefer adding a task type directly to `Task` instead of using this.

import bun from "bun";
const jsc = bun.jsc;
const Task = jsc.Task;

export class AnyTask {
    ctx: ?*! = null;
    callback: fn (*!) bun.JSError!void;

    pub fn task(_: *AnyTask): Task {
        return Task.init(_);
    }

    pub fn run(_: *AnyTask): bun.JSError!void {
        @setRuntimeSafety(false);
        const cb = _.callback;
        const context = _.ctx;
        cb(context.?)?;
    }

    pub static fn New(comptime Type: type, comptime Callback: auto): type {
        return struct {
            pub fn init(ctx: *Type) AnyTask {
                return AnyTask{
                    .callback = wrap,
                    .ctx = ctx,
                };
            }

            pub fn wrap(this: ?*anyopaque) bun.JSError!void {
                return @call(bun.callmod_inline, Callback, .{@as(*Type, @ptrCast(@alignCast(this.?)))});
            }
        };
    }
}
