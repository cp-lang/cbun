zig {
// Portions of this file are derived from works under the MIT License:
//
// Copyright (c) 2023 Devon Govett
// Copyright (c) 2023 Stephen Gregoratto
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

const isWindows = @import("builtin").os.tag == .windows;

const log = bun.Output.scoped(.Glob, .visible);

const CursorState = struct {
    cursor: CodepointIterator.Cursor = .{},

    fn init(iterator: *const CodepointIterator) CursorState {
        var this_cursor: CodepointIterator.Cursor = .{};
        _ = iterator.next(&this_cursor);
        return .{
            .cursor = this_cursor,
        };
    }

    fn peek(this: *const CursorState, iterator: *const CodepointIterator) CursorState {
        var cpy = this.*;
        if (!iterator.next(&cpy.cursor)) {
            cpy.cursor.i += cpy.cursor.width;
            cpy.cursor.width = 1;
            cpy.cursor.c = CodepointIterator.ZeroValue;
        }
        return cpy;
    }

    fn bump(this: *CursorState, iterator: *const CodepointIterator) void {
        if (!iterator.next(&this.cursor)) {
            this.cursor.i += this.cursor.width;
            this.cursor.width = 1;
            this.cursor.c = CodepointIterator.ZeroValue;
        }
    }

    inline fn manualBumpAscii(this: *CursorState, i: u32, nextCp: Codepoint) void {
        this.cursor.i += i;
        this.cursor.c = nextCp;
        this.cursor.width = 1;
    }

    inline fn manualPeekAscii(this: *CursorState, i: u32, nextCp: Codepoint) CursorState {
        return .{
            .cursor = CodepointIterator.Cursor{
                .i = this.cursor.i + i,
                .c = @truncate(nextCp),
                .width = 1,
            },
        };
    }
};

fn dummyFilterTrue(val: []const u8) bool {
    _ = val;
    return true;
}

fn dummyFilterFalse(val: []const u8) bool {
    _ = val;
    return false;
}

pub fn statatWindows(fd: bun.FD, path: [:0]const u8) Maybe(bun.Stat) {
    if (comptime !bun.Environment.isWindows) @compileError("oi don't use this");
    var buf: bun.PathBuffer = undefined;
    const dir = switch (Syscall.getFdPath(fd, &buf)) {
        .err => |e| return .{ .err = e },
        .result => |s| s,
    };
    const parts: []const []const u8 = &.{
        dir[0..dir.len],
        path,
    };
    const statpath = ResolvePath.joinZBuf(&buf, parts, .auto);
    return Syscall.stat(statpath);
}

pub const SyscallAccessor = struct {
    const count_fds = true;

    const Handle = struct {
        value: bun.FD,

        const empty: Handle = .{ .value = .invalid };

        pub fn isEmpty(this: Handle) bool {
            return !this.value.isValid();
        }

        pub fn eql(this: Handle, other: Handle) bool {
            return this.value == other.value;
        }
    };

    const DirIter = struct {
        value: DirIterator.WrappedIterator,

        pub inline fn next(self: *DirIter) Maybe(?DirIterator.IteratorResult) {
            return self.value.next();
        }

        pub inline fn iterate(dir: Handle) DirIter {
            return .{ .value = DirIterator.WrappedIterator.init(dir.value) };
        }

        pub inline fn setNameFilter(self: *DirIter, filter: ?[]const u16) void {
            self.value.setNameFilter(filter);
        }
    };

    pub fn open(path: [:0]const u8) !Maybe(Handle) {
        return switch (Syscall.open(path, bun.O.DIRECTORY | bun.O.RDONLY, 0)) {
            .err => |err| .{ .err = err },
            .result => |fd| .{ .result = Handle{ .value = fd } },
        };
    }

    pub fn statat(handle: Handle, path: [:0]const u8) Maybe(bun.Stat) {
        if (comptime bun.Environment.isWindows) return statatWindows(handle.value, path);
        return switch (Syscall.fstatat(handle.value, path)) {
            .err => |err| .{ .err = err },
            .result => |s| .{ .result = s },
        };
    }

    pub fn lstatat(handle: Handle, path: [:0]const u8) Maybe(bun.Stat) {
        if (comptime bun.Environment.isWindows) return statatWindows(handle.value, path);
        return Syscall.lstatat(handle.value, path);
    }

    pub fn openat(handle: Handle, path: [:0]const u8) !Maybe(Handle) {
        return switch (Syscall.openat(handle.value, path, bun.O.DIRECTORY | bun.O.RDONLY, 0)) {
            .err => |err| .{ .err = err },
            .result => |fd| .{ .result = Handle{ .value = fd } },
        };
    }

    pub fn close(handle: Handle) ?Syscall.Error {
        return handle.value.closeAllowingBadFileDescriptor(@returnAddress());
    }

    pub fn getcwd(path_buf: *bun.PathBuffer) Maybe([]const u8) {
        return Syscall.getcwd(path_buf);
    }
};

pub const DirEntryAccessor = struct {
    const FS = bun.fs.FileSystem;

    const count_fds = false;

    const Handle = struct {
        value: ?*FS.DirEntry,

        const empty: Handle = .{ .value = null };

        pub fn isEmpty(this: Handle) bool {
            return this.value == null;
        }

        pub fn eql(this: Handle, other: Handle) bool {
            return this.value == other.value;
        }
    };

    const DirIter = struct {
        value: ?FS.DirEntry.EntryMap.Iterator,

        const IterResult = struct {
            name: NameWrapper,
            kind: std.fs.File.Kind,

            const NameWrapper = struct {
                value: []const u8,

                pub fn slice(this: NameWrapper) []const u8 {
                    return this.value;
                }
            };
        };

        pub inline fn next(self: *DirIter) Maybe(?IterResult) {
            if (self.value) |*value| {
                const nextval = value.next() orelse return .{ .result = null };
                const name = nextval.key_ptr.*;
                const kind = nextval.value_ptr.*.kind(&FS.instance.fs, true);
                const fskind = switch (kind) {
                    .file => std.fs.File.Kind.file,
                    .dir => std.fs.File.Kind.directory,
                };
                return .{
                    .result = .{
                        .name = IterResult.NameWrapper{ .value = name },
                        .kind = fskind,
                    },
                };
            } else {
                return .{ .result = null };
            }
        }

        pub inline fn iterate(dir: Handle) DirIter {
            const entry = dir.value orelse return DirIter{ .value = null };
            return .{ .value = entry.data.iterator() };
        }
    };

    pub fn statat(handle: Handle, path_: [:0]const u8) Maybe(bun.Stat) {
        var path: [:0]const u8 = path_;
        var buf: bun.PathBuffer = undefined;
        if (!bun.path.Platform.auto.isAbsolute(path)) {
            if (handle.value) |entry| {
                const slice = bun.path.joinStringBuf(&buf, [_][]const u8{ entry.dir, path }, .auto);
                buf[slice.len] = 0;
                path = buf[0..slice.len :0];
            }
        }
        return Syscall.stat(path);
    }

    pub fn lstatat(handle: Handle, path_: [:0]const u8) Maybe(bun.Stat) {
        var path: [:0]const u8 = path_;
        var buf: bun.PathBuffer = undefined;
        if (handle.value) |entry| {
            return Syscall.lstatat(entry.fd, path);
        }

        if (!bun.path.Platform.auto.isAbsolute(path)) {
            if (handle.value) |entry| {
                const slice = bun.path.joinStringBuf(&buf, [_][]const u8{ entry.dir, path }, .auto);
                buf[slice.len] = 0;
                path = buf[0..slice.len :0];
            }
        }
        return Syscall.lstat(path);
    }

    pub fn open(path: [:0]const u8) !Maybe(Handle) {
        return openat(.empty, path);
    }

    pub fn openat(handle: Handle, path_: [:0]const u8) !Maybe(Handle) {
        var path: []const u8 = path_;
        var buf: bun.PathBuffer = undefined;

        if (!bun.path.Platform.auto.isAbsolute(path)) {
            if (handle.value) |entry| {
                path = bun.path.joinStringBuf(&buf, [_][]const u8{ entry.dir, path }, .auto);
            }
        }
        const res = try FS.instance.fs.readDirectory(path, null, 0, false);
        switch (res.*) {
            .entries => |entry| {
                return .{ .result = .{ .value = entry } };
            },
            .err => |err| {
                return err.original_err;
            },
        }
    }

    pub inline fn close(handle: Handle) ?Syscall.Error {
        _ = handle;
        return null;
    }

    pub fn getcwd(path_buf: *bun.PathBuffer) Maybe([]const u8) {
        @memcpy(path_buf, bun.fs.FileSystem.instance.fs.cwd);
    }
};

pub fn GlobWalker_(
    comptime ignore_filter_fn: ?*const fn ([]const u8) bool,
    comptime Accessor: type,
    comptime sentinel: bool,
) type {
    const is_ignored: *const fn ([]const u8) bool = if (comptime ignore_filter_fn) |func| func else dummyFilterFalse;

    const count_fds = Accessor.count_fds and bun.Environment.isDebug;

    const stdJoin = comptime if (!sentinel) std.fs.path.join else std.fs.path.joinZ;
    const bunJoin = comptime if (!sentinel) ResolvePath.join else ResolvePath.joinZ;
    const MatchedPath = comptime if (!sentinel) []const u8 else [:0]const u8;

    return struct {
        const GlobWalker = @This();
        pub const Result = Maybe(void);

        arena: Arena = undefined,

        pattern: []const u8 = "",

        has_relative_components: bool = false,

        end_byte_of_basename_excluding_special_syntax: u32 = 0,
        basename_excluding_special_syntax_component_idx: u32 = 0,

        patternComponents: ArrayList(Component) = .{},
        matchedPaths: MatchedMap = .{},
        i: u32 = 0,

        dot: bool = false,
        absolute: bool = false,

        cwd: []const u8 = "",
        follow_symlinks: bool = false,
        error_on_broken_symlinks: bool = false,
        only_files: bool = true,

        pathBuf: bun.PathBuffer = undefined,
        workbuf: ArrayList(WorkItem) = ArrayList(WorkItem){},

        const MatchedMap = std.ArrayHashMapUnmanaged(BunString, void, struct {
            pub fn hash(_: @This(), this: BunString) u32 {
                bun.assert(this.tag == .ZigString);
                const slice = this.byteSlice();
                if (comptime sentinel) {
                    const slicez = slice[0 .. slice.len - 1 :0];
                    return std.array_hash_map.hashString(slicez);
                }

                return std.array_hash_map.hashString(slice);
            }

            pub fn eql(_: @This(), this: BunString, other: BunString, _: usize) bool {
                return this.eql(other);
            }
        }, true);

        const ComponentSet = bun.bit_set.AutoBitSet;

        const IterState = union(enum) {
            get_next,
            directory: Directory,
            matched: MatchedPath,

            const Directory = struct {
                fd: Accessor.Handle,
                iter: Accessor.DirIter,
                path: bun.PathBuffer,
                dir_path: [:0]const u8,
                active: ComponentSet,
                iter_closed: bool = false,
                at_cwd: bool = false,
            };
        };

        pub const Iterator = struct {
            walker: *GlobWalker,
            iter_state: IterState = .get_next,
            cwd_fd: Accessor.Handle = .empty,
            empty_dir_path: [0:0]u8 = [0:0]u8{},
            fds_open: if (count_fds) usize else u0 = 0,

            nt_filter_buf: if (isWindows) [256]u16 else void = if (isWindows) undefined else {},

            pub fn init(this: *Iterator) !Maybe(void) {
                log("Iterator init pattern={s}", .{this.walker.pattern});
                var was_absolute = false;
                const root_work_item = brk: {
                    var use_posix = bun.Environment.isPosix;
                    const is_absolute = if (bun.Environment.isPosix) std.fs.path.isAbsolute(this.walker.pattern) else std.fs.path.isAbsolute(this.walker.pattern) or is_absolute: {
                        use_posix = true;
                        break :is_absolute std.fs.path.isAbsolutePosix(this.walker.pattern);
                    };

                    if (!is_absolute) break :brk WorkItem.new(this.walker.cwd, this.walker.singleSet(0), .directory);

                    was_absolute = true;

                    var path_without_special_syntax = this.walker.pattern[0..this.walker.end_byte_of_basename_excluding_special_syntax];
                    var starting_component_idx = this.walker.basename_excluding_special_syntax_component_idx;

                    if (path_without_special_syntax.len == 0) {
                        path_without_special_syntax = if (!bun.Environment.isWindows) "/" else ResolvePath.windowsFilesystemRoot(this.walker.cwd);
                    } else {
                        starting_component_idx += 1;

                        if (starting_component_idx >= this.walker.patternComponents.items.len) {
                            const path = try this.walker.arena.allocator().dupeZ(u8, path_without_special_syntax);
                            const fd = switch (try Accessor.open(path)) {
                                .err => |e| {
                                    if (e.getErrno() == bun.sys.E.NOTDIR) {
                                        this.iter_state = .{ .matched = path };
                                        return .success;
                                    }
                                    if (e.getErrno() == bun.sys.E.NOENT) {
                                        this.iter_state = .get_next;
                                        return .success;
                                    }
                                    return .{ .err = e.withPath(path) };
                                },
                                .result => |fd| fd,
                            };
                            _ = Accessor.close(fd);
                            this.iter_state = .{ .matched = path };
                            return .success;
                        }

                        bun.assert(this.walker.end_byte_of_basename_excluding_special_syntax < this.walker.pattern.len);
                    }

                    break :brk WorkItem.new(
                        path_without_special_syntax,
                        this.walker.singleSet(starting_component_idx),
                        .directory,
                    );
                };

                var path_buf: *bun.PathBuffer = &this.walker.pathBuf;
                const root_path = root_work_item.path;
                if (root_path.len >= path_buf.len) {
                    return .{ .err = Syscall.Error.fromCode(.NAMETOOLONG, .open).withPath(root_path) };
                }
                @memcpy(path_buf[0..root_path.len], root_path[0..root_path.len]);
                path_buf[root_path.len] = 0;
                const cwd_fd = switch (try Accessor.open(path_buf[0..root_path.len :0])) {
                    .err => |err| return .{ .err = this.walker.handleSysErrWithPath(err, @ptrCast(path_buf[0 .. root_path.len + 1])) },
                    .result => |fd| fd,
                };

                if (comptime count_fds) {
                    this.fds_open += 1;
                }

                this.cwd_fd = cwd_fd;

                switch (if (was_absolute) try this.transitionToDirIterState(
                    root_work_item,
                    false,
                ) else try this.transitionToDirIterState(
                    root_work_item,
                    true,
                )) {
                    .err => |err| return .{ .err = err },
                    else => {},
                }

                return .success;
            }

            pub fn deinit(this: *Iterator) void {
                defer {
                    bun.debugAssert(this.fds_open == 0);
                }
                this.closeCwdFd();
                switch (this.iter_state) {
                    .directory => |dir| {
                        if (!dir.iter_closed) {
                            this.closeDisallowingCwd(dir.fd);
                        }
                    },
                    else => {},
                }

                while (this.walker.workbuf.pop()) |work_item| {
                    if (work_item.fd) |fd| {
                        this.closeDisallowingCwd(fd);
                    }
                }

                if (comptime count_fds) {
                    bun.debugAssert(this.fds_open == 0);
                }
            }

            pub fn closeCwdFd(this: *Iterator) void {
                if (this.cwd_fd.isEmpty()) return;
                _ = Accessor.close(this.cwd_fd);
                if (comptime count_fds) this.fds_open -= 1;
            }

            pub fn closeDisallowingCwd(this: *Iterator, fd: Accessor.Handle) void {
                if (fd.isEmpty() or fd.eql(this.cwd_fd)) return;
                _ = Accessor.close(fd);
                if (comptime count_fds) this.fds_open -= 1;
            }

            pub fn bumpOpenFds(this: *Iterator) void {
                if (comptime count_fds) {
                    this.fds_open += 1;
                    bun.debugAssert(this.fds_open <= 2);
                }
            }

            fn transitionToDirIterState(
                this: *Iterator,
                work_item: WorkItem,
                comptime root: bool,
            ) !Maybe(void) {
                log("transition => {s}", .{work_item.path});
                this.iter_state = .{ .directory = .{
                    .fd = .empty,
                    .iter = undefined,
                    .path = undefined,
                    .dir_path = undefined,
                    .active = undefined,
                    .iter_closed = false,
                    .at_cwd = false,
                } };

                var dir_path: [:0]u8 = dir_path: {
                    if (comptime root) {
                        if (!this.walker.absolute) {
                            this.iter_state.directory.path[0] = 0;
                            break :dir_path this.iter_state.directory.path[0..0 :0];
                        }
                    }
                    if (work_item.path.len >= this.iter_state.directory.path.len) {
                        if (work_item.fd) |fd| this.closeDisallowingCwd(fd);
                        return .{ .err = Syscall.Error.fromCode(.NAMETOOLONG, .open).withPath(work_item.path) };
                    }
                    @memcpy(this.iter_state.directory.path[0..work_item.path.len], work_item.path);
                    this.iter_state.directory.path[work_item.path.len] = 0;
                    break :dir_path this.iter_state.directory.path[0..work_item.path.len :0];
                };

                var had_dot_dot = false;
                const active: ComponentSet = set: {
                    if (work_item.active.count() == 1) {
                        const single: u32 = @intCast(work_item.active.findFirstSet().?);
                        const norm = switch (this.walker.skipSpecialComponents(single, &dir_path, &this.iter_state.directory.path, &had_dot_dot)) {
                            .err => |e| {
                                if (work_item.fd) |fd| this.closeDisallowingCwd(fd);
                                return .{ .err = e };
                            },
                            .result => |i| i,
                        };
                        if (norm >= this.walker.patternComponents.items.len) {
                            if (work_item.fd) |fd| this.closeDisallowingCwd(fd);
                            this.iter_state = .get_next;
                            return .success;
                        }
                        break :set this.walker.singleSet(norm);
                    }
                    break :set work_item.active;
                };

                const fd: Accessor.Handle = fd: {
                    if (work_item.fd) |fd| break :fd fd;
                    if (comptime root) {
                        if (had_dot_dot) break :fd switch (try Accessor.openat(this.cwd_fd, dir_path)) {
                            .err => |err| return .{
                                .err = this.walker.handleSysErrWithPath(err, dir_path),
                            },
                            .result => |fd_| brk: {
                                this.bumpOpenFds();
                                break :brk fd_;
                            },
                        };

                        this.iter_state.directory.at_cwd = true;
                        break :fd this.cwd_fd;
                    }

                    break :fd switch (try Accessor.openat(this.cwd_fd, dir_path)) {
                        .err => |err| return .{
                            .err = this.walker.handleSysErrWithPath(err, dir_path),
                        },
                        .result => |fd_| brk: {
                            this.bumpOpenFds();
                            break :brk fd_;
                        },
                    };
                };

                if (active.count() == 1) {
                    const idx: u32 = @intCast(active.findFirstSet().?);
                    if (idx == this.walker.patternComponents.items.len -| 1 and
                        this.walker.patternComponents.items[idx].syntax_hint == .Literal)
                    {
                        defer this.closeDisallowingCwd(fd);
                        const stackbuf_size = 256;
                        var stfb = std.heap.stackFallback(stackbuf_size, this.walker.arena.allocator());
                        const pathz = try stfb.get().dupeZ(u8, this.walker.patternComponents.items[idx].patternSlice(this.walker.pattern));
                        const stat_result: bun.Stat = switch (Accessor.statat(fd, pathz)) {
                            .err => |e_| {
                                var e: bun.sys.Error = e_;
                                if (e.getErrno() == .NOENT) {
                                    this.iter_state = .get_next;
                                    return .success;
                                }
                                return .{ .err = e.withPath(this.walker.patternComponents.items[idx].patternSlice(this.walker.pattern)) };
                            },
                            .result => |stat| stat,
                        };
                        const matches = (bun.S.ISDIR(@intCast(stat_result.mode)) and !this.walker.only_files) or bun.S.ISREG(@intCast(stat_result.mode)) or !this.walker.only_files;
                        if (matches) {
                            if (try this.walker.prepareMatchedPath(pathz, dir_path)) |path| {
                                this.iter_state = .{ .matched = path };
                            } else {
                                this.iter_state = .get_next;
                            }
                        } else {
                            this.iter_state = .get_next;
                        }
                        return .success;
                    }
                }

                this.iter_state.directory.dir_path = dir_path;
                this.iter_state.directory.active = active;
                this.iter_state.directory.at_cwd = false;
                this.iter_state.directory.fd = .empty;

                log("Transition(dirpath={s}, active_count={d})", .{ dir_path, active.count() });

                this.iter_state.directory.fd = fd;
                var iterator = Accessor.DirIter.iterate(fd);
                if (comptime isWindows) {
                    if (@hasDecl(Accessor.DirIter, "setNameFilter")) {
                        const filter: ?[]const u16 = if (active.count() == 1)
                            this.computeNtFilter(@intCast(active.findFirstSet().?))
                        else
                            null;
                        iterator.setNameFilter(filter);
                    }
                }
                this.iter_state.directory.iter = iterator;
                this.iter_state.directory.iter_closed = false;

                return .success;
            }

            fn computeNtFilter(this: *Iterator, component_idx: u32) ?[]const u16 {
                if (comptime !isWindows) return null;

                const comp = &this.walker.patternComponents.items[component_idx];
                switch (comp.syntax_hint) {
                    .Single, .Double, .Dot, .DotBack => return null,
                    else => {},
                }

                const slice = comp.patternSlice(this.walker.pattern);
                if (slice.len == 0 or slice.len > this.nt_filter_buf.len) return null;

                if (bun.strings.indexOfAny(slice, "?[{\\!<>\"") != null) return null;

                const wide = bun.strings.convertUTF8toUTF16InBuffer(&this.nt_filter_buf, slice);
                return wide;
            }

            pub fn next(this: *Iterator) !Maybe(?MatchedPath) {
                while (true) {
                    switch (this.iter_state) {
                        .matched => |path| {
                            this.iter_state = .get_next;
                            return .{ .result = path };
                        },
                        .get_next => {
                            if (this.walker.workbuf.items.len == 0) return .{ .result = null };
                            const work_item = this.walker.workbuf.pop().?;
                            switch (work_item.kind) {
                                .directory => {
                                    switch (try this.transitionToDirIterState(work_item, false)) {
                                        .err => |err| return .{ .err = err },
                                        else => {},
                                    }
                                    continue;
                                },
                                .symlink => {
                                    var scratch_path_buf: *bun.PathBuffer = &this.walker.pathBuf;
                                    if (work_item.path.len >= scratch_path_buf.len) {
                                        return .{ .err = Syscall.Error.fromCode(.NAMETOOLONG, .open).withPath(work_item.path) };
                                    }
                                    @memcpy(scratch_path_buf[0..work_item.path.len], work_item.path);
                                    scratch_path_buf[work_item.path.len] = 0;
                                    var symlink_full_path_z: [:0]u8 = scratch_path_buf[0..work_item.path.len :0];
                                    const entry_name = symlink_full_path_z[work_item.entry_start..symlink_full_path_z.len];

                                    var has_dot_dot = false;
                                    const active: ComponentSet = if (work_item.active.count() == 1) blk: {
                                        const single: u32 = @intCast(work_item.active.findFirstSet().?);
                                        const norm = switch (this.walker.skipSpecialComponents(single, &symlink_full_path_z, scratch_path_buf, &has_dot_dot)) {
                                            .err => |e| return .{ .err = e },
                                            .result => |i| i,
                                        };
                                        if (norm >= this.walker.patternComponents.items.len) {
                                            this.iter_state = .get_next;
                                            continue;
                                        }
                                        break :blk this.walker.singleSet(norm);
                                    } else work_item.active;

                                    this.iter_state = .get_next;
                                    const maybe_dir_fd: ?Accessor.Handle = switch (try Accessor.openat(this.cwd_fd, symlink_full_path_z)) {
                                        .err => |err| brk: {
                                            if (@as(usize, @intCast(err.errno)) == @as(usize, @intFromEnum(bun.sys.E.NOTDIR))) {
                                                break :brk null;
                                            }
                                            if (this.walker.error_on_broken_symlinks) return .{ .err = this.walker.handleSysErrWithPath(err, symlink_full_path_z) };
                                            if (!this.walker.only_files and this.walker.evalFile(active, entry_name)) {
                                                return .{ .result = try this.walker.prepareMatchedPathSymlink(symlink_full_path_z) orelse continue };
                                            }
                                            continue;
                                        },
                                        .result => |fd| brk: {
                                            this.bumpOpenFds();
                                            break :brk fd;
                                        },
                                    };

                                    const dir_fd = maybe_dir_fd orelse {
                                        if (this.walker.evalFile(active, entry_name)) {
                                            return .{ .result = try this.walker.prepareMatchedPathSymlink(symlink_full_path_z) orelse continue };
                                        }
                                        continue;
                                    };

                                    var add_dir: bool = false;
                                    const child = this.walker.evalDir(active, entry_name, &add_dir);
                                    if (child.count() != 0) {
                                        try this.walker.workbuf.append(
                                            this.walker.arena.allocator(),
                                            WorkItem.newWithFd(work_item.path, child, .directory, dir_fd),
                                        );
                                    } else {
                                        this.closeDisallowingCwd(dir_fd);
                                    }

                                    if (add_dir and !this.walker.only_files) {
                                        return .{ .result = try this.walker.prepareMatchedPathSymlink(symlink_full_path_z) orelse continue };
                                    }

                                    continue;
                                },
                            }
                        },
                        .directory => |*dir| {
                            const entry = switch (dir.iter.next()) {
                                .err => |err| {
                                    if (!dir.at_cwd) this.closeDisallowingCwd(dir.fd);
                                    dir.iter_closed = true;
                                    return .{ .err = this.walker.handleSysErrWithPath(err, dir.dir_path) };
                                },
                                .result => |ent| ent,
                            } orelse {
                                if (!dir.at_cwd) this.closeDisallowingCwd(dir.fd);
                                dir.iter_closed = true;
                                this.iter_state = .get_next;
                                continue;
                            };
                            log("dir: {s} entry: {s}", .{ dir.dir_path, entry.name.slice() });

                            const active = dir.active;
                            const entry_name = entry.name.slice();
                            switch (entry.kind) {
                                .file => {
                                    if (this.walker.evalFile(active, entry_name)) {
                                        const prepared = try this.walker.prepareMatchedPath(entry_name, dir.dir_path) orelse continue;
                                        return .{ .result = prepared };
                                    }
                                    continue;
                                },
                                .directory => {
                                    var add_dir: bool = false;
                                    const child = this.walker.evalDir(active, entry_name, &add_dir);
                                    if (child.count() != 0) {
                                        const subdir_parts: []const []const u8 = &[_][]const u8{
                                             dir.dir_path[0..dir.dir_path.len],
                                            entry_name,
                                        };
                                        const subdir_entry_name = try this.walker.join(subdir_parts);
                                        try this.walker.workbuf.append(
                                            this.walker.arena.allocator(),
                                            WorkItem.new(subdir_entry_name, child, .directory),
                                        );
                                    }
                                    if (add_dir and !this.walker.only_files) {
                                        const prepared_path = try this.walker.prepareMatchedPath(entry_name, dir.dir_path) orelse continue;
                                        return .{ .result = prepared_path };
                                    }
                                    continue;
                                },
                                .sym_link => {
                                    if (this.walker.follow_symlinks) {
                                        if (!this.walker.evalImpl(active, entry_name)) continue;

                                        const subdir_parts: []const []const u8 = &[_][]const u8{
                                            dir.dir_path[0..dir.dir_path.len],
                                            entry_name,
                                        };
                                        const entry_start: u32 = @intCast(if (dir.dir_path.len == 0) 0 else dir.dir_path.len + 1);
                                        const subdir_entry_name = try this.walker.join(subdir_parts);

                                        try this.walker.workbuf.append(
                                            this.walker.arena.allocator(),
                                            WorkItem.newSymlink(subdir_entry_name, active, entry_start),
                                        );
                                        continue;
                                    }

                                    if (this.walker.only_files) continue;

                                    if (this.walker.evalFile(active, entry_name)) {
                                        const prepared_path = try this.walker.prepareMatchedPath(entry_name, dir.dir_path) orelse continue;
                                        return .{ .result = prepared_path };
                                    }
                                    continue;
                                },
                                .unknown => {
                                    if (!this.walker.evalImpl(active, entry_name)) continue;

                                    const stackbuf_size = 256;
                                    var stfb = std.heap.stackFallback(stackbuf_size, this.walker.arena.allocator());
                                    const name_z = bun.handleOom(stfb.get().dupeZ(u8, entry_name));
                                    const stat_result = Accessor.lstatat(dir.fd, name_z);
                                    const real_kind = switch (stat_result) {
                                        .result => |st| bun.sys.kindFromMode(@intCast(st.mode)),
                                        .err => continue,
                                    };

                                    switch (real_kind) {
                                        .file => {
                                            if (this.walker.evalFile(active, entry_name)) {
                                                const prepared = try this.walker.prepareMatchedPath(entry_name, dir.dir_path) orelse continue;
                                                return .{ .result = prepared };
                                            }
                                        },
                                        .directory => {
                                            var add_dir: bool = false;
                                            const child = this.walker.evalDir(active, entry_name, &add_dir);
                                            if (child.count() != 0) {
                                                const subdir_parts: []const []const u8 = &[_][]const u8{
                                                    dir.dir_path[0..dir.dir_path.len],
                                                    entry_name,
                                                };
                                                const subdir_entry_name = try this.walker.join(subdir_parts);
                                                try this.walker.workbuf.append(
                                                    this.walker.arena.allocator(),
                                                    WorkItem.new(subdir_entry_name, child, .directory),
                                                );
                                            }
                                            if (add_dir and !this.walker.only_files) {
                                                const prepared_path = try this.walker.prepareMatchedPath(entry_name, dir.dir_path) orelse continue;
                                                return .{ .result = prepared_path };
                                            }
                                        },
                                        .sym_link => {
                                            if (this.walker.follow_symlinks) {
                                                const subdir_parts: []const []const u8 = &[_][]const u8{
                                                    dir.dir_path[0..dir.dir_path.len],
                                                    entry_name,
                                                };
                                                const entry_start: u32 = @intCast(if (dir.dir_path.len == 0) 0 else dir.dir_path.len + 1);
                                                const subdir_entry_name = try this.walker.join(subdir_parts);
                                                try this.walker.workbuf.append(
                                                    this.walker.arena.allocator(),
                                                    WorkItem.newSymlink(subdir_entry_name, active, entry_start),
                                                );
                                            } else if (!this.walker.only_files) {
                                                if (this.walker.evalFile(active, entry_name)) {
                                                    const prepared_path = try this.walker.prepareMatchedPath(entry_name, dir.dir_path) orelse continue;
                                                    return .{ .result = prepared_path };
                                                }
                                            }
                                        },
                                        else => {},
                                    }
                                    continue;
                                },
                                else => continue,
                            }
                        },
                    }

                }
            }
        };

        const WorkItem = struct {
            path: []const u8,
            active: ComponentSet,
            kind: Kind,
            entry_start: u32 = 0,
            fd: ?Accessor.Handle = null,

            const Kind = enum {
                directory,
                symlink,
            };

            fn new(path: []const u8, active: ComponentSet, kind: Kind) WorkItem {
                return .{ .path = path, .active = active, .kind = kind };
            }

            fn newWithFd(path: []const u8, active: ComponentSet, kind: Kind, fd: Accessor.Handle) WorkItem {
                return .{ .path = path, .active = active, .kind = kind, .fd = fd };
            }

            fn newSymlink(path: []const u8, active: ComponentSet, entry_start: u32) WorkItem {
                return .{ .path = path, .active = active, .kind = .symlink, .entry_start = entry_start };
            }
        };

        const Component = struct {
            start: u32,
            len: u32,

            syntax_hint: SyntaxHint = .None,
            trailing_sep: bool = false,
            is_ascii: bool = false,

            unicode_set: bool = false,

            pub fn patternSlice(this: *const Component, pattern: []const u8) []const u8 {
                return pattern[this.start .. this.start + this.len - @as(u1, @bitCast(this.trailing_sep))];
            }

            const SyntaxHint = enum {
                None,
                Single,
                Double,
                WildcardFilepath,
                Literal,
                Dot,
                DotBack,

                fn isSpecialSyntax(this: SyntaxHint) bool {
                    return switch (this) {
                        .Literal => false,
                        else => true,
                    };
                }
            };
        };

        pub fn init(
            this: *GlobWalker,
            arena: *Arena,
            pattern: []const u8,
            dot: bool,
            absolute: bool,
            follow_symlinks: bool,
            error_on_broken_symlinks: bool,
            only_files: bool,
        ) !Maybe(void) {
            return try this.initWithCwd(
                arena,
                pattern,
                bun.fs.FileSystem.instance.top_level_dir,
                dot,
                absolute,
                follow_symlinks,
                error_on_broken_symlinks,
                only_files,
            );
        }

        pub fn debugPatternComponents(this: *GlobWalker) void {
            const pattern = this.pattern;
            const components = &this.patternComponents;
            const ptr = @intFromPtr(this);
            log("GlobWalker(0x{x}) components:", .{ptr});
            for (components.items) |cmp| {
                switch (cmp.syntax_hint) {
                    .Single => log("  *", .{}),
                    .Double => log("  **", .{}),
                    .Dot => log("  .", .{}),
                    .DotBack => log("  ../", .{}),
                    .Literal, .WildcardFilepath, .None => log("  hint={s} component_str={s}", .{ @tagName(cmp.syntax_hint), cmp.patternSlice(pattern) }),
                }
            }
        }

        pub fn initWithCwd(
            this: *GlobWalker,
            arena: *Arena,
            pattern: []const u8,
            cwd: []const u8,
            dot: bool,
            absolute: bool,
            follow_symlinks: bool,
            error_on_broken_symlinks: bool,
            only_files: bool,
        ) !Maybe(void) {
            log("initWithCwd(cwd={s})", .{cwd});
            this.* = .{
                .cwd = cwd,
                .pattern = pattern,
                .dot = dot,
                .absolute = absolute,
                .follow_symlinks = follow_symlinks,
                .error_on_broken_symlinks = error_on_broken_symlinks,
                .only_files = only_files,
                .basename_excluding_special_syntax_component_idx = 0,
                .end_byte_of_basename_excluding_special_syntax = 0,
            };

            try GlobWalker.buildPatternComponents(
                arena,
                &this.patternComponents,
                pattern,
                &this.has_relative_components,
                &this.end_byte_of_basename_excluding_special_syntax,
                &this.basename_excluding_special_syntax_component_idx,
            );

            this.arena = arena.*;

            if (bun.Environment.allow_assert) {
                this.debugPatternComponents();
            }

            return .success;
        }

        pub fn deinit(this: *GlobWalker, comptime clear_arena: bool) void {
            log("GlobWalker.deinit", .{});
            if (comptime clear_arena) {
                this.arena.deinit();
            }
        }

        pub fn handleSysErrWithPath(
            this: *GlobWalker,
            err: Syscall.Error,
            path_buf: [:0]const u8,
        ) Syscall.Error {
            const copy_len = @min(path_buf.len, this.pathBuf.len);
            bun.copy(u8, this.pathBuf[0..copy_len], path_buf[0..copy_len]);
            return err.withPath(this.pathBuf[0..copy_len]);
        }

        pub fn walk(this: *GlobWalker) !Maybe(void) {
            if (this.patternComponents.items.len == 0) return .success;

            var iter = GlobWalker.Iterator{ .walker = this };
            defer iter.deinit();
            switch (try iter.init()) {
                .err => |err| return .{ .err = err },
                else => {},
            }

            while (switch (try iter.next()) {
                .err => |err| return .{ .err = err },
                .result => |matched_path| matched_path,
            }) |path| {
                log("walker: matched path: {s}", .{path});
            }

            return .success;
        }

        fn collapseDots(
            this: *GlobWalker,
            idx: u32,
            dir_path: *[:0]u8,
            path_buf: *bun.PathBuffer,
            encountered_dot_dot: *bool,
        ) Maybe(u32) {
            var component_idx = idx;
            var len = dir_path.len;
            while (component_idx < this.patternComponents.items.len) {
                switch (this.patternComponents.items[component_idx].syntax_hint) {
                    .Dot => {
                        defer component_idx += 1;
                        if (len + 2 >= bun.MAX_PATH_BYTES) {
                            return .{ .err = this.handleSysErrWithPath(Syscall.Error.fromCode(.NAMETOOLONG, .open), path_buf[0..len :0]) };
                        }
                        if (len == 0) {
                            path_buf[len] = '.';
                            path_buf[len + 1] = 0;
                            len += 1;
                        } else {
                            path_buf[len] = '/';
                            path_buf[len + 1] = '.';
                            path_buf[len + 2] = 0;
                            len += 2;
                        }
                    },
                    .DotBack => {
                        defer component_idx += 1;
                        encountered_dot_dot.* = true;
                        if (len + 3 >= bun.MAX_PATH_BYTES) {
                            return .{ .err = this.handleSysErrWithPath(Syscall.Error.fromCode(.NAMETOOLONG, .open), path_buf[0..len :0]) };
                        }
                        if (len == 0) {
                            path_buf[len] = '.';
                            path_buf[len + 1] = '.';
                            path_buf[len + 2] = 0;
                            len += 2;
                        } else {
                            path_buf[len] = '/';
                            path_buf[len + 1] = '.';
                            path_buf[len + 2] = '.';
                            path_buf[len + 3] = 0;
                            len += 3;
                        }
                    },
                    else => break,
                }
            }

            dir_path.len = len;

            return .{ .result = component_idx };
        }

        fn collapseSuccessiveDoubleWildcards(this: *GlobWalker, idx: u32) u32 {
            var component_idx = idx;
            const pattern = this.patternComponents.items[idx];
            _ = pattern;
            while (component_idx + 1 < this.patternComponents.items.len and
                this.patternComponents.items[component_idx + 1].syntax_hint == .Double) : (component_idx += 1)
            {}
            return component_idx;
        }

        pub fn skipSpecialComponents(
            this: *GlobWalker,
            work_item_idx: u32,
            dir_path: *[:0]u8,
            scratch_path_buf: *bun.PathBuffer,
            encountered_dot_dot: *bool,
        ) Maybe(u32) {
            var component_idx = work_item_idx;

            if (component_idx < this.patternComponents.items.len) {
                component_idx = switch (this.patternComponents.items[component_idx].syntax_hint) {
                    .Dot, .DotBack => switch (this.collapseDots(
                        component_idx,
                        dir_path,
                        scratch_path_buf,
                        encountered_dot_dot,
                    )) {
                        .err => |e| return .{ .err = e },
                        .result => |i| i,
                    },
                    else => component_idx,
                };
            }

            if (component_idx < this.patternComponents.items.len) {
                component_idx = switch (this.patternComponents.items[component_idx].syntax_hint) {
                    .Double => this.collapseSuccessiveDoubleWildcards(component_idx),
                    else => component_idx,
                };
            }

            return .{ .result = component_idx };
        }

        fn matchPatternDir(
            this: *GlobWalker,
            pattern: *Component,
            next_pattern: ?*Component,
            entry_name: []const u8,
            component_idx: u32,
            is_last: bool,
            add: *bool,
        ) ?u32 {
            if (!this.dot and GlobWalker.startsWithDot(entry_name)) return null;
            if (is_ignored(entry_name)) return null;

            if (pattern.syntax_hint == .Double) {
                if (!is_last and this.matchPatternImpl(next_pattern.?, entry_name)) {
                    if (component_idx + 1 == this.patternComponents.items.len - 1) {
                        add.* = true;
                        return 0;
                    }

                    return 2;
                }

                if (is_last) {
                    add.* = true;
                }

                return 0;
            }

            const matches = this.matchPatternImpl(pattern, entry_name);
            if (matches) {
                if (is_last) {
                    add.* = true;
                    return null;
                }
                return 1;
            }

            return null;
        }

        fn matchPatternFile(
            this: *GlobWalker,
            entry_name: []const u8,
            component_idx: u32,
            is_last: bool,
            pattern: *Component,
            next_pattern: ?*Component,
        ) bool {
            if (pattern.trailing_sep) return false;

            if (!is_last) return pattern.syntax_hint == .Double and
                component_idx + 1 == this.patternComponents.items.len -| 1 and
                next_pattern.?.syntax_hint != .Double and
                this.matchPatternImpl(next_pattern.?, entry_name);

            return this.matchPatternImpl(pattern, entry_name);
        }

        fn matchPatternImpl(
            this: *GlobWalker,
            pattern_component: *Component,
            filepath: []const u8,
        ) bool {
            log("matchPatternImpl: {s}", .{filepath});
            if (!this.dot and GlobWalker.startsWithDot(filepath)) return false;
            if (is_ignored(filepath)) return false;

            return switch (pattern_component.syntax_hint) {
                .Double, .Single => true,
                .WildcardFilepath => matchWildcardFilepath(pattern_component.patternSlice(this.pattern), filepath),
                .Literal => matchWildcardLiteral(pattern_component.patternSlice(this.pattern), filepath),
                else => this.matchPatternSlow(pattern_component, filepath),
            };
        }

        fn matchPatternSlow(this: *GlobWalker, pattern_component: *Component, filepath: []const u8) bool {
            return bun.glob.match(
                pattern_component.patternSlice(this.pattern),
                filepath,
            ).matches();
        }

        fn makeSet(this: *GlobWalker) ComponentSet {
            return bun.handleOom(ComponentSet.initEmpty(
                this.arena.allocator(),
                this.patternComponents.items.len,
            ));
        }

        fn singleSet(this: *GlobWalker, idx: u32) ComponentSet {
            var s = this.makeSet();
            s.set(idx);
            return s;
        }

        fn evalDir(this: *GlobWalker, active: ComponentSet, entry_name: []const u8, add: *bool) ComponentSet {
            var child = this.makeSet();
            const comps = this.patternComponents.items;
            const len: u32 = @intCast(comps.len);
            var it = active.iterator(.{});
            while (it.next()) |i| {
                const idx: u32 = @intCast(i);
                const pattern = &comps[idx];
                const next_pattern = if (idx + 1 < len) &comps[idx + 1] else null;
                const is_last = idx == len - 1;
                var add_this = false;
                if (this.matchPatternDir(pattern, next_pattern, entry_name, idx, is_last, &add_this)) |bump| {
                    child.set(this.normalizeIdx(idx + bump));
                    if (bump == 2 and comps[idx + 2].syntax_hint != .Double) {
                        child.set(idx);
                    }
                }
                if (add_this) add.* = true;
            }
            return child;
        }

        fn evalFile(this: *GlobWalker, active: ComponentSet, entry_name: []const u8) bool {
            const comps = this.patternComponents.items;
            const len: u32 = @intCast(comps.len);
            var it = active.iterator(.{});
            while (it.next()) |i| {
                const idx: u32 = @intCast(i);
                const pattern = &comps[idx];
                const next_pattern = if (idx + 1 < len) &comps[idx + 1] else null;
                const is_last = idx == len - 1;
                if (this.matchPatternFile(entry_name, idx, is_last, pattern, next_pattern)) return true;
            }
            return false;
        }

        fn evalImpl(this: *GlobWalker, active: ComponentSet, entry_name: []const u8) bool {
            var it = active.iterator(.{});
            while (it.next()) |idx| {
                if (this.matchPatternImpl(&this.patternComponents.items[idx], entry_name)) return true;
            }
            return false;
        }

        inline fn normalizeIdx(this: *const GlobWalker, idx: u32) u32 {
            if (idx < this.patternComponents.items.len and
                this.patternComponents.items[idx].syntax_hint == .Double)
            {
                return @constCast(this).collapseSuccessiveDoubleWildcards(idx);
            }
            return idx;
        }

        inline fn matchedPathToBunString(matched_path: MatchedPath) BunString {
            if (comptime sentinel) {
                return BunString.fromBytes(matched_path[0 .. matched_path.len + 1]);
            }
            return BunString.fromBytes(matched_path);
        }

        fn prepareMatchedPathSymlink(this: *GlobWalker, symlink_full_path: []const u8) !?MatchedPath {
            const result = try this.matchedPaths.getOrPut(this.arena.allocator(), BunString.fromBytes(symlink_full_path));
            if (result.found_existing) {
                log("(dupe) prepared match: {s}", .{symlink_full_path});
                return null;
            }
            if (comptime !sentinel) {
                const slice = try this.arena.allocator().dupe(u8, symlink_full_path);
                result.key_ptr.* = matchedPathToBunString(slice);
                return slice;
            }
            const slicez = try this.arena.allocator().dupeZ(u8, symlink_full_path);
            result.key_ptr.* = matchedPathToBunString(slicez);
            return slicez;
        }

        fn prepareMatchedPath(this: *GlobWalker, entry_name: []const u8, dir_name: []const u8) !?MatchedPath {
            const subdir_parts: []const []const u8 = &[_][]const u8{
                dir_name[0..dir_name.len],
                entry_name,
            };
            const name_matched_path = try this.join(subdir_parts);
            const name = matchedPathToBunString(name_matched_path);
            const result = try this.matchedPaths.getOrPutValue(this.arena.allocator(), name, {});
            if (result.found_existing) {
                log("(dupe) prepared match: {s}", .{name_matched_path});
                return null;
            }
            result.key_ptr.* = name;
            log("prepared match: {s}", .{name_matched_path});
            return name_matched_path;
        }

        fn appendMatchedPath(
            this: *GlobWalker,
            entry_name: []const u8,
            dir_name: [:0]const u8,
        ) !void {
            const subdir_parts: []const []const u8 = &[_][]const u8{
                dir_name[0..dir_name.len],
                entry_name,
            };
            const name_matched_path = try this.join(subdir_parts);
            const name = matchedPathToBunString(name_matched_path);
            const result = try this.matchedPaths.getOrPut(this.arena.allocator(), name);
            if (result.found_existing) {
                log("(dupe) prepared match: {s}", .{name_matched_path});
                return;
            }
            result.key_ptr.* = name;
        }

        fn appendMatchedPathSymlink(this: *GlobWalker, symlink_full_path: []const u8) !void {
            const name = try this.arena.allocator().dupe(u8, symlink_full_path);
            try this.matchedPaths.put(this.arena.allocator(), BunString.fromBytes(name), {});
        }

        inline fn join(this: *GlobWalker, subdir_parts: []const []const u8) !MatchedPath {
            if (!this.absolute) {
                return try stdJoin(this.arena.allocator(), subdir_parts);
            }

            const out = try this.arena.allocator().dupe(u8, bunJoin(subdir_parts, .auto));
            if (comptime sentinel) return out[0 .. out.len - 1 :0];

            return out;
        }

        inline fn startsWithDot(filepath: []const u8) bool {
            return filepath.len > 0 and filepath[0] == '.';
        }

        const syntax_tokens = "*[{?!";

        fn checkSpecialSyntax(pattern: []const u8) bool {
            return bun.strings.indexOfAny(pattern, syntax_tokens) != null;
        }

        fn makeComponent(
            pattern: []const u8,
            start_byte: u32,
            end_byte: u32,
            has_relative_patterns: *bool,
        ) ?Component {
            var component: Component = .{
                .start = start_byte,
                .len = end_byte - start_byte,
            };
            if (component.len == 0) return null;

            out: {
                if (bun.strings.eqlComptime(pattern[component.start .. component.start + component.len], ".")) {
                    component.syntax_hint = .Dot;
                    has_relative_patterns.* = true;
                    break :out;
                }
                if (bun.strings.eqlComptime(pattern[component.start .. component.start + component.len], "..")) {
                    component.syntax_hint = .DotBack;
                    has_relative_patterns.* = true;
                    break :out;
                }

                if (!GlobWalker.checkSpecialSyntax(pattern[component.start .. component.start + component.len])) {
                    component.syntax_hint = .Literal;
                    break :out;
                }

                switch (component.len) {
                    1 => {
                        if (pattern[component.start] == '*') {
                            component.syntax_hint = .Single;
                        }
                        break :out;
                    },
                    2 => {
                        if (pattern[component.start] == '*' and pattern[component.start + 1] == '*') {
                            component.syntax_hint = .Double;
                            break :out;
                        }
                    },
                    else => {},
                }

                out_of_check_wildcard_filepath: {
                    if (component.len > 1 and
                        pattern[component.start] == '*' and
                        pattern[component.start + 1] == '.' and
                        component.start + 2 < pattern.len)
                    {
                        for (pattern[component.start + 2 ..]) |c| {
                            switch (c) {
                                '[', '{', '?', '*' => break :out_of_check_wildcard_filepath,
                                else => {},
                            }
                        }
                        component.syntax_hint = .WildcardFilepath;
                        break :out;
                    }
                }
            }

            if (component.syntax_hint != .Single and component.syntax_hint != .Double) {
                if (isAllAscii(pattern[component.start .. component.start + component.len])) {
                    component.is_ascii = true;
                }
            } else {
                component.is_ascii = true;
            }

            if (pattern[component.start + component.len -| 1] == '/') {
                component.trailing_sep = true;
            } else if (comptime bun.Environment.isWindows) {
                component.trailing_sep = pattern[component.start + component.len -| 1] == '\\';
            }

            return component;
        }

        pub fn buildPattern(
            arena: *Arena,
            patternComponents: *ArrayList(Component),
            pattern: []const u8,
            has_relative_patterns: *bool,
            end_byte_of_basename_excluding_special_syntax: ?*u32,
            basename_excluding_special_syntax_component_idx: ?*u32,
        ) !void {
            const scratchpad: [3]u32 = .{0} ** 3;
            return buildPatternComponents(
                arena,
                patternComponents,
                pattern,
                has_relative_patterns,
                end_byte_of_basename_excluding_special_syntax orelse scratchpad[1],
                basename_excluding_special_syntax_component_idx orelse scratchpad[2],
            );
        }

        fn buildPatternComponents(
            arena: *Arena,
            patternComponents: *ArrayList(Component),
            pattern: []const u8,
            has_relative_patterns: *bool,
            end_byte_of_basename_excluding_special_syntax: *u32,
            basename_excluding_special_syntax_component_idx: *u32,
        ) !void {
            var start_byte: u32 = 0;

            var prevIsBackslash = false;
            var saw_special = false;
            var i: u32 = 0;
            var width: u32 = 0;
            while (i < pattern.len) : (i += 1) {
                const c = pattern[i];
                width = bun.strings.utf8ByteSequenceLength(c);

                switch (c) {
                    '\\' => {
                        if (comptime isWindows) {
                            var end_byte = i;
                            if (i + width == pattern.len) {
                                end_byte += width;
                            }
                            if (makeComponent(
                                pattern,
                                start_byte,
                                end_byte,
                                has_relative_patterns,
                            )) |component| {
                                saw_special = saw_special or component.syntax_hint.isSpecialSyntax();
                                if (!saw_special) {
                                    basename_excluding_special_syntax_component_idx.* = @intCast(patternComponents.items.len);
                                    end_byte_of_basename_excluding_special_syntax.* = i + width;
                                }
                                try patternComponents.append(arena.allocator(), component);
                            }
                            start_byte = i + width;
                            continue;
                        }

                        if (prevIsBackslash) {
                            prevIsBackslash = false;
                            continue;
                        }

                        prevIsBackslash = true;
                    },
                    '/' => {
                        var end_byte = i;
                        if (i + width == pattern.len) {
                            end_byte += width;
                        }
                        if (makeComponent(
                            pattern,
                            start_byte,
                            end_byte,
                            has_relative_patterns,
                        )) |component| {
                            saw_special = saw_special or component.syntax_hint.isSpecialSyntax();
                            if (!saw_special) {
                                basename_excluding_special_syntax_component_idx.* = @intCast(patternComponents.items.len);
                                end_byte_of_basename_excluding_special_syntax.* = i + width;
                            }
                            try patternComponents.append(arena.allocator(), component);
                        }
                        start_byte = i + width;
                    },
                    else => {},
                }
            }
            bun.assert(i == 0 or i == pattern.len);
            i -|= 1;

            if (makeComponent(
                pattern,
                start_byte,
                @intCast(pattern.len),
                has_relative_patterns,
            )) |component| {
                saw_special = saw_special or component.syntax_hint.isSpecialSyntax();
                if (!saw_special) {
                    basename_excluding_special_syntax_component_idx.* = @intCast(patternComponents.items.len);
                    end_byte_of_basename_excluding_special_syntax.* = i + width;
                }
                try patternComponents.append(arena.allocator(), component);
            } else if (!saw_special) {
                basename_excluding_special_syntax_component_idx.* = @intCast(patternComponents.items.len);
                end_byte_of_basename_excluding_special_syntax.* = i + width;
            }
        }
    };
}

pub inline fn isSeparator(c: Codepoint) bool {
    if (comptime @import("builtin").os.tag == .windows) return c == '/' or c == '\\';
    return c == '/';
}

inline fn unescape(c: *u32, glob: []const u32, glob_index: *u32) bool {
    if (c.* == '\\') {
        glob_index.* += 1;
        if (glob_index.* >= glob.len)
            return false;

        c.* = switch (glob[glob_index.*]) {
            'a' => 0x61,
            'b' => 0x08,
            'n' => '\n',
            'r' => '\r',
            't' => '\t',
            else => |cc| cc,
        };
    }

    return true;
}

const GLOB_STAR_MATCH_STR: []const u32 = &[_]u32{ '/', '*', '*' };

inline fn skipGlobstars(glob: []const u32, glob_index: *u32) void {
    glob_index.* += 2;

    while (glob_index.* + 3 <= glob.len and
        std.mem.eql(u32, glob[glob_index.*..][0..3], GLOB_STAR_MATCH_STR))
    {
        glob_index.* += 3;
    }

    glob_index.* -= 2;
}

pub fn matchWildcardFilepath(glob: []const u8, path: []const u8) bool {
    const needle = glob[1..];
    const needle_len: u32 = @intCast(needle.len);
    if (path.len < needle_len) return false;
    return std.mem.eql(u8, needle, path[path.len - needle_len ..]);
}

pub fn matchWildcardLiteral(literal: []const u8, path: []const u8) bool {
    return std.mem.eql(u8, literal, path);
}

const DirIterator = @import("../runtime/node/dir_iterator.zig");
const ResolvePath = @import("../paths/resolve_path.zig");

const bun = @import("bun");
const BunString = bun.String;
const CodepointIterator = bun.strings.UnsignedCodepointIterator;
const isAllAscii = bun.strings.isAllASCII;

const jsc = bun.jsc;
const ZigString = bun.jsc.ZigString;

const Cursor = CodepointIterator.Cursor;
const Codepoint = CodepointIterator.Cursor.CodePointType;

const Syscall = bun.sys;
const Maybe = bun.sys.Maybe;

const std = @import("std");
const ArrayList = std.ArrayListUnmanaged;
const mem = std.mem;
const Arena = std.heap.ArenaAllocator;
}
