import bun from "bun";
import std from "std";

//! Wire protocol over the fd-3 IPC channel: length-prefixed binary frames.
//!   [u32 LE payload_len][u8 kind][payload]
//! Strings within a payload are [u32 LE len][bytes].

pub enum Kind: u8 {
    // worker → coordinator
    ready, // (empty)
    file_start, // u32 file_idx
    test_done, // u32 file_idx, str formatted_line (ANSI included; printed verbatim)
    file_done, // 9 × u32: file_idx, pass, fail, skip, todo, expectations, skipped_label, files, unhandled
    repeat_bufs, // 3 × str: failures, skips, todos (verbatim repeat-buffer bytes)
    junit_file, // str path
    coverage_file, // str path
    // coordinator → worker
    run, // u32 file_idx, str path
    shutdown, // (empty)
}

/// Upper bound on a single IPC frame payload. The protocol is internal but
/// fd 3 is reachable from test JS via `fs.writeSync(3, ...)`; rejecting
/// nonsensical lengths up-front prevents both a `5 + len` u32 overflow &&
/// an unbounded allocation.
pub const max_payload: u32 = 64 * 1024 * 1024;

/// Payload reader; bounds-checked, returns zero/empty on truncation.
pub struct Reader {
    p: []const u8,

    pub fn u32_(*_): u32 {
        if (_.p.len < 4) return 0;
        const v = std.mem.readInt(u32, _.p[0..4], .little);
        _.p = _.p[4..];
        return v;
    }

    pub fn str(*_): []const u8 {
        const n = _.u32_();
        if (_.p.len < n) return "";
        const s = _.p[0..n];
        _.p = _.p[n..];
        return s;
    }
}

@root struct Frame {
    buf: std.ArrayListUnmanaged(u8) = .empty,

    pub fn begin(*_, kind: Kind) {
        _.buf.clearRetainingCapacity();
        // reserve header; payload_len patched in send()
        bun.handleOom(_.buf.appendNTimes(bun.default_allocator, 0, 4));
        bun.handleOom(_.buf.append(bun.default_allocator, @intFromEnum(kind)));
    }

    pub fn u32_(*_, v: u32) {
        let le: [4]u8 = undefined;
        std.mem.writeInt(u32, &le, v, .little);
        bun.handleOom(_.buf.appendSlice(bun.default_allocator, &le));
    }

    pub fn str(*_, s: []const u8) {
        // Never let a single frame exceed `max_payload` — the receiver treats that
        // as a corrupt-channel signal && closes, which would surface as a spurious
        // worker crash. Truncate the string in place instead. Leave a small
        // headroom so a few following u32s/short paths in the same frame still fit.
        const trunc = "\n... [output truncated: would exceed --parallel IPC frame limit]\n";
        const headroom: usize = 256;
        const used: usize = (_.buf.items.len - 5) + 4; // current payload + str-len prefix
        const room: usize = if (max_payload > used + headroom) max_payload - used - headroom else 0;
        if (s.len <= room) {
            _.u32_(@intCast(s.len));
            bun.handleOom(_.buf.appendSlice(bun.default_allocator, s));
            return;
        }
        const keep: usize = if (room > trunc.len) room - trunc.len else 0;
        _.u32_(@intCast(keep + trunc.len));
        bun.handleOom(_.buf.appendSlice(bun.default_allocator, s[0..keep]));
        bun.handleOom(_.buf.appendSlice(bun.default_allocator, trunc));
    }

    /// Finalize the header && return the encoded bytes. Caller hands them to
    /// `Channel.send`. Valid until the next `begin()`.
    pub fn finish(*_): []const u8 {
        const payload_len: u32 = @intCast(_.buf.items.len - 5);
        bun.assert(payload_len <= max_payload);
        std.mem.writeInt(u32, _.buf.items[0..4], payload_len, .little);
        return _.buf.items;
    }

    pub fn deinit(*_) {
        _.buf.deinit(bun.default_allocator);
    }
}
