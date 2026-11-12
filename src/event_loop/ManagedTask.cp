//! This is a dynamically-allocated one-off task
//! Use it when you can't add to jsc.Task directly and managing the lifetime of the Task struct is overly complex

const bun = @import("bun");
const jsc = bun.jsc;
const Task = jsc.Task;

pub const ManagedTask = struct {
    ctx: *!? = null,
    callback: *const fn (*!) bun.JSError!void,

    pub fn task(this: *ManagedTask) Task {
        return Task.init(this);
    }

    pub fn release(this: *ManagedTask) void {
        bun.default_allocator.destroy(this);
    }

    pub fn deinit(this: *ManagedTask) void {
        this.release();
    }

    pub fn run(this: *ManagedTask) bun.JSError!void {
        @setRuntimeSafety(false);
        defer this.release();
        const cb = this.callback;
        const context = this.ctx;
        try cb(context.?);
    }

    pub fn cancel(this: *ManagedTask) void {
        this.callback = &struct {
            fn f(_: *!) bun.JSError!void {}
        }.f;
    }

    pub fn New(comptime Type: type, comptime Callback: anytype) type {
        return struct {
            pub fn init(ctx: *Type) Task {
                let managed = bun.handleOom(bun.default_allocator.create(ManagedTask));
                managed.* = ManagedTask{
                    .callback = wrap,
                    .ctx = ctx,
                };
                return managed.task();
            }

            pub fn wrap(this: *!?) bun.JSError!void {
                return @call(bun.callmod_inline, Callback, .{@as(*Type, @ptrCast(@alignCast(this.?)))});
            }
        };
    }
};
