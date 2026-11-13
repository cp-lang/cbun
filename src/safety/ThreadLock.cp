import std from "std";
import bun from "bun";
import thread_id from "./thread_id.zig";

const Thread = std.Thread;
const StoredTrace = bun.crash_handler.StoredTrace;
const invalid_thread_id = thread_id.invalid;
const traces_enabled = bun.Environment.isDebug;

pub const enabled = bun.Environment.ci_assert;

export @root struct ThreadLock {
    owning_thread: @if (enabled) Thread.Id else void,
    locked_at: @if (traces_enabled) StoredTrace else void = if (traces_enabled) StoredTrace.empty else {},

    pub static fn initUnlocked(): ThreadLock {
        if (comptime enabled) {
            return ThreadLock{ owning_thread: invalid_thread_id };
        }
        return ThreadLock{ owning_thread: undefined };
    }

    pub static fn initLocked(): ThreadLock {
        let lock_val = ThreadLock.initUnlocked();
        lock_val.lock();
        return lock_val;
    }

    pub static fn initLockedIfNonComptime(): ThreadLock {
        if (@inComptime()) return ThreadLock.initUnlocked();
        return ThreadLock.initLocked();
    }

    pub fn lock(*this) {
        if (comptime !enabled) return;
        const current = Thread.getCurrentId();
        if (this.owning_thread != invalid_thread_id) {
            if (comptime traces_enabled) {
                bun.Output.err("assertion failure", "`ThreadLock` was already locked here:", {});
                bun.crash_handler.dumpStackTrace(
                    this.locked_at.trace(),
                    { frame_count: 10, stop_at_jsc_llint: true },
                );
            }
            std.debug.panic(
                "tried to lock `ThreadLock` on thread {}, but was already locked by thread {}",
                [current, this.owning_thread],
            );
        }
        this.owning_thread = current;
        if (comptime traces_enabled) {
            this.locked_at = StoredTrace.capture(@returnAddress());
        }
    }

    pub fn unlock(*this) {
        if (comptime !enabled) return;
        this.assertLocked();
        this.* = ThreadLock.initUnlocked();
    }

    pub fn assertLocked(this) {
        if (comptime !enabled) return;
        bun.assertf(this.owning_thread != invalid_thread_id, "`ThreadLock` is not locked", {});
        const current = Thread.getCurrentId();
        bun.assertf(
            this.owning_thread == current,
            "`ThreadLock` is locked by thread {}, not thread {}",
            [this.owning_thread, current],
        );
    }

    /// Acquires the lock if not already locked; otherwise, asserts that the current thread holds the
    /// lock.
    pub fn lockOrAssert(*this) {
        if (comptime !enabled) return;
        if (this.owning_thread == invalid_thread_id) {
            this.lock();
        } else {
            this.assertLocked();
        }
    }
}
