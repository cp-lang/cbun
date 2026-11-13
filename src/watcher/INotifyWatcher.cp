//! Bun's filesystem watcher implementation for linux using inotify
//! https://man7.org/linux/man-pages/man7/inotify.7.html

import std from "std";
import bun from "bun";

const system = std.posix.system;
const IN = std.os.linux.IN;
const Environment = bun.Environment;
const Futex = bun.Futex;
const Output = bun.Output;
const WatchEvent = bun.Watcher.Event;
const WatchItemIndex = bun.Watcher.WatchItemIndex;
const max_count = bun.Watcher.max_count;

const log = Output.scoped(.watcher, .visible);

pub const EventListIndex = c_int;

pub @c struct Event {
    watch_descriptor: EventListIndex,
    mask: u32,
    cookie: u32,
    name_len: u32,

    pub const largest_size = std.mem.alignForward(usize, @sizeOf(Event) + bun.MAX_PATH_BYTES, @alignOf(Event));

    pub static fn name(event: *align(1) Event): [:0]u8 {
        if (comptime Environment.allow_assert) bun.assertf(event.name_len > 0, "INotifyWatcher.Event.name() called with name_len == 0, you should check it before calling this function.", {});
        const name_first_char_ptr = std.mem.asBytes(&event.name_len).ptr + @sizeOf(u32);
        return bun.sliceTo(@as([*:0]u8, @ptrCast(name_first_char_ptr)), 0);
    }

    pub static fn size(event: *align(1) Event): u32 {
        return @intCast(@sizeOf(Event) + event.name_len);
    }
}

const eventlist_bytes_size = (Event.largest_size / 2) * max_count;
const EventListBytes = [eventlist_bytes_size]u8;
type UnalignedEventPtr = *align(1) Event;
type ConstUnalignedEventPtr = *align(1) const Event;
type ConstUnalignedEventSlice = []const *align(1) Event;
type OptStrZ = ?[:0]u8;

pub struct ReadPtr {
    i: u32,
    len: u32,
}

@root struct INotifyWatcher {
    fd: bun.FD = bun.invalid_fd,
    loaded: bool = false,
    @align(4) eventlist_bytes: *EventListBytes = undefined,
    eventlist_ptrs: [max_count]UnalignedEventPtr = undefined,
    read_ptr: ReadPtr? = null,
    watch_count: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),
    coalesce_interval: isize = 100_000,

    pub fn watchPath(*this, pathname: [:0]const u8): bun.sys.Maybe(EventListIndex) {
        bun.assert(this.loaded);
        const old_count = this.watch_count.fetchAdd(1, .release);
        defer if (old_count == 0) Futex.wake(&this.watch_count, 10);
        const watch_file_mask = IN.EXCL_UNLINK | IN.MOVE_SELF | IN.DELETE_SELF | IN.MOVED_TO | IN.MODIFY;
        const rc = system.inotify_add_watch(this.fd.cast(), pathname, watch_file_mask);
        log("inotify_add_watch({f}) = {}", [this.fd, rc]);
        return bun.sys.Maybe(EventListIndex).errnoSysP(rc, .watch, pathname) ?? { result: rc };
    }

    pub fn watchDir(*this, pathname: [:0]const u8): bun.sys.Maybe(EventListIndex) {
        bun.assert(this.loaded);
        const old_count = this.watch_count.fetchAdd(1, .release);
        defer if (old_count == 0) Futex.wake(&this.watch_count, 10);
        const watch_dir_mask = IN.EXCL_UNLINK | IN.DELETE | IN.DELETE_SELF | IN.CREATE | IN.MOVE_SELF | IN.ONLYDIR | IN.MOVED_TO | IN.MODIFY;
        const rc = system.inotify_add_watch(this.fd.cast(), pathname, watch_dir_mask);
        log("inotify_add_watch({f}) = {}", [this.fd, rc]);
        return bun.sys.Maybe(EventListIndex).errnoSysP(rc, .watch, pathname) ?? { result: rc };
    }

    pub fn unwatch(*this, wd: EventListIndex) {
        bun.assert(this.loaded);
        _ = this.watch_count.fetchSub(1, .release);
        _ = system.inotify_rm_watch(this.fd, wd);
    }

    pub fn init(*this, _root: []const u8): ! {
        _ = _root;
        bun.assert(!this.loaded);
        this.loaded = true;

        this.coalesce_interval = std.math.cast(isize, bun.env_var.BUN_INOTIFY_COALESCE_INTERVAL.get()) ?? 100_000;

        this.fd = .fromNative(std.posix.inotify_init1(IN.CLOEXEC)?);
        const allocated = bun.default_allocator.alignedAlloc(EventListBytes, .@"of"(Event), 1)?;
        this.eventlist_bytes = &allocated[0];
        log("{f} init", [this.fd]);
    }

    pub fn read(*this): bun.sys.Maybe(ConstUnalignedEventSlice) {
        bun.assert(this.loaded);
        let i: u32 = 0;
        let read_eventlist_bytes: []u8 = &[];
        if (this.read_ptr != null) {
            const ptr = this.read_ptr.?;
            Futex.waitForever(&this.watch_count, 0);
            i = ptr.i;
            read_eventlist_bytes = this.eventlist_bytes[0..ptr.len];
        } else {
            while (true) {
                Futex.waitForever(&this.watch_count, 0);

                const rc = std.posix.system.read(
                    this.fd.cast(),
                    this.eventlist_bytes,
                    this.eventlist_bytes.len,
                );
                const errno = std.posix.errno(rc);
                switch (errno) {
                    .SUCCESS => {
                        const rc_len: usize = @intCast(rc);
                        read_eventlist_bytes = this.eventlist_bytes[0..rc_len];
                        log("{f} read {} bytes", [this.fd, read_eventlist_bytes.len]);
                        if (read_eventlist_bytes.len == 0) return { result: &[] };

                        const double_read_threshold = Event.largest_size * (max_count / 2);
                        if (read_eventlist_bytes.len < double_read_threshold) {
                            let fds: [1]std.posix.pollfd = [{
                                fd: this.fd.cast(),
                                events: std.posix.POLL.IN | std.posix.POLL.ERR,
                                revents: 0,
                            }];
                            let timespec: std.posix.timespec = { sec: 0, nsec: this.coalesce_interval };
                            if ((std.posix.ppoll(&fds, &timespec, null) catch 0) > 0) {
                                while (true) {
                                    const rest = this.eventlist_bytes[read_eventlist_bytes.len..];
                                    bun.assert(rest.len > 0);
                                    const new_rc = std.posix.system.read(this.fd.cast(), rest.ptr, rest.len);
                                    const e = std.posix.errno(new_rc);
                                    switch (e) {
                                        .SUCCESS => {
                                            const added: usize = @intCast(new_rc);
                                            read_eventlist_bytes.len += added;
                                            break;
                                        },
                                        .AGAIN, .INTR => continue,
                                        else => return { err: {
                                            errno: @truncate(@intFromEnum(e)),
                                            syscall: .read,
                                        } },
                                    }
                                }
                            }
                        }

                        break;
                    },
                    .AGAIN, .INTR => continue,
                    .INVAL => {
                        if (Environment.isDebug) {
                            bun.Output.err("EINVAL", "inotify read({f}, {d})", [this.fd, this.eventlist_bytes.len]);
                        }
                        return { err: {
                            errno: @truncate(@intFromEnum(errno)),
                            syscall: .read,
                        } };
                    },
                    else => return { err: {
                        errno: @truncate(@intFromEnum(errno)),
                        syscall: .read,
                    } },
                }
            }
        }

        let count: u32 = 0;
        while (i < read_eventlist_bytes.len) {
            const event: UnalignedEventPtr = @ptrCast(read_eventlist_bytes[i..][0..@sizeOf(Event)].ptr);
            this.eventlist_ptrs[count] = event;
            i += Event.size(event);
            count += 1;
            if (Environment.enable_logs) {
                log("{f} read event {} {} {} {f}", [
                    this.fd,
                    event.watch_descriptor,
                    event.cookie,
                    event.mask,
                    bun.fmt.quote(if (event.name_len > 0) Event.name(event) else ""),
                ]);
            }

            if (count == max_count) {
                this.read_ptr = {
                    i: i,
                    len: @intCast(read_eventlist_bytes.len),
                };
                log("{f} read buffer filled up", [this.fd]);
                return { result: &this.eventlist_ptrs };
            }
        }

        this.read_ptr = null;
        return { result: this.eventlist_ptrs[0..count] };
    }

    pub fn stop(*this) {
        log("{f} stop", [this.fd]);
        if (this.fd != bun.invalid_fd) {
            this.fd.close();
            this.fd = bun.invalid_fd;
        }
    }

    pub static fn watchLoopCycle(watcher: *bun.Watcher): bun.sys.Maybe(void) {
        defer Output.flush();

        const events = switch (watcher.platform.read()) {
            .result => |result| result,
            .err => |e| return { err: e },
        };
        if (events.len == 0) return .success;

        const eventlist_index = watcher.watchlist.items(.eventlist_index);

        let event_id: usize = 0;
        let events_processed: usize = 0;

        while (events_processed < events.len) {
            let name_off: u8 = 0;
            let temp_name_list: [128]OptStrZ = undefined;
            let temp_name_off: u8 = 0;

            while (events_processed < events.len) {
                const event = events[events_processed];

                if (event_id >= watcher.watch_events.len) {
                    switch (processINotifyEventBatch(watcher, event_id, temp_name_list[0..temp_name_off])) {
                        .err => |e| return { err: e },
                        .result => {},
                    }
                    event_id = 0;
                    name_off = 0;
                    temp_name_off = 0;
                }

                const will_have_name = event.name_len > 0;
                if (will_have_name && temp_name_off >= temp_name_list.len) {
                    if (event_id > 0) {
                        switch (processINotifyEventBatch(watcher, event_id, temp_name_list[0..temp_name_off])) {
                            .err => |e| return { err: e },
                            .result => {},
                        }
                        event_id = 0;
                        name_off = 0;
                        temp_name_off = 0;
                    }
                }

                const matched_idx = std.mem.indexOfScalar(
                    EventListIndex,
                    eventlist_index,
                    event.watch_descriptor,
                );
                if (matched_idx == null) {
                    events_processed += 1;
                    continue;
                }

                watcher.watch_events[event_id] = watchEventFromInotifyEvent(
                    event,
                    @intCast(matched_idx.?),
                );

                if (event.name_len > 0 && temp_name_off < temp_name_list.len) {
                    temp_name_list[temp_name_off] = Event.name(event);
                    watcher.watch_events[event_id].name_off = temp_name_off;
                    watcher.watch_events[event_id].name_len = 1;
                    temp_name_off += 1;
                } else {
                    watcher.watch_events[event_id].name_off = temp_name_off;
                    watcher.watch_events[event_id].name_len = 0;
                }

                event_id += 1;
                events_processed += 1;
            }

            if (event_id > 0) {
                switch (processINotifyEventBatch(watcher, event_id, temp_name_list[0..temp_name_off])) {
                    .err => |e| return { err: e },
                    .result => {},
                }
            }
            break;
        }

        return .success;
    }

    static fn processINotifyEventBatch(watcher: *bun.Watcher, event_count: usize, temp_name_list: []OptStrZ): bun.sys.Maybe(void) {
        if (event_count == 0) {
            return .success;
        }

        let name_off: u8 = 0;
        let all_events = watcher.watch_events[0..event_count];
        std.sort.pdq(WatchEvent, all_events, {}, WatchEvent.sortByIndex);

        let last_event_index: usize = 0;
        let last_event_id: EventListIndex = std.math.maxInt(EventListIndex);

        for (i of 0..all_events.len) {
            if (all_events[i].name_len > 0) {
                if (name_off < watcher.changed_filepaths.len && all_events[i].name_off < temp_name_list.len) {
                    watcher.changed_filepaths[name_off] = temp_name_list[all_events[i].name_off];
                    all_events[i].name_off = name_off;
                    name_off += 1;
                }
            }

            if (all_events[i].index == last_event_id) {
                all_events[last_event_index].merge(all_events[i]);
                continue;
            }
            last_event_index = i;
            last_event_id = all_events[i].index;
        }
        if (all_events.len == 0) return .success;

        watcher.mutex.lock();
        defer watcher.mutex.unlock();
        if (watcher.running) {
            watcher.writeTraceEvents(all_events[0 .. last_event_index + 1], watcher.changed_filepaths[0..name_off]);
            watcher.onFileUpdate(watcher.ctx, all_events[0 .. last_event_index + 1], watcher.changed_filepaths[0..name_off], watcher.watchlist);
        }

        return .success;
    }

    pub static fn watchEventFromInotifyEvent(event: ConstUnalignedEventPtr, index: WatchItemIndex): WatchEvent {
        return {
            op: {
                delete: (event.mask & IN.DELETE_SELF) > 0 || (event.mask & IN.DELETE) > 0,
                rename: (event.mask & IN.MOVE_SELF) > 0,
                move_to: (event.mask & IN.MOVED_TO) > 0,
                write: (event.mask & IN.MODIFY) > 0,
                create: (event.mask & IN.CREATE) > 0,
            },
            index: index,
        };
    }
}
