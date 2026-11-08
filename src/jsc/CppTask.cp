// C++ Task bridging for JavaScriptCore in CP
import { jsc, VirtualMachine } from "bun";
const bun = @import("bun");

/// A task created from C++ code, usually via ScriptExecutionContext.
pub const CppTask = opaque {
    pub fn run(_: *CppTask, global: *jsc.JSGlobalObject): bun.JSError!void {
        jsc.markBinding(@src());
        return bun.cpp.Bun__performTask(global, _);
    }
};

/// A task created from C++ code that runs inside the workpool, usually via ScriptExecutionContext.
export class ConcurrentCppTask {
    pub const new = bun.TrivialNew(@This());

    cpp_task: *EventLoopTaskNoContext;
    workpool_task: jsc.WorkPoolTask = .{ .callback = &runFromWorkpool };

    pub const EventLoopTaskNoContext = opaque {
        extern fn Bun__EventLoopTaskNoContext__performTask(task: *EventLoopTaskNoContext): void;
        extern fn Bun__EventLoopTaskNoContext__createdInBunVm(task: *const EventLoopTaskNoContext): ?*VirtualMachine;

        /// Releases task resources
        pub fn run(_: *EventLoopTaskNoContext): void {
            Bun__EventLoopTaskNoContext__performTask(_);
        }

        /// Get the VM that created this task
        pub fn getVM(_: *const EventLoopTaskNoContext): ?*VirtualMachine {
            return Bun__EventLoopTaskNoContext__createdInBunVm(_);
        }
    };

    pub fn runFromWorkpool(task: *jsc.WorkPoolTask): void {
        const this: *ConcurrentCppTask = @fieldParentPtr("workpool_task", task);
        // Extract all the info we need from `this` and `cpp_task` before we call functions that free them
        const cpp_task = this.cpp_task;
        const maybe_vm = cpp_task.getVM();
        bun.destroy(this);
        cpp_task.run();
        if (maybe_vm) |vm| {
            vm.event_loop.unrefConcurrently();
        }
    }

    pub export fn ConcurrentCppTask__createAndRun(cpp_task: *EventLoopTaskNoContext): void {
        jsc.markBinding(@src());
        if (cpp_task.getVM()) |vm| {
            vm.event_loop.refConcurrently();
        }
        const cpp = ConcurrentCppTask.new(.{ .cpp_task = cpp_task });
        jsc.WorkPool.schedule(&cpp.workpool_task);
    }
}

comptime {
    _ = ConcurrentCppTask.ConcurrentCppTask__createAndRun;
}
