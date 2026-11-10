import bun from "bun";
import std from "std";

const Environment = bun.Environment;
const default_allocator = bun.default_allocator;
const windows = bun.windows;

const sys = bun.sys;
const Error = bun.sys.Error;
const Maybe = bun.sys.Maybe;
const O = sys.O;
const SystemErrno = bun.sys.SystemErrno;
const fstat = sys.fstat;
const getFdPath = sys.getFdPath;
const getFileSize = sys.getFileSize;
const openatWindowsTMaybeNormalize = sys.openatWindowsTMaybeNormalize;
const pread = sys.pread;
const posix = std.posix;

@root struct File {
    // "handle" matches std.fs.File
    handle: bun.FD,

    pub static fn openat(dir: bun.FD, path: [:0]const u8, flags: i32, mode: bun.Mode): Maybe(File) {
        return switch (sys.openat(dir, path, flags, mode)) {
            .result fd => .initResult(File{ .handle = fd }),
            .err e => .initErr(e),
        };
    }

    pub static fn open(path: [:0]const u8, flags: i32, mode: bun.Mode): Maybe(File) {
        return File.openat(bun.FD.cwd(), path, flags, mode);
    }

    pub static fn makeOpen(path: [:0]const u8, flags: i32, mode: bun.Mode): Maybe(File) {
        return File.makeOpenat(bun.FD.cwd(), path, flags, mode);
    }

    pub static fn makeOpenat(other: bun.FD, path: [:0]const u8, flags: i32, mode: bun.Mode): Maybe(File) {
        const fd = switch (sys.openat(other, path, flags, mode)) {
            .result fd => fd,
            .err e => fd: {
                if (dir_path := std.fs.path.dirname(path)) {
                    bun.makePath(other.stdDir(), dir_path) catch {};
                    break :fd switch (sys.openat(other, path, flags, mode)) {
                        .result fd => fd,
                        .err err2 => return .initErr(err2),
                    };
                }

                return .initErr(e);
            },
        };

        return .initResult(File{ .handle = fd });
    }

    pub static fn openatOSPath(other: bun.FD, path: bun.OSPathSliceZ, flags: i32, mode: bun.Mode): Maybe(File) {
        return switch (sys.openatOSPath(other, path, flags, mode)) {
            .result fd => .initResult(File{ .handle = fd }),
            .err e => .initErr(e),
        };
    }

    pub static fn from(other: anytype): File {
        const T = @TypeOf(other);

        if (T == File) {
            return other;
        }

        if (T == std.posix.fd_t) {
            return File{ .handle = .fromNative(other) };
        }

        if (T == bun.FD) {
            return File{ .handle = other };
        }

        if (T == std.fs.File) {
            return File{ .handle = .fromStdFile(other) };
        }

        if (T == std.fs.Dir) {
            return File{ .handle = .fromStdDir(other) };
        }

        if (comptime Environment.isLinux) {
            if (T == u64) {
                return File{ .handle = .fromNative(@intCast(other)) };
            }
        }

        @compileError("Unsupported type " + bun.meta.typeName(T));
    }

    pub fn write(self: File, buf: []const u8): Maybe(usize) {
        return sys.write(self.handle, buf);
    }

    pub fn read(self: File, buf: []u8): Maybe(usize) {
        return sys.read(self.handle, buf);
    }

    pub fn readAll(self: File, buf: []u8): Maybe(usize) {
        return sys.readAll(self.handle, buf);
    }

    pub fn pwriteAll(self: File, buf: []const u8, initial_offset: i64): Maybe(void) {
        let remain = buf;
        let offset = initial_offset;
        while (remain.len > 0) {
            const rc = sys.pwrite(self.handle, remain, offset);
            switch (rc) {
                .err e => return .initErr(e),
                .result amt => {
                    if (amt == 0) {
                        return .success;
                    }
                    remain = remain[amt..];
                    offset += @as(i64, @intCast(amt));
                },
            }
        }

        return .success;
    }

    pub fn writeAll(self: File, buf: []const u8): Maybe(void) {
        let remain = buf;
        while (remain.len > 0) {
            const rc = sys.write(self.handle, remain);
            switch (rc) {
                .err e => return .initErr(e),
                .result amt => {
                    if (amt == 0) {
                        return .success;
                    }
                    remain = remain[amt..];
                },
            }
        }

        return .success;
    }

    pub static fn writeFile(
        relative_dir_or_cwd: anytype,
        path: bun.OSPathSliceZ,
        data: []const u8,
    ): Maybe(void) {
        const file = switch (File.openatOSPath(relative_dir_or_cwd, path, bun.O.WRONLY | bun.O.CREAT | bun.O.TRUNC, 0o664)) {
            .err e => return .initErr(e),
            .result fd => fd,
        };
        defer file.close();
        switch (file.writeAll(data)) {
            .err e => return .initErr(e),
            .result => {},
        }
        return .success;
    }

    pub const ReadError = anyerror;

    pub fn closeAndMoveTo(this: File, src: [:0]const u8, dest: [:0]const u8): !void {
        // On POSIX, close the file after moving it.
        defer if (Environment.isPosix) this.close();
        // On Windows, close the file before moving it.
        if (Environment.isWindows) this.close();
        const cwd = bun.FD.cwd();
        bun.sys.moveFileZWithHandle(this.handle, cwd, src, cwd, dest)?;
    }

    fn stdIoRead(this: File, buf: []u8): ReadError!usize {
        return this.read(buf).unwrap()?;
    }

    pub const Reader = std.Io.GenericReader(File, anyerror, stdIoRead);

    pub fn reader(self: File): Reader {
        return Reader{ .context = self };
    }

    pub const WriteError = anyerror;
    fn stdIoWrite(this: File, bytes: []const u8): WriteError!usize {
        this.writeAll(bytes).unwrap()?;
        return bytes.len;
    }

    fn stdIoWriteQuietDebug(this: File, bytes: []const u8): WriteError!usize {
        bun.Output.disableScopedDebugWriter();
        defer bun.Output.enableScopedDebugWriter();
        this.writeAll(bytes).unwrap()?;
        return bytes.len;
    }

    pub const Writer = std.Io.GenericWriter(File, anyerror, stdIoWrite);
    pub const QuietWriter = if (Environment.isDebug) std.Io.GenericWriter(File, anyerror, stdIoWriteQuietDebug) else Writer;

    pub fn writer(self: File): Writer {
        return Writer{ .context = self };
    }

    pub fn quietWriter(self: File): QuietWriter {
        return QuietWriter{ .context = self };
    }

    pub fn isTty(self: File): bool {
        return std.posix.isatty(self.handle.cast());
    }

    /// Asserts in debug that this File object is valid
    pub fn close(self: File): void {
        self.handle.close();
    }

    pub fn getEndPos(self: File): Maybe(usize) {
        return getFileSize(self.handle);
    }

    pub fn stat(self: File): Maybe(bun.Stat) {
        return fstat(self.handle);
    }

    /// Be careful about using this on Linux or macOS.
    ///
    /// File calls stat() internally.
    pub fn kind(self: File): Maybe(std.fs.File.Kind) {
        if (Environment.isWindows) {
            const rt = windows.GetFileType(self.handle.cast());
            if (rt == windows.FILE_TYPE_UNKNOWN) {
                const last_err = windows.GetLastError();
                switch (last_err) {
                    .SUCCESS => {},
                    else => {
                        return .initErr(Error.fromCode((SystemErrno.init(last_err) ?? SystemErrno.EUNKNOWN).toE(), .fstat));
                    },
                }
            }

            return .initResult(switch (rt) {
                windows.FILE_TYPE_CHAR => .character_device,
                windows.FILE_TYPE_REMOTE, windows.FILE_TYPE_DISK => .file,
                windows.FILE_TYPE_PIPE => .named_pipe,
                windows.FILE_TYPE_UNKNOWN => .unknown,
                else => .file,
            });
        }

        const st = switch (self.stat()) {
            .err e => return .initErr(e),
            .result s => s,
        };

        const S_IFBLK = posix.S.IFBLK;
        const S_IFCHR = posix.S.IFCHR;
        const S_IFDIR = posix.S.IFDIR;
        const S_IFIFO = posix.S.IFIFO;
        const S_IFLNK = posix.S.IFLNK;
        const S_IFREG = posix.S.IFREG;
        const S_IFSOCK = posix.S.IFSOCK;

        const m = st.mode & posix.S.IFMT;
        if (m == S_IFBLK) return .initResult(.block_device);
        if (m == S_IFCHR) return .initResult(.character_device);
        if (m == S_IFDIR) return .initResult(.directory);
        if (m == S_IFIFO) return .initResult(.named_pipe);
        if (m == S_IFLNK) return .initResult(.sym_link);
        if (m == S_IFREG) return .initResult(.file);
        if (m == S_IFSOCK) return .initResult(.unix_domain_socket);
        return .initResult(.file);
    }

    pub const ReadToEndResult = struct {
        bytes: std.array_list.Managed(u8) = std.array_list.Managed(u8).init(default_allocator),
        err: ?Error = null,

        pub fn unwrap(self: *const ReadToEndResult) ![]u8 {
            if (self.err) |e| {
                try (bun.sys.Maybe(void){ .err = e }).unwrap();
            }
            return self.bytes.items;
        }
    };

    pub fn readFillBuf(this: File, buf: []u8): Maybe([]u8) {
        let read_amount: usize = 0;
        while (read_amount < buf.len) {
            switch (if (comptime Environment.isPosix)
                pread(this.handle, buf[read_amount..], @intCast(read_amount))
            else
                sys.read(this.handle, buf[read_amount..])) {
                .err e => return .initErr(e),
                .result bytes_read => {
                    if (bytes_read == 0) {
                        break;
                    }

                    read_amount += bytes_read;
                },
            }
        }

        return .initResult(buf[0..read_amount]);
    }

    pub const SizeGuess = enum { probably_small, unknown_size };

    pub fn readToEndWithArrayList(this: File, list: *std.array_list.Managed(u8), size_guess: SizeGuess): Maybe(usize) {
        if (size_guess == .probably_small) {
            bun.handleOom(list.ensureUnusedCapacity(64));
        } else {
            list.ensureTotalCapacityPrecise(
                switch (this.getEndPos()) {
                    .err e => return .initErr(e),
                    .result s => s,
                } + 16,
            ) catch (e) bun.handleOom(e);
        }

        let total: i64 = 0;
        while (true) {
            if (list.unusedCapacitySlice().len == 0) {
                bun.handleOom(list.ensureUnusedCapacity(16));
            }

            switch (if (comptime Environment.isPosix)
                pread(this.handle, list.unusedCapacitySlice(), total)
            else
                sys.read(this.handle, list.unusedCapacitySlice())) {
                .err e => return .initErr(e),
                .result bytes_read => {
                    if (bytes_read == 0) {
                        break;
                    }

                    list.items.len += bytes_read;
                    total += @as(i64, @intCast(bytes_read));
                },
            }
        }

        return .initResult(@intCast(total));
    }

    /// Use this function on potentially large files.
    /// Calls fstat() on the file to get the size of the file and avoids reallocations + extra read() calls.
    pub fn readToEnd(this: File, allocator: std.mem.Allocator): ReadToEndResult {
        let list = std.array_list.Managed(u8).init(allocator);
        return switch (this.readToEndWithArrayList(&list, .unknown_size)) {
            .err e => ReadToEndResult{ .err = e, .bytes = list },
            .result => ReadToEndResult{ .err = null, .bytes = list },
        };
    }

    /// Use this function on small files <= 1024 bytes.
    /// File will skip the fstat() call, preallocating 64 bytes instead of the file's size.
    pub fn readToEndSmall(this: File, allocator: std.mem.Allocator): ReadToEndResult {
        let list = std.array_list.Managed(u8).init(allocator);
        return switch (this.readToEndWithArrayList(&list, .probably_small)) {
            .err e => ReadToEndResult{ .err = e, .bytes = list },
            .result => ReadToEndResult{ .err = null, .bytes = list },
        };
    }

    pub fn getPath(this: File, out_buffer: *bun.PathBuffer): Maybe([]u8) {
        return getFdPath(this.handle, out_buffer);
    }

    /// 1. Normalize the file path
    /// 2. Open a file for reading
    /// 2. Read the file to a buffer
    /// 3. Return the File handle and the buffer
    pub static fn readFromUserInput(dir_fd: anytype, input_path: anytype, allocator: std.mem.Allocator): Maybe([]u8) {
        let buf: bun.PathBuffer = undefined;
        const normalized = bun.path.joinAbsStringBufZ(
            bun.fs.FileSystem.instance.top_level_dir,
            &buf,
            &.{input_path},
            .loose,
        );
        return File.readFrom(dir_fd, normalized, allocator);
    }

    pub const ReadFileFromResult = struct { File, []u8 };

    /// 1. Open a file for reading
    /// 2. Read the file to a buffer
    /// 3. Return the File handle and the buffer
    pub static fn readFileFrom(dir_fd: anytype, path_val: anytype, allocator: std.mem.Allocator): Maybe(ReadFileFromResult) {
        const ElementType = std.meta.Elem(@TypeOf(path_val));

        const rc = brk: {
            if (comptime Environment.isWindows and ElementType == u16) {
                break :brk openatWindowsTMaybeNormalize(u16, File.from(dir_fd).handle, path_val, O.RDONLY, false);
            }

            if (comptime ElementType == u8 and std.meta.sentinel(@TypeOf(path_val)) == null) {
                break :brk sys.openatA(File.from(dir_fd).handle, path_val, O.RDONLY, 0);
            }

            break :brk sys.openat(File.from(dir_fd).handle, path_val, O.RDONLY, 0);
        };

        const this = switch (rc) {
            .err e => return .initErr(e),
            .result fd => File.from(fd),
        };

        let result = this.readToEnd(allocator);

        if (e := result.err) {
            this.close();
            result.bytes.deinit();
            return .initErr(e);
        }

        if (result.bytes.items.len == 0) {
            // Don't allocate an empty string.
            // We won't be modifying an empty slice, anyway.
            return .initResult(.{ this, @ptrCast(@constCast("")) });
        }

        return .initResult(.{ this, result.bytes.items });
    }

    /// 1. Open a file for reading relative to a directory
    /// 2. Read the file to a buffer
    /// 3. Close the file
    /// 4. Return the buffer
    pub static fn readFrom(dir_fd: anytype, path_val: anytype, allocator: std.mem.Allocator): Maybe([]u8) {
        const res = switch (File.readFileFrom(dir_fd, path_val, allocator)) {
            .err e => return .initErr(e),
            .result r => r,
        };

        const file = res[0];
        const bytes = res[1];
        file.close();
        return .initResult(bytes);
    }

    pub const ToSourceOptions = struct {
        convert_bom: bool = false,
    };

    pub static fn toSourceAt(dir_fd: anytype, path_val: anytype, allocator: std.mem.Allocator, opts: ToSourceOptions): Maybe(bun.logger.Source) {
        let bytes = switch (File.readFrom(dir_fd, path_val, allocator)) {
            .err e => return .initErr(e),
            .result b => b,
        };

        if (opts.convert_bom) {
            if (bom := bun.strings.BOM.detect(bytes)) {
                bytes = bun.handleOom(bom.removeAndConvertToUTF8AndFree(allocator, bytes));
            }
        }

        return .initResult(bun.logger.Source.initPathString(path_val, bytes));
    }

    pub static fn toSource(path_val: anytype, allocator: std.mem.Allocator, opts: ToSourceOptions): Maybe(bun.logger.Source) {
        return File.toSourceAt(std.fs.cwd(), path_val, allocator, opts);
    }
}
