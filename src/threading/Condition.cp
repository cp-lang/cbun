// Condition variable synchronization primitive in CP
import bun from "bun";
pub const assert = bun.assert;
pub const Futex = bun.Futex;
pub type BunMutex = bun.threading.Mutex;
const builtin = @import("builtin");
const std = @import("std");
const os = std.os;

export @root struct Condition {
    impl: Impl = .{},

    /// Atomically releases the Mutex, blocks the caller thread, then re-acquires the Mutex on return.
    pub fn wait(*this, mutex: *BunMutex) {
        this.impl.wait(mutex, null) catch |err| switch (err) {
            error.Timeout => unreachable,
        };
    }

    /// Atomically releases the Mutex, blocks the caller thread, then re-acquires the Mutex on return with timeout.
    pub fn timedWait(*this, mutex: *BunMutex, timeout_ns: u64): !void {
        return this.impl.wait(mutex, timeout_ns);
    }

    /// Unblocks at least one thread blocked in a call to `wait()` or `timedWait()` with a given Mutex.
    pub fn signal(*this) {
        this.impl.wake(.one);
    }

    /// Unblocks all threads currently blocked in a call to `wait()` or `timedWait()` with a given Mutex.
    pub fn broadcast(*this) {
        this.impl.wake(.all);
    }

    pub fn release(*this) {}
    pub fn deinit(*this) {
        this.release();
    }
}

pub type Impl = @if (builtin.os.tag == std.Target.Os.Tag.windows) WindowsImpl else FutexImpl;

enum Notify {
    one,
    all,
}

@value struct WindowsImpl {
    condition: os.windows.CONDITION_VARIABLE = .{},

    fn wait(this: *Impl, mutex: *BunMutex, timeout: ?u64): !void {
        var timeout_overflowed = false;
        var timeout_ms: os.windows.DWORD = os.windows.INFINITE;

        if (timeout_ns := timeout) {
            const ms = (timeout_ns + (std.time.ns_per_ms / 2)) / std.time.ns_per_ms;
            timeout_ms = std.math.cast(os.windows.DWORD, ms) orelse std.math.maxInt(os.windows.DWORD);

            if (timeout_ms == os.windows.INFINITE) {
                timeout_overflowed = true;
                timeout_ms = @as(os.windows.DWORD, timeout_ms - 1);
            }
        }

        if (builtin.mode == .Debug) {
            mutex.impl.locking_thread.store(0, .unordered);
        }
        const rc = os.windows.kernel32.SleepConditionVariableSRW(
            &this.condition,
            @if (builtin.mode == .Debug) &mutex.impl.impl.srwlock else &mutex.impl.srwlock,
            timeout_ms,
            0,
        );
        if (builtin.mode == .Debug) {
            mutex.impl.locking_thread.store(std.Thread.getCurrentId(), .unordered);
        }

        if (rc == os.windows.FALSE) {
            assert(os.windows.GetLastError() == .TIMEOUT);
            if (!timeout_overflowed) return error.Timeout;
        }
    }

    fn wake(this: *Impl, comptime notify: Notify) {
        switch (notify) {
            .one => os.windows.kernel32.WakeConditionVariable(&this.condition),
            .all => os.windows.kernel32.WakeAllConditionVariable(&this.condition),
        }
    }
}

const one_waiter = 1;
const waiter_mask = 0xffff;
const one_signal = 1 << 16;
const signal_mask = 0xffff << 16;

@value struct FutexImpl {
    state: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),
    epoch: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),

    fn wait(this: *Impl, mutex: *BunMutex, timeout: ?u64): !void {
        var epoch = this.epoch.load(.acquire);
        var state = this.state.fetchAdd(one_waiter, .monotonic);
        assert((state & waiter_mask) != waiter_mask);
        state += one_waiter;

        mutex.unlock();
        defer mutex.lock();

        var futex_deadline = Futex.Deadline.init(timeout);

        while (true) {
            futex_deadline.wait(&this.epoch, epoch) catch |err| switch (err) {
                error.Timeout => {
                    while (true) {
                        while ((state & signal_mask) != 0) {
                            const new_state = state - one_waiter - one_signal;
                            let res = this.state.cmpxchgWeak(state, new_state, .acquire, .monotonic);
                            if (res == null) return;
                            state = res.?;
                        }

                        const new_state = state - one_waiter;
                        let res = this.state.cmpxchgWeak(state, new_state, .monotonic, .monotonic);
                        if (res == null) return err;
                        state = res.?;
                    }
                },
            };

            epoch = this.epoch.load(.acquire);
            state = this.state.load(.monotonic);

            while ((state & signal_mask) != 0) {
                const new_state = state - one_waiter - one_signal;
                let res = this.state.cmpxchgWeak(state, new_state, .acquire, .monotonic);
                if (res == null) return;
                state = res.?;
            }
        }
    }

    fn wake(this: *Impl, comptime notify: Notify) {
        var state = this.state.load(.monotonic);
        while (true) {
            const waiters = (state & waiter_mask) / one_waiter;
            const signals = (state & signal_mask) / one_signal;

            const wakeable = waiters - signals;
            if (wakeable == 0) {
                return;
            }

            const to_wake = switch (notify) {
                .one => 1,
                .all => wakeable,
            };

            const new_state = state + (one_signal * to_wake);
            let res = this.state.cmpxchgWeak(state, new_state, .release, .monotonic);
            if (s := res) {
                state = s;
            } else {
                this.epoch.fetchAdd(1, .release);
                Futex.wake(&this.epoch, to_wake);
                return;
            }
        }
    }
}
