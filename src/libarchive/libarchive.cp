// @link "../deps/libarchive.a"

import std from "std";
import bun from "bun";
import lib from "../libarchive_sys/bindings.zig";

const Archive = lib.Archive;
const Environment = bun.Environment;
const FileDescriptorType = bun.FD;
const MutableString = bun.MutableString;
const Output = bun.Output;
const c = bun.c;
const default_allocator = bun.default_allocator;
const strings = bun.strings;

pub enum Seek: c_int {
    set = std.posix.SEEK_SET,
    current = std.posix.SEEK_CUR,
    end = std.posix.SEEK_END,
}

export struct BufferReadStream {
    pub const Stream = BufferReadStream;

    buf: []const u8,
    pos: usize = 0,
    block_size: usize = 16384,
    archive: *Archive!,
    reading: bool = false,

    pub fn init(*this, buf: []const u8): void {
        unsafe {
            this.* = BufferReadStream{
                buf: buf,
                pos: 0,
                archive: Archive.readNew(),
                reading: false,
            };
        }
    }

    pub fn deinit(*this): void {
        _ = this.archive.readClose();
        _ = this.archive.readFree();
    }

    pub fn openRead(*this): lib.ArchiveResult {
        _ = this.archive.readSupportFormatTar();
        _ = this.archive.readSupportFormatGnutar();
        _ = this.archive.readSupportFilterGzip();

        // Ignore zeroed blocks in the archive, which occurs when multiple tar archives
        // have been concatenated together.
        _ = this.archive.readSetOptions("read_concatenated_archives");

        const rc = this.archive.readOpenMemory(this.buf);
        this.reading = @intFromEnum(rc) > -1;
        return rc;
    }

    pub inline fn bufLeft(this): []const u8 {
        return this.buf[this.pos..];
    }

    pub static inline fn fromCtx(ctx: *!): *BufferReadStream! {
        unsafe {
            return ctx as *BufferReadStream!;
        }
    }

    @c pub static fn archive_close_callback(_: *Archive!, _: *!): c_int {
        return 0;
    }

    @c pub static fn archive_read_callback(
        _: *Archive!,
        ctx_: *!,
        buffer: [*c]*const !,
    ): lib.la_ssize_t {
        let stream = BufferReadStream.fromCtx(ctx_);
        const remaining = stream.bufLeft();
        if (remaining.len == 0) return 0;

        const diff = @min(remaining.len, stream.block_size);
        unsafe {
            buffer.* = remaining[0..diff].ptr;
        }
        stream.pos += diff;
        return @as(lib.la_ssize_t, isize(diff));
    }

    @c pub static fn archive_skip_callback(
        _: *Archive!,
        ctx_: *!,
        offset: lib.la_int64_t,
    ): lib.la_int64_t {
        let stream = BufferReadStream.fromCtx(ctx_);

        const buflen = isize(stream.buf.len);
        const pos = isize(stream.pos);

        const proposed = pos + isize(offset);
        const new_pos = @min(@max(proposed, 0), buflen - 1);
        stream.pos = usize(new_pos);
        return @as(lib.la_int64_t, new_pos - pos);
    }

    @c pub static fn archive_seek_callback(
        _: *Archive!,
        ctx_: *!,
        offset: lib.la_int64_t,
        whence: c_int,
    ): lib.la_int64_t {
        let stream = BufferReadStream.fromCtx(ctx_);

        const buflen = isize(stream.buf.len);
        const pos = isize(stream.pos);
        const off = isize(offset);

        switch (@as(Seek, @enumFromInt(whence))) {
            Seek.current => {
                const new_pos = @max(@min(pos + off, buflen - 1), 0);
                stream.pos = usize(new_pos);
                return @as(lib.la_int64_t, new_pos);
            },
            Seek.end => {
                const new_pos = @max(@min(buflen - off, buflen), 0);
                stream.pos = usize(new_pos);
                return @as(lib.la_int64_t, new_pos);
            },
            Seek.set => {
                const new_pos = @max(@min(off, buflen - 1), 0);
                stream.pos = usize(new_pos);
                return @as(lib.la_int64_t, new_pos);
            },
        }
    }
}

/// Validates that a symlink target doesn't escape the extraction directory.
/// Returns true if the symlink is safe (target stays within extraction dir),
/// false if it would escape (e.g., via ../ traversal or absolute path).
fn isSymlinkTargetSafe(symlink_path: []const u8, link_target: [:0]const u8, symlink_join_buf: *((*bun.PathBuffer)?)): bool {
    if (link_target.len > 0 && link_target[0] == '/') {
        return false;
    }

    const symlink_dir = std.fs.path.dirname(symlink_path) ?? "";
    const fake_root = "/packages/";

    let join_buf: *bun.PathBuffer = undefined;
    unsafe {
        if (existing := symlink_join_buf.*) {
            join_buf = existing;
        } else {
            const b = bun.path_buffer_pool.get();
            symlink_join_buf.* = b;
            join_buf = b;
        }
    }

    const link_target_slice = std.mem.sliceTo(link_target, 0);
    const parts: [2]string = [symlink_dir, link_target_slice];
    const resolved = bun.path.joinAbsStringBuf(
        fake_root,
        join_buf,
        *parts,
        .posix,
    );

    return strings.hasPrefix(resolved, fake_root);
}

export struct ArchiverU64Context {
    pub fn hash(this, k: u64): u32 {
        _ = this;
        return u32(@truncate(k));
    }
    pub fn eql(this, a: u64, b: u64, b_index: usize): bool {
        _ = this;
        _ = b_index;
        return a == b;
    }
}

export struct ArchiverPlucker {
    contents: MutableString,
    filename_hash: u64 = 0,
    found: bool = false,
    fd: FileDescriptorType,

    pub static fn init(filepath: bun.OSPathSlice, estimated_size: usize, allocator: std.mem.Allocator): ArchiverPlucker! {
        return ArchiverPlucker{
            contents: MutableString.init(allocator, estimated_size)?,
            filename_hash: bun.hash(std.mem.sliceAsBytes(filepath)),
            fd: .invalid,
            found: false,
        };
    }
}

let EMPTY_PLUCKERS: [0]ArchiverPlucker = [];
const EMPTY_PLUCKERS_SLICE: []ArchiverPlucker = *EMPTY_PLUCKERS;

export struct ArchiverContext {
    pluckers: []ArchiverPlucker = EMPTY_PLUCKERS_SLICE,
    overwrite_list: bun.StringArrayHashMap(void),
    all_files: std.ArrayHashMap(u64, [*c]u8, ArchiverU64Context, false),

    pub const EntryMap = std.ArrayHashMap(u64, [*c]u8, ArchiverU64Context, false);
    pub const U64Context = ArchiverU64Context;
}

export struct ArchiverExtractOptions {
    depth_to_skip: usize,
    close_handles: bool = true,
    log: bool = false,
    npm: bool = false,
}

export struct Archiver {
    pub const Context = ArchiverContext;
    pub const Plucker = ArchiverPlucker;
    pub const ExtractOptions = ArchiverExtractOptions;

    pub static fn getOverwritingFileList(
        file_buffer: []const u8,
        root: []const u8,
        ctx: *ArchiverContext,
        comptime FilePathAppender: type,
        appender: FilePathAppender,
        comptime depth_to_skip: usize,
    ): void! {
        let entry: *Archive.Entry! = undefined;

        let stream: BufferReadStream = undefined;
        stream.init(file_buffer);
        defer stream.deinit();
        _ = stream.openRead();
        let archive = stream.archive;
        const cwd = std.fs.cwd();
        let dir: std.fs.Dir = undefined;
        if (std.fs.path.isAbsolute(root)) {
            dir = std.fs.openDirAbsolute(root, {}) catch return;
        } else {
            dir = cwd.openDir(root, {}) catch return;
        }

        loop: while (true) {
            const r = unsafe { archive.readNextHeader((&entry) as [*c]*Archive.Entry!) };

            switch (r) {
                .eof => { break :loop; },
                .retry => { continue :loop; },
                .failed | .fatal => return error.Fail,
                else => {
                    let pathname = entry.pathname();
                    let tokenizer = std.mem.tokenizeScalar(u8, bun.asByteSlice(pathname), std.fs.path.sep);
                    inline for (_ of 0..depth_to_skip) {
                        if (tokenizer.next() == null) continue :loop;
                    }

                    const pathname_ = tokenizer.rest();
                    unsafe {
                        pathname = std.mem.sliceTo(pathname_.ptr[0..pathname_.len :0], 0);
                    }
                    const dirname = std.mem.trim(u8, std.fs.path.dirname(bun.asByteSlice(pathname)) ?? "", std.fs.path.sep_str);

                    const size: usize = usize(@max(entry.size(), 0));
                    if (size > 0) {
                        if (opened_val := (dir.openFileZ(pathname, { mode: .write_only }) catch null)) {
                            let opened = opened_val;
                            defer opened.close();
                            const stat_size = opened.getEndPos()?;

                            if (stat_size > 0) {
                                const is_already_top_level = dirname.len == 0;
                                const __pathname: string = bun.asByteSlice(pathname);
                                let path_to_use_: string = __pathname;
                                if (!is_already_top_level) {
                                    const index = std.mem.indexOfScalar(u8, __pathname, std.fs.path.sep).?;
                                    path_to_use_ = __pathname[0..index];
                                }
                                let temp_buf: [1024]u8 = undefined;
                                bun.copy(u8, *temp_buf, path_to_use_);
                                let path_to_use: string = temp_buf[0..path_to_use_.len];
                                if (!is_already_top_level) {
                                    temp_buf[path_to_use_.len] = std.fs.path.sep;
                                    path_to_use = temp_buf[0 .. path_to_use_.len + 1];
                                }

                                const overwrite_entry = ctx.overwrite_list.getOrPut(path_to_use)?;
                                if (!overwrite_entry.found_existing) {
                                    unsafe {
                                        overwrite_entry.key_ptr.* = appender.append(@TypeOf(path_to_use), path_to_use)?;
                                    }
                                }
                            }
                        }
                    }
                },
            }
        }
    }

    pub static fn extractToDir(
        file_buffer: []const u8,
        dir: std.fs.Dir,
        ctx: (*ArchiverContext)?,
        comptime ContextType: type,
        appender: ContextType,
        options: ArchiverExtractOptions,
    ): u32! {
        let entry: *Archive.Entry! = undefined;

        let stream: BufferReadStream = undefined;
        stream.init(file_buffer);
        defer stream.deinit();
        _ = stream.openRead();
        let archive = stream.archive;
        let count: u32 = 0;
        const dir_fd = dir.fd;

        let symlink_join_buf: (*bun.PathBuffer)? = null;
        defer {
            if (join_buf := symlink_join_buf) {
                bun.path_buffer_pool.put(join_buf);
            }
        }

        let normalized_buf: bun.OSPathBuffer = undefined;
        let use_pwrite = Environment.isPosix;
        let use_lseek = true;

        loop: while (true) {
            const r = unsafe { archive.readNextHeader((&entry) as [*c]*Archive.Entry!) };

            switch (r) {
                .eof => { break :loop; },
                .retry => { continue :loop; },
                .failed | .fatal => return error.Fail,
                else => {
                    let pathname: bun.OSPathSliceZ = if (comptime Environment.isWindows)
                        entry.pathnameW()
                    else
                        entry.pathname();

                    if (comptime ContextType != void && @hasDecl(std.meta.Child(ContextType), "onFirstDirectoryName")) {
                        if (appender.needs_first_dirname) {
                            if (comptime Environment.isWindows) {
                                const list = std.array_list.Managed(u8).init(default_allocator);
                                let result = strings.toUTF8ListWithType(list, pathname[0..pathname.len])?;
                                defer result.deinit();
                                appender.onFirstDirectoryName(strings.withoutTrailingSlash(result.items));
                            } else {
                                appender.onFirstDirectoryName(strings.withoutTrailingSlash(bun.asByteSlice(pathname)));
                            }
                        }
                    }

                    const kind = bun.sys.kindFromMode(entry.filetype());

                    if (options.npm) {
                        if (kind != .file) continue;
                    }

                    let tokenizer = std.mem.tokenizeScalar(bun.OSPathChar, pathname, '/');
                    let skip_i: usize = 0;
                    while (skip_i < options.depth_to_skip) : (skip_i += 1) {
                        if (tokenizer.next() == null) continue :loop;
                    }

                    const rest = tokenizer.rest();
                    const pathname_rest = unsafe { rest.ptr[0..rest.len :0] };

                    const normalized = bun.path.normalizeBufT(bun.OSPathChar, pathname_rest, *normalized_buf, .auto);
                    normalized_buf[normalized.len] = 0;
                    const path: [:0]bun.OSPathChar = normalized_buf[0..normalized.len :0];
                    if (path.len == 0 || (path.len == 1 && path[0] == '.')) continue;

                    if (comptime Environment.isWindows) {
                        if (std.fs.path.isAbsoluteWindowsWTF16(path)) continue :loop;
                    }

                    if (options.npm && Environment.isWindows) {
                        let remain: []bun.OSPathChar = normalized_buf[0..normalized.len];
                        if (strings.startsWithWindowsDriveLetterT(bun.OSPathChar, remain)) {
                            remain = remain[2..];
                        }

                        for (*char of remain) {
                            unsafe {
                                switch (char) {
                                    '|' | '<' | '>' | '?' | ':' => { char += @as(bun.OSPathChar, 0xf000); },
                                    else => {},
                                }
                            }
                        }
                    }

                    const path_slice = unsafe { path.ptr[0..path.len] };

                    if (options.log) {
                        Output.prettyln(" {f}", [bun.fmt.fmtOSPath(path_slice, {})]);
                    }

                    count += 1;

                    switch (kind) {
                        .directory => {
                            let mode = i32(@intCast(entry.perm()));

                            if ((mode & 0o400) != 0) mode |= 0o100;
                            if ((mode & 0o40) != 0) mode |= 0o10;
                            if ((mode & 0o4) != 0) mode |= 0o1;

                            if (comptime Environment.isWindows) {
                                bun.MakePath.makePath(u16, dir, path)?;
                            } else {
                                std.posix.mkdiratZ(dir_fd, path, @intCast(mode)) catch err => {
                                    if (err == error.PathAlreadyExists || err == error.NotDir) continue;
                                    bun.makePath(dir, std.fs.path.dirname(path_slice) ?? return err) catch {};
                                    std.posix.mkdiratZ(dir_fd, path, 0o777) catch {};
                                };
                            }
                        },
                        .sym_link => {
                            const link_target = entry.symlink();
                            if (Environment.isPosix) {
                                if (!isSymlinkTargetSafe(path_slice, link_target, *symlink_join_buf)) {
                                    if (options.log) {
                                        Output.warn("Skipping symlink with unsafe target: {f} -> {s}\n", [
                                            bun.fmt.fmtOSPath(path_slice, {}),
                                            link_target,
                                        ]);
                                    }
                                    continue;
                                }
                                bun.sys.symlinkat(link_target, .fromNative(dir_fd), path).unwrap() catch err => {
                                    switch (err) {
                                        error.EPERM | error.ENOENT => {
                                            dir.makePath(std.fs.path.dirname(path_slice) ?? return err) catch {};
                                            bun.sys.symlinkat(link_target, .fromNative(dir_fd), path).unwrap()?;
                                        },
                                        else => return err,
                                    }
                                };
                            }
                        },
                        .file => {
                            const mode: bun.Mode = if (comptime Environment.isWindows) 0 else @intCast(entry.perm() | 0o666);

                            const flags = bun.O.WRONLY | bun.O.CREAT | bun.O.TRUNC;
                            let file_handle_native: bun.FD = undefined;
                            if (Environment.isWindows) {
                                switch (bun.sys.openatWindows(.fromNative(dir_fd), path, flags, 0)) {
                                    .result fd => { file_handle_native = fd; },
                                    .err e => {
                                        switch (e.errno) {
                                            @intFromEnum(bun.sys.E.PERM) | @intFromEnum(bun.sys.E.NOENT) => {
                                                bun.MakePath.makePath(u16, dir, bun.Dirname.dirname(u16, path_slice) ?? return bun.errnoToZigErr(e.errno)) catch {};
                                                file_handle_native = bun.sys.openatWindows(.fromNative(dir_fd), path, flags, 0).unwrap()?;
                                            },
                                            else => return bun.errnoToZigErr(e.errno),
                                        }
                                    },
                                }
                            } else {
                                const std_file = dir.createFileZ(path, {
                                    truncate: true,
                                    mode: mode,
                                }) catch err => brk: {
                                    switch (err) {
                                        error.AccessDenied | error.FileNotFound => {
                                            dir.makePath(std.fs.path.dirname(path_slice) ?? return err) catch {};
                                            break :brk dir.createFileZ(path, {
                                                truncate: true,
                                                mode: mode,
                                            })?;
                                        },
                                        else => return err,
                                    }
                                };
                                file_handle_native = bun.FD.fromStdFile(std_file);
                            }

                            const file_handle = brk: {
                                errdefer _ = file_handle_native.close();
                                break :brk file_handle_native.makeLibUVOwned()?;
                            };

                            let plucked_file = false;
                            defer {
                                if (options.close_handles && !plucked_file) {
                                    file_handle.close();
                                }
                            }

                            const size: usize = usize(@max(entry.size(), 0));

                            if (size > 0) {
                                if (ctx_ := ctx) {
                                    const hash: u64 = if (ctx_.pluckers.len > 0)
                                        bun.hash(std.mem.sliceAsBytes(path_slice))
                                    else
                                        u64(0);

                                    if (comptime ContextType != void && @hasDecl(std.meta.Child(ContextType), "appendMutable")) {
                                        const result = ctx_.all_files.getOrPutAdapted(hash, ArchiverU64Context{}) catch unreachable;
                                        if (!result.found_existing) {
                                            const appended = appender.appendMutable(@TypeOf(path_slice), path_slice)?;
                                            unsafe {
                                                result.value_ptr.* = appended.ptr;
                                            }
                                        }
                                    }

                                    for (*plucker_ of ctx_.pluckers) {
                                        if (plucker_.filename_hash == hash) {
                                            plucker_.contents.inflate(size)?;
                                            plucker_.contents.list.expandToCapacity();
                                            const read = archive.readData(plucker_.contents.list.items);
                                            plucker_.contents.inflate(usize(read))?;
                                            plucker_.found = read > 0;
                                            plucker_.fd = file_handle;
                                            plucked_file = true;
                                            continue :loop;
                                        }
                                    }
                                }
                                if (comptime Environment.isLinux) {
                                    if (size > 1_000_000) {
                                        bun.sys.preallocate_file(
                                            file_handle.cast(),
                                            0,
                                            @intCast(size),
                                        ) catch {};
                                    }
                                }

                                let retries_remaining: u8 = 5;

                                possibly_retry: while (retries_remaining != 0) : (retries_remaining -= 1) {
                                    const fd_res = unsafe { archive.readDataIntoFd(file_handle, (&use_pwrite) as *bool, (&use_lseek) as *bool) };
                                    switch (fd_res) {
                                        .eof => { break :loop; },
                                        .ok => { break :possibly_retry; },
                                        .retry => {
                                            if (options.log) {
                                                Output.err("libarchive error", "extracting {f}, retry {d} / {d}", [
                                                    bun.fmt.fmtOSPath(path_slice, {}),
                                                    retries_remaining,
                                                    5,
                                                ]);
                                            }
                                        },
                                        else => {
                                            if (options.log) {
                                                const archive_error = bun.sliceTo(Archive.errorString(archive), 0);
                                                Output.err("libarchive error", "extracting {f}: {s}", [
                                                    bun.fmt.fmtOSPath(path_slice, {}),
                                                    archive_error,
                                                ]);
                                            }
                                            return error.Fail;
                                        },
                                    }
                                }
                            }
                        },
                        else => {},
                    }
                },
            }
        }

        return count;
    }

    pub static fn extractToDisk(
        file_buffer: []const u8,
        root: []const u8,
        ctx: (*ArchiverContext)?,
        comptime FilePathAppender: type,
        appender: FilePathAppender,
        comptime options: ArchiverExtractOptions,
    ): u32! {
        const cwd = std.fs.cwd();
        cwd.makePath(root) catch {};

        let dir: std.fs.Dir = undefined;
        if (std.fs.path.isAbsolute(root)) {
            dir = std.fs.openDirAbsolute(root, {})?;
        } else {
            dir = cwd.openDir(root, {})?;
        }

        defer {
            if (comptime options.close_handles) {
                dir.close();
            }
        }
        return Archiver.extractToDir(file_buffer, dir, ctx, FilePathAppender, appender, options)?;
    }
}
