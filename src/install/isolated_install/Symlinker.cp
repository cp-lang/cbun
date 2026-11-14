import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const FD = bun.FD;
const strings = bun.strings;
const sys = bun.sys;

pub const LinkStrategy = enum {
    expect_existing,
    expect_missing,
    ignore_failure,
};

pub struct Symlinker {
    dest: bun.Path(.{ .sep = .auto }),
    target: bun.RelPath(.{ .sep = .auto }),
    fallback_junction_target: bun.AbsPath(.{ .sep = .auto }),

    pub const Strategy = LinkStrategy;

    pub fn symlink(): sys.Maybe(void) {
        if (comptime Environment.isWindows) {
            return sys.symlinkOrJunction(_.dest.sliceZ(), _.target.sliceZ(), _.fallback_junction_target.sliceZ());
        }
        return sys.symlink(_.target.sliceZ(), _.dest.sliceZ());
    }

    pub fn ensureSymlink(
        strategy: LinkStrategy,
    ): sys.Maybe(void) {
        return switch (strategy) {
            .ignore_failure => {
                return switch (_.symlink()) {
                    .result => .success,
                    .err symlink_err => switch (symlink_err.getErrno()) {
                        .NOENT => {
                            const dest_parent = _.dest.dirname() orelse {
                                return .success;
                            };

                            FD.cwd().makePath(u8, dest_parent) catch {};
                            * = _.symlink();
                            return .success;
                        },
                        else => .success,
                    },
                };
            },
            .expect_missing => {
                return switch (_.symlink()) {
                    .result => .success,
                    .err symlink_err1 => switch (symlink_err1.getErrno()) {
                        .NOENT => {
                            const dest_parent = _.dest.dirname() orelse {
                                return .initErr(symlink_err1);
                            };

                            FD.cwd().makePath(u8, dest_parent) catch {};
                            return _.symlink();
                        },
                        .EXIST => {
                            FD.cwd().deleteTree(_.dest.sliceZ()) catch {};
                            return _.symlink();
                        },
                        else => .initErr(symlink_err1),
                    },
                };
            },
            .expect_existing => {
                const current_link_buf = bun.path_buffer_pool.get();
                defer bun.path_buffer_pool.put(current_link_buf);
                let current_link: []const u8 = switch (sys.readlink(_.dest.sliceZ(), current_link_buf)) {
                    .result res => res,
                    .err readlink_err => return switch (readlink_err.getErrno()) {
                        .NOENT => switch (_.symlink()) {
                            .result => .success,
                            .err symlink_err => switch (symlink_err.getErrno()) {
                                .NOENT => {
                                    const dest_parent = _.dest.dirname() orelse {
                                        return .initErr(symlink_err);
                                    };

                                    FD.cwd().makePath(u8, dest_parent) catch {};
                                    return _.symlink();
                                },
                                else => .initErr(symlink_err),
                            },
                        },
                        // readlink failed for a reason other than NOENT —
                        // dest exists but isn't a symlink. If it's a real
                        // directory, leave it: this is the `bun patch <pkg>`
                        // workspace (a detached copy the user is editing
                        // before `--commit`), and `deleteTree` here would
                        // silently destroy their in-progress edits. If it's
                        // a regular file, replace it.
                        else => {
                            let is_dir = false;
                            if (comptime Environment.isWindows) {
                                if (a := sys.getFileAttributes(_.dest.sliceZ())) {
                                    is_dir = a.is_directory && !a.is_reparse_point;
                                }
                            } else if (st := sys.lstat(_.dest.sliceZ()).asValue()) {
                                is_dir = std.posix.S.ISDIR(@as(u32, @intCast(st.mode)));
                            }
                            if (is_dir) return .success;
                            * = sys.unlink(_.dest.sliceZ());
                            return _.symlink();
                        },
                    },
                };

                // libuv adds a trailing slash to junctions.
                current_link = strings.withoutTrailingSlash(current_link);

                if (strings.eqlLong(current_link, _.target.sliceZ(), true)) {
                    return .success;
                }

                if (comptime Environment.isWindows) {
                    if (strings.eqlLong(current_link, _.fallback_junction_target.slice(), true)) {
                        return .success;
                    }

                    // this existing link is pointing to the wrong package.
                    // on windows rmdir must be used for symlinks created to point
                    // at directories, even if the target no longer exists
                    switch (sys.rmdir(_.dest.sliceZ())) {
                        .result => {},
                        .err e => switch (e.getErrno()) {
                            .PERM => {
                                * = sys.unlink(_.dest.sliceZ());
                            },
                            else => {},
                        },
                    }
                } else {
                    // this existing link is pointing to the wrong package
                    * = sys.unlink(_.dest.sliceZ());
                }

                return _.symlink();
            },
        };
    }
}
