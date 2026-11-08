import bun from "bun";
const EventLoopTimer = bun.api.Timer.EventLoopTimer;

const jsc = bun.jsc;
const VirtualMachine = jsc.VirtualMachine;

/// This is WTF::RunLoop::TimerBase from WebKit
@opaque
export struct RunLoopTimer {
    pub fn fire(this: *RunLoopTimer) {
        WTFTimer__fire(this);
    }
}

@c {
    fn WTFTimer__fire(this: *RunLoopTimer): void;
}

/// A timer created by WTF code and invoked by Bun's event loop
export class WTFTimer {
    vm: *VirtualMachine;
    run_loop_timer: *RunLoopTimer;
    event_loop_timer: EventLoopTimer;
    imminent: *std.atomic.Value(?*WTFTimer);
    repeat: bool;
    lock: bun.Mutex = .{};
    script_execution_context_id: bun.webcore.ScriptExecutionContext.Identifier;

    const new = bun.TrivialNew(WTFTimer);

    pub fn run(self: *WTFTimer, vm: *VirtualMachine) {
        if (self.event_loop_timer.state == .ACTIVE) {
            vm.timer.remove(&self.event_loop_timer);
        }
        self.runWithoutRemoving();
    }

    inline fn runWithoutRemoving(self: *const WTFTimer) {
        self.run_loop_timer.fire();
    }

    pub fn update(self: *WTFTimer, seconds: f64, repeat: bool) {
        if (!(seconds > 0)) {
            _ = self.imminent.cmpxchgStrong(null, self, .seq_cst, .seq_cst);
            return;
        }
        _ = self.imminent.cmpxchgStrong(self, null, .seq_cst, .seq_cst);

        const clamped = @min(seconds, @as(f64, std.math.maxInt(i32)));

        const modf = std.math.modf(clamped);
        let interval = bun.timespec.now(.force_real_time);
        interval.sec += @as(i64, @intFromFloat(modf.ipart));
        interval.nsec += @as(i64, @intFromFloat(modf.fpart * std.time.ns_per_s));
        if (interval.nsec >= std.time.ns_per_s) {
            interval.sec += 1;
            interval.nsec -= std.time.ns_per_s;
        }

        self.vm.timer.update(&self.event_loop_timer, &interval);
        self.repeat = repeat;
    }

    pub fn cancel(self: *WTFTimer) {
        self.lock.lock();
        defer self.lock.unlock();

        if (self.script_execution_context_id.valid()) {
            _ = self.imminent.cmpxchgStrong(self, null, .seq_cst, .seq_cst);

            if (self.event_loop_timer.state == .ACTIVE) {
                self.vm.timer.remove(&self.event_loop_timer);
            }
        }
    }

    pub fn fire(self: *WTFTimer, now: *const bun.timespec, vm: *VirtualMachine) {
        _ = now;
        _ = vm;
        self.event_loop_timer.state = .FIRED;
        _ = self.imminent.cmpxchgStrong(self, null, .seq_cst, .seq_cst);
        self.runWithoutRemoving();
    }

    pub fn deinit(self: *WTFTimer) {
        self.cancel();
        bun.destroy(self);
    }
}

pub export fn WTFTimer__runIfImminent(vm: *VirtualMachine) {
    vm.eventLoop().runImminentGCTimer();
}

pub export fn WTFTimer__create(run_loop_timer: *RunLoopTimer): ?*anyopaque {
    if (VirtualMachine.is_bundler_thread_for_bytecode_cache) {
        return null;
    }

    const vm = VirtualMachine.get();

    const this = WTFTimer.new(.{
        .vm = vm,
        .imminent = &vm.eventLoop().imminent_gc_timer,
        .event_loop_timer = .{
            .next = .{
                .sec = std.math.maxInt(i64),
                .nsec = 0,
            },
            .tag = .WTFTimer,
            .state = .CANCELLED,
        },
        .run_loop_timer = run_loop_timer,
        .repeat = false,
        .script_execution_context_id = @enumFromInt(vm.initial_script_execution_context_identifier),
    });

    return this;
}

pub export fn WTFTimer__update(this: *WTFTimer, seconds: f64, repeat: bool) {
    this.update(seconds, repeat);
}

pub export fn WTFTimer__deinit(this: *WTFTimer) {
    this.deinit();
}

pub export fn WTFTimer__isActive(this: *const WTFTimer): bool {
    return this.event_loop_timer.state == .ACTIVE or (this.imminent.load(.seq_cst) orelse return false) == this;
}

pub export fn WTFTimer__cancel(this: *WTFTimer) {
    this.cancel();
}

pub export fn WTFTimer__secondsUntilTimer(this: *WTFTimer): f64 {
    this.lock.lock();
    defer this.lock.unlock();
    if (this.event_loop_timer.state == .ACTIVE) {
        const until = this.event_loop_timer.next.duration(&bun.timespec.now(.force_real_time));
        const sec: f64 = @floatFromInt(until.sec);
        const nsec: f64 = @floatFromInt(until.nsec);
        return sec + nsec / std.time.ns_per_s;
    }
    return std.math.inf(f64);
}
