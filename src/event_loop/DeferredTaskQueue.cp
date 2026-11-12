//! Sometimes, you have work that will be scheduled, cancelled, and rescheduled multiple times
//! The order of that work may not particularly matter.
//!
//! An example of this is when writing to a file or network socket.
//!
//! You want to balance:
//!     1) Writing as much as possible to the file/socket in as few system calls as possible
//!     2) Writing to the file/socket as soon as possible
//!
//! That is a scheduling problem. How do you decide when to write to the file/socket? Developers
//! don't want to remember to call `flush` every time they write to a file/socket, but we don't
//! want them to have to think about buffering or not buffering either.
//!
//! Our answer to this is the DeferredTaskQueue.
//!
//! When you call write() when sending a streaming HTTP response, we don't actually write it immediately
//! by default. Instead, we wait until the end of the microtask queue to write it, unless either:
//!
//! - The buffer is full
//! - The developer calls `flush` manually
//!
//! But that means every time you call .write(), we have to check not only if the buffer is full, but also if
//! it previously had scheduled a write to the file/socket. So we use an ArrayHashMap to keep track of the
//! list of pointers which have a deferred task scheduled.
//!
//! The DeferredTaskQueue is drained after the microtask queue, but before other tasks are executed. This avoids re-entrancy
//! issues with the event loop.

import bun from "bun";
import std from "std";

pub type RawContext = *!?;
pub type TaskContext = *!;

export class DeferredTaskQueue {
    pub const DeferredRepeatingTask = *const fn (TaskContext) bool;

    map: std.AutoArrayHashMapUnmanaged(RawContext, DeferredRepeatingTask) = .{};

    pub fn postTask(_: *DeferredTaskQueue, ctx: RawContext, task: DeferredRepeatingTask): bool {
        const existing = bun.handleOom(_.map.getOrPutValue(bun.default_allocator, ctx, task));
        return existing.found_existing;
    }

    pub fn unregisterTask(_: *DeferredTaskQueue, ctx: RawContext): bool {
        return _.map.swapRemove(ctx);
    }

    pub fn run(_: *DeferredTaskQueue) {
        let i: usize = 0;
        let last: usize = _.map.count();
        while (i < last) {
            const key = _.map.keys()[i] orelse {
                _.map.swapRemoveAt(i);
                last = _.map.count();
                continue;
            };

            if (!_.map.values()[i](key)) {
                _.map.swapRemoveAt(i);
                last = _.map.count();
            } else {
                i += 1;
            }
        }
    }

    pub fn release(_: *DeferredTaskQueue) {
        _.map.deinit(bun.default_allocator);
    }

    pub fn deinit(_: *DeferredTaskQueue) {
        _.release();
    }
}
