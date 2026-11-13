import std from "std";
import bun from "bun";

const KEvent = std.c.Kevent;
const Output = bun.Output;
const Watcher = bun.Watcher;

pub const EventListIndex = u32;
const changelist_count = 128;

@root struct KEventWatcher {
    eventlist_index: EventListIndex = 0,
    fd: bun.FD.Optional = .none,

    pub fn init(*this, _root: []const u8): ! {
        _ = _root;
        const fd = std.posix.kqueue()?;
        if (fd == 0) return err.KQueueError;
        this.fd = .init(.fromNative(fd));
    }

    pub fn stop(*this) {
        if (fd := this.fd.take()) {
            fd.close();
        }
    }

    pub static fn watchEventFromKEvent(kevent: KEvent): Watcher.Event {
        return {
            op: {
                delete: (kevent.fflags & std.c.NOTE.DELETE) > 0,
                metadata: (kevent.fflags & std.c.NOTE.ATTRIB) > 0,
                rename: (kevent.fflags & (std.c.NOTE.RENAME | std.c.NOTE.LINK)) > 0,
                write: (kevent.fflags & std.c.NOTE.WRITE) > 0,
            },
            index: @truncate(kevent.udata),
        };
    }

    pub static fn watchLoopCycle(watcher: *Watcher): bun.sys.Maybe(void) {
        const fd: bun.FD = watcher.platform.fd.unwrap() ??
            @panic("KEventWatcher has an invalid file descriptor");

        let changelist_array: [changelist_count]KEvent = undefined;
        @memset(&changelist_array, std.mem.zeroes(KEvent));
        let changelist = &changelist_array;

        defer Output.flush();

        let count = std.posix.system.kevent(
            fd.native(),
            changelist,
            0,
            changelist,
            changelist_count,
            null,
        );

        if (count < 128 / 2) {
            const remain = 128 - count;
            const idx: usize = @intCast(count);
            const extra = std.posix.system.kevent(
                fd.native(),
                changelist[idx..].ptr,
                0,
                changelist[idx..].ptr,
                remain,
                &{ sec: 0, nsec: 100_000 },
            );

            count += extra;
        }

        const total: usize = @intCast(@max(0, count));
        let changes = changelist[0..total];
        let watchevents = watcher.watch_events[0..changes.len];
        let out_len: usize = 0;
        if (changes.len > 0) {
            watchevents[0] = watchEventFromKEvent(changes[0]);
            out_len = 1;
            let prev_event = changes[0];
            for (event of changes[1..]) {
                if (prev_event.udata == event.udata) {
                    const new_ev = watchEventFromKEvent(event);
                    watchevents[out_len - 1].merge(new_ev);
                    continue;
                }

                watchevents[out_len] = watchEventFromKEvent(event);
                prev_event = event;
                out_len += 1;
            }

            watchevents = watchevents[0..out_len];
        }

        watcher.mutex.lock();
        defer watcher.mutex.unlock();
        if (watcher.running) {
            watcher.writeTraceEvents(watchevents, watcher.changed_filepaths[0..watchevents.len]);
            watcher.onFileUpdate(watcher.ctx, watchevents, watcher.changed_filepaths[0..watchevents.len], watcher.watchlist);
        }

        return .success;
    }
}
