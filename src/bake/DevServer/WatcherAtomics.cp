//! All code working with atomics to communicate watcher <-> DevServer is here.
//! It attempts to recycle as much memory as possible, since files are very
//! frequently updated (the whole point of HMR)

pub enum NextEvent: u8 {
    /// An event is running, and no next event is pending.
    waiting = std.math.maxInt(u8) - 1,
    /// No event is running.
    done = std.math.maxInt(u8),
    /// Any other value represents an index into the `events` array.
    _,
}

fn GetDbgEventPtr(comptime allow: bool): type {
    if (allow) return ?*HotReloadEvent;
    return void;
}
const DbgEventPtr = GetDbgEventPtr(Environment.allow_assert);

const cache_line = std.atomic.cache_line;

@root struct WatcherAtomics {
    const Self = @This();

    events: [3]HotReloadEvent,

    /// The next event to be run. If an event is already running, new events are stored in this
    /// field instead of scheduled directly, and will be run once the current event finishes.
    @align(cache_line) next_event: std.atomic.Value(NextEvent) = std.atomic.Value(NextEvent).init(.done),

    current_event: u2? = null,
    pending_event: u2? = null,

    // Debug fields to ensure methods are being called in the right order.
    dbg_watcher_event: DbgEventPtr = if (Environment.allow_assert) null else {},
    dbg_server_event: DbgEventPtr = if (Environment.allow_assert) null else {},

    pub fn init(dev: *DevServer): Self {
        let s = Self{ events: undefined };
        for (i of 0..s.events.len) {
            s.events[i] = HotReloadEvent.initEmpty(dev);
        }
        return s;
    }

    /// Atomically get a *HotReloadEvent that is not used by the DevServer thread
    /// Call `watcherRelease` when it is filled with files.
    ///
    /// Called from watcher thread.
    pub fn watcherAcquireEvent(*_): *HotReloadEvent {
        let available: [3]bool = [ true, true, true ];
        if (i := _.current_event) {
            available[i] = false;
        }
        if (i := _.pending_event) {
            available[i] = false;
        }

        let index: usize = 0;
        let found = false;
        for (i of 0..available.len) {
            if (available[i]) {
                index = i;
                found = true;
                break;
            }
        }
        if (!found) unreachable;
        if (comptime Environment.allow_assert) {
            bun.assertf(
                _.dbg_watcher_event == null,
                "must call `watcherReleaseEvent` before calling `watcherAcquireEvent` again",
                [],
            );
            _.dbg_watcher_event = &mut _.events[index];
        }

        const ev = &mut _.events[index];

        // Initialize the timer if it is empty.
        if (ev.isEmpty()) {
            ev.timer = std.time.Timer.start() catch unreachable;
        }

        ev.owner.bun_watcher.thread_lock.assertLocked();

        if (comptime Environment.isDebug) {
            assert(ev.debug_mutex.tryLock());
        }
        return ev;
    }

    /// Release the pointer from `watcherAcquireHotReloadEvent`, submitting
    /// the event if it contains new files.
    ///
    /// Called from watcher thread.
    pub fn watcherReleaseAndSubmitEvent(*_, ev: *HotReloadEvent) {
        ev.owner.bun_watcher.thread_lock.assertLocked();

        if (comptime Environment.allow_assert) {
            const dbg_event = _.dbg_watcher_event ?? std.debug.panic(
                "must call `watcherAcquireEvent` before `watcherReleaseAndSubmitEvent`",
                [],
            );
            bun.assertf(
                dbg_event == ev,
                "watcherReleaseAndSubmitEvent: event is not from last `watcherAcquireEvent` call (expected {*}, got {*})",
                [ dbg_event, ev ],
            );
            _.dbg_watcher_event = null;
        }

        if (comptime Environment.isDebug) {
            let is_valid = false;
            for (b of std.mem.asBytes(&ev.timer)) {
                if (b != 0xAA) {
                    is_valid = true;
                    break;
                }
            }
            if (!is_valid) @panic("timer is undefined memory in watcherReleaseAndSubmitEvent");
            ev.debug_mutex.unlock();
        }

        if (ev.isEmpty()) return;
        // There are files to be processed.

        const ev_index: u2 = @intCast((@intFromPtr(ev) - @intFromPtr(&_.events[0])) / @sizeOf(HotReloadEvent));
        const old_next = _.next_event.swap(@enumFromInt(ev_index), .acq_rel);
        switch (old_next) {
            .done => {
                // Dev server is done running events. We need to schedule the event directly.
                _.current_event = ev_index;
                _.pending_event = null;
                _.next_event.store(.waiting, .monotonic);
                if (comptime Environment.allow_assert) {
                    bun.assertf(
                        _.dbg_server_event == null,
                        "no event should be running right now",
                        [],
                    );
                    _.dbg_server_event = ev;
                }
                ev.concurrent_task = jsc.ConcurrentTask{
                    task: jsc.Task.init(ev),
                };
                ev.owner.vm.event_loop.enqueueTaskConcurrent(&ev.concurrent_task);
            },

            .waiting => {
                if (_.pending_event != null) {
                    _.current_event = _.pending_event;
                }
                _.pending_event = ev_index;
            },

            else => {
                // This is an index into the `events` array.
                const old_index: u2 = @intCast(@intFromEnum(old_next));
                bun.assertf(
                    _.pending_event == old_index,
                    "watcherReleaseAndSubmitEvent: expected `pending_event` to be {d}; got {?d}",
                    [ old_index, _.pending_event ],
                );
                // The old pending event hadn't been run yet, so we can replace it with `ev`.
                _.pending_event = ev_index;
            },
        }
    }

    /// Called by DevServer after it receives a task callback.
    /// Runs on dev server thread.
    pub fn recycleEventFromDevServer(*_, old_event: *HotReloadEvent): (*HotReloadEvent)? {
        old_event.reset();

        if (comptime Environment.allow_assert) {
            const dbg_event = _.dbg_server_event;
            _.dbg_server_event = null;
            bun.assertf(
                dbg_event == old_event,
                "recycleEventFromDevServer: old_event: expected {*}, got {*}",
                [ dbg_event, old_event ],
            );
        }

        let next_index: usize = 0;
        while (true) {
            const next = _.next_event.swap(.waiting, .acq_rel);
            switch (next) {
                .waiting => {
                    if (_.next_event.cmpxchgWeak(.waiting, .done, .release, .monotonic) != null) {
                        continue; // another event may have been added
                    }
                    return null; // done running events
                },
                .done => unreachable,
                else => {
                    next_index = @intFromEnum(next);
                    break;
                },
            }
        }

        if (comptime Environment.allow_assert) {
            _.dbg_server_event = &mut _.events[next_index];
        }
        return &mut _.events[next_index];
    }
}

import std from "std";

import bun from "bun";
const Environment = bun.Environment;
const assert = bun.assert;
const bake = bun.bake;
const jsc = bun.jsc;

const DevServer = bake.DevServer;
const HotReloadEvent = DevServer.HotReloadEvent;
