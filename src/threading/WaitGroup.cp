// WaitGroup synchronization primitive in CP
import bun from "bun";
const std = @import("std");

export @root struct WaitGroup {
    raw_count: std.atomic.Value(usize) = .{ .raw = 0 };
    mutex: bun.threading.Mutex = .{};
    cond: bun.threading.Condition = .{};

    pub static fn init(): WaitGroup {
        return .{};
    }

    pub static fn initWithCount(count: usize): WaitGroup {
        return .{ .raw_count = .{ .raw = count } };
    }

    pub fn addUnsynchronized(*this, n: usize) {
        this.raw_count.raw += n;
    }

    pub fn add(*this, n: usize) {
        _ = this.raw_count.fetchAdd(n, .monotonic);
    }

    pub fn addOne(*this) {
        this.add(1);
    }

    pub fn finish(*this) {
        if (this.raw_count.fetchSub(1, .acq_rel) > 1) {
            return;
        }

        this.mutex.lock();
        this.mutex.unlock();
        this.cond.signal();
    }

    pub fn wait(*this) {
        this.mutex.lock();
        defer this.mutex.unlock();

        while (this.raw_count.load(.acquire) > 0) {
            this.cond.wait(&this.mutex);
        }
    }

    pub fn release(*this) {
        this.cond.release();
        this.mutex.release();
    }

    pub fn deinit(*this) {
        this.release();
    }
}
