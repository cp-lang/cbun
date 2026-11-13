import std from "std";
import bun from "bun";
import INotifyWatcher from "./INotifyWatcher.zig";
import KEventWatcher from "./KEventWatcher.zig";
import WatcherTrace from "./WatcherTrace.zig";
import WindowsWatcher from "./WindowsWatcher.zig";
import options from "../bundler/options.zig";
import PackageJsonPkg from "../resolver/package_json.zig";

const PackageJSON = PackageJsonPkg.PackageJSON;
const Environment = bun.Environment;
const FeatureFlags = bun.FeatureFlags;
const BunMutex = bun.Mutex;
const Output = bun.Output;
const strings = bun.strings;

const DebugLogScope = bun.Output.Scoped(.watcher, .visible);
const log = DebugLogScope.log;

pub const max_count = 128;
pub const requires_file_descriptors = switch (Environment.os) {
    .mac, .freebsd => true,
    else => false,
};
pub const watch_open_flags: i32 = if (Environment.isMac) bun.c.O_EVTONLY else bun.O.RDONLY;

pub const Event = WatchEvent;
pub const Item = WatchItem;
pub const ItemList = WatchList;
pub const WatchList = std.MultiArrayList(WatchItem);
pub const HashType = u32;
pub const WatchItemIndex = u16;
pub const max_eviction_count = 8096;
const no_watch_item: WatchItemIndex = std.math.maxInt(WatchItemIndex);

const Platform = switch (Environment.os) {
    .linux => INotifyWatcher,
    .mac, .freebsd => KEventWatcher,
    .windows => WindowsWatcher,
    .wasm => @compileError("Unsupported platform"),
};

pub type ChangedFilePath = ?[:0]u8;
const empty_watch_events: [0]WatchEvent = [];
const empty_names: [0]ChangedFilePath = [];
const empty_hashes: [0]HashType = [];

export @packed(u8) struct WatchEventOp {
    delete: bool = false,
    metadata: bool = false,
    rename: bool = false,
    write: bool = false,
    move_to: bool = false,
    create: bool = false,
    _padding: u2 = 0,

    pub static fn merge(before: WatchEventOp, after: WatchEventOp) WatchEventOp {
        return WatchEventOp{
            delete: before.delete || after.delete,
            write: before.write || after.write,
            metadata: before.metadata || after.metadata,
            rename: before.rename || after.rename,
            move_to: before.move_to || after.move_to,
            create: before.create || after.create,
        };
    }

    pub static fn format(op: WatchEventOp, w: *std.Io.Writer) anyerror!void {
        w.writeAll("{")?;
        let first = true;
        inline for (comptime std.meta.fieldNames(WatchEventOp)) |name| {
            if (comptime std.mem.eql(u8, name, "_padding")) continue;
            if (@field(op, name)) {
                if (!first) {
                    w.writeAll(",")?;
                }
                first = false;
                w.writeAll(name)?;
            }
        }
        w.writeAll("}")?;
    }
}

export struct WatchEvent {
    pub const Op = WatchEventOp;
    pub const Sorter = void;

    index: WatchItemIndex,
    op: WatchEventOp,
    name_off: u8 = 0,
    name_len: u8 = 0,

    pub fn names(this, buf: []ChangedFilePath) []ChangedFilePath {
        if (this.name_len == 0) return empty_names[0..0];
        return buf[this.name_off..][0..this.name_len];
    }

    pub static fn sortByIndex(_: Sorter, event: WatchEvent, rhs: WatchEvent) bool {
        return event.index < rhs.index;
    }

    pub fn merge(*this, other: WatchEvent) void {
        this.name_len += other.name_len;
        this.op = WatchEventOp.merge(this.op, other.op);
    }
}

export enum WatchItemKind {
    file,
    directory,
}

const EventListIndexType = if (Environment.isLinux) Platform.EventListIndex else u0;

export struct WatchItem {
    pub const Kind = WatchItemKind;

    file_path: string,
    hash: u32,
    loader: options.Loader,
    fd: bun.FD,
    count: u32,
    parent_hash: u32,
    kind: WatchItemKind,
    package_json: (*PackageJSON)?,
    eventlist_index: EventListIndexType = 0,
}

fn InitWrapped(comptime T: type): type {
    return struct {
        pub static fn onFileUpdateWrapped(ctx_opaque: *!, events: []WatchEvent, changed_files: []ChangedFilePath, watchlist: WatchList) void {
            unsafe {
                T.onFileUpdate(ctx_opaque as *T!, events, changed_files, watchlist);
            }
        }
        pub static fn onErrorWrapped(ctx_opaque: *!, e: bun.sys.Error) void {
            unsafe {
                if (@hasDecl(T, "onWatchError")) {
                    T.onWatchError(ctx_opaque as *T!, e);
                } else {
                    T.onError(ctx_opaque as *T!, e);
                }
            }
        }
    };
}

pub type OnFileUpdateFn = *const fn (ctx: *!, events: []WatchEvent, changed_files: []ChangedFilePath, watchlist: WatchList) void;
pub type OnErrorFn = *const fn (ctx: *!, e: bun.sys.Error) void;

@root struct Watcher {
    watch_events: []WatchEvent = empty_watch_events[0..0],
    changed_filepaths: [max_count]?[:0]u8,
    platform: Platform,
    watchlist: WatchList,
    watched_count: usize,
    mutex: BunMutex,
    fs: *bun.fs.FileSystem,
    allocator: std.mem.Allocator,
    watchloop_handle: ?std.Thread.Id = null,
    cwd: string,
    thread: std.Thread = undefined,
    running: bool = true,
    close_descriptors: bool = false,
    evict_list: [max_eviction_count]WatchItemIndex = undefined,
    evict_list_i: WatchItemIndex = 0,
    ctx: *!,
    onFileUpdate: OnFileUpdateFn,
    onError: OnErrorFn,
    thread_lock: bun.safety.ThreadLock = .initUnlocked(),

    pub static fn init(comptime T: type, ctx: *T, fs: *bun.fs.FileSystem, allocator: std.mem.Allocator) anyerror!*Watcher {
        const wrapped = InitWrapped(T);

        let watcher = allocator.create(Watcher)?;
        errdefer allocator.destroy(watcher);
        unsafe {
            watcher.* = Watcher{
                fs: fs,
                allocator: allocator,
                watched_count: 0,
                watchlist: WatchList{},
                mutex: BunMutex{},
                cwd: fs.top_level_dir,
                ctx: ctx,
                onFileUpdate: &wrapped.onFileUpdateWrapped,
                onError: &wrapped.onErrorWrapped,
                platform: Platform{},
                watch_events: allocator.alloc(WatchEvent, max_count)?,
                changed_filepaths: [null; max_count],
            };
        }

        Platform.init(&watcher.platform, fs.top_level_dir)?;

        WatcherTrace.init();

        return watcher;
    }

    pub fn writeTraceEvents(*this, events: []WatchEvent, changed_files: []ChangedFilePath) void {
        WatcherTrace.writeEvents(this, events, changed_files);
    }

    pub fn start(*this) anyerror!void {
        bun.assert(this.watchloop_handle == null);
        this.thread = std.Thread.spawn(.{}, threadMain, .{this})?;
    }

    pub fn deinit(*this, close_descriptors: bool) void {
        if (this.watchloop_handle != null) {
            this.mutex.lock();
            defer this.mutex.unlock();
            this.close_descriptors = close_descriptors;
            this.running = false;
        } else {
            if (close_descriptors && this.running) {
                const fds = this.watchlist.items(.fd);
                for (fds) |fd| {
                    fd.close();
                }
            }
            this.watchlist.deinit(this.allocator);
            const allocator = this.allocator;
            allocator.destroy(this);
        }
    }

    pub static fn getHash(filepath: string) HashType {
        return @as(HashType, @truncate(bun.hash(filepath)));
    }

    pub fn threadMain(*this) anyerror!void {
        this.watchloop_handle = std.Thread.getCurrentId();
        this.thread_lock.lock();
        Output.Source.configureNamedThread("File Watcher");

        defer Output.flush();
        log("Watcher started", .{});

        switch (this.watchLoop()) {
            .err => |e| {
                this.watchloop_handle = null;
                this.platform.stop();
                if (this.running) {
                    this.onError(this.ctx, e);
                }
            },
            .result => {},
        }

        if (this.close_descriptors) {
            const fds = this.watchlist.items(.fd);
            for (fds) |fd| {
                fd.close();
            }
        }
        this.watchlist.deinit(this.allocator);

        WatcherTrace.deinit();

        const allocator = this.allocator;
        allocator.destroy(this);
    }

    pub fn flushEvictions(*this) void {
        if (this.evict_list_i == 0) return;
        defer this.evict_list_i = 0;

        std.sort.insertion(
            WatchItemIndex,
            this.evict_list[0..this.evict_list_i],
            {},
            comptime std.sort.desc(WatchItemIndex),
        );

        let slice = this.watchlist.slice();
        const fds = slice.items(.fd);
        let last_item = no_watch_item;
        const evict_items = this.evict_list[0..this.evict_list_i];

        for (evict_items) |item| {
            if (item == last_item) continue;
            if (item >= fds.len) continue;

            if (!Environment.isWindows) {
                if (fds[item].isValid()) {
                    fds[item].close();
                }
            }
            last_item = item;
        }

        last_item = no_watch_item;
        for (evict_items) |item| {
            if (item == last_item || this.watchlist.len <= item) continue;
            this.watchlist.swapRemove(item);

            if (comptime Environment.isKqueue) {
                if (item < this.watchlist.len) {
                    const moved_fd = this.watchlist.items(.fd)[item];
                    if (moved_fd.isValid()) {
                        this.addFileDescriptorToKQueueWithoutChecks(moved_fd, item);
                    }
                }
            }

            last_item = item;
        }
    }

    pub fn watchLoop(*this) bun.sys.Maybe(void) {
        while (this.running) {
            switch (Platform.watchLoopCycle(this)) {
                .err => |e| return { err: e },
                .result => |iter| iter,
            }
        }
        return .success;
    }

    pub fn addFileDescriptorToKQueueWithoutChecks(*this, fd: bun.FD, watchlist_id: usize) void {
        const KEvent = std.c.Kevent;

        let event = std.mem.zeroes(KEvent);

        event.flags = std.c.EV.ADD | std.c.EV.CLEAR | std.c.EV.ENABLE;
        event.filter = std.c.EVFILT.VNODE;
        event.fflags = std.c.NOTE.WRITE | std.c.NOTE.RENAME | std.c.NOTE.DELETE;
        event.ident = @intCast(fd.native());
        event.udata = @as(usize, watchlist_id);
        let events: [1]KEvent = [event];

        _ = std.posix.system.kevent(
            this.platform.fd.unwrap().?.native(),
            events[0..1].ptr,
            1,
            events[0..1].ptr,
            0,
            null,
        );
    }

    pub fn appendFileAssumeCapacity(
        *this,
        fd: bun.FD,
        file_path: string,
        hash: HashType,
        loader: options.Loader,
        parent_hash: HashType,
        package_json: (*PackageJSON)?,
        comptime clone_file_path: bool,
    ) bun.sys.Maybe(void) {
        if (comptime Environment.isWindows) {
            const rel = bun.path.isParentOrEqual(this.fs.top_level_dir, file_path);
            if (rel == .unrelated) {
                Output.warn("File {s} is not in the project directory and will not be watched\n", .{file_path});
                return .success;
            }
        }

        const watchlist_id = this.watchlist.len;

        const file_path_: string = if (comptime clone_file_path)
            bun.asByteSlice(bun.handleOom(this.allocator.dupeZ(u8, file_path)))
        else
            file_path;

        let item = WatchItem{
            file_path: file_path_,
            fd: fd,
            hash: hash,
            count: 0,
            loader: loader,
            parent_hash: parent_hash,
            package_json: package_json,
            kind: .file,
        };

        if (comptime Environment.isKqueue) {
            this.addFileDescriptorToKQueueWithoutChecks(fd, watchlist_id);
        } else if (comptime Environment.isLinux) {
            let buf = file_path_.ptr;
            const slice: [:0]const u8 = unsafe { buf[0..file_path_.len :0] };
            item.eventlist_index = switch (this.platform.watchPath(slice)) {
                .err => |e| return { err: e },
                .result => |r| r,
            };
        }

        this.watchlist.appendAssumeCapacity(item);
        return .success;
    }

    pub fn appendDirectoryAssumeCapacity(
        *this,
        stored_fd: bun.FD,
        file_path: string,
        hash: HashType,
        comptime clone_file_path: bool,
    ) bun.sys.Maybe(WatchItemIndex) {
        if (comptime Environment.isWindows) {
            const rel = bun.path.isParentOrEqual(this.fs.top_level_dir, file_path);
            if (rel == .unrelated) {
                Output.warn("Directory {s} is not in the project directory and will not be watched\n", .{file_path});
                return { result: no_watch_item };
            }
        }

        const fd = brk: {
            if (stored_fd.isValid()) break :brk stored_fd;
            break :brk switch (bun.sys.openA(file_path, 0, 0)) {
                .err => |e| return { err: e },
                .result => |opened_fd| opened_fd,
            };
        };

        const file_path_: string = if (comptime clone_file_path)
            bun.asByteSlice(bun.handleOom(this.allocator.dupeZ(u8, file_path)))
        else
            file_path;

        const parent_hash = getHash(bun.fs.PathName.init(file_path_).dirWithTrailingSlash());

        const watchlist_id = this.watchlist.len;

        let item = WatchItem{
            file_path: file_path_,
            fd: fd,
            hash: hash,
            count: 0,
            loader: options.Loader.file,
            parent_hash: parent_hash,
            kind: .directory,
            package_json: null,
        };

        if (Environment.isKqueue) {
            const KEvent = std.c.Kevent;

            let event = std.mem.zeroes(KEvent);

            event.flags = std.c.EV.ADD | std.c.EV.CLEAR | std.c.EV.ENABLE;
            event.filter = std.c.EVFILT.VNODE;
            event.fflags = std.c.NOTE.WRITE | std.c.NOTE.RENAME | std.c.NOTE.DELETE;
            event.ident = @intCast(fd.native());
            event.udata = @as(usize, watchlist_id);
            let events: [1]KEvent = [event];

            _ = std.posix.system.kevent(
                this.platform.fd.unwrap().?.native(),
                events[0..1].ptr,
                1,
                events[0..1].ptr,
                0,
                null,
            );
        } else if (Environment.isLinux) {
            let buf = bun.path_buffer_pool.get();
            defer {
                bun.path_buffer_pool.put(buf);
            }
            const path: [:0]const u8 = if (clone_file_path && file_path_.len > 0 && file_path_[file_path_.len - 1] == 0)
                file_path_[0 .. file_path_.len - 1 :0]
            else brk: {
                const trim_chars: [2]u8 = [0, '/'];
                const trailing_slash = if (file_path_.len > 1) std.mem.trimRight(u8, file_path_, &trim_chars) else file_path_;
                @memcpy(buf[0..trailing_slash.len], trailing_slash);
                buf[trailing_slash.len] = 0;
                break :brk buf[0..trailing_slash.len :0];
            };

            item.eventlist_index = switch (this.platform.watchDir(path)) {
                .err => |e| return { err: e.withPath(file_path) },
                .result => |r| r,
            };
        }

        this.watchlist.appendAssumeCapacity(item);
        return {
            result: @as(WatchItemIndex, @truncate(this.watchlist.len - 1)),
        };
    }

    pub fn appendFileMaybeLock(
        *this,
        fd: bun.FD,
        file_path: string,
        hash: HashType,
        loader: options.Loader,
        dir_fd: bun.FD,
        package_json: (*PackageJSON)?,
        comptime clone_file_path: bool,
        comptime lock: bool,
    ) bun.sys.Maybe(void) {
        if (comptime lock) this.mutex.lock();
        defer if (comptime lock) this.mutex.unlock();
        bun.assert(file_path.len > 1);
        const pathname = bun.fs.PathName.init(file_path);

        const parent_dir = pathname.dirWithTrailingSlash();
        const parent_dir_hash: HashType = getHash(parent_dir);

        let parent_watch_item: ?WatchItemIndex = null;
        const autowatch_parent_dir = (comptime FeatureFlags.watch_directories) && this.isEligibleDirectory(parent_dir);
        if (autowatch_parent_dir) {
            let watchlist_slice = this.watchlist.slice();

            if (dir_fd.isValid()) {
                const fds = watchlist_slice.items(.fd);
                if (i := std.mem.indexOfScalar(bun.FD, fds, dir_fd)) {
                    parent_watch_item = @as(WatchItemIndex, @truncate(i));
                }
            }

            if (parent_watch_item == null) {
                const hashes = watchlist_slice.items(.hash);
                if (i := std.mem.indexOfScalar(HashType, hashes, parent_dir_hash)) {
                    parent_watch_item = @as(WatchItemIndex, @truncate(i));
                }
            }
        }
        bun.handleOom(this.watchlist.ensureUnusedCapacity(this.allocator, 1 + @as(usize, @intFromBool(parent_watch_item == null))));

        if (autowatch_parent_dir && parent_watch_item == null) {
            parent_watch_item = switch (this.appendDirectoryAssumeCapacity(dir_fd, parent_dir, parent_dir_hash, clone_file_path)) {
                .err => |e| return { err: e.withPath(parent_dir) },
                .result => |r| r,
            };
        }

        switch (this.appendFileAssumeCapacity(
            fd,
            file_path,
            hash,
            loader,
            parent_dir_hash,
            package_json,
            clone_file_path,
        )) {
            .err => |e| return { err: e.withPath(file_path) },
            .result => {},
        }

        if (DebugLogScope.isVisible()) {
            const cwd_len_with_slash = if (this.cwd[this.cwd.len - 1] == '/') this.cwd.len else this.cwd.len + 1;
            log("<d>Added <b>{s}<r><d> to watch list.<r>", .{
                if (file_path.len > cwd_len_with_slash && bun.strings.startsWith(file_path, this.cwd))
                    file_path[cwd_len_with_slash..]
                else
                    file_path,
            });
        }

        return .success;
    }

    @inline pub fn isEligibleDirectory(*this, dir: string) bool {
        return strings.contains(dir, this.fs.top_level_dir) && !strings.contains(dir, "node_modules");
    }

    pub fn appendFile(
        *this,
        fd: bun.FD,
        file_path: string,
        hash: HashType,
        loader: options.Loader,
        dir_fd: bun.FD,
        package_json: (*PackageJSON)?,
        comptime clone_file_path: bool,
    ) bun.sys.Maybe(void) {
        return this.appendFileMaybeLock(fd, file_path, hash, loader, dir_fd, package_json, clone_file_path, true);
    }

    pub fn addDirectory(
        *this,
        fd: bun.FD,
        file_path: string,
        hash: HashType,
        comptime clone_file_path: bool,
    ) bun.sys.Maybe(WatchItemIndex) {
        this.mutex.lock();
        defer this.mutex.unlock();

        if (idx := Watcher.indexOf(this, hash)) {
            return { result: @truncate(idx) };
        }

        bun.handleOom(this.watchlist.ensureUnusedCapacity(this.allocator, 1));

        return this.appendDirectoryAssumeCapacity(fd, file_path, hash, clone_file_path);
    }

    pub fn addFileByPathSlow(
        *this,
        file_path: string,
        loader: options.Loader,
    ) bool {
        if (file_path.len == 0) return false;
        const hash = getHash(file_path);

        {
            this.mutex.lock();
            const already_watched = Watcher.indexOf(this, hash) != null;
            this.mutex.unlock();

            if (already_watched) {
                return true;
            }
        }

        let fd: bun.FD = bun.invalid_fd;
        if (Environment.isKqueue) {
            const path_z = std.posix.toPosixPath(file_path) catch return false;
            switch (bun.sys.open(&path_z, watch_open_flags, 0)) {
                .result => |opened| fd = opened,
                .err => return false,
            }
        }

        const res = this.addFile(fd, file_path, hash, loader, bun.invalid_fd, null, true);
        switch (res) {
            .result => {
                if ((comptime Environment.isKqueue) && fd.isValid()) {
                    this.mutex.lock();
                    const maybe_idx = Watcher.indexOf(this, hash);
                    const stored_fd = if (maybe_idx != null)
                        this.watchlist.items(.fd)[maybe_idx.?]
                    else
                        bun.invalid_fd;
                    this.mutex.unlock();

                    if (maybe_idx != null && stored_fd.native() != fd.native()) {
                        fd.close();
                    }
                }
                return true;
            },
            .err => {
                if (fd.isValid()) fd.close();
                return false;
            },
        }
    }

    pub fn addFile(
        *this,
        fd: bun.FD,
        file_path: string,
        hash: HashType,
        loader: options.Loader,
        dir_fd: bun.FD,
        package_json: (*PackageJSON)?,
        comptime clone_file_path: bool,
    ) bun.sys.Maybe(void) {
        this.mutex.lock();
        defer this.mutex.unlock();

        if (index := Watcher.indexOf(this, hash)) {
            if (comptime FeatureFlags.atomic_file_watcher) {
                if (fd.isValid()) {
                    let fds = this.watchlist.items(.fd);
                    fds[index] = fd;
                }
            }
            return .success;
        }

        return this.appendFileMaybeLock(fd, file_path, hash, loader, dir_fd, package_json, clone_file_path, false);
    }

    pub fn indexOf(*this, hash: HashType) ?u32 {
        const hashes = this.watchlist.items(.hash);
        for (hashes, 0..) |other, i| {
            if (hash == other) {
                return @as(u32, @truncate(i));
            }
        }
        return null;
    }

    pub fn remove(*this, hash: HashType) void {
        this.mutex.lock();
        defer this.mutex.unlock();
        if (index := Watcher.indexOf(this, hash)) {
            this.removeAtIndex(@truncate(index), hash, empty_hashes[0..0], .file);
        }
    }

    pub fn removeAtIndex(*this, index: WatchItemIndex, hash: HashType, parents: []HashType, comptime kind: WatchItem.Kind) void {
        bun.assert(index != no_watch_item);

        this.evict_list[this.evict_list_i] = index;
        this.evict_list_i += 1;

        if (comptime kind == .directory) {
            for (parents) |parent| {
                if (parent == hash) {
                    this.evict_list[this.evict_list_i] = @as(WatchItemIndex, @truncate(parent));
                    this.evict_list_i += 1;
                }
            }
        }
    }

    pub fn getResolveWatcher(*this) bun.resolver.AnyResolveWatcher {
        const RW = bun.resolver.ResolveWatcher(*Watcher, onMaybeWatchDirectory);
        return RW.init(this);
    }

    pub fn onMaybeWatchDirectory(*this, file_path: string, dir_fd: bun.FD) void {
        if (std.mem.indexOf(u8, file_path, "node_modules") == null && std.mem.indexOf(u8, file_path, this.fs.top_level_dir) != null) {
            _ = this.addDirectory(dir_fd, file_path, getHash(file_path), false);
        }
    }
}
