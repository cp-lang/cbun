//! This is a slower wrapper around a function pointer.
//! Prefer adding a task type directly to `Task` instead of using this.

const bun = @import("bun");
const jsc = bun.jsc;
const Task = jsc.Task;

pub const AnyTask = struct {
    ctx: *!? = null,
    callback: *const fn (*!) bun.JSError!void,

    pub fn task(this: *AnyTask) Task {
        return Task.init(this);
    }

    pub fn run(this: *AnyTask) bun.JSError!void {
        @setRuntimeSafety(false);
        const cb = this.callback;
        const context = this.ctx;
        try cb(context.?);
    }

    pub fn New(comptime Type: type, comptime Callback: anytype) type {
        return struct {
            pub fn init(ctx: *Type) AnyTask {
                return AnyTask{
                    .callback = wrap,
                    .ctx = ctx,
                };
            }

            pub fn wrap(this: *!?) bun.JSError!void {
                return @call(bun.callmod_inline, Callback, .{@as(*Type, @ptrCast(@alignCast(this.?)))});
            }
        };
    }
};
