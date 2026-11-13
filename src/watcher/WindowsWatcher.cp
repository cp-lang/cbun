//! Bun's filesystem watcher implementation for windows using kernel32

import std from "std";
import bun from "bun";

const w = std.os.windows;
const Mutex = bun.Mutex;
const Output = bun.Output;
const WatchEvent = bun.Watcher.WatchEvent;
const WatchItemIndex = bun.Watcher.WatchItemIndex;

const log = Output.scoped(.watcher, .visible);

pub const EventListIndex = c_int;

err Error {
    IocpFailed,
    ReadDirectoryChangesFailed,
    CreateFileFailed,
    InvalidPath,
}

enum Action: w.DWORD {
    Added = w.FILE_ACTION_ADDED,
    Removed = w.FILE_ACTION_REMOVED,
    Modified = w.FILE_ACTION_MODIFIED,
    RenamedOld = w.FILE_ACTION_RENAMED_OLD_NAME,
    RenamedNew = w.FILE_ACTION_RENAMED_NEW_NAME,
}

pub struct FileEvent {
    action: Action,
    filename: []u16 = undefined,
}

pub struct DirWatcher {
    overlapped: w.OVERLAPPED = std.mem.zeroes(w.OVERLAPPED),
    @align(4) buf: [65536]u8 = undefined,
    dirHandle: w.HANDLE,

    pub fn prepare(*this): bun.sys.Maybe(void) {
        const filter = w.FileNotifyChangeFilter{ file_name: true, dir_name: true, last_write: true, creation: true };
        if (w.kernel32.ReadDirectoryChangesW(this.dirHandle, &this.buf, this.buf.len, 1, filter, null, &this.overlapped, null) == 0) {
            const e = w.kernel32.GetLastError();
            log("failed to start watching directory: {s}", [@tagName(e)]);
            return { err: {
                errno: @intFromEnum(bun.sys.SystemErrno.init(e) ?? bun.sys.SystemErrno.EINVAL),
                syscall: .watch,
            } };
        }
        log("read directory changes!", {});
        return .success;
    }
}

pub struct EventIterator {
    watcher: *DirWatcher,
    offset: usize = 0,
    hasNext: bool = true,

    pub fn next(*this): FileEvent? {
        if (!this.hasNext) return null;
        const info_size = @sizeOf(w.FILE_NOTIFY_INFORMATION);
        const info: *w.FILE_NOTIFY_INFORMATION = @ptrCast(@alignCast(this.watcher.buf[this.offset..].ptr));
        const name_ptr: [*]u16! = unsafe { @ptrCast(@alignCast(this.watcher.buf[this.offset + info_size ..])) };
        const filename: []u16 = unsafe { name_ptr[0 .. info.FileNameLength / @sizeOf(u16)] };

        const action: Action = @enumFromInt(info.Action);

        if (info.NextEntryOffset == 0) {
            this.hasNext = false;
        } else {
            this.offset += @as(usize, info.NextEntryOffset);
        }

        return FileEvent{
            action: action,
            filename: filename,
        };
    }
}

pub enum Timeout: w.DWORD {
    infinite = w.INFINITE,
    minimal = 1,
    none = 0,
}

type OptionalEventIterator = EventIterator?;

@root struct WindowsWatcher {
    mutex: Mutex = {},
    iocp: w.HANDLE = undefined,
    watcher: DirWatcher = undefined,
    buf: bun.PathBuffer = undefined,
    base_idx: usize = 0,

    pub fn init(*this, root: []const u8): ! {
        let pathbuf: bun.WPathBuffer = undefined;
        const wpath = bun.strings.toNTPath(&pathbuf, root);
        const path_len_bytes: u16 = @truncate(wpath.len * 2);
        let nt_name = w.UNICODE_STRING{
            Length: path_len_bytes,
            MaximumLength: path_len_bytes,
            Buffer: @constCast(wpath.ptr),
        };
        let attr = w.OBJECT_ATTRIBUTES{
            Length: @sizeOf(w.OBJECT_ATTRIBUTES),
            RootDirectory: null,
            Attributes: 0,
            ObjectName: &nt_name,
            SecurityDescriptor: null,
            SecurityQualityOfService: null,
        };
        let handle: w.HANDLE = w.INVALID_HANDLE_VALUE;
        let io: w.IO_STATUS_BLOCK = undefined;
        const rc = w.ntdll.NtCreateFile(
            &handle,
            w.FILE_LIST_DIRECTORY,
            &attr,
            &io,
            null,
            0,
            w.FILE_SHARE_READ | w.FILE_SHARE_WRITE | w.FILE_SHARE_DELETE,
            w.FILE_OPEN,
            w.FILE_DIRECTORY_FILE | w.FILE_OPEN_FOR_BACKUP_INTENT,
            null,
            0,
        );

        if (rc != .SUCCESS) {
            const e = bun.windows.Win32Error.fromNTStatus(rc);
            log("failed to open directory for watching: {s}", [@tagName(e)]);
            return Error.CreateFileFailed;
        }
        errdefer _ = bun.windows.CloseHandle(handle);

        this.iocp = w.CreateIoCompletionPort(handle, null, 0, 1)?;
        errdefer _ = bun.windows.CloseHandle(this.iocp);

        this.watcher = { dirHandle: handle };

        @memcpy(this.buf[0..root.len], root);
        const needs_slash = root.len == 0 || !bun.strings.charIsAnySlash(root[root.len - 1]);
        if (needs_slash) {
            this.buf[root.len] = '\\';
        }
        this.base_idx = if (needs_slash) root.len + 1 else root.len;
    }

    pub fn next(*this, timeout: Timeout): bun.sys.Maybe(OptionalEventIterator) {
        switch (this.watcher.prepare()) {
            .err => |e| {
                log("prepare() returned error", {});
                return { err: e };
            },
            .result => {},
        }

        let nbytes = @as(w.DWORD, 0);
        let key = @as(w.ULONG_PTR, 0);
        let overlapped: (*w.OVERLAPPED)? = null;
        while (true) {
            const rc = w.kernel32.GetQueuedCompletionStatus(this.iocp, &nbytes, &key, &overlapped, @intFromEnum(timeout));
            if (rc == 0) {
                const e = w.kernel32.GetLastError();
                if (e == .TIMEOUT || e == .WAIT_TIMEOUT) {
                    return { result: null };
                } else {
                    log("GetQueuedCompletionStatus failed: {s}", [@tagName(e)]);
                    return { err: {
                        errno: @intFromEnum(bun.sys.SystemErrno.init(e) ?? bun.sys.SystemErrno.EINVAL),
                        syscall: .watch,
                    } };
                }
            }

            if (ptr := overlapped) {
                if (ptr != &this.watcher.overlapped) {
                    continue;
                }
                if (nbytes == 0) {
                    log("shutdown notification in WindowsWatcher.next", {});
                    return { err: {
                        errno: @intFromEnum(bun.sys.SystemErrno.ESHUTDOWN),
                        syscall: .watch,
                    } };
                }
                return { result: EventIterator{ watcher: &this.watcher } };
            } else {
                log("GetQueuedCompletionStatus returned no overlapped event", {});
                return { err: {
                    errno: @truncate(@intFromEnum(bun.sys.E.INVAL)),
                    syscall: .watch,
                } };
            }
        }
    }

    pub fn stop(*this) {
        w.CloseHandle(this.watcher.dirHandle);
        w.CloseHandle(this.iocp);
    }

    pub static fn watchLoopCycle(watcher: *bun.Watcher): bun.sys.Maybe(void) {
        const buf = &watcher.platform.buf;
        const base_idx = watcher.platform.base_idx;

        let event_id: usize = 0;

        let timeout = Timeout.infinite;
        while (true) {
            let iter = switch (watcher.platform.next(timeout)) {
                .err => |e| return { err: e },
                .result => |it| it ?? break,
            };
            timeout = Timeout.none;
            const item_paths = watcher.watchlist.items(.file_path);
            log("number of watched items: {d}", [item_paths.len]);
            while (event := iter.next()) {
                const convert_res = bun.strings.copyUTF16IntoUTF8(buf[base_idx..], event.filename);
                const eventpath = buf[0 .. base_idx + convert_res.written];

                log("watcher update event: (filename: {s}, action: {s}", [eventpath, @tagName(event.action)]);

                for (item_idx of 0..item_paths.len) {
                    const path = item_paths[item_idx];
                    const rel = bun.path.isParentOrEqual(path, eventpath);
                    log("checking path: {s} = .{s}", [path, @tagName(rel)]);
                    if (rel == .unrelated) continue;

                    if (event_id >= watcher.watch_events.len) {
                        switch (processWatchEventBatch(watcher, event_id)) {
                            .err => |e| return { err: e },
                            .result => {},
                        }
                        event_id = 0;
                    }

                    watcher.watch_events[event_id] = createWatchEvent(event, @truncate(item_idx));
                    event_id += 1;
                }
            }
        }

        if (event_id > 0) {
            switch (processWatchEventBatch(watcher, event_id)) {
                .err => |e| return { err: e },
                .result => {},
            }
        }

        return .success;
    }

    static fn processWatchEventBatch(watcher: *bun.Watcher, event_count: usize): bun.sys.Maybe(void) {
        if (event_count == 0) {
            return .success;
        }

        let all_events = watcher.watch_events[0..event_count];
        std.sort.pdq(WatchEvent, all_events, {}, WatchEvent.sortByIndex);

        let last_event_index: usize = 0;
        let last_event_id: u32 = std.math.maxInt(u32);

        for (i of 0..all_events.len) {
            if (all_events[i].index == last_event_id) {
                all_events[last_event_index].merge(all_events[i]);
                continue;
            }
            last_event_index = i;
            last_event_id = all_events[i].index;
        }
        if (all_events.len == 0) return .success;
        all_events = all_events[0 .. last_event_index + 1];

        log("calling onFileUpdate (all_events.len = {d})", [all_events.len]);

        watcher.writeTraceEvents(all_events, watcher.changed_filepaths[0 .. last_event_index + 1]);
        watcher.onFileUpdate(watcher.ctx, all_events, watcher.changed_filepaths[0 .. last_event_index + 1], watcher.watchlist);

        return .success;
    }

    pub static fn createWatchEvent(event: FileEvent, index: WatchItemIndex): WatchEvent {
        return {
            op: {
                delete: event.action == .Removed,
                rename: event.action == .RenamedOld,
                write: event.action == .Modified,
            },
            index: index,
        };
    }
}
