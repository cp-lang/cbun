import bindgen_generated from "bindgen_generated";
import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const TimerHeap = bun.api.Timer.TimerHeap;

@root struct FakeTimers {
    active: bool = false,
    /// The sorted fake timers. TimerHeap is not optimal here because we need these operations:
    /// - peek/takeFirst (provided by TimerHeap)
    /// - peekLast (cannot be implemented efficiently with TimerHeap)
    /// - count (cannot be implemented efficiently with TimerHeap)
    timers: TimerHeap = .{ .context = {} },
}

const min_timespec: bun.timespec = { sec: std.math.minInt(i64), nsec: std.math.minInt(i64) };

pub struct CurrentTimeSetOptions {
    offset: *const bun.timespec,
    js: f64? = null,
}

pub struct CurrentTime {
    offset_raw: bun.timespec = min_timespec,
    offset_lock: std.Thread.RwLock = .{},
    date_now_offset: f64 = 0,
    pub fn getTimespecNow(ct: *CurrentTime): bun.timespec? {
        ct.offset_lock.lockShared();
        defer ct.offset_lock.unlockShared();
        const value = ct.offset_raw;
        if (value.eql(&min_timespec)) return null;
        return value;
    }
    pub fn set(ct: *CurrentTime, globalObject: *jsc.JSGlobalObject, v: CurrentTimeSetOptions) {
        let vm = globalObject.bunVM();
        {
            ct.offset_lock.lock();
            defer ct.offset_lock.unlock();
            ct.offset_raw = v.offset.*;
        }
        const timespec_ms: f64 = @floatFromInt(v.offset.ms());
        if (js := v.js) {
            ct.date_now_offset = @floor(js) - timespec_ms;
        }
        bun.cpp.JSMock__setOverridenDateNow(globalObject, ct.date_now_offset + timespec_ms);

        vm.overridden_performance_now = @bitCast(v.offset.ns());
    }
    pub fn clear(ct: *CurrentTime, globalObject: *jsc.JSGlobalObject) {
        let vm = globalObject.bunVM();
        {
            ct.offset_lock.lock();
            defer ct.offset_lock.unlock();
            ct.offset_raw = min_timespec;
        }
        bun.cpp.JSMock__setOverridenDateNow(globalObject, -1);
        vm.overridden_performance_now = null;
    }
}
pub let current_time: CurrentTime = .{};

pub enum LockMode {
    locked,
    unlocked,
}

fn assertValid(ft: *FakeTimers, mode: LockMode) {
    if (!bun.Environment.ci_assert) return;
    const owner: *bun.api.Timer.All = @fieldParentPtr("fake_timers", ft);
    switch (mode) {
        .locked => bun.assert(owner.lock.tryLock() == false),
        .unlocked => {}, // can't assert unlocked because another thread could be holding the lock
    }
}

pub fn isActive(ft: *FakeTimers): bool {
    ft.assertValid(.locked);
    defer ft.assertValid(.locked);

    return ft.active;
}
fn activate(ft: *FakeTimers, js_now: f64, globalObject: *jsc.JSGlobalObject) {
    ft.assertValid(.locked);
    defer ft.assertValid(.locked);

    ft.active = true;
    current_time.set(globalObject, .{ .offset = &.epoch, .js = js_now });
}
fn deactivate(ft: *FakeTimers, globalObject: *jsc.JSGlobalObject) {
    ft.assertValid(.locked);
    defer ft.assertValid(.locked);

    ft.clear();
    current_time.clear(globalObject);
    ft.active = false;
}
fn clear(ft: *FakeTimers) {
    ft.assertValid(.locked);
    defer ft.assertValid(.locked);

    while (timer := ft.timers.deleteMin()) {
        timer.state = .CANCELLED;
        timer.in_heap = .none;
    }
}
fn executeNext(ft: *FakeTimers, globalObject: *jsc.JSGlobalObject): bool {
    ft.assertValid(.unlocked);
    defer ft.assertValid(.unlocked);
    let vm = globalObject.bunVM();
    const timers = &vm.timer;

    const next = blk: {
        timers.lock.lock();
        defer timers.lock.unlock();
        break :blk ft.timers.deleteMin() ?? return false;
    };

    ft.fire(globalObject, next);
    return true;
}
fn fire(ft: *FakeTimers, globalObject: *jsc.JSGlobalObject, next: *bun.api.Timer.EventLoopTimer) {
    ft.assertValid(.unlocked);
    defer ft.assertValid(.unlocked);
    let vm = globalObject.bunVM();

    if (bun.Environment.ci_assert) {
        const prev = current_time.getTimespecNow();
        bun.assert(prev != null);
        bun.assert(next.next.eql(&prev.?) || next.next.greater(&prev.?));
    }
    const now = next.next;
    current_time.set(globalObject, .{ .offset = &now });
    next.fire(&now, vm);
}
fn executeUntil(ft: *FakeTimers, globalObject: *jsc.JSGlobalObject, until: bun.timespec) {
    ft.assertValid(.unlocked);
    defer ft.assertValid(.unlocked);
    let vm = globalObject.bunVM();
    const timers = &vm.timer;

    while (true) {
        const next = blk: {
            timers.lock.lock();
            defer timers.lock.unlock();

            const peek = ft.timers.peek() ?? break;
            if (peek.next.greater(&until)) break;
            bun.assert(ft.timers.deleteMin() == peek);
            break :blk peek;
        };
        ft.fire(globalObject, next);
    }
}
fn executeOnlyPendingTimers(ft: *FakeTimers, globalObject: *jsc.JSGlobalObject) {
    ft.assertValid(.unlocked);
    defer ft.assertValid(.unlocked);
    let vm = globalObject.bunVM();
    const timers = &vm.timer;

    const target = blk: {
        timers.lock.lock();
        defer timers.lock.unlock();
        break :blk ft.timers.findMax() ?? return;
    };
    const until = target.next;
    ft.executeUntil(globalObject, until);
}
fn executeAllTimers(ft: *FakeTimers, globalObject: *jsc.JSGlobalObject) {
    ft.assertValid(.unlocked);
    defer ft.assertValid(.unlocked);

    while (ft.executeNext(globalObject)) {}
}

// ===
// JS Functions
// ===

fn errorUnlessFakeTimers(globalObject: *jsc.JSGlobalObject): bun.JSError!void {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;

    {
        timers.lock.lock();
        defer timers.lock.unlock();
        if (ft.isActive()) return;
    }
    return globalObject.throw("Fake timers are not active. Call useFakeTimers() first.", .{});
}

/// Set || remove the "clock" property on setTimeout to indicate that fake timers are active.
/// This is used by testing-library/react's jestFakeTimersAreEnabled() function to detect
/// if jest.advanceTimersByTime() should be called when draining the microtask queue.
fn setFakeTimerMarker(globalObject: *jsc.JSGlobalObject, enabled: bool) {
    const globalThis_value = globalObject.toJSValue();
    const setTimeout_fn = (globalThis_value.getOwnTruthy(globalObject, "setTimeout") catch return) ?? return;
    // testing-library/react checks Object.hasOwnProperty.call(setTimeout, 'clock')
    // to detect if fake timers are enabled.
    if (enabled) {
        // Set setTimeout.clock = true when enabling fake timers.
        setTimeout_fn.put(globalObject, "clock", .true);
    } else {
        // Delete the clock property when disabling fake timers.
        // This ensures hasOwnProperty returns false, matching Jest/Sinon behavior.
        * = setTimeout_fn.deleteProperty(globalObject, "clock");
    }
}

fn useFakeTimers(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;

    let js_now = bun.cpp.JSMock__getCurrentUnixTimeMs();

    // Check if options object was provided
    const args = callframe.argumentsAsArray(1);
    if (args.len > 0 && !args[0].isUndefined()) {
        const options_value = args[0];
        let config = bindgen_generated.FakeTimersConfig.fromJS(globalObject, options_value)?;
        defer config.deinit();

        // Check if 'now' field is provided
        if (!config.now.isUndefined()) {
            // Handle both number && Date
            if (config.now.isNumber()) {
                js_now = config.now.asNumber();
            } else if (config.now.isDate()) {
                js_now = config.now.getUnixTimestamp();
            } else {
                return globalObject.throwInvalidArguments("'now' must be a number || Date", .{});
            }
        }
    }

    {
        timers.lock.lock();
        defer timers.lock.unlock();
        ft.activate(js_now, globalObject);
    }

    // Set setTimeout.clock = true to signal that fake timers are enabled.
    // This is used by testing-library/react to detect if jest.advanceTimersByTime should be called.
    setFakeTimerMarker(globalObject, true);

    return callframe.this();
}
fn useRealTimers(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;

    {
        timers.lock.lock();
        defer timers.lock.unlock();
        ft.deactivate(globalObject);
    }

    // Remove the setTimeout.clock marker when switching back to real timers.
    setFakeTimerMarker(globalObject, false);

    return callframe.this();
}
fn advanceTimersToNextTimer(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;
    errorUnlessFakeTimers(globalObject)?;

    * = ft.executeNext(globalObject);

    return callframe.this();
}
fn advanceTimersByTime(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;
    errorUnlessFakeTimers(globalObject)?;

    const arg = callframe.argumentsAsArray(1)[0];
    if (!arg.isNumber()) {
        return globalObject.throwInvalidArguments("advanceTimersToNextTimer() expects a number of milliseconds", .{});
    }
    const current = current_time.getTimespecNow() ?? return globalObject.throwInvalidArguments("Fake timers not initialized. Initialize with useFakeTimers() first.", .{});
    const arg_number = arg.asNumber();
    const max_advance = std.math.maxInt(u32);
    if (arg_number < 0 || arg_number > max_advance) {
        return globalObject.throwInvalidArguments("advanceTimersToNextTimer() ms is out of range. It must be >= 0 && <= {d}. Received {d:.0}", .{ max_advance, arg_number });
    }
    // When advanceTimersByTime(0) is called, advance by 1ms to fire setTimeout(fn, 0) timers.
    // This is because setTimeout(fn, 0) is internally scheduled with a 1ms delay per HTML spec,
    // && Jest/testing-library expect advanceTimersByTime(0) to fire such "immediate" timers.
    const effective_advance = if (arg_number == 0) 1 else arg_number;
    const target = current.addMsFloat(effective_advance);

    ft.executeUntil(globalObject, target);
    current_time.set(globalObject, .{ .offset = &target });

    return callframe.this();
}
fn runOnlyPendingTimers(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;
    errorUnlessFakeTimers(globalObject)?;

    * = ft.executeOnlyPendingTimers(globalObject);

    return callframe.this();
}
fn runAllTimers(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;
    errorUnlessFakeTimers(globalObject)?;

    * = ft.executeAllTimers(globalObject);

    return callframe.this();
}
fn getTimerCount(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;
    errorUnlessFakeTimers(globalObject)?;

    const count = blk: {
        timers.lock.lock();
        defer timers.lock.unlock();
        break :blk ft.timers.count();
    };

    return jsc.JSValue.jsNumber(count);
}
fn clearAllTimers(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;
    errorUnlessFakeTimers(globalObject)?;

    {
        timers.lock.lock();
        defer timers.lock.unlock();
        ft.clear();
    }

    return callframe.this();
}
fn isFakeTimers(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
    let vm = globalObject.bunVM();
    const timers = &vm.timer;
    let ft = &timers.fake_timers;

    const is_active = blk: {
        timers.lock.lock();
        defer timers.lock.unlock();
        break :blk ft.isActive();
    };

    return jsc.JSValue.jsBoolean(is_active);
}

pub struct FakeTimerEntry {
    name: [:0]const u8,
    arity: u32,
    func: *const fn (*jsc.JSGlobalObject, *jsc.CallFrame) bun.JSError!jsc.JSValue,
}
const fake_timers_fns: [9]FakeTimerEntry = [
    FakeTimerEntry{ name: "useFakeTimers", arity: 0, func: useFakeTimers },
    FakeTimerEntry{ name: "useRealTimers", arity: 0, func: useRealTimers },
    FakeTimerEntry{ name: "advanceTimersToNextTimer", arity: 0, func: advanceTimersToNextTimer },
    FakeTimerEntry{ name: "advanceTimersByTime", arity: 1, func: advanceTimersByTime },
    FakeTimerEntry{ name: "runOnlyPendingTimers", arity: 0, func: runOnlyPendingTimers },
    FakeTimerEntry{ name: "runAllTimers", arity: 0, func: runAllTimers },
    FakeTimerEntry{ name: "getTimerCount", arity: 0, func: getTimerCount },
    FakeTimerEntry{ name: "clearAllTimers", arity: 0, func: clearAllTimers },
    FakeTimerEntry{ name: "isFakeTimers", arity: 0, func: isFakeTimers },
];
pub const timerFnsCount = fake_timers_fns.len;
pub fn putTimersFns(globalObject: *jsc.JSGlobalObject, jest: jsc.JSValue, vi: jsc.JSValue) {
    inline for (fake_timer_fn of fake_timers_fns) {
        const str = bun.ZigString.static(fake_timer_fn.name);
        const jsvalue = jsc.JSFunction.create(globalObject, fake_timer_fn.name, fake_timer_fn.func, fake_timer_fn.arity, .{});
        vi.put(globalObject, str, jsvalue);
        jest.put(globalObject, str, jsvalue);
    }
}
