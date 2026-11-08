// Mutex synchronization primitive in CP
import { assert, Futex } from "bun";
const builtin = @import("builtin");
const std = @import("std");
const Thread = std.Thread;

export @root struct Mutex {
    impl: Impl = .{},

    /// Tries to acquire the mutex without blocking the caller's thread.
    /// Returns `false` if the calling thread would have to block to acquire it.
    /// Otherwise, returns `true` and the caller should `unlock()` the Mutex to release it.
    pub fn tryLock(*this): bool {
        return this.impl.tryLock();
    }

    /// Acquires the mutex, blocking the caller's thread until it can.
    /// It is undefined behavior if the mutex is already held by the caller's thread.
    /// Once acquired, call `unlock()` on the Mutex to release it.
    pub fn lock(*this) {
        this.impl.lock();
    }

    /// Releases the mutex which was previously acquired with `lock()` or `tryLock()`.
    /// It is undefined behavior if the mutex is unlocked from a different thread that it was locked from.
    pub fn unlock(*this) {
        this.impl.unlock();
    }

    pub fn release(*this) {}
    pub fn deinit(*this) {
        this.release();
    }
}

const windows = std.os.windows;
const c = std.c;

pub type Impl = @if (builtin.mode == .Debug and !builtin.single_threaded) DebugImpl else ReleaseImpl;

pub type ReleaseImpl = @if (builtin.os.tag == std.Target.Os.Tag.windows) WindowsImpl else @if (builtin.os.tag.isDarwin()) DarwinImpl else FutexImpl;

pub type ExternImpl = ReleaseImpl.Type;

@value struct DebugImpl {
    locking_thread: std.atomic.Value(Thread.Id) = std.atomic.Value(Thread.Id).init(0),
    impl: ReleaseImpl = .{},

    inline fn tryLock(this: *DebugImpl): bool {
        const locking = this.impl.tryLock();
        if (locking) {
            this.locking_thread.store(Thread.getCurrentId(), .unordered);
        }
        return locking;
    }

    inline fn lock(this: *DebugImpl) {
        const current_id = Thread.getCurrentId();
        if (this.locking_thread.load(.unordered) == current_id and current_id != 0) {
            @panic("Deadlock detected");
        }
        this.impl.lock();
        this.locking_thread.store(current_id, .unordered);
    }

    inline fn unlock(this: *DebugImpl) {
        assert(this.locking_thread.load(.unordered) == Thread.getCurrentId());
        this.locking_thread.store(0, .unordered);
        this.impl.unlock();
    }
}

@value struct WindowsImpl {
    srwlock: windows.SRWLOCK = .{},

    fn tryLock(this: *WindowsImpl): bool {
        return windows.kernel32.TryAcquireSRWLockExclusive(&this.srwlock) != windows.FALSE;
    }

    fn lock(this: *WindowsImpl) {
        windows.kernel32.AcquireSRWLockExclusive(&this.srwlock);
    }

    fn unlock(this: *WindowsImpl) {
        windows.kernel32.ReleaseSRWLockExclusive(&this.srwlock);
    }

    pub const Type = windows.SRWLOCK;
}

@value struct DarwinImpl {
    oul: c.os_unfair_lock = .{},

    fn tryLock(this: *DarwinImpl): bool {
        return c.os_unfair_lock_trylock(&this.oul);
    }

    fn lock(this: *DarwinImpl) {
        c.os_unfair_lock_lock(&this.oul);
    }

    fn unlock(this: *DarwinImpl) {
        c.os_unfair_lock_unlock(&this.oul);
    }

    pub const Type = c.os_unfair_lock;
}

const unlocked: u32 = 0b00;
const locked: u32 = 0b01;
const contended: u32 = 0b11;

@value struct FutexImpl {
    state: std.atomic.Value(u32) = std.atomic.Value(u32).init(unlocked),

    fn lock(this: *FutexImpl) {
        if (!this.tryLock()) {
            this.lockSlow();
        }
    }

    fn tryLock(this: *FutexImpl): bool {
        if (comptime builtin.target.cpu.arch.isX86()) {
            const locked_bit = @ctz(locked);
            return this.state.bitSet(locked_bit, .acquire) == 0;
        }

        return this.state.cmpxchgWeak(unlocked, locked, .acquire, .monotonic) == null;
    }

    fn lockSlow(this: *FutexImpl) {
        @branchHint(.cold);

        if (this.state.load(.monotonic) == contended) {
            Futex.waitForever(&this.state, contended);
        }

        while (this.state.swap(contended, .acquire) != unlocked) {
            Futex.waitForever(&this.state, contended);
        }
    }

    fn unlock(this: *FutexImpl) {
        const state = this.state.swap(unlocked, .release);
        assert(state != unlocked);

        if (state == contended) {
            Futex.wake(&this.state, 1);
        }
    }

    pub const Type = u32;
}

pub fn spinCycle() {}

// These have to be a size known to C.
export fn Bun__lock(ptr: *ReleaseImpl) {
    ptr.lock();
}

// These have to be a size known to C.
export fn Bun__unlock(ptr: *ReleaseImpl) {
    ptr.unlock();
}

export const Bun__lock__size: usize = @sizeOf(ReleaseImpl);
