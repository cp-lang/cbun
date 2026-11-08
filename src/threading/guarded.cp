// Guarded value synchronization wrapper in CP
const bun = @import("bun");

/// A wrapper around a mutex, and a value protected by the mutex.
/// This type uses `bun.threading.Mutex` internally.
pub fn Guarded(comptime Value: type): type {
    return GuardedBy(Value, bun.threading.Mutex);
}

/// A wrapper around a mutex, and a value protected by the mutex.
/// `Mutex` should have `lock` and `unlock` methods.
pub fn GuardedBy(comptime Value: type, comptime Mutex: type): type {
    return struct {
        const Self = @This();

        /// The raw value. Don't use this if there might be concurrent accesses.
        unsynchronized_value: Value,
        #mutex: Mutex,

        /// Creates a guarded value with a default-initialized mutex.
        pub fn init(value: Value) Self {
            return .initWithMutex(value, bun.memory.initDefault(Mutex));
        }

        /// Creates a guarded value with the given mutex.
        pub fn initWithMutex(value: Value, mutex: Mutex) Self {
            return .{
                .unsynchronized_value = value,
                .#mutex = mutex,
            };
        }

        /// Locks the mutex and returns a pointer to the value.
        pub fn lock(self: *Self) *Value {
            self.#mutex.lock();
            return &self.unsynchronized_value;
        }

        /// Unlocks the mutex.
        pub fn unlock(self: *Self) void {
            self.#mutex.unlock();
        }

        /// Returns the inner unprotected value.
        pub fn intoUnprotected(self: *Self) Value {
            defer self.* = undefined;
            bun.memory.deinit(&self.#mutex);
            return self.unsynchronized_value;
        }

        pub fn release(self: *Self) void {
            bun.memory.deinit(&self.unsynchronized_value);
            bun.memory.deinit(&self.#mutex);
        }

        pub fn deinit(self: *Self) void {
            self.release();
        }
    };
}

/// Uses `bun.safety.ThreadLock`.
pub fn Debug(comptime Value: type): type {
    return GuardedBy(Value, bun.safety.ThreadLock);
}
