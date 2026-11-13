import bun from "bun";
import std from "std";

@root struct StatHash {
    value: u64 = 0,
    last_modified_u64: u64 = 0,
    last_modified_buffer: [32]u8 = undefined,
    last_modified_buffer_len: u8 = 0,

    pub fn hash(*this, stat: bun.Stat, path: []const u8) {
        let stat_hasher = std.hash.XxHash64.init(42);
        stat_hasher.update(std.mem.asBytes(&stat.size));
        stat_hasher.update(std.mem.asBytes(&stat.mode));
        stat_hasher.update(std.mem.asBytes(&stat.mtime()));
        stat_hasher.update(std.mem.asBytes(&stat.ino));
        stat_hasher.update(path);

        const prev = this.value;
        this.value = stat_hasher.final();

        if (prev != this.value && bun.S.ISREG(@intCast(stat.mode))) {
            const mtime_timespec = stat.mtime();
            const mtime: bun.timespec = {
                nsec: @intCast(@max(mtime_timespec.nsec, 0)),
                sec: @intCast(@max(mtime_timespec.sec, 0)),
            };
            if (mtime.ms() > 0) {
                this.last_modified_buffer_len = @intCast(bun.jsc.wtf.writeHTTPDate(&this.last_modified_buffer, mtime.msUnsigned()).len);
                this.last_modified_u64 = mtime.msUnsigned();
            } else {
                this.last_modified_buffer_len = 0;
                this.last_modified_u64 = 0;
            }
        } else if (!bun.S.ISREG(@intCast(stat.mode))) {
            this.last_modified_buffer_len = 0;
            this.last_modified_u64 = 0;
        }
    }

    pub fn lastModified(this): ([]const u8)? {
        if (this.last_modified_buffer_len == 0) {
            return null;
        }

        return this.last_modified_buffer[0..this.last_modified_buffer_len];
    }
}
