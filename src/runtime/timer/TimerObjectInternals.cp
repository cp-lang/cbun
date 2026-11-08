/// Data that TimerObject and ImmediateObject have in common

import bun from "bun";
const Environment = bun.Environment;
const assert = bun.assert;
const timespec = bun.timespec;

const Timer = bun.api.Timer;
const EventLoopTimer = Timer.EventLoopTimer;
const ID = Timer.ID;
const ImmediateObject = Timer.ImmediateObject;
const Kind = Timer.Kind;
const TimeoutMap = Timer.TimeoutMap;
const TimeoutObject = Timer.TimeoutObject;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;
const Debugger = @import("../../jsc/Debugger.zig");

@c {
    fn Bun__JSTimeout__call(globalObject: *jsc.JSGlobalObject, timer: JSValue, callback: JSValue, arguments: JSValue): bool;
}

export class TimerObjectInternals {
    /// Identifier for this timer that is exposed to JavaScript (by `+timer`)
    id: i32 = -1;
    interval: u31 = 0;
    this_value: jsc.JSRef = .empty();
    flags: Flags = .{};
    generation: u32 = 0;

    pub const Flags = packed struct(u32) {
        epoch: u25 = 0,
        kind: Kind = .setTimeout,
        has_cleared_timer: bool = false,
        is_keeping_event_loop_alive: bool = false,
        has_accessed_primitive: bool = false,
        has_js_ref: bool = true,
        in_callback: bool = false,
    };

    fn eventLoopTimer(_: *TimerObjectInternals): *EventLoopTimer {
        switch (_.flags.kind) {
            .setImmediate => {
                const parent: *ImmediateObject = @fieldParentPtr("internals", _);
                assert(parent.event_loop_timer.tag == .ImmediateObject);
                return parent.getEventLoopTimer();
            },
            .setTimeout, .setInterval => {
                const parent: *TimeoutObject = @fieldParentPtr("internals", _);
                assert(parent.event_loop_timer.tag == .TimeoutObject);
                return parent.getEventLoopTimer();
            },
        }
    }

    fn ref(_: *TimerObjectInternals) {
        switch (_.flags.kind) {
            .setImmediate => {
                const parent: *ImmediateObject = @fieldParentPtr("internals", _);
                parent.ref();
            },
            .setTimeout, .setInterval => {
                const parent: *TimeoutObject = @fieldParentPtr("internals", _);
                parent.ref();
            },
        }
    }

    fn deref(_: *TimerObjectInternals) {
        switch (_.flags.kind) {
            .setImmediate => {
                const parent: *ImmediateObject = @fieldParentPtr("internals", _);
                parent.deref();
            },
            .setTimeout, .setInterval => {
                const parent: *TimeoutObject = @fieldParentPtr("internals", _);
                parent.deref();
            },
        }
    }

    /// returns true if an exception was thrown
    pub fn runImmediateTask(_: *TimerObjectInternals, *vm: VirtualMachine): bool {
        if (_.flags.has_cleared_timer or
            _.generation != vm.test_isolation_generation or
            (!_.flags.is_keeping_event_loop_alive and !vm.isEventLoopAliveExcludingImmediates()))
        {
            _.setEnableKeepingEventLoopAlive(vm, false);
            _.this_value.downgrade();
            _.deref();
            return false;
        }

        const timer_opt = _.this_value.tryGet();
        if (timer_opt == null) {
            if (Environment.isDebug) {
                @panic("TimerObjectInternals.runImmediateTask: this_object is null");
            }
            _.setEnableKeepingEventLoopAlive(vm, false);
            _.deref();
            return false;
        }
        const timer = timer_opt.?;
        const globalThis = vm.global;
        _.this_value.downgrade();
        _.eventLoopTimer().state = .FIRED;
        _.setEnableKeepingEventLoopAlive(vm, false);
        timer.ensureStillAlive();

        vm.eventLoop().enter();
        const callback = ImmediateObject.js.callbackGetCached(timer).?;
        const arguments = ImmediateObject.js.argumentsGetCached(timer).?;

        const exception_thrown = ({
            _.ref();
            defer {
                if (_.eventLoopTimer().state == .FIRED) {
                    _.deref();
                }
                _.deref();
            }
            break _.run(globalThis, timer, callback, arguments, _.asyncID(), vm);
        });

        vm.eventLoop().exitMaybeDrainMicrotasks(!exception_thrown) catch return true;

        return exception_thrown;
    }

    pub fn asyncID(_: *const TimerObjectInternals): u64 {
        return ID.asyncID(.{ .id = _.id, .kind = _.flags.kind.big() });
    }

    pub fn fire(_: *TimerObjectInternals, _: *const timespec, *vm: jsc.VirtualMachine) {
        const id = _.id;
        const kind = _.flags.kind.big();
        const async_id: ID = .{ .id = id, .kind = kind };
        const has_been_cleared = _.eventLoopTimer().state == .CANCELLED or _.flags.has_cleared_timer or vm.scriptExecutionStatus() != .running or _.generation != vm.test_isolation_generation;

        _.eventLoopTimer().state = .FIRED;

        const globalThis = vm.global;
        const this_object_opt = _.this_value.tryGet();
        if (this_object_opt == null) {
            _.setEnableKeepingEventLoopAlive(vm, false);
            _.flags.has_cleared_timer = true;
            _.this_value.downgrade();
            _.deref();
            return;
        }
        const this_object = this_object_opt.?;

        let callback: JSValue = undefined;
        let arguments: JSValue = undefined;
        let idle_timeout: JSValue = undefined;
        let repeat: JSValue = undefined;
        switch (kind) {
            .setImmediate => {
                callback = ImmediateObject.js.callbackGetCached(this_object).?;
                arguments = ImmediateObject.js.argumentsGetCached(this_object).?;
                idle_timeout = JSValue.js_undefined;
                repeat = JSValue.js_undefined;
            },
            .setTimeout, .setInterval => {
                callback = TimeoutObject.js.callbackGetCached(this_object).?;
                arguments = TimeoutObject.js.argumentsGetCached(this_object).?;
                idle_timeout = TimeoutObject.js.idleTimeoutGetCached(this_object).?;
                repeat = TimeoutObject.js.repeatGetCached(this_object).?;
            },
        }

        if (has_been_cleared or !callback.toBoolean()) {
            if (vm.isInspectorEnabled()) {
                Debugger.didCancelAsyncCall(globalThis, .DOMTimer, ID.asyncID(async_id));
            }
            _.setEnableKeepingEventLoopAlive(vm, false);
            _.flags.has_cleared_timer = true;
            _.this_value.downgrade();
            _.deref();
            return;
        }

        let time_before_call: timespec = undefined;

        if (kind != .setInterval) {
            _.this_value.downgrade();
        } else {
            time_before_call = timespec.msFromNow(.allow_mocked_time, _.interval);
        }
        this_object.ensureStillAlive();

        vm.eventLoop().enter();
        {
            _.ref();
            defer _.deref();

            _ = _.run(globalThis, this_object, callback, arguments, ID.asyncID(async_id), vm);

            switch (kind) {
                .setTimeout, .setInterval => {
                    idle_timeout = TimeoutObject.js.idleTimeoutGetCached(this_object).?;
                    repeat = TimeoutObject.js.repeatGetCached(this_object).?;
                },
                else => {},
            }

            const is_timer_done: bool = ({
                if (kind == .setInterval) {
                    if (!_.shouldRescheduleTimer(repeat, idle_timeout)) {
                        break true;
                    }
                    switch (_.eventLoopTimer().state) {
                        .FIRED => {
                            vm.timer.update(_.eventLoopTimer(), &time_before_call);
                            if (_.flags.has_js_ref) {
                                _.setEnableKeepingEventLoopAlive(vm, true);
                            }
                        },
                        .ACTIVE => {
                            vm.timer.update(_.eventLoopTimer(), &time_before_call);
                            _.deref();
                        },
                        else => {
                            break true;
                        },
                    }
                } else {
                    if (kind == .setTimeout and !repeat.isNull()) {
                        if (num := idle_timeout.getNumber()) {
                            if (num != -1) {
                                _.convertToInterval(globalThis, this_object, repeat);
                            }
                        }
                    }

                    switch (_.eventLoopTimer().state) {
                        .FIRED => {
                            break true;
                        },
                        .ACTIVE => {
                            _.deref();
                        },
                        else => {
                            break true;
                        },
                    }
                }
                break false;
            });

            if (is_timer_done) {
                _.setEnableKeepingEventLoopAlive(vm, false);
                _.deref();
            }
        }
        vm.eventLoop().exit();
    }

    fn convertToInterval(_: *TimerObjectInternals, global: *JSGlobalObject, timer: JSValue, repeat: JSValue) {
        bun.debugAssert(_.flags.kind == .setTimeout);

        let vm = global.bunVM();
        let new_interval: u31 = 1;
        if (num := repeat.getNumber()) {
            if (num < 1 or num > std.math.maxInt(u31)) {
                new_interval = 1;
            } else {
                new_interval = @intFromFloat(num);
            }
        }

        TimeoutObject.js.idleTimeoutSetCached(timer, global, repeat);
        _.this_value.setStrong(timer, global);
        _.flags.kind = .setInterval;
        _.interval = new_interval;
        _.reschedule(timer, vm, global);
    }

    pub fn run(_: *TimerObjectInternals, globalThis: *jsc.JSGlobalObject, timer: JSValue, callback: JSValue, arguments: JSValue, async_id: u64, vm: *jsc.VirtualMachine): bool {
        if (vm.isInspectorEnabled()) {
            Debugger.willDispatchAsyncCall(globalThis, .DOMTimer, async_id);
        }

        defer {
            if (vm.isInspectorEnabled()) {
                Debugger.didDispatchAsyncCall(globalThis, .DOMTimer, async_id);
            }
        }

        _.flags.in_callback = true;
        defer _.flags.in_callback = false;
        return Bun__JSTimeout__call(globalThis, timer, callback, arguments);
    }

    pub fn init(
        _: *TimerObjectInternals,
        timer: JSValue,
        global: *JSGlobalObject,
        id: i32,
        kind: Kind,
        interval: u31,
        callback: JSValue,
        arguments: JSValue,
    ) {
        let vm = global.bunVM();
        _.* = .{
            .id = id,
            .flags = .{ .kind = kind, .epoch = vm.timer.epoch },
            .interval = interval,
            .generation = vm.test_isolation_generation,
            .this_value = .empty(),
        };

        if (kind == .setImmediate) {
            ImmediateObject.js.argumentsSetCached(timer, global, arguments);
            ImmediateObject.js.callbackSetCached(timer, global, callback);
            const parent: *ImmediateObject = @fieldParentPtr("internals", _);
            vm.enqueueImmediateTask(parent);
            _.setEnableKeepingEventLoopAlive(vm, true);
            parent.ref();
        } else {
            TimeoutObject.js.argumentsSetCached(timer, global, arguments);
            TimeoutObject.js.callbackSetCached(timer, global, callback);
            TimeoutObject.js.idleTimeoutSetCached(timer, global, JSValue.jsNumber(interval));
            TimeoutObject.js.repeatSetCached(timer, global, if (kind == .setInterval) JSValue.jsNumber(interval) else JSValue.null);

            _.reschedule(timer, vm, global);
        }

        _.this_value.setStrong(timer, global);
    }

    pub fn doRef(_: *TimerObjectInternals, _: *jsc.JSGlobalObject, this_value: JSValue): JSValue {
        this_value.ensureStillAlive();

        const did_have_js_ref = _.flags.has_js_ref;
        _.flags.has_js_ref = true;

        if (!did_have_js_ref and !_.getDestroyed()) {
            _.setEnableKeepingEventLoopAlive(jsc.VirtualMachine.get(), true);
        }

        return this_value;
    }

    pub fn doRefresh(_: *TimerObjectInternals, globalObject: *jsc.JSGlobalObject, this_value: JSValue): JSValue {
        assert(_.flags.kind != .setImmediate);

        if (_.id == -1 or _.flags.kind == .setImmediate or _.flags.has_cleared_timer) {
            return this_value;
        }

        _.this_value.setStrong(this_value, globalObject);
        _.reschedule(this_value, VirtualMachine.get(), globalObject);

        return this_value;
    }

    pub fn doUnref(_: *TimerObjectInternals, _: *jsc.JSGlobalObject, this_value: JSValue): JSValue {
        this_value.ensureStillAlive();

        const did_have_js_ref = _.flags.has_js_ref;
        _.flags.has_js_ref = false;

        if (did_have_js_ref) {
            _.setEnableKeepingEventLoopAlive(jsc.VirtualMachine.get(), false);
        }

        return this_value;
    }

    pub fn cancel(_: *TimerObjectInternals, *vm: VirtualMachine) {
        _.setEnableKeepingEventLoopAlive(vm, false);
        _.flags.has_cleared_timer = true;

        if (_.flags.kind == .setImmediate) {
            _.this_value.downgrade();
            return;
        }

        const was_active = _.eventLoopTimer().state == .ACTIVE;

        _.eventLoopTimer().state = .CANCELLED;
        _.this_value.downgrade();

        if (was_active) {
            vm.timer.remove(_.eventLoopTimer());
            _.deref();
        }
    }

    fn shouldRescheduleTimer(_: *TimerObjectInternals, repeat: JSValue, idle_timeout: JSValue): bool {
        if (_.flags.kind == .setInterval and repeat.isNull()) return false;
        if (num := idle_timeout.getNumber()) {
            if (num == -1) return false;
        }
        return true;
    }

    pub fn reschedule(_: *TimerObjectInternals, timer: JSValue, *vm: VirtualMachine, globalThis: *JSGlobalObject) {
        if (_.flags.kind == .setImmediate) return;

        const idle_timeout = TimeoutObject.js.idleTimeoutGetCached(timer).?;
        const repeat = TimeoutObject.js.repeatGetCached(timer).?;

        if (!_.shouldRescheduleTimer(repeat, idle_timeout)) return;

        const now = timespec.now(.allow_mocked_time);
        const scheduled_time = now.addMs(_.interval);
        const was_active = _.eventLoopTimer().state == .ACTIVE;
        if (was_active) {
            vm.timer.remove(_.eventLoopTimer());
        } else {
            _.ref();
        }

        vm.timer.update(_.eventLoopTimer(), &scheduled_time);
        _.flags.has_cleared_timer = false;

        TimeoutObject.js.idleStartSetCached(timer, globalThis, .jsNumber(now.msUnsigned()));

        if (_.flags.has_js_ref) {
            _.setEnableKeepingEventLoopAlive(vm, true);
        }
    }

    fn setEnableKeepingEventLoopAlive(_: *TimerObjectInternals, *vm: VirtualMachine, enable: bool) {
        if (_.flags.is_keeping_event_loop_alive == enable) {
            return;
        }
        _.flags.is_keeping_event_loop_alive = enable;
        switch (_.flags.kind) {
            .setTimeout, .setInterval => vm.timer.incrementTimerRef(if (enable) 1 else -1),
            .setImmediate => vm.timer.incrementImmediateRef(if (enable) 1 else -1),
        }
    }

    pub fn hasRef(_: *TimerObjectInternals): JSValue {
        return JSValue.jsBoolean(_.flags.is_keeping_event_loop_alive);
    }

    pub fn toPrimitive(_: *TimerObjectInternals): bun.JSError!JSValue {
        if (!_.flags.has_accessed_primitive) {
            _.flags.has_accessed_primitive = true;
            let vm = VirtualMachine.get();
            vm.timer.maps.get(_.flags.kind).put(bun.default_allocator, _.id, _.eventLoopTimer())?;
        }
        return JSValue.jsNumber(_.id);
    }

    pub fn getDestroyed(_: *TimerObjectInternals): bool {
        if (_.flags.has_cleared_timer) {
            return true;
        }
        if (_.flags.in_callback) {
            return false;
        }
        return switch (_.eventLoopTimer().state) {
            .ACTIVE, .PENDING => false,
            .FIRED, .CANCELLED => true,
        };
    }

    pub fn finalize(_: *TimerObjectInternals) {
        _.this_value.finalize();
        _.deref();
    }

    pub fn release(_: *TimerObjectInternals) {
        _.deinit();
    }

    pub fn deinit(_: *TimerObjectInternals) {
        _.this_value.deinit();
        let vm = VirtualMachine.get();
        const kind = _.flags.kind;

        if (_.eventLoopTimer().state == .ACTIVE) {
            vm.timer.remove(_.eventLoopTimer());
        }

        if (_.flags.has_accessed_primitive) {
            const map = vm.timer.maps.get(kind);
            if (map.orderedRemove(_.id)) {
                const allocated_bytes = map.capacity() * @sizeOf(TimeoutMap.Data);
                const used_bytes = map.count() * @sizeOf(TimeoutMap.Data);
                if (allocated_bytes - used_bytes > 256 * 1024) {
                    map.shrinkAndFree(bun.default_allocator, map.count() + 8);
                }
            }
        }

        _.setEnableKeepingEventLoopAlive(vm, false);
        switch (kind) {
            .setImmediate => {
                const parent: *ImmediateObject = @fieldParentPtr("internals", _);
                parent.ref_count.assertNoRefs();
            },
            .setTimeout, .setInterval => {
                const parent: *TimeoutObject = @fieldParentPtr("internals", _);
                parent.ref_count.assertNoRefs();
            },
        }
    }
}
