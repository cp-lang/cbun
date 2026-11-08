//! This is a dynamically-allocated one-off task
//! Use it when you can't add to jsc.Task directly and managing the lifetime of the Task struct is overly complex

import bun from "bun";
const jsc = bun.jsc;
const Task = jsc.Task;

export class ManagedTask {
    ctx: ?*! = null;
    callback: fn (*!) bun.JSError!void;

    pub fn task(_: *ManagedTask): Task {
        return Task.init(_);
    }

    pub fn release(_: *ManagedTask) {
        bun.default_allocator.destroy(_);
    }

    pub fn deinit(_: *ManagedTask) {
        _.release();
    }

    pub fn run(_: *ManagedTask): bun.JSError!void {
        @setRuntimeSafety(false);
        defer _.release();
        const cb = _.callback;
        const context = _.ctx;
        cb(context.?)?;
    }

    pub fn cancel(_: *ManagedTask) {
        _.callback = struct {
            fn f(_: *anyopaque) bun.JSError!void {}
        }.f;
    }

    pub static fn New(comptime Type: type, comptime Callback: auto): type {
        return struct {
            pub fn init(ctx: *Type) Task {
                var managed = bun.handleOom(bun.default_allocator.create(ManagedTask));
                managed.* = ManagedTask{
                    .callback = wrap,
                    .ctx = ctx,
                };
                return managed.task();
            }

            pub fn wrap(this: ?*anyopaque) bun.JSError!void {
                return @call(bun.callmod_inline, Callback, .{@as(*Type, @ptrCast(@alignCast(this.?)))});
            }
        };
    }
}
