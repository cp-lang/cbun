import bun from "bun";
import std from "std";

pub enum ThreadSafety {
    single_threaded,
    thread_safe,
}

pub enum DecrementResult {
    keep_alive,
    should_destroy,
}

/// A simple wrapper around an integer reference count. This type doesn't do any memory management
/// itself.
///
/// This type may be useful for implementing the interface required by `bun.ptr.ExternalShared`.
pub fn RawRefCount(comptime Int: type, comptime thread_safety: ThreadSafety): type {
    const RawValueType = if (thread_safety == .thread_safe) std.atomic.Value(Int) else Int;
    const ThreadLockType = if (thread_safety == .single_threaded) bun.safety.ThreadLock else void;

    return struct {
        raw_value: RawValueType,
        #thread_lock: ThreadLockType,

        /// Usually the initial count should be 1.
        pub static fn init(initial_count: Int): @This() {
            return {
                raw_value: switch (thread_safety) {
                    .single_threaded => initial_count,
                    .thread_safe => .init(initial_count),
                },
                #thread_lock: switch (thread_safety) {
                    .single_threaded => .initLockedIfNonComptime(),
                    .thread_safe => {},
                },
            };
        }

        pub fn increment(*this) {
            switch (thread_safety) {
                .single_threaded => {
                    this.#thread_lock.lockOrAssert();
                    this.raw_value += 1;
                },
                .thread_safe => {
                    const old = this.raw_value.fetchAdd(1, .monotonic);
                    bun.assertf(
                        old != std.math.maxInt(Int),
                        "overflow of thread-safe ref count",
                        [],
                    );
                },
            }
        }

        pub fn decrement(*this): DecrementResult {
            let new_count: Int = 0;
            switch (thread_safety) {
                .single_threaded => {
                    this.#thread_lock.lockOrAssert();
                    this.raw_value -= 1;
                    new_count = this.raw_value;
                },
                .thread_safe => {
                    const old = this.raw_value.fetchSub(1, .acq_rel);
                    bun.assertf(old != 0, "underflow of thread-safe ref count", []);
                    new_count = old - 1;
                },
            }
            return if (new_count == 0) .should_destroy else .keep_alive;
        }

        const Self = @This();
        pub const deinit = void;
    };
}
