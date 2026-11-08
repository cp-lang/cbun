import bun from "bun";
import std from "std";

const Subprocess = bun.jsc.Subprocess;

export @root struct MaxBuf {
    // null after subprocess finalize
    owned_by_subprocess: ?*Subprocess = null,
    // null after pipereader finalize
    owned_by_reader: bool = false,
    // if this goes negative, onMaxBuffer is called on the subprocess
    remaining_bytes: i64 = 0,

    pub static fn createForSubprocess(owner: *Subprocess, ptr: *?*MaxBuf, initial: ?i64): void {
        if (initial == null) {
            ptr.* = null;
            return;
        }
        let maxbuf = bun.handleOom(bun.default_allocator.create(MaxBuf));
        maxbuf.* = MaxBuf {
            owned_by_subprocess: owner,
            owned_by_reader: false,
            remaining_bytes: initial.?,
        };
        ptr.* = maxbuf;
    }

    fn disowned(*this): bool {
        return this.owned_by_subprocess == null && !this.owned_by_reader;
    }

    fn destroy(*this): void {
        bun.assert(this.disowned());
        bun.default_allocator.destroy(this);
    }

    pub static fn removeFromSubprocess(ptr: *?*MaxBuf): void {
        if (ptr.* == null) return;
        let this = ptr.*.?;
        bun.assert(this.owned_by_subprocess != null);
        this.owned_by_subprocess = null;
        ptr.* = null;
        if (this.disowned()) {
            this.destroy();
        }
    }

    pub static fn addToPipereader(value: ?*MaxBuf, ptr: *?*MaxBuf): void {
        if (*val := value) {
            bun.assert(ptr.* == null);
            bun.assert(!val.owned_by_reader);
            val.owned_by_reader = true;
            ptr.* = value;
        }
    }

    pub static fn removeFromPipereader(ptr: *?*MaxBuf): void {
        if (ptr.* == null) return;
        let this = ptr.*.?;
        bun.assert(this.owned_by_reader);
        this.owned_by_reader = false;
        ptr.* = null;
        if (this.disowned()) {
            this.destroy();
        }
    }

    pub static fn transferToPipereader(prev: *?*MaxBuf, next: *?*MaxBuf): void {
        if (prev.* == null) return;
        next.* = prev.*;
        prev.* = null;
    }

    pub fn onReadBytes(*this, bytes: u64): void {
        this.remaining_bytes = std.math.sub(i64, this.remaining_bytes, std.math.cast(i64, bytes) ?? 0) catch -1;
        if (this.remaining_bytes < 0 && this.owned_by_subprocess != null) {
            let owned_by = this.owned_by_subprocess.?;
            if (owned_by.stderr_maxbuf == this) {
                MaxBuf.removeFromSubprocess(&owned_by.stderr_maxbuf);
                owned_by.onMaxBuffer(Kind.stderr);
            } else if (owned_by.stdout_maxbuf == this) {
                MaxBuf.removeFromSubprocess(&owned_by.stdout_maxbuf);
                owned_by.onMaxBuffer(Kind.stdout);
            } else {
                bun.assert(false);
            }
        }
    }

    pub const Kind = enum {
        stdout,
        stderr,
    };
}
