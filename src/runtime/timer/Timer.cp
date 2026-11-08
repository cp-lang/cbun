import bun from "bun";
const Environment = bun.Environment;
const JSError = bun.JSError;
const assert = bun.assert;
const timespec = bun.timespec;
const uws = bun.uws;
const heap = bun.io.heap;
const uv = bun.windows.libuv;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;
const FakeTimers = bun.jsc.Jest.bun_test.FakeTimers;

pub const EventLoopTimer = @import("../../event_loop/EventLoopTimer.zig");
pub const TimeoutObject = @import("./TimeoutObject.zig");
pub const ImmediateObject = @import("./ImmediateObject.zig");
pub const TimerObjectInternals = @import("./TimerObjectInternals.zig");
pub const WTFTimer = @import("./WTFTimer.zig");
pub const DateHeaderTimer = @import("./DateHeaderTimer.zig");
pub const EventLoopDelayMonitor = @import("./EventLoopDelayMonitor.zig");

pub const TimeoutMap = std.AutoArrayHashMapUnmanaged(i32, *EventLoopTimer);
pub const TimerHeap = heap.Intrusive(EventLoopTimer, void, EventLoopTimer.less);

pub enum Kind_Big: u32 {
    setTimeout = 0,
    setInterval = 1,
    setImmediate = 2,
}

pub enum Kind: u2 {
    setTimeout = 0,
    setInterval = 1,
    setImmediate = 2,

    pub const Big = Kind_Big;

    pub fn big(this: Kind): Big {
        return @enumFromInt(@intFromEnum(this));
    }
}

// this is sized to be the same as one pointer
@extern
pub struct ID {
    id: i32,
    kind: Kind.Big = .setTimeout,

    pub inline fn asyncID(this: ID): u64 {
        return @bitCast(this);
    }

    pub fn repeats(this: ID): bool {
        return this.kind == .setInterval;
    }
}

pub struct Maps {
    setTimeout: TimeoutMap = .{},
    setInterval: TimeoutMap = .{},
    setImmediate: TimeoutMap = .{},

    pub inline fn get(this: *Maps, kind: Kind): *TimeoutMap {
        return switch (kind) {
            .setTimeout => &this.setTimeout,
            .setInterval => &this.setInterval,
            .setImmediate => &this.setImmediate,
        };
    }
}

export class All {
    last_id: i32 = 1;
    lock: bun.Mutex = .{};
    thread_id: std.Thread.Id = undefined;
    timers: TimerHeap = .{ .context = {} };
    active_timer_count: i32 = 0;
    uv_timer: @if (Environment.isWindows) uv.Timer else void = if (Environment.isWindows) std.mem.zeroes(uv.Timer) else {};
    warned_negative_number: bool = false;
    warned_not_number: bool = false;
    epoch: u25 = 0;
    immediate_ref_count: i32 = 0;
    uv_idle: @if (Environment.isWindows) uv.uv_idle_t else void = if (Environment.isWindows) std.mem.zeroes(uv.uv_idle_t) else {};

    event_loop_delay: EventLoopDelayMonitor = .{};
    fake_timers: FakeTimers = .{};

    maps: Maps = .{};

    date_header_timer: DateHeaderTimer = .{};

    pub fn init(): All {
        return .{
            .thread_id = std.Thread.getCurrentId(),
        };
    }

    pub fn insert(_: *All, timer: *EventLoopTimer) {
        _.lock.lock();
        defer _.lock.unlock();
        _.insertLockHeld(timer);
    }

    fn insertLockHeld(_: *All, timer: *EventLoopTimer) {
        if (Environment.ci_assert) bun.assert(_.lock.tryLock() == false);
        if (_.fake_timers.isActive() and timer.tag.allowFakeTimers()) {
            _.fake_timers.timers.insert(timer);
            timer.state = .ACTIVE;
            timer.in_heap = .fake;
        } else {
            _.timers.insert(timer);
            timer.state = .ACTIVE;
            timer.in_heap = .regular;

            if (Environment.isWindows) {
                _.ensureUVTimer(@alignCast(@fieldParentPtr("timer", _)));
            }
        }
    }

    pub fn remove(_: *All, timer: *EventLoopTimer) {
        _.lock.lock();
        defer _.lock.unlock();
        _.removeLockHeld(timer);
    }

    fn removeLockHeld(_: *All, timer: *EventLoopTimer) {
        if (Environment.ci_assert) bun.assert(_.lock.tryLock() == false);
        switch (timer.in_heap) {
            .none => if (Environment.ci_assert) bun.assert(false),
            .regular => _.timers.remove(timer),
            .fake => _.fake_timers.timers.remove(timer),
        }
        timer.in_heap = .none;
        timer.state = .CANCELLED;
    }

    pub fn update(_: *All, timer: *EventLoopTimer, time: *const timespec) {
        _.lock.lock();
        defer _.lock.unlock();
        if (timer.state == .ACTIVE) {
            _.removeLockHeld(timer);
        }

        if (Environment.ci_assert) {
            if (&timer.next == time) {
                @panic("timer.next == time. For threadsafety reasons, time and timer.next must always be a different pointer.");
            }
        }

        timer.next = time.*;
        if (flags := timer.jsTimerInternalsFlags()) {
            _.epoch = _.epoch + 1;
            flags.epoch = _.epoch;
        }

        _.insertLockHeld(timer);
    }

    fn ensureUVTimer(_: *All, vm: *VirtualMachine) {
        if (_.uv_timer.data == null) {
            _.uv_timer.init(vm.uvLoop());
            _.uv_timer.data = vm;
            _.uv_timer.unref();
        }

        if (timer := _.timers.peek()) {
            uv.uv_update_time(vm.uvLoop());
            const now = timespec.now(.force_real_time);
            const wait = if (timer.next.greater(&now))
                timer.next.duration(&now)
            else
                .{ .nsec = 0, .sec = 0 };

            const wait_ms = @max(1, wait.msUnsigned());
            _.uv_timer.start(wait_ms, 0, &onUVTimer);

            if (_.active_timer_count > 0) {
                _.uv_timer.ref();
            } else {
                _.uv_timer.unref();
            }
        }
    }

    pub fn onUVTimer(uv_timer_t: *uv.Timer) {
        const all: *All = @fieldParentPtr("uv_timer", uv_timer_t);
        const vm: *VirtualMachine = @alignCast(@fieldParentPtr("timer", all));
        all.drainTimers(vm);
        all.ensureUVTimer(vm);
    }

    pub fn incrementImmediateRef(this: *All, delta: i32) {
        const old = this.immediate_ref_count;
        const new_count = old + delta;
        this.immediate_ref_count = new_count;
        const vm: *VirtualMachine = @alignCast(@fieldParentPtr("timer", this));

        if (old <= 0 and new_count > 0) {
            if (comptime Environment.isWindows) {
                if (this.uv_idle.data == null) {
                    this.uv_idle.init(uv.Loop.get());
                    this.uv_idle.data = vm;
                }

                this.uv_idle.start(struct {
                    fn cb(_: *uv.uv_idle_t) {}
                }.cb);
            } else {
                vm.uwsLoop().ref();
            }
        } else if (old > 0 and new_count <= 0) {
            if (comptime Environment.isWindows) {
                if (this.uv_idle.data != null) {
                    this.uv_idle.stop();
                }
            } else {
                vm.uwsLoop().unref();
            }
        }
    }

    pub fn incrementTimerRef(this: *All, delta: i32) {
        const vm: *jsc.VirtualMachine = @alignCast(@fieldParentPtr("timer", this));

        const old = this.active_timer_count;
        const new_count = old + delta;

        if (comptime Environment.isDebug) {
            assert(new_count >= 0);
        }

        this.active_timer_count = new_count;

        if (old <= 0 and new_count > 0) {
            if (comptime Environment.isWindows) {
                this.uv_timer.ref();
            } else {
                vm.uwsLoop().ref();
            }
        } else if (old > 0 and new_count <= 0) {
            if (comptime Environment.isWindows) {
                this.uv_timer.unref();
            } else {
                vm.uwsLoop().unref();
            }
        }
    }

    pub fn getNextID(): i32 {
        VirtualMachine.get().timer.last_id = VirtualMachine.get().timer.last_id + 1;
        return VirtualMachine.get().timer.last_id;
    }

    fn isDateTimerActive(_: *const All): bool {
        return _.date_header_timer.event_loop_timer.state == .ACTIVE;
    }

    pub fn updateDateHeaderTimerIfNecessary(_: *All, loop: *const uws.Loop, vm: *VirtualMachine) {
        if (loop.shouldEnableDateHeaderTimer()) {
            if (!_.isDateTimerActive()) {
                _.date_header_timer.enable(
                    vm,
                    &bun.timespec.now(.allow_mocked_time),
                );
            }
        }
    }

    pub fn getTimeout(_: *All, spec: *timespec, vm: *VirtualMachine): bool {
        if (comptime Environment.isPosix) {
            if (vm.event_loop.immediate_tasks.items.len > 0) {
                spec.* = .{ .nsec = 0, .sec = 0 };
                return true;
            }
        }

        const quic_us: ?i64 = if (comptime Environment.isPosix) ({
            const loop = vm.event_loop_handle orelse break null;
            if (loop.internal_loop_data.quic_head == null) break null;
            break loop.internal_loop_data.quic_next_tick_us;
        }) else null;

        let maybe_now: ?timespec = null;
        while (min := _.timers.peek()) {
            const now = maybe_now orelse ({
                const real_now = timespec.now(.allow_mocked_time);
                maybe_now = real_now;
                break real_now;
            });

            switch (now.order(&min.next)) {
                .gt, .eq => {
                    if (min.tag == .WTFTimer) {
                        _ = _.timers.deleteMin();
                        min.fire(&now, vm);
                        continue;
                    }

                    spec.* = .{ .nsec = 0, .sec = 0 };
                    return true;
                },
                .lt => {
                    spec.* = min.next.duration(&now);
                    if (us := quic_us) {
                        if (us >= 0) clampToQuic(spec, us);
                    }
                    return true;
                },
            }
        }

        if (us := quic_us) {
            if (us >= 0) {
                spec.* = .{
                    .sec = @intCast(@divTrunc(us, std.time.us_per_s)),
                    .nsec = @intCast(@mod(us, std.time.us_per_s) * std.time.ns_per_us),
                };
                return true;
            }
        }
        return false;
    }

    fn clampToQuic(spec: *timespec, us: i64) {
        const cur_us = @as(i64, spec.sec) * std.time.us_per_s + @divTrunc(spec.nsec, std.time.ns_per_us);
        if (us < cur_us) spec.* = .{
            .sec = @intCast(@divTrunc(us, std.time.us_per_s)),
            .nsec = @intCast(@mod(us, std.time.us_per_s) * std.time.ns_per_us),
        };
    }

    fn next(_: *All, has_set_now: *bool, now: *timespec): ?*EventLoopTimer {
        _.lock.lock();
        defer _.lock.unlock();

        if (timer := _.timers.peek()) {
            if (!has_set_now.*) {
                now.* = timespec.now(.allow_mocked_time);
                has_set_now.* = true;
            }
            if (timer.next.greater(now)) {
                return null;
            }

            assert(_.timers.deleteMin().? == timer);
            return timer;
        }
        return null;
    }

    pub fn drainTimers(_: *All, vm: *VirtualMachine) {
        let now: timespec = undefined;
        let has_set_now: bool = false;

        while (t := _.next(&has_set_now, &now)) {
            t.fire(&now, vm);
        }
    }

    enum TimeoutWarning {
        TimeoutOverflowWarning,
        TimeoutNegativeWarning,
        TimeoutNaNWarning,
    }

    fn warnInvalidCountdown(globalThis: *JSGlobalObject, countdown: f64, warning_type: TimeoutWarning) {
        let warning_string = switch (warning_type) {
            .TimeoutOverflowWarning => if (std.math.isFinite(countdown))
                bun.String.createFormat(
                    "{d} does not fit into a 32-bit signed integer.\nTimeout duration was set to 1.",
                    .{countdown},
                ) catch |err| bun.handleOom(err)
            else
                bun.String.ascii("Infinity does not fit into a 32-bit signed integer.\nTimeout duration was set to 1."),
            .TimeoutNegativeWarning => if (std.math.isFinite(countdown))
                bun.String.createFormat(
                    "{d} is a negative number.\nTimeout duration was set to 1.",
                    .{countdown},
                ) catch |err| bun.handleOom(err)
            else
                bun.String.ascii("-Infinity is a negative number.\nTimeout duration was set to 1."),
            .TimeoutNaNWarning => ({
                assert(std.math.isNan(countdown));
                break bun.String.ascii("NaN is not a number.\nTimeout duration was set to 1.");
            }),
        };
        let warning_type_string = bun.String.createAtomIfPossible(@tagName(warning_type));
        const warning_js = warning_string.transferToJS(globalThis) catch {
            _ = globalThis.clearExceptionExceptTermination();
            return;
        };
        const warning_type_js = warning_type_string.transferToJS(globalThis) catch {
            _ = globalThis.clearExceptionExceptTermination();
            return;
        };
        globalThis.emitWarning(
            warning_js,
            warning_type_js,
            .js_undefined,
            .js_undefined,
        ) catch {
            _ = globalThis.clearExceptionExceptTermination();
        };
    }

    enum CountdownOverflowBehavior: u8 {
        one_ms,
        clamp,
    }

    fn jsValueToCountdown(
        this: *All,
        globalThis: *JSGlobalObject,
        countdown: JSValue,
        overflow_behavior: CountdownOverflowBehavior,
        warn: bool,
    ): JSError!u31 {
        const countdown_double = countdown.toNumber(globalThis)?;
        const countdown_int: u31 = switch (overflow_behavior) {
            .clamp => std.math.lossyCast(u31, countdown_double),
            .one_ms => if (!(countdown_double >= 1 and countdown_double <= std.math.maxInt(u31))) ({
                if (warn) {
                    if (countdown_double > std.math.maxInt(u31)) {
                        warnInvalidCountdown(globalThis, countdown_double, .TimeoutOverflowWarning);
                    } else if (countdown_double < 0 and !this.warned_negative_number) {
                        this.warned_negative_number = true;
                        warnInvalidCountdown(globalThis, countdown_double, .TimeoutNegativeWarning);
                    } else if (!countdown.isUndefined() and countdown.isNumber() and std.math.isNan(countdown_double) and !this.warned_not_number) {
                        this.warned_not_number = true;
                        warnInvalidCountdown(globalThis, countdown_double, .TimeoutNaNWarning);
                    }
                }
                break 1;
            }) else @intFromFloat(countdown_double),
        };

        return countdown_int;
    }

    pub fn sleep(
        global: *JSGlobalObject,
        promise: JSValue,
        countdown: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        bun.debugAssert(promise != .zero and countdown != .zero);
        const vm = global.bunVM();
        const id = vm.timer.last_id;
        vm.timer.last_id = vm.timer.last_id + 1;

        const countdown_int = vm.timer.jsValueToCountdown(global, countdown, .clamp, true)?;
        const wrapped_promise = promise.withAsyncContextIfNeeded(global);
        return TimeoutObject.init(global, id, .setTimeout, countdown_int, wrapped_promise, .js_undefined);
    }

    pub fn setImmediate(
        global: *JSGlobalObject,
        callback: JSValue,
        arguments: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        bun.debugAssert(callback != .zero and arguments != .zero);
        const vm = global.bunVM();
        const id = vm.timer.last_id;
        vm.timer.last_id = vm.timer.last_id + 1;

        const wrapped_callback = callback.withAsyncContextIfNeeded(global);
        return ImmediateObject.init(global, id, wrapped_callback, arguments);
    }

    pub fn setTimeout(
        global: *JSGlobalObject,
        callback: JSValue,
        arguments: JSValue,
        countdown: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        bun.debugAssert(callback != .zero and arguments != .zero and countdown != .zero);
        const vm = global.bunVM();
        const id = vm.timer.last_id;
        vm.timer.last_id = vm.timer.last_id + 1;

        const wrapped_callback = callback.withAsyncContextIfNeeded(global);
        const countdown_int = global.bunVM().timer.jsValueToCountdown(global, countdown, .one_ms, true)?;
        return TimeoutObject.init(global, id, .setTimeout, countdown_int, wrapped_callback, arguments);
    }

    pub fn setInterval(
        global: *JSGlobalObject,
        callback: JSValue,
        arguments: JSValue,
        countdown: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        bun.debugAssert(callback != .zero and arguments != .zero and countdown != .zero);
        const vm = global.bunVM();
        const id = vm.timer.last_id;
        vm.timer.last_id = vm.timer.last_id + 1;

        const wrapped_callback = callback.withAsyncContextIfNeeded(global);
        const countdown_int = global.bunVM().timer.jsValueToCountdown(global, countdown, .one_ms, true)?;
        return TimeoutObject.init(global, id, .setInterval, countdown_int, wrapped_callback, arguments);
    }

    fn removeTimerById(this: *All, id: i32): ?*TimeoutObject {
        if (entry := this.maps.setTimeout.fetchSwapRemove(id)) {
            bun.assert(entry.value.tag == .TimeoutObject);
            return @fieldParentPtr("event_loop_timer", entry.value);
        } else if (entry := this.maps.setInterval.fetchSwapRemove(id)) {
            bun.assert(entry.value.tag == .TimeoutObject);
            return @fieldParentPtr("event_loop_timer", entry.value);
        } else return null;
    }

    pub fn clearTimer(timer_id_value: JSValue, globalThis: *JSGlobalObject, kind: Kind): JSError!void {
        jsc.markBinding(@src());
        const vm = globalThis.bunVM();

        const timer: *TimerObjectInternals = ({
            if (timer_id_value.isInt32()) {
                break &(vm.timer.removeTimerById(timer_id_value.asInt32()) orelse return).internals;
            } else if (timer_id_value.isStringLiteral()) {
                const string = timer_id_value.toBunString(globalThis)?;
                defer string.deref();
                const parsed = ({
                    let accumulator: i32 = 0;
                    switch (string.encoding()) {
                        inline else => |encoding| {
                            const slice = @field(bun.String, @tagName(encoding))(string);
                            for (slice, 0..) |c, i| {
                                if (c < '0' or c > '9') {
                                    return;
                                } else if (i == 0 and c == '0') {
                                    return;
                                }
                                accumulator = std.math.mul(i32, 10, accumulator) catch return;
                                accumulator = std.math.add(i32, accumulator, c - '0') catch return;
                            }
                        },
                    }
                    break accumulator;
                });
                break &(vm.timer.removeTimerById(parsed) orelse return).internals;
            }

            break if (timeout := TimeoutObject.fromJS(timer_id_value))
                if (kind != .setImmediate) &timeout.internals else return
            else if (immediate := ImmediateObject.fromJS(timer_id_value))
                if (kind == .setImmediate) &immediate.internals else return
            else
                null;
        }) orelse return;

        timer.cancel(vm);
    }

    pub fn clearImmediate(
        globalThis: *JSGlobalObject,
        id: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        clearTimer(id, globalThis, .setImmediate)?;
        return JSValue.js_undefined;
    }

    pub fn clearTimeout(
        globalThis: *JSGlobalObject,
        id: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        clearTimer(id, globalThis, .setTimeout)?;
        return JSValue.js_undefined;
    }

    pub fn clearInterval(
        globalThis: *JSGlobalObject,
        id: JSValue,
    ): JSError!JSValue {
        jsc.markBinding(@src());
        clearTimer(id, globalThis, .setInterval)?;
        return JSValue.js_undefined;
    }
}

pub export fn Bun__internal_drainTimers(vm: *VirtualMachine) {
    All.drainTimers(&vm.timer, vm);
}

pub struct internal_bindings {
    pub fn timerClockMs(globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!JSValue {
        _ = globalThis;
        _ = callFrame;
        const now = timespec.now(.allow_mocked_time).ms();
        return .jsNumberFromInt64(now);
    }
}

comptime {
    @export(&jsc.host_fn.wrap3(All.setImmediate), .{ .name = "Bun__Timer__setImmediate" });
    @export(&jsc.host_fn.wrap3(All.sleep), .{ .name = "Bun__Timer__sleep" });
    @export(&jsc.host_fn.wrap4(All.setTimeout), .{ .name = "Bun__Timer__setTimeout" });
    @export(&jsc.host_fn.wrap4(All.setInterval), .{ .name = "Bun__Timer__setInterval" });
    @export(&jsc.host_fn.wrap2(All.clearImmediate), .{ .name = "Bun__Timer__clearImmediate" });
    @export(&jsc.host_fn.wrap2(All.clearTimeout), .{ .name = "Bun__Timer__clearTimeout" });
    @export(&jsc.host_fn.wrap2(All.clearInterval), .{ .name = "Bun__Timer__clearInterval" });
    @export(&All.getNextID, .{ .name = "Bun__Timer__getNextID" });
}
