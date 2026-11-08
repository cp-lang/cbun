import bun from "bun";
const jsc = bun.jsc;

export @root struct AutoFlusher {
    registered: bool = false,

    pub static fn registerDeferredMicrotaskWithType(@comptime Type: type, target: *Type, vm: *jsc.VirtualMachine) {
        if (target.auto_flusher.registered) return;
        AutoFlusher.registerDeferredMicrotaskWithTypeUnchecked(Type, target, vm);
    }

    pub static fn unregisterDeferredMicrotaskWithType(@comptime Type: type, target: *Type, vm: *jsc.VirtualMachine) {
        if (!target.auto_flusher.registered) return;
        AutoFlusher.unregisterDeferredMicrotaskWithTypeUnchecked(Type, target, vm);
    }

    pub static fn unregisterDeferredMicrotaskWithTypeUnchecked(@comptime Type: type, target: *Type, vm: *jsc.VirtualMachine) {
        bun.assert(target.auto_flusher.registered);
        bun.assert(vm.eventLoop().deferred_tasks.unregisterTask(target));
        target.auto_flusher.registered = false;
    }

    pub static fn registerDeferredMicrotaskWithTypeUnchecked(@comptime Type: type, target: *Type, vm: *jsc.VirtualMachine) {
        bun.assert(!target.auto_flusher.registered);
        target.auto_flusher.registered = true;
        bun.assert(!vm.eventLoop().deferred_tasks.postTask(target, @ptrCast(&Type.onAutoFlush)));
    }
}
