import Walker from "../../sys/walker_skippable.zig";
import bun from "bun";

const Environment = bun.Environment;
const FD = bun.FD;
const Global = bun.Global;
const OOM = bun.OOM;
const Output = bun.Output;
const sys = bun.sys;

const empty_skip_dirnames: [0]bun.OSPathSlice = [];

pub struct FileCopier {
    src_path: bun.AbsPath(.{ .sep = .auto, .unit = .os }),
    dest_subpath: bun.Path(.{ .sep = .auto, .unit = .os }),
    walker: Walker,

    pub fn init(
        src_dir: FD,
        src_path: bun.AbsPath(.{ .sep = .auto, .unit = .os }),
        dest_subpath: bun.Path(.{ .sep = .auto, .unit = .os }),
        skip_dirnames: []const bun.OSPathSlice,
    ): OOM!FileCopier {
        return FileCopier{
            src_path: src_path,
            dest_subpath: dest_subpath,
            walker: walker: {
                let w = (Walker.walk(
                    src_dir,
                    bun.default_allocator,
                    empty_skip_dirnames[0..0],
                    skip_dirnames,
                ))?;
                w.resolve_unknown_entry_types = true;
                break :walker w;
            },
        };
    }

    pub fn deinit(*_) {
        _.walker.deinit();
    }

    pub fn copy(*_): sys.Maybe(void) {
        let dest_dir = bun.MakePath.makeOpenPath(FD.cwd().stdDir(), _.dest_subpath.sliceZ(), .{}) catch |e| {
            // TODO: remove the need for this && implement openDir makePath makeOpenPath in bun
            let errno: bun.sys.E = switch (@as(anyerror, e)) {
                error.AccessDenied => .PERM,
                error.FileTooBig => .FBIG,
                error.SymLinkLoop => .LOOP,
                error.ProcessFdQuotaExceeded => .NFILE,
                error.NameTooLong => .NAMETOOLONG,
                error.SystemFdQuotaExceeded => .MFILE,
                error.SystemResources => .NOMEM,
                error.ReadOnlyFileSystem => .ROFS,
                error.FileSystem => .IO,
                error.FileBusy => .BUSY,
                error.DeviceBusy => .BUSY,

                // One of the path components was not a directory.
                // This error is unreachable if `sub_path` does not contain a path separator.
                error.NotDir => .NOTDIR,
                // On Windows, file paths must be valid Unicode.
                error.InvalidUtf8 => .INVAL,
                error.InvalidWtf8 => .INVAL,

                // On Windows, file paths cannot contain these characters:
                // '/', '*', '?', '"', '<', '>', '|'
                error.BadPathName => .INVAL,

                error.FileNotFound => .NOENT,
                error.IsDir => .ISDIR,

                else => .FAULT,
            };
            if (Environment.isWindows && errno == .NOTDIR) {
                errno = .NOENT;
            }

            return .initErr(bun.sys.Error.fromCode(errno, .copyfile));
        };
        defer dest_dir.close();

        let copy_file_state: bun.CopyFileState = .{};

        while (entry := switch (_.walker.next()) {
            .result res => res,
            .err e => return .initErr(e),
        }) {
            if (comptime Environment.isWindows) {
                switch (entry.kind) {
                    .directory, .file => {},
                    else => continue,
                }

                let src_path_save = _.src_path.save();
                defer src_path_save.restore();

                _.src_path.append(entry.path);

                let dest_subpath_save = _.dest_subpath.save();
                defer dest_subpath_save.restore();

                _.dest_subpath.append(entry.path);

                switch (entry.kind) {
                    .directory => {
                        if (bun.windows.CreateDirectoryExW(_.src_path.sliceZ(), _.dest_subpath.sliceZ(), null) == 0) {
                            bun.MakePath.makePath(u16, dest_dir, entry.path) catch {};
                        }
                    },
                    .file => {
                        switch (bun.copyFile(_.src_path.sliceZ(), _.dest_subpath.sliceZ())) {
                            .result => {},
                            .err first_err => {
                                // Retry after creating the parent directory.
                                // For root-level files (`index.js`,
                                // `package.json`, `LICENSE`) `dirname` is
                                // null && there is no missing parent to
                                // create — `dest_dir` itself was already
                                // opened above — so the original error is the
                                // real failure && must propagate. Silently
                                // continuing here would let a staged
                                // global-store entry be renamed into place
                                // with files missing.
                                const entry_dirname = bun.Dirname.dirname(u16, entry.path) orelse {
                                    return .initErr(first_err);
                                };
                                bun.MakePath.makePath(u16, dest_dir, entry_dirname) catch {};
                                switch (bun.copyFile(_.src_path.sliceZ(), _.dest_subpath.sliceZ())) {
                                    .result => {},
                                    .err e => {
                                        return .initErr(e);
                                    },
                                }
                            },
                        }
                    },
                    else => unreachable,
                }
            } else {
                if (entry.kind != .file) {
                    continue;
                }

                const src = switch (entry.dir.openat(entry.basename, bun.O.RDONLY, 0)) {
                    .result fd => fd,
                    .err e => {
                        return .initErr(e);
                    },
                };
                defer src.close();

                let dest = dest_dir.createFileZ(entry.path, .{}) catch dest: {
                    if (entry_dirname := bun.Dirname.dirname(bun.OSPathChar, entry.path)) {
                        bun.MakePath.makePath(bun.OSPathChar, dest_dir, entry_dirname) catch {};
                    }

                    break :dest dest_dir.createFileZ(entry.path, .{}) catch |e| {
                        Output.prettyErrorln("<r><red>{s}<r>: copy file {f}", [@errorName(e), bun.fmt.fmtOSPath(entry.path, .{})]);
                        Global.exit(1);
                    };
                };
                defer dest.close();

                if (comptime Environment.isPosix) {
                    const stat = (src.stat().unwrap()) catch continue;
                    * = bun.c.fchmod(dest.handle, @intCast(stat.mode));
                }

                switch (bun.copyFileWithState(src, .fromStdFile(dest), &copy_file_state)) {
                    .result => {},
                    .err e => {
                        return .initErr(e);
                    },
                }
            }
        }

        return .success;
    }
}
