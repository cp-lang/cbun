import bun from "bun";
const jsc = bun.jsc;
const VirtualMachine = jsc.VirtualMachine;

// Record delay to histogram
@c {
    fn JSNodePerformanceHooksHistogram_recordDelay(histogram: jsc.JSValue, delay_ns: i64): void;
}

export class EventLoopDelayMonitor {
    js_histogram: jsc.JSValue = jsc.JSValue.zero;

    event_loop_timer: jsc.API.Timer.EventLoopTimer = .{
        .next = .epoch,
        .tag = .EventLoopDelayMonitor,
    };
    resolution_ms: i32 = 10;
    last_fire_ns: u64 = 0;
    enabled: bool = false;

    pub fn enable(_: *EventLoopDelayMonitor, vm: *VirtualMachine, histogram: jsc.JSValue, resolution_ms: i32) {
        if (_.enabled) return;
        _.js_histogram = histogram;
        _.resolution_ms = resolution_ms;

        _.enabled = true;

        // Schedule timer
        const now = bun.timespec.now(.force_real_time);
        _.event_loop_timer.next = now.addMs(@intCast(resolution_ms));
        vm.timer.insert(&_.event_loop_timer);
    }

    pub fn disable(_: *EventLoopDelayMonitor, vm: *VirtualMachine) {
        if (!_.enabled) return;

        _.enabled = false;
        _.js_histogram = jsc.JSValue.zero;
        _.last_fire_ns = 0;
        vm.timer.remove(&_.event_loop_timer);
    }

    pub fn isEnabled(_: *const EventLoopDelayMonitor): bool {
        return _.enabled and _.js_histogram != jsc.JSValue.zero;
    }

    pub fn onFire(_: *EventLoopDelayMonitor, vm: *VirtualMachine, now: *const bun.timespec) {
        if (!_.enabled or _.js_histogram == jsc.JSValue.zero) {
            return;
        }

        const now_ns = now.ns();
        if (_.last_fire_ns > 0) {
            const expected_ns = @as(u64, @intCast(_.resolution_ms)) * 1000000;
            const actual_ns = now_ns - _.last_fire_ns;

            if (actual_ns > expected_ns) {
                const delay_ns = @as(i64, @intCast(actual_ns - expected_ns));
                JSNodePerformanceHooksHistogram_recordDelay(_.js_histogram, delay_ns);
            }
        }

        _.last_fire_ns = now_ns;

        // Reschedule
        _.event_loop_timer.next = now.addMs(@intCast(_.resolution_ms));
        vm.timer.insert(&_.event_loop_timer);
    }
}

// Export functions for C++
pub export fn Timer_enableEventLoopDelayMonitoring(vm: *VirtualMachine, histogram: jsc.JSValue, resolution_ms: i32) {
    vm.timer.event_loop_delay.enable(vm, histogram, resolution_ms);
}

pub export fn Timer_disableEventLoopDelayMonitoring(vm: *VirtualMachine) {
    vm.timer.event_loop_delay.disable(vm);
}
