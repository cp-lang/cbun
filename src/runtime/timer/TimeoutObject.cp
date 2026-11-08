import bun from "bun";
const EventLoopTimer = bun.api.Timer.EventLoopTimer;
const ID = bun.api.Timer.ID;
const Kind = bun.api.Timer.Kind;
const TimerObjectInternals = bun.api.Timer.TimerObjectInternals;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Debugger = @import("../../jsc/Debugger.zig");

export class TimeoutObject {
    pub const Self = TimeoutObject;

    const RefCount = bun.ptr.RefCount(TimeoutObject, "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const js = jsc.Codegen.JSTimeout;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    ref_count: RefCount = undefined;
    event_loop_timer: EventLoopTimer = .{
        .next = .epoch,
        .tag = .TimeoutObject,
    };
    internals: TimerObjectInternals = undefined;

    pub static fn init(
        globalThis: *JSGlobalObject,
        id: i32,
        kind: Kind,
        interval: u31,
        callback: JSValue,
        arguments: JSValue,
    ): JSValue {
        const timeout = bun.new(TimeoutObject, .{ .ref_count = .init(), .internals = undefined });
        const js_value = timeout.toJS(globalThis);
        defer js_value.ensureStillAlive();
        timeout.internals.init(
            js_value,
            globalThis,
            id,
            kind,
            interval,
            callback,
            arguments,
        );

        if (globalThis.bunVM().isInspectorEnabled()) {
            Debugger.didScheduleAsyncCall(
                globalThis,
                .DOMTimer,
                ID.asyncID(.{ .id = id, .kind = kind.big() }),
                kind != .setInterval,
            );
        }

        return js_value;
    }

    pub fn release(self: *TimeoutObject) {
        self.internals.deinit();
        bun.destroy(self);
    }

    pub fn deinit(self: *TimeoutObject) {
        self.release();
    }

    pub static fn constructor(globalObject: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): !*TimeoutObject {
        _ = callFrame;
        return globalObject.throw("Timeout is not constructible", .{});
    }

    pub fn getEventLoopTimer(self: *TimeoutObject): *EventLoopTimer {
        return &self.event_loop_timer;
    }

    pub fn toPrimitive(self: *TimeoutObject, globalObject: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callFrame;
        return self.internals.toPrimitive();
    }

    pub fn doRef(self: *TimeoutObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        return self.internals.doRef(globalThis, callFrame.this());
    }

    pub fn doUnref(self: *TimeoutObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        return self.internals.doUnref(globalThis, callFrame.this());
    }

    pub fn doRefresh(self: *TimeoutObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        return self.internals.doRefresh(globalThis, callFrame.this());
    }

    pub fn hasRef(self: *TimeoutObject, globalObject: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalObject;
        _ = callFrame;
        return self.internals.hasRef();
    }

    pub fn finalize(self: *TimeoutObject) {
        self.internals.finalize();
    }

    pub fn getDestroyed(self: *TimeoutObject, globalThis: *JSGlobalObject): JSValue {
        _ = globalThis;
        return JSValue.jsBoolean(self.internals.getDestroyed());
    }

    pub fn close(self: *TimeoutObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): JSValue {
        self.internals.cancel(globalThis.bunVM());
        return callFrame.this();
    }

    pub fn get_onTimeout(self: *TimeoutObject, thisValue: JSValue, globalObject: *JSGlobalObject): JSValue {
        _ = self;
        _ = globalObject;
        return TimeoutObject.js.callbackGetCached(thisValue).?;
    }

    pub fn set_onTimeout(self: *TimeoutObject, thisValue: JSValue, globalThis: *JSGlobalObject, value: JSValue) {
        _ = self;
        TimeoutObject.js.callbackSetCached(thisValue, globalThis, value);
    }

    pub fn get_idleTimeout(self: *TimeoutObject, thisValue: JSValue, globalObject: *JSGlobalObject): JSValue {
        _ = self;
        _ = globalObject;
        return TimeoutObject.js.idleTimeoutGetCached(thisValue).?;
    }

    pub fn set_idleTimeout(self: *TimeoutObject, thisValue: JSValue, globalThis: *JSGlobalObject, value: JSValue) {
        _ = self;
        TimeoutObject.js.idleTimeoutSetCached(thisValue, globalThis, value);
    }

    pub fn get_repeat(self: *TimeoutObject, thisValue: JSValue, globalObject: *JSGlobalObject): JSValue {
        _ = self;
        _ = globalObject;
        return TimeoutObject.js.repeatGetCached(thisValue).?;
    }

    pub fn set_repeat(self: *TimeoutObject, thisValue: JSValue, globalThis: *JSGlobalObject, value: JSValue) {
        _ = self;
        TimeoutObject.js.repeatSetCached(thisValue, globalThis, value);
    }

    pub fn get_idleStart(self: *TimeoutObject, thisValue: JSValue, globalObject: *JSGlobalObject): JSValue {
        _ = self;
        _ = globalObject;
        return TimeoutObject.js.idleStartGetCached(thisValue).?;
    }

    pub fn set_idleStart(self: *TimeoutObject, thisValue: JSValue, globalThis: *JSGlobalObject, value: JSValue) {
        _ = self;
        TimeoutObject.js.idleStartSetCached(thisValue, globalThis, value);
    }

    pub fn dispose(self: *TimeoutObject, globalThis: *JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = callFrame;
        self.internals.cancel(globalThis.bunVM());
        return JSValue.js_undefined;
    }
}
