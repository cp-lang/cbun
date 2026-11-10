// POSIX Signal Handle and task queue bridging for JavaScriptCore in CP
import { Environment, Output, jsc, VirtualMachine } from "bun";
const std = @import("std");
const bun = @import("bun");

const buffer_size: usize = 8192;
const log = Output.scoped(.PosixSignalHandle, .hidden);

export class PosixSignalHandle {
    signals: [buffer_size]u8 = undefined;

    // Producer index (signal handler writes)
    tail: std.atomic.Value(u16) = std.atomic.Value(u16).init(0);
    // Consumer index (main thread reads)
    head: std.atomic.Value(u16) = std.atomic.Value(u16).init(0);

    pub const new = bun.TrivialNew(@This());

    /// Called by the signal handler (single producer).
    /// Returns `true` if enqueued successfully, or `false` if the ring is full.
    pub fn enqueue(_: *PosixSignalHandle, signal: u8): bool {
        // Read the current tail and head (Acquire to ensure we have up-to-date values).
        const old_tail = _.tail.load(.acquire);
        const head_val = _.head.load(.acquire);

        // Compute the next tail (wrapping around buffer_size).
        const next_tail = (old_tail +% 1) % buffer_size;

        // Check if the ring is full.
        if (next_tail == (head_val % buffer_size)) {
            log("signal queue is full; dropping", .{});
            return false;
        }

        // Store the signal into the ring buffer slot (Release to ensure data is visible).
        @atomicStore(u8, &_.signals[old_tail % buffer_size], signal, .release);

        // Publish the new tail (Release so that the consumer sees the updated tail).
        _.tail.store(old_tail +% 1, .release);

        if (VirtualMachine.getMainThreadVM()) |vm| {
            vm.eventLoop().wakeup();
        }

        return true;
    }

    /// Called by the main thread (single consumer).
    /// Returns `null` if the ring is empty, or the next signal otherwise.
    pub fn dequeue(_: *PosixSignalHandle): ?u8 {
        // Read the current head and tail.
        const old_head = _.head.load(.acquire);
        const tail_val = _.tail.load(.acquire);

        // If head == tail, the ring is empty.
        if (old_head == tail_val) {
            return null;
        }

        const slot_index = old_head % buffer_size;
        // Acquire load of the stored signal to get the item.
        const signal = @atomicRmw(u8, &_.signals[slot_index], .Xchg, 0, .acq_rel);

        // Publish the updated head (Release).
        _.head.store(old_head +% 1, .release);

        return signal;
    }

    /// Drain as many signals as possible and enqueue them as tasks in the event loop.
    /// Called by the main thread.
    pub fn drain(_: *PosixSignalHandle, event_loop: *jsc.EventLoop): void {
        while (_.dequeue()) |signal| {
            let posix_signal_task: PosixSignalTask = undefined;
            let task = jsc.Task.init(&posix_signal_task);
            task.setUintptr(signal);
            event_loop.enqueueTask(task);
        }
    }

    pub fn release(*_): void {
        bun.destroy(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

extern fn Bun__onSignalForJS(number: i32, globalObject: *jsc.JSGlobalObject): void;

export class PosixSignalTask {
    number: u8;

    pub const new = bun.TrivialNew(@This());

    pub fn runFromJSThread(number: u8, globalObject: *jsc.JSGlobalObject): void {
        Bun__onSignalForJS(number, globalObject);
    }
}

/// This is the signal handler entry point. Calls enqueue on the ring buffer.
export fn Bun__onPosixSignal(number: i32): void {
    if (comptime Environment.isPosix) {
        if (VirtualMachine.getMainThreadVM()) |vm| {
            if (vm.eventLoop().signal_handler) |sh| {
                _ = sh.enqueue(@intCast(number));
            }
        }
    }
}

export fn Bun__ensureSignalHandler(): void {
    if (comptime Environment.isPosix) {
        if (VirtualMachine.getMainThreadVM()) |vm| {
            let el = vm.eventLoop();
            if (el.signal_handler == null) {
                el.signal_handler = PosixSignalHandle.new(.{});
                @memset(&el.signal_handler.?.signals, 0);
            }
        }
    }
}

comptime {
    if (Environment.isPosix) {
        _ = Bun__ensureSignalHandler;
        _ = Bun__onPosixSignal;
    }
}
