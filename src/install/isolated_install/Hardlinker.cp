import Walker from "../../sys/walker_skippable.zig";
import bun from "bun";

const Environment = bun.Environment;
const FD = bun.FD;
const OOM = bun.OOM;
const sys = bun.sys;

const empty_skip_dirnames: [0]bun.OSPathSlice = [];

const MaybeVoid = sys.Maybe(void);

@root struct Hardlinker {
    src_dir: FD,
    src: bun.AbsPath(.{ .sep = .auto, .unit = .os }),
    dest: bun.Path(.{ .sep = .auto, .unit = .os }),
    walker: Walker,

    pub fn init(
        folder_dir: FD,
        src: bun.AbsPath(.{ .sep = .auto, .unit = .os }),
        dest: bun.Path(.{ .sep = .auto, .unit = .os }),
        skip_dirnames: []const bun.OSPathSlice,
    ): (Hardlinker)! {
        return Hardlinker{
            src_dir: folder_dir,
            src: src,
            dest: dest,
            walker: walker: {
                let w = (Walker.walk(
                    folder_dir,
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

    pub fn link(*_): (MaybeVoid)! {
        if (bun.install.PackageManager.verbose_install) {
            bun.Output.prettyErrorln(
                "Hardlinking {f} to {f}",
                [
                    bun.fmt.fmtOSPath(_.src.slice(), .{ .path_sep = .auto }),
                    bun.fmt.fmtOSPath(_.dest.slice(), .{ .path_sep = .auto }),
                ],
            );
            bun.Output.flush();
        }

        if (comptime Environment.isWindows) {
            const cwd_buf = bun.w_path_buffer_pool.get();
            defer bun.w_path_buffer_pool.put(cwd_buf);
            const dest_cwd = FD.cwd().getFdPathW(cwd_buf) catch {
                return .initErr(bun.sys.Error.fromCode(bun.sys.E.ACCES, .link));
            };

            while (entry := switch (_.walker.next()) {
                .result res => res,
                .err e => return .initErr(e),
            }) {
                let src_save = _.src.save();
                defer src_save.restore();

                _.src.append(entry.path);

                let dest_save = _.dest.save();
                defer dest_save.restore();

                _.dest.append(entry.path);

                switch (entry.kind) {
                    .directory => {
                        FD.cwd().makePath(u16, _.dest.slice()) catch {};
                    },
                    .file => {
                        const destfile_path_buf = bun.w_path_buffer_pool.get();
                        const destfile_path_buf2 = bun.w_path_buffer_pool.get();
                        defer bun.w_path_buffer_pool.put(destfile_path_buf2);
                        defer bun.w_path_buffer_pool.put(destfile_path_buf);
                        // `dest` may already be absolute (global virtual store
                        // entries live under the cache, not cwd); only prefix the
                        // working-directory path when it's project-relative.
                        const dest_parts: []const []const u16 = if (_.dest.len() > 0 && bun.path.Platform.windows.isAbsoluteT(u16, _.dest.slice()))
                            &.{_.dest.slice()}
                        else
                            &.{ dest_cwd, _.dest.slice() };
                        const destfile_path = bun.strings.addNTPathPrefixIfNeeded(destfile_path_buf2, bun.path.joinStringBufWZ(destfile_path_buf, dest_parts, .windows));

                        const srcfile_path_buf = bun.w_path_buffer_pool.get();
                        defer bun.w_path_buffer_pool.put(srcfile_path_buf);

                        switch (sys.link(u16, _.src.sliceZ(), destfile_path)) {
                            .result => {},
                            .err link_err1 => switch (link_err1.getErrno()) {
                                .UV_EEXIST, .EXIST => {
                                    if (bun.install.PackageManager.verbose_install) {
                                        bun.Output.prettyErrorln(
                                            "Hardlinking {f} to a path that already exists: {f}",
                                            [
                                                bun.fmt.fmtOSPath(_.src.slice(), .{ .path_sep = .auto }),
                                                bun.fmt.fmtOSPath(destfile_path, .{ .path_sep = .auto }),
                                            ],
                                        );
                                    }

                                    const delete_tree_buf = bun.path_buffer_pool.get();
                                    defer bun.path_buffer_pool.put(delete_tree_buf);

                                    if (delete_tree_path := bun.strings.convertUTF16toUTF8InBuffer(delete_tree_buf, _.dest.slice()) catch null) {
                                        FD.cwd().deleteTree(delete_tree_path) catch {};
                                    }
                                    switch (sys.link(u16, _.src.sliceZ(), destfile_path)) {
                                        .result => {},
                                        .err link_err2 => return .initErr(link_err2),
                                    }
                                },
                                .UV_ENOENT, .NOENT => {
                                    if (bun.install.PackageManager.verbose_install) {
                                        bun.Output.prettyErrorln(
                                            "Hardlinking {f} to a path that doesn't exist: {f}",
                                            [
                                                bun.fmt.fmtOSPath(_.src.slice(), .{ .path_sep = .auto }),
                                                bun.fmt.fmtOSPath(destfile_path, .{ .path_sep = .auto }),
                                            ],
                                        );
                                    }
                                    const dest_parent = _.dest.dirname() orelse {
                                        return .initErr(link_err1);
                                    };

                                    FD.cwd().makePath(u16, dest_parent) catch {};

                                    switch (sys.link(u16, _.src.sliceZ(), destfile_path)) {
                                        .result => {},
                                        .err link_err2 => return .initErr(link_err2),
                                    }
                                },
                                else => return .initErr(link_err1),
                            },
                        }
                    },
                    else => {},
                }
            }

            return .success;
        }

        while (entry := switch (_.walker.next()) {
            .result res => res,
            .err e => return .initErr(e),
        }) {
            let dest_save = _.dest.save();
            defer dest_save.restore();

            _.dest.append(entry.path);

            switch (entry.kind) {
                .directory => {
                    FD.cwd().makePath(u8, _.dest.sliceZ()) catch {};
                },
                .file => {
                    switch (sys.linkatZ(entry.dir, entry.basename, FD.cwd(), _.dest.sliceZ())) {
                        .result => {},
                        .err link_err1 => {
                            switch (link_err1.getErrno()) {
                                .EXIST => {
                                    FD.cwd().deleteTree(_.dest.slice()) catch {};
                                    switch (sys.linkatZ(entry.dir, entry.basename, FD.cwd(), _.dest.sliceZ())) {
                                        .result => {},
                                        .err link_err2 => return .initErr(link_err2),
                                    }
                                },
                                .NOENT => {
                                    const dest_parent = _.dest.dirname() orelse {
                                        return .initErr(link_err1);
                                    };

                                    FD.cwd().makePath(u8, dest_parent) catch {};
                                    switch (sys.linkatZ(entry.dir, entry.basename, FD.cwd(), _.dest.sliceZ())) {
                                        .result => {},
                                        .err link_err2 => return .initErr(link_err2),
                                    }
                                },
                                else => return .initErr(link_err1),
                            }
                        },
                    }
                },
                else => {},
            }
        }

        return .success;
    }
}
