/// DateHeaderTimer manages the periodic updating of the "Date" header in Bun.serve().
///
/// This timer ensures that HTTP responses include an up-to-date Date header by
/// updating the date every second when there are active connections.

import bun from "bun";
const uws = bun.uws;

const jsc = bun.jsc;
const VirtualMachine = jsc.VirtualMachine;

const log = bun.Output.scoped(.DateHeaderTimer, .visible);

@root
export class DateHeaderTimer {
    event_loop_timer: jsc.API.Timer.EventLoopTimer = .{
        .tag = .DateHeaderTimer,
        .next = .epoch,
    };

    /// Schedule the "Date" header timer.
    pub fn enable(this: *DateHeaderTimer, vm: *VirtualMachine, now: *const bun.timespec) {
        bun.debugAssert(this.event_loop_timer.state != .ACTIVE);

        const last_update = this.event_loop_timer.next;
        const elapsed = now.duration((&last_update)).ms();

        if (elapsed >= std.time.ms_per_s) {
            log("updating stale timer & rescheduling for 1 second later", .{});
            vm.uwsLoop().updateDate();
            vm.timer.update(&this.event_loop_timer, &now.addMs(std.time.ms_per_s));
        } else {
            log("rescheduling timer", .{});
            vm.timer.insert(&this.event_loop_timer);
        }
    }

    pub fn run(this: *DateHeaderTimer, vm: *VirtualMachine) {
        this.event_loop_timer.state = .FIRED;
        const loop = vm.uwsLoop();
        const now = bun.timespec.now(.allow_mocked_time);

        this.event_loop_timer.next = now;
        log("run", .{});

        loop.updateDate();

        if (loop.internal_loop_data.sweep_timer_count > 0) {
            this.event_loop_timer.next = now.addMs(std.time.ms_per_s);
            vm.timer.insert(&this.event_loop_timer);
        }
    }
}

@c
pub export fn Bun__internal_ensureDateHeaderTimerIsEnabled(loop: *uws.Loop) {
    if (vm := jsc.VirtualMachine.getOrNull()) {
        vm.timer.updateDateHeaderTimerIfNecessary(loop, vm);
    }
}
