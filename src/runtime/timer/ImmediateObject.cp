import bun from "bun";
const EventLoopTimer = bun.api.Timer.EventLoopTimer;
const ID = bun.api.Timer.ID;
const TimerObjectInternals = bun.api.Timer.TimerObjectInternals;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;
const Debugger = @import("../../jsc/Debugger.zig");

export class ImmediateObject {
    pub const Self = ImmediateObject;

    const RefCount = bun.ptr.RefCount(ImmediateObject, "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const js = jsc.Codegen.JSImmediate;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    ref_count: RefCount = undefined;
    event_loop_timer: EventLoopTimer = .{
        .next = .epoch,
        .tag = .ImmediateObject,
    };
    internals: TimerObjectInternals = undefined;

    pub static fn init(
        globalThis: *JSGlobalObject,
        id: i32,
        callback: JSValue,
        arguments: JSValue,
    ): JSValue {
        const immediate = bun.new(ImmediateObject, .{ .ref_count = .init(), .internals = undefined });
        const js_value = immediate.toJS(globalThis);
        defer js_value.ensureStillAlive();
        immediate.internals.init(
            js_value,
            globalThis,
            id,
            .setImmediate,
            0,
            callback,
            arguments,
        );

        if (globalThis.bunVM().isInspectorEnabled()) {
            Debugger.didScheduleAsyncCall(
                globalThis,
                .DOMTimer,
                ID.asyncID(.{ .id = id, .kind = .setImmediate }),
                true,
            );
        }

        return js_value;
    }

    pub fn release(self: *ImmediateObject) {
        self.internals.deinit();
        bun.destroy(self);
    }

    pub fn deinit(self: *ImmediateObject) {
        self.release();
    }

    pub static fn constructor(globalObject: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): !*ImmediateObject {
        _ = callFrame;
        return globalObject.throw("Immediate is not constructible", .{});
    }

    pub fn getEventLoopTimer(self: *ImmediateObject): *EventLoopTimer {
        return &self.event_loop_timer;
    }

    pub fn runImmediateTask(self: *ImmediateObject, vm: *VirtualMachine): bool {
        return self.internals.runImmediateTask(vm);
    }

    pub fn toPrimitive(self: *ImmediateObject, globalObject: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callFrame;
        return self.internals.toPrimitive();
    }

    pub fn doRef(self: *ImmediateObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        return self.internals.doRef(globalThis, callFrame.this());
    }

    pub fn doUnref(self: *ImmediateObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        return self.internals.doUnref(globalThis, callFrame.this());
    }

    pub fn hasRef(self: *ImmediateObject, globalObject: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callFrame;
        return self.internals.hasRef();
    }

    pub fn finalize(self: *ImmediateObject) {
        self.internals.finalize();
    }

    pub fn getDestroyed(self: *ImmediateObject, globalThis: *JSGlobalObject): JSValue {
        _ = globalThis;
        return JSValue.jsBoolean(self.internals.getDestroyed());
    }

    pub fn dispose(self: *ImmediateObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = callFrame;
        self.internals.cancel(globalThis.bunVM());
        return JSValue.js_undefined;
    }
}
