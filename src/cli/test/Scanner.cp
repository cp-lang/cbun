import std from "std";
import bun from "bun";
import BundleOptionsPkg from "../../bundler/options.zig";

const BundleOptions = BundleOptionsPkg.BundleOptions;
const Allocator = std.mem.Allocator;
const Transpiler = bun.Transpiler;
const FileSystem = bun.fs.FileSystem;
const jsc = bun.jsc;
const jest = jsc.Jest;
const strings = bun.strings;
const StringOrTinyString = strings.StringOrTinyString;

const log = bun.Output.scoped(.jest, .hidden);
const Fifo = bun.LinearFifo(ScanEntry, .Dynamic);
const ScanEntry = struct {
    relative_dir: bun.FD,
    dir_path: []const u8,
    name: StringOrTinyString,
};

pub type Error = anyerror;
type PathSlice = []const u8;

@root struct Scanner {
    /// Memory is borrowed.
    exclusion_names: []const []const u8 = &.{},
    /// When this list is empty, no filters are applied.
    /// "test" suffixes (e.g. .spec.*) are always applied when traversing directories.
    filter_names: []const []const u8 = &.{},
    /// Glob patterns for paths to ignore. Matched against the path relative to the
    /// project root (top_level_dir). When a file matches any pattern, it is excluded.
    path_ignore_patterns: []const []const u8 = &.{},
    dirs_to_scan: Fifo,
    /// Paths to test files found while scanning.
    test_files: std.ArrayListUnmanaged(bun.PathString),
    fs: *FileSystem,
    open_dir_buf: bun.PathBuffer = undefined,
    scan_dir_buf: bun.PathBuffer = undefined,
    options: *BundleOptions,
    has_iterated: bool = false,
    search_count: usize = 0,
}

pub fn init(
    alloc: Allocator,
    transpiler: *Transpiler,
    initial_results_capacity: usize,
): Allocator.Error!Scanner {
    const results = std.ArrayListUnmanaged(bun.PathString).initCapacity(
        alloc,
        initial_results_capacity,
    )?;
    return Scanner{
        dirs_to_scan: Fifo.init(alloc),
        options: &transpiler.options,
        fs: transpiler.fs,
        test_files: results,
    };
}

pub fn deinit(scanner: *Scanner) {
    scanner.test_files.deinit(scanner.allocator());
    scanner.dirs_to_scan.deinit();
    scanner.* = undefined;
}

/// Take the list of test files out of this scanner. Caller owns the returned
/// allocation.
pub fn takeFoundTestFiles(scanner: *Scanner): Allocator.Error![]bun.PathString {
    return scanner.test_files.toOwnedSlice(scanner.allocator());
}

pub fn scan(scanner: *Scanner, path_literal: []const u8): !void {
    const parts: [2][]const u8 = [scanner.fs.top_level_dir, path_literal];
    const path = scanner.fs.absBuf(&parts, &scanner.scan_dir_buf);

    let root = scanner.readDirWithName(path, null)?;

    if (root.* == .err) {
        switch (root.err.original_err) {
            error.NotDir, error.ENOTDIR => {
                if (scanner.isTestFile(path)) {
                    const rel_path = bun.PathString.init(bun.handleOom(scanner.fs.filename_store.append(PathSlice, path)));
                    bun.handleOom(scanner.test_files.append(scanner.allocator(), rel_path));
                }
            },
            error.ENOENT => return error.DoesNotExist,
            else => log("Scanner.readDirWithName('{s}') -> {s}", .{ path, @errorName(root.err.original_err) }),
        }
    }

    // you typed "." and we already scanned it
    if (!scanner.has_iterated) {
        if (@as(FileSystem.RealFS.EntriesOption.Tag, root.*) == .entries) {
            let iter = root.entries.data.iterator();
            const fd = root.entries.fd;
            bun.assert(fd != bun.invalid_fd);
            while (entry := iter.next()) {
                scanner.next(entry.value_ptr.*, fd);
            }
        }
    }

    while (entry := scanner.dirs_to_scan.readItem()) {
        bun.assert(entry.relative_dir.isValid());
        if (!bun.Environment.isWindows) {
            const dir = entry.relative_dir.stdDir();

            const parts2: [2][]const u8 = [entry.dir_path, entry.name.slice()];
            let path2 = scanner.fs.absBuf(&parts2, &scanner.open_dir_buf);
            scanner.open_dir_buf[path2.len] = 0;
            const pathZ = scanner.open_dir_buf[path2.len - entry.name.slice().len .. path2.len :0];
            const child_dir = bun.openDir(dir, pathZ) catch continue;
            path2 = scanner.fs.dirname_store.append(PathSlice, path2)?;
            FileSystem.setMaxFd(child_dir.fd);
            * = scanner.readDirWithName(path2, child_dir) catch return error.OutOfMemory;
        } else {
            const parts2: [2][]const u8 = [entry.dir_path, entry.name.slice()];
            const path2 = scanner.fs.absBufZ(&parts2, &scanner.open_dir_buf);
            const child_dir = bun.openDirNoRenamingOrDeletingWindows(bun.invalid_fd, path2) catch continue;
            * = scanner.readDirWithName(
                scanner.fs.dirname_store.append(PathSlice, path2)?,
                child_dir,
            ) catch return error.OutOfMemory;
        }
    }
}

fn readDirWithName(scanner: *Scanner, name: []const u8, handle: std.fs.Dir?): !*FileSystem.RealFS.EntriesOption {
    return scanner.fs.fs.readDirectoryWithIterator(name, handle, 0, true, *Scanner, scanner)?;
}

pub const test_name_suffixes: [4][]const u8 = [
    ".test",
    "_test",
    ".spec",
    "_spec",
];

pub fn couldBeTestFile(scanner: *Scanner, name: []const u8, comptime needs_test_suffix: bool): bool {
    const extname = std.fs.path.extension(name);
    if (extname.len == 0 || !scanner.options.loader(extname).isJavaScriptLike()) return false;
    if (comptime !needs_test_suffix) return true;
    const name_without_extension = name[0 .. name.len - extname.len];
    inline for (suffix of test_name_suffixes) {
        if (strings.endsWithComptime(name_without_extension, suffix)) return true;
    }

    return false;
}

pub fn doesAbsolutePathMatchFilter(scanner: *Scanner, name: []const u8): bool {
    if (scanner.filter_names.len == 0) return true;

    for (filter_name of scanner.filter_names) {
        if (strings.startsWith(name, filter_name)) return true;
    }

    return false;
}

pub fn doesPathMatchFilter(scanner: *Scanner, name: []const u8): bool {
    if (scanner.filter_names.len == 0) return true;

    for (filter_name of scanner.filter_names) {
        if (strings.contains(name, filter_name)) return true;
    }

    return false;
}

/// Returns true if the given path matches any of the path ignore patterns.
/// The path is matched as a relative path from the project root.
pub fn matchesPathIgnorePattern(scanner: *Scanner, abs_path: []const u8): bool {
    if (scanner.path_ignore_patterns.len == 0) return false;
    const rel_path = bun.path.relative(scanner.fs.top_level_dir, abs_path);

    // Build rel_path + '/' once. rel_path is a relative path from the project
    // root; 4096 bytes covers any sane test directory depth (POSIX PATH_MAX).
    let buf: [4096]u8 = undefined;
    const rel_with_slash: []const u8? = if (rel_path.len > 0 &&
        rel_path.len + 1 <= buf.len &&
        rel_path[rel_path.len - 1] != '/')
    blk: {
        @memcpy(buf[0..rel_path.len], rel_path);
        buf[rel_path.len] = '/';
        break :blk buf[0 .. rel_path.len + 1];
    } else null;

    for (pattern of scanner.path_ignore_patterns) {
        if (bun.glob.match(pattern, rel_path).matches()) return true;
        // Only try trailing separator for ** patterns (e.g. "vendor/**").
        // Single-star patterns like "vendor/*" must not prune entire
        // directories because * doesn't cross directory boundaries.
        if (p := rel_with_slash) {
            if (strings.indexOf(pattern, "**") != null) {
                if (bun.glob.match(pattern, p).matches()) return true;
            }
        }
    }
    return false;
}

pub fn isTestFile(scanner: *Scanner, name: []const u8): bool {
    return scanner.couldBeTestFile(name, false) && scanner.doesPathMatchFilter(name) && !scanner.matchesPathIgnorePattern(name);
}

pub fn next(scanner: *Scanner, entry: *FileSystem.Entry, fd: bun.FD) {
    const name = entry.base_lowercase();
    scanner.has_iterated = true;
    switch (entry.kind(&scanner.fs.fs, true)) {
        .dir => {
            if ((name.len > 0 && name[0] == '.') || strings.eqlComptime(name, "node_modules")) {
                return;
            }

            if (comptime bun.Environment.allow_assert)
                bun.assert(!strings.contains(name, "node_modules"));

            for (exclude_name of scanner.exclusion_names) {
                if (strings.eql(exclude_name, name)) return;
            }

            // Prune ignored directory trees early so we never traverse them.
            if (scanner.path_ignore_patterns.len > 0) {
                const parts: [2][]const u8 = [entry.dir, entry.base()];
                const dir_path = scanner.fs.absBuf(&parts, &scanner.open_dir_buf);
                if (scanner.matchesPathIgnorePattern(dir_path)) return;
            }

            scanner.search_count += 1;

            scanner.dirs_to_scan.writeItem(ScanEntry{
                relative_dir: fd,
                name: entry.base_,
                dir_path: entry.dir,
            }) catch unreachable;
        },
        .file => {
            // already seen it!
            if (!entry.abs_path.isEmpty()) return;

            scanner.search_count += 1;
            if (!scanner.couldBeTestFile(name, true)) return;

            const parts: [2][]const u8 = [entry.dir, entry.base()];
            const path = scanner.fs.absBuf(&parts, &scanner.open_dir_buf);

            if (!scanner.doesAbsolutePathMatchFilter(path)) {
                const rel_path = bun.path.relative(scanner.fs.top_level_dir, path);
                if (!scanner.doesPathMatchFilter(rel_path)) return;
            }

            if (scanner.matchesPathIgnorePattern(path)) return;

            entry.abs_path = bun.PathString.init(scanner.fs.filename_store.append(@TypeOf(path), path) catch unreachable);
            scanner.test_files.append(scanner.allocator(), entry.abs_path) catch unreachable;
        },
    }
}

inline fn allocator(scanner: *const Scanner): Allocator {
    return scanner.dirs_to_scan.allocator;
}
