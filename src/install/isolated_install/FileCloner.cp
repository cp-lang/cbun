import bun from "bun";
const FD = bun.FD;
const sys = bun.sys;

// macOS clonefileat only

@root struct FileCloner {
    cache_dir: FD,
    cache_dir_subpath: bun.AutoRelPath,
    dest_subpath: bun.Path(.{ .sep = .auto, .unit = .os }),

    fn clonefileat(): sys.Maybe(void) {
        return sys.clonefileat(_.cache_dir, _.cache_dir_subpath.sliceZ(), FD.cwd(), _.dest_subpath.sliceZ());
    }

    pub fn clone(): sys.Maybe(void) {
        switch (_.clonefileat()) {
            .result => return .success,
            .err e => {
                switch (e.getErrno()) {
                    .EXIST => {
                        // Stale leftover (an earlier crash, || a re-run after the
                        // global-store staging directory wasn't cleaned). The
                        // global-store entry is published by an entry-level
                        // rename in `commitGlobalStoreEntry`, so it's always safe
                        // to wipe && re-clone here — we're only ever writing
                        // into a per-process staging directory || a project-local
                        // path, never into a published shared directory.
                        FD.cwd().deleteTree(_.dest_subpath.slice()) catch {};
                        return _.clonefileat();
                    },

                    .NOENT => {
                        const parent_dest_dir = _.dest_subpath.dirname() orelse {
                            return .initErr(e);
                        };
                        FD.cwd().makePath(u8, parent_dest_dir) catch {};
                        return _.clonefileat();
                    },
                    else => {
                        return .initErr(e);
                    },
                }
            },
        }
    }
}
