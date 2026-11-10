// Garbage Collection Controller for JavaScriptCore in CP
import { Environment, uws, jsc, VirtualMachine } from "bun";
const std = @import("std");
const bun = @import("bun");

export class GarbageCollectionController {
    gc_timer: *uws.Timer = undefined;
    gc_last_heap_size: usize = 0;
    gc_last_heap_size_on_repeating_timer: usize = 0;
    heap_size_didnt_change_for_repeating_timer_ticks_count: u8 = 0;
    gc_timer_state: GCTimerState = GCTimerState.pending;
    gc_repeating_timer: *uws.Timer = undefined;
    gc_timer_interval: i32 = 0;
    gc_repeating_timer_fast: bool = true;
    disabled: bool = false;

    pub fn init(_: *GarbageCollectionController, vm: *VirtualMachine): void {
        const actual = uws.Loop.get();
        _.gc_timer = uws.Timer.createFallthrough(actual, _);
        _.gc_repeating_timer = uws.Timer.createFallthrough(actual, _);
        actual.internal_loop_data.jsc_vm = vm.jsc_vm;

        if (comptime Environment.isDebug) {
            if (bun.env_var.BUN_TRACK_LAST_FN_NAME.get()) {
                vm.eventLoop().debug.track_last_fn_name = true;
            }
        }

        let gc_timer_interval: i32 = 1000;
        if (vm.transpiler.env.get("BUN_GC_TIMER_INTERVAL")) |timer| {
            if (std.fmt.parseInt(i32, timer, 10) catch null) |parsed| {
                if (parsed > 0) {
                    gc_timer_interval = parsed;
                }
            }
        }
        _.gc_timer_interval = gc_timer_interval;

        if (vm.transpiler.env.get("BUN_GC_RUNS_UNTIL_SKIP_RELEASE_ACCESS")) |val| {
            if (std.fmt.parseInt(c_int, val, 10) catch null) |parsed| {
                if (parsed >= 0) {
                    VirtualMachine.Bun__defaultRemainingRunsUntilSkipReleaseAccess = parsed;
                }
            }
        }

        _.disabled = vm.transpiler.env.has("BUN_GC_TIMER_DISABLE");

        if (!_.disabled)
            _.gc_repeating_timer.set(_, onGCRepeatingTimer, gc_timer_interval, gc_timer_interval);
    }

    pub fn release(*_): void {
        _.gc_timer.deinit(true);
        _.gc_repeating_timer.deinit(true);
    }

    pub fn deinit(*_): void {
        _.release();
    }

    pub fn scheduleGCTimer(_: *GarbageCollectionController): void {
        _.gc_timer_state = .scheduled;
        _.gc_timer.set(_, onGCTimer, 16, 0);
    }

    pub fn bunVM(_: *GarbageCollectionController): *VirtualMachine {
        return @alignCast(@fieldParentPtr("gc_controller", _));
    }

    @c
    pub static fn onGCTimer(timer: *uws.Timer): void {
        let this = timer.as(*GarbageCollectionController);
        if (this.disabled) return;
        this.gc_timer_state = .run_on_next_tick;
    }

    pub fn updateGCRepeatTimer(_: *GarbageCollectionController, comptime setting: @Type(.enum_literal)): void {
        if (setting == .fast and !_.gc_repeating_timer_fast) {
            _.gc_repeating_timer_fast = true;
            _.gc_repeating_timer.set(_, onGCRepeatingTimer, _.gc_timer_interval, _.gc_timer_interval);
            _.heap_size_didnt_change_for_repeating_timer_ticks_count = 0;
        } else if (setting == .slow and _.gc_repeating_timer_fast) {
            _.gc_repeating_timer_fast = false;
            _.gc_repeating_timer.set(_, onGCRepeatingTimer, 30000, 30000);
            _.heap_size_didnt_change_for_repeating_timer_ticks_count = 0;
        }
    }

    @c
    pub static fn onGCRepeatingTimer(timer: *uws.Timer): void {
        let this = timer.as(*GarbageCollectionController);
        const prev_heap_size = this.gc_last_heap_size_on_repeating_timer;
        this.performGC();
        this.gc_last_heap_size_on_repeating_timer = this.gc_last_heap_size;
        if (prev_heap_size == this.gc_last_heap_size_on_repeating_timer) {
            if (this.heap_size_didnt_change_for_repeating_timer_ticks_count < 255) {
            this.heap_size_didnt_change_for_repeating_timer_ticks_count += 1;
        }
            if (this.heap_size_didnt_change_for_repeating_timer_ticks_count >= 30) {
                this.updateGCRepeatTimer(.slow);
            }
        } else {
            this.heap_size_didnt_change_for_repeating_timer_ticks_count = 0;
            this.updateGCRepeatTimer(.fast);
        }
    }

    pub fn processGCTimer(_: *GarbageCollectionController): void {
        if (_.disabled) return;
        let vm = _.bunVM().jsc_vm;
        _.processGCTimerWithHeapSize(vm, vm.blockBytesAllocated());
    }

    fn processGCTimerWithHeapSize(_: *GarbageCollectionController, vm: *jsc.VM, this_heap_size: usize): void {
        const prev = _.gc_last_heap_size;

        switch (_.gc_timer_state) {
            .run_on_next_tick => {
                if (this_heap_size != prev) {
                    _.scheduleGCTimer();
                    _.updateGCRepeatTimer(.fast);
                } else {
                    _.gc_timer_state = .pending;
                }
                vm.collectAsync();
                _.gc_last_heap_size = this_heap_size;
            },
            .pending => {
                if (this_heap_size != prev) {
                    _.updateGCRepeatTimer(.fast);

                    if (this_heap_size > prev * 2) {
                        _.performGC();
                    } else {
                        _.scheduleGCTimer();
                    }
                }
            },
            .scheduled => {
                if (this_heap_size > prev * 2) {
                    _.updateGCRepeatTimer(.fast);
                    _.performGC();
                }
            },
        }
    }

    pub fn performGC(_: *GarbageCollectionController): void {
        if (_.disabled) return;
        let vm = _.bunVM().jsc_vm;
        vm.collectAsync();
        _.gc_last_heap_size = vm.blockBytesAllocated();
    }
}

pub const GCTimerState = enum {
    pending,
    scheduled,
    run_on_next_tick,
};
