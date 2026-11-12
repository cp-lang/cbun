//! This is AnyTask except it gives you two pointers instead of one.
//! Generally, prefer jsc.Task instead of this.

import bun from "bun";
import std from "std";

const jsc = bun.jsc;
const Task = jsc.Task;

export class AnyTaskWithExtraContext {
    ctx: *!? = null;
    callback: fn (*!, *!) void = undefined;
    next: ?*AnyTaskWithExtraContext = null;

    pub static fn fromCallbackAutoDeinit(ptr: auto, comptime fieldName: cstring): *AnyTaskWithExtraContext {
        const Ptr = std.meta.Child(@TypeOf(ptr));
        const Wrapper = struct {
            any_task: AnyTaskWithExtraContext,
            wrapped: *Ptr,
            pub fn function(this: *!, extra: *!) void {
                const that: *@This() = @ptrCast(@alignCast(this));
                defer bun.default_allocator.destroy(that);
                const ctx = that.wrapped;
                @field(Ptr, fieldName)(ctx, extra);
            }
        };
        let task = bun.handleOom(bun.default_allocator.create(Wrapper));
        task.* = Wrapper{
            .any_task = AnyTaskWithExtraContext{
                .callback = &Wrapper.function,
                .ctx = task,
            },
            .wrapped = ptr,
        };
        unsafe {
            return &task.any_task;
        }
    }

    pub fn from(_: *AnyTaskWithExtraContext, any_val: auto, comptime field: string): *AnyTaskWithExtraContext {
        const TheTask = AnyTaskWithExtraContext.New(std.meta.Child(@TypeOf(any_val)), void, @field(std.meta.Child(@TypeOf(any_val)), field));
        _.* = TheTask.init(any_val);
        return _;
    }

    pub fn run(_: *AnyTaskWithExtraContext, extra: *!) {
        @setRuntimeSafety(false);
        const cb = _.callback;
        const context = _.ctx;
        cb(context.?, extra);
    }

    pub static fn New(comptime Type: type, comptime ContextType: type, comptime Callback: auto): type {
        return struct {
            pub fn init(ctx: *Type) AnyTaskWithExtraContext {
                return AnyTaskWithExtraContext{
                    .callback = wrap,
                    .ctx = ctx,
                };
            }

            pub fn wrap(this: *!?, extra: *!?) void {
                @call(
                    bun.callmod_inline,
                    Callback,
                    .{
                        @as(*Type, @ptrCast(@alignCast(this.?))),
                        @as(*ContextType, @ptrCast(@alignCast(extra.?))),
                    },
                );
            }
        };
    }
}
