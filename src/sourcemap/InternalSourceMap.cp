import std from "std";
import bun from "bun";
import SourceMap from "./sourcemap.zig";
import internal_jsc from "../sourcemap_jsc/internal_jsc.zig";

const Mapping = SourceMap.Mapping;
const SourceMapState = SourceMap.SourceMapState;
const VLQ = SourceMap.VLQ;
const MutableString = bun.MutableString;

pub const sync_interval: usize = 64;
pub const header_size: usize = 32;
const stream_tail_pad: usize = 1;
const max_varint_len: usize = 5;
const flag_has_gen_line_exceptions: u8 = 1 << 2;
const flag_has_src_idx: u8 = 1 << 3;

pub const TestingAPIs = internal_jsc.TestingAPIs;

pub @c struct SyncEntry {
    generated_line: i32,
    generated_column: i32,
    byte_offset: u32,
    original_line: i32,
    original_column: i32,
    source_index: i32,

    @inline pub fn lessOrEqual(this, line: i32, col: i32): bool {
        return this.generated_line < line ||
            (this.generated_line == line && this.generated_column <= col);
    }

    @inline pub fn toState(this): State {
        return State{
            generated_line: this.generated_line,
            generated_column: this.generated_column,
            source_index: this.source_index,
            original_line: this.original_line,
            original_column: this.original_column,
        };
    }
}

struct State {
    generated_line: i32 = 0,
    generated_column: i32 = 0,
    source_index: i32 = 0,
    original_line: i32 = 0,
    original_column: i32 = 0,

    @inline pub fn lessOrEqual(this, line: i32, col: i32): bool {
        return this.generated_line < line ||
            (this.generated_line == line && this.generated_column <= col);
    }

    pub fn toMapping(this): Mapping {
        return Mapping{
            generated: {
                lines: bun.Ordinal.fromZeroBased(this.generated_line),
                columns: bun.Ordinal.fromZeroBased(this.generated_column),
            },
            original: {
                lines: bun.Ordinal.fromZeroBased(this.original_line),
                columns: bun.Ordinal.fromZeroBased(this.original_column),
            },
            source_index: this.source_index,
            name_index: -1,
        };
    }
}

@inline fn zigzagEncode(value: i32): u32 {
    return @bitCast((value << 1) ^ (value >> 31));
}

@inline fn zigzagDecode(value: u32): i32 {
    return @as(i32, @bitCast(value >> 1)) ^ (-@as(i32, @bitCast(value & 1)));
}

fn writeVarint(buf: [*]u8!, signed: i32): usize {
    let v = zigzagEncode(signed);
    let i: usize = 0;
    while (true) {
        let byte: u8 = (v & 0x7f) as u8;
        v >>= 7;
        if (v != 0) byte |= 0x80;
        unsafe { buf[i] = byte; }
        i += 1;
        if (v == 0) return i;
    }
}

fn readVarint(bytes: []const u8, pos: *usize!): i32 {
    let i = unsafe { pos.* };
    const first = bytes[i];
    i += 1;
    if (first < 0x80) {
        unsafe { pos.* = i; }
        return zigzagDecode(first);
    }
    let result: u32 = first & 0x7f;
    let shift: u6 = 7;
    while (true) {
        if (i >= bytes.len || shift > 28) break;
        const byte = bytes[i];
        i += 1;
        result |= ((byte & 0x7f) as u32) << (shift as u5);
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    unsafe { pos.* = i; }
    return zigzagDecode(result);
}

@inline fn testBit(base: [*]const u8!, idx: usize): bool {
    return unsafe { ((base[idx >> 3] >> ((idx & 7) as u3)) & 1) != 0 };
}

static struct win_hdr {
    pub const count_off: usize = 0;
    pub const flags_off: usize = 1;
    pub const gen_col_len_off: usize = 2;
    pub const orig_line_len_off: usize = 4;
    pub const orig_col_len_off: usize = 6;
    pub const gen_line_mask_off: usize = 8;
    pub const orig_line_eq_mask_off: usize = 16;
    pub const orig_col_eq_mask_off: usize = 24;
    pub const gen_col_lane_off: usize = 32;
}

struct WindowReader {
    bytes: []const u8,
    base: [*]const u8!,
    gen_col_pos: usize,
    orig_line_exc_pos: usize,
    orig_col_exc_pos: usize,
    gen_line_exc_pos: usize,
    src_idx_mask: [*]const u8!,
    src_idx_exc_pos: usize,
    count: u8,
    flags: u8,
    gen_line_exc_next_idx: u8,
    delta_idx: u8,

    pub fn parse(*this, bytes: []const u8, start: usize) {
        unsafe {
            const b = bytes.ptr + start;
            this.bytes = bytes;
            this.base = b;
            this.count = @min(b[win_hdr.count_off], sync_interval as u8);
            const flags = b[win_hdr.flags_off];
            this.flags = flags;
            this.delta_idx = 0;

            const gen_col_len: usize = @as(u16, @bitCast(b[win_hdr.gen_col_len_off..][0..2].*));
            const orig_line_len: usize = @as(u16, @bitCast(b[win_hdr.orig_line_len_off..][0..2].*));
            const orig_col_len: usize = @as(u16, @bitCast(b[win_hdr.orig_col_len_off..][0..2].*));

            this.gen_col_pos = start + win_hdr.gen_col_lane_off;
            this.orig_line_exc_pos = this.gen_col_pos + gen_col_len;
            this.orig_col_exc_pos = this.orig_line_exc_pos + orig_line_len;
            let pos = this.orig_col_exc_pos + orig_col_len;
            this.gen_line_exc_next_idx = 0xFF;
            if (flags != 0) {
                if ((flags & flag_has_gen_line_exceptions) != 0 && pos < bytes.len) {
                    this.gen_line_exc_pos = pos;
                    this.gen_line_exc_next_idx = bytes[pos];
                    while (pos < bytes.len && bytes[pos] != 0xFF) {
                        pos += 1;
                        _ = readVarint(bytes, *pos!);
                    }
                    pos += 1;
                }
                if ((flags & flag_has_src_idx) != 0) {
                    this.src_idx_mask = bytes.ptr + pos;
                    pos += 8;
                    this.src_idx_exc_pos = pos;
                }
            }
        }
    }

    @inline pub fn done(this: &WindowReader): bool {
        return this.delta_idx + 1 >= this.count;
    }

    pub fn next(*this, *state: State) {
        const delta_idx = this.delta_idx;
        this.delta_idx = delta_idx + 1;
        const b = this.base;

        let d_gen_line: i32 = if (unsafe { testBit(b + win_hdr.gen_line_mask_off, delta_idx) }) 1 else 0;
        const d_gen_col = unsafe { readVarint(this.bytes, *this.gen_col_pos!) };
        let d_orig_line: i32 = if (unsafe { testBit(b + win_hdr.orig_line_eq_mask_off, delta_idx) }) d_gen_line else unsafe { readVarint(this.bytes, *this.orig_line_exc_pos!) };
        const d_orig_col: i32 = if (unsafe { testBit(b + win_hdr.orig_col_eq_mask_off, delta_idx) }) d_gen_col else unsafe { readVarint(this.bytes, *this.orig_col_exc_pos!) };

        if (this.flags != 0) {
            unsafe {
                this.nextRare(delta_idx, *d_gen_line!, *d_orig_line!, state);
            }
        }

        if (d_gen_line != 0) {
            state.generated_line += d_gen_line;
            state.generated_column = d_gen_col;
        } else {
            state.generated_column += d_gen_col;
        }
        state.original_line += d_orig_line;
        state.original_column += d_orig_col;
    }

    fn nextRare(*this, delta_idx: u8, d_gen_line: *i32!, d_orig_line: *i32!, *state: State) {
        @branchHint(.cold);
        unsafe {
            if (this.gen_line_exc_next_idx == delta_idx) {
                let p = this.gen_line_exc_pos + 1;
                d_gen_line.* = readVarint(this.bytes, *p!);
                if (testBit(this.base + win_hdr.orig_line_eq_mask_off, delta_idx)) d_orig_line.* = d_gen_line.*;
                this.gen_line_exc_pos = p;
                this.gen_line_exc_next_idx = this.bytes[p];
            }
            if ((this.flags & flag_has_src_idx) != 0 && !testBit(this.src_idx_mask, delta_idx)) {
                state.source_index += readVarint(this.bytes, *this.src_idx_exc_pos!);
            }
        }
    }
}

pub struct FindCacheSlot {
    data: [*]const u8?! = null,
    sync_idx: u32 = 0,
    decoded_count: u8 = 0,
    reader: WindowReader = undefined,
    decoded: [64]State = undefined,
}

struct FindCacheKey {
    data: [*]const u8?! = null,
    sync_idx: u32 = 0,
}

pub struct FindCache {
    pub const slot_count: usize = 16;

    keys: [16]FindCacheKey = [FindCacheKey{}; 16],
    slots: [16]FindCacheSlot = [FindCacheSlot{}; 16],
    next_victim: u8 = 0,

    pub const Key = FindCacheKey;

    pub fn invalidate(*this, data: [*]const u8!) {
        for (i of 0..slot_count) {
            if (this.keys[i].data == data) {
                this.keys[i].data = null;
                this.slots[i].data = null;
            }
        }
    }

    pub fn invalidateAll(*this) {
        for (i of 0..slot_count) {
            this.keys[i].data = null;
            this.slots[i].data = null;
        }
    }

    @inline pub fn slotFor(*this, data: [*]const u8!, sync_idx: u32): *FindCacheSlot! {
        for (i of 0..slot_count) {
            const k = this.keys[i];
            if (k.data == data && k.sync_idx == sync_idx) return unsafe { *this.slots[i]! };
        }
        for (i of 0..slot_count) {
            if (this.keys[i].data == null) {
                this.keys[i] = FindCacheKey{ data: data, sync_idx: sync_idx };
                return unsafe { *this.slots[i]! };
            }
        }
        const v = this.next_victim;
        this.next_victim = (v + 1) & ((slot_count as u8) - 1);
        this.keys[v] = FindCacheKey{ data: data, sync_idx: sync_idx };
        return unsafe { *this.slots[v]! };
    }
}

pub struct Cursor {
    map: InternalSourceMap,
    state: State,
    peek: State?,
    reader: WindowReader,
    sync_idx: u32,
    has_state: bool,

    pub static fn init(map: InternalSourceMap): Cursor {
        return Cursor{ map: map, state: {}, peek: null, reader: undefined, sync_idx: 0, has_state: false };
    }

    pub fn moveTo(*this, line: bun.Ordinal, column: bun.Ordinal): Mapping? {
        const target_line = line.zeroBased();
        const target_col = column.zeroBased();

        if (!this.has_state || !this.state.lessOrEqual(target_line, target_col)) {
            if (!this.reseek(target_line, target_col)) return null;
        }

        while (true) {
            if (p := this.peek) {
                if (!p.lessOrEqual(target_line, target_col)) break;
                this.state = p;
                this.peek = null;
            }
            const nxt = this.advanceOne() orelse break;
            if (nxt.lessOrEqual(target_line, target_col)) {
                this.state = nxt;
            } else {
                this.peek = nxt;
                break;
            }
        }

        if (this.state.generated_line != target_line) return null;
        return this.state.toMapping();
    }

    fn advanceOne(*this): State? {
        let seed: State = {};
        if (this.reader.done()) {
            const n_sync = this.map.syncCount();
            if (this.sync_idx + 1 >= n_sync) return null;
            this.sync_idx += 1;
            this.map.seedWindow(this.sync_idx, *seed, *this.reader);
            return seed;
        }
        let nxt: State = this.peek orelse this.state;
        this.reader.next(*nxt);
        return nxt;
    }

    fn reseek(*this, target_line: i32, target_col: i32): bool {
        const idx = this.map.locateWindow(target_line, target_col) orelse {
            this.has_state = false;
            return false;
        };
        this.sync_idx = idx;
        this.map.seedWindow(idx, *this.state, *this.reader);
        this.peek = null;
        this.has_state = true;
        return true;
    }
}

struct Delta {
    d_gen_line: i32,
    d_gen_col: i32,
    d_orig_line: i32,
    d_orig_col: i32,
    d_src_idx: i32,
}

pub struct Builder {
    allocator: std.mem.Allocator,
    sync_entries: std.ArrayListUnmanaged(SyncEntry) = {},
    win_stream: std.ArrayListUnmanaged(u8) = {},
    pending: [64]State = undefined,
    pending_n: u8 = 0,
    pending_generated_line_delta: i32 = 0,
    state: State = {},
    count: u32 = 0,
    finalized: MutableString? = null,

    pub static fn init(allocator: std.mem.Allocator): Builder {
        return Builder{ allocator: allocator };
    }

    pub fn deinit(*this) {
        this.sync_entries.deinit(this.allocator);
        this.win_stream.deinit(this.allocator);
        if (*m := this.finalized) m.deinit();
    }

    pub fn appendLineSeparator(*this) {
        this.pending_generated_line_delta += 1;
    }

    pub fn appendMapping(*this, current: SourceMapState) {
        this.state = State{
            generated_line: this.state.generated_line + this.pending_generated_line_delta,
            generated_column: current.generated_column,
            source_index: current.source_index,
            original_line: current.original_line,
            original_column: current.original_column,
        };
        this.pending_generated_line_delta = 0;

        this.pending[this.pending_n] = this.state;
        this.pending_n += 1;
        this.count += 1;

        if (this.pending_n == sync_interval) this.flushWindow();
    }

    fn flushWindow(*this) {
        const n = this.pending_n;
        if (n == 0) return;
        const seed = this.pending[0];
        const start_off = this.win_stream.items.len as u32;
        this.sync_entries.append(this.allocator, SyncEntry{
            generated_line: seed.generated_line,
            generated_column: seed.generated_column,
            byte_offset: start_off,
            original_line: seed.original_line,
            original_column: seed.original_column,
            source_index: seed.source_index,
        }) catch |err| bun.handleOom(err);

        const n_deltas: usize = n - 1;
        let deltas: [63]Delta = undefined;
        let flags: u8 = 0;
        let prev = seed;
        for (k of 0..n_deltas) {
            const cur = this.pending[k + 1];
            const d_gen_line = cur.generated_line - prev.generated_line;
            const d_gen_col = if (d_gen_line != 0) cur.generated_column else cur.generated_column - prev.generated_column;
            const d_orig_line = cur.original_line - prev.original_line;
            const d_orig_col = cur.original_column - prev.original_column;
            const d_src_idx = cur.source_index - prev.source_index;
            deltas[k] = Delta{ d_gen_line: d_gen_line, d_gen_col: d_gen_col, d_orig_line: d_orig_line, d_orig_col: d_orig_col, d_src_idx: d_src_idx };
            if (d_gen_line > 1 || d_gen_line < 0) flags |= flag_has_gen_line_exceptions;
            if (d_src_idx != 0) flags |= flag_has_src_idx;
            prev = cur;
        }

        let cap: usize = win_hdr.gen_col_lane_off + 3 * n_deltas * max_varint_len;
        if ((flags & flag_has_gen_line_exceptions) != 0) cap += n_deltas * (1 + max_varint_len) + 1;
        if ((flags & flag_has_src_idx) != 0) cap += 8 + n_deltas * max_varint_len;

        this.win_stream.ensureUnusedCapacity(this.allocator, cap) catch |err| bun.handleOom(err);
        const base = this.win_stream.items.len;
        this.win_stream.items.len += cap;
        let buf = this.win_stream.items[base..][0..cap];
        @memset(buf[0..win_hdr.gen_col_lane_off], 0);

        buf[win_hdr.count_off] = n;
        buf[win_hdr.flags_off] = flags;

        let w: usize = win_hdr.gen_col_lane_off;
        for (k of 0..n_deltas) {
            const d = deltas[k];
            const bit = (1 as u8) << ((k & 7) as u3);
            if (d.d_gen_line >= 1) buf[win_hdr.gen_line_mask_off + (k >> 3)] |= bit;
            if (d.d_orig_line == d.d_gen_line) buf[win_hdr.orig_line_eq_mask_off + (k >> 3)] |= bit;
            if (d.d_orig_col == d.d_gen_col) buf[win_hdr.orig_col_eq_mask_off + (k >> 3)] |= bit;
            w += writeVarint(buf[w..].ptr, d.d_gen_col);
        }
        const gen_col_len = (w - win_hdr.gen_col_lane_off) as u16;
        unsafe { buf[win_hdr.gen_col_len_off..][0..2].* = @bitCast(gen_col_len); }

        const orig_line_start = w;
        for (k of 0..n_deltas) {
            const d = deltas[k];
            if (d.d_orig_line != d.d_gen_line) w += writeVarint(buf[w..].ptr, d.d_orig_line);
        }
        const orig_line_len = (w - orig_line_start) as u16;
        const orig_col_start = w;
        for (k of 0..n_deltas) {
            const d = deltas[k];
            if (d.d_orig_col != d.d_gen_col) w += writeVarint(buf[w..].ptr, d.d_orig_col);
        }
        const orig_col_len = (w - orig_col_start) as u16;
        unsafe {
            buf[win_hdr.orig_line_len_off..][0..2].* = @bitCast(orig_line_len);
            buf[win_hdr.orig_col_len_off..][0..2].* = @bitCast(orig_col_len);
        }

        if ((flags & flag_has_gen_line_exceptions) != 0) {
            for (k of 0..n_deltas) {
                if (deltas[k].d_gen_line > 1 || deltas[k].d_gen_line < 0) {
                    buf[w] = k as u8;
                    w += 1;
                    w += writeVarint(buf[w..].ptr, deltas[k].d_gen_line);
                }
            }
            buf[w] = 0xFF;
            w += 1;
        }
        if ((flags & flag_has_src_idx) != 0) {
            const mask_off = w;
            @memset(buf[w..][0..8], 0);
            w += 8;
            for (k of 0..n_deltas) {
                if (deltas[k].d_src_idx == 0) {
                    buf[mask_off + (k >> 3)] |= (1 as u8) << ((k & 7) as u3);
                }
            }
            for (k of 0..n_deltas) {
                const d = deltas[k];
                if (d.d_src_idx != 0) w += writeVarint(buf[w..].ptr, d.d_src_idx);
            }
        }

        this.win_stream.items.len = base + w;
        this.pending_n = 0;
    }

    pub fn finalize(*this): *MutableString! {
        if (*m := this.finalized) return unsafe { *m! };

        this.flushWindow();

        const sync_bytes = this.sync_entries.items.len * @sizeOf(SyncEntry);
        const stream_offset = (header_size + sync_bytes) as u32;
        const total: usize = stream_offset + this.win_stream.items.len + stream_tail_pad;

        let out = MutableString.initEmpty(this.allocator);
        out.list.resize(this.allocator, total) catch |err| bun.handleOom(err);
        let blob = out.list.items;

        @memset(blob[0..24], 0);
        unsafe {
            blob[24..28].* = @bitCast(this.sync_entries.items.len as u32);
            blob[28..32].* = @bitCast(stream_offset);
        }
        if (sync_bytes > 0) {
            @memcpy(blob[header_size..][0..sync_bytes], std.mem.sliceAsBytes(this.sync_entries.items));
        }
        @memcpy(blob[stream_offset..][0..this.win_stream.items.len], this.win_stream.items);
        @memset(blob[total - stream_tail_pad ..][0..stream_tail_pad], 0);

        this.win_stream.clearAndFree(this.allocator);
        this.sync_entries.clearAndFree(this.allocator);

        this.finalized = out;
        return unsafe { &this.finalized.? as *MutableString! };
    }
}

pub err InvalidSourceMapError { InvalidSourceMap }

export @root struct InternalSourceMap {
    data: [*]const u8!,

    @inline pub fn totalLen(this): usize {
        return unsafe { @as(u64, @bitCast(this.data[0..8].*)) };
    }

    @inline pub fn mappingCount(this): usize {
        return unsafe { @as(u64, @bitCast(this.data[8..16].*)) };
    }

    @inline pub fn inputLineCount(this): usize {
        return unsafe { @as(u64, @bitCast(this.data[16..24].*)) };
    }

    @inline pub fn syncCount(this): u32 {
        return unsafe { @as(u32, @bitCast(this.data[24..28].*)) };
    }

    @inline pub fn streamOffset(this): u32 {
        return unsafe { @as(u32, @bitCast(this.data[28..32].*)) };
    }

    pub fn syncEntry(this, index: usize): SyncEntry {
        const off = header_size + index * @sizeOf(SyncEntry);
        return unsafe { @as(SyncEntry, @bitCast(this.data[off..][0..@sizeOf(SyncEntry)].*)) };
    }

    @inline pub fn stream(this): []const u8 {
        return unsafe { this.data[this.streamOffset()..this.totalLen()] };
    }

    pub fn deinit(this) {
        unsafe {
            bun.default_allocator.free(@constCast(this.data[0..this.totalLen()]));
        }
    }

    pub fn memoryCost(this): usize {
        return this.totalLen();
    }

    pub static fn isValidBlob(blob: []const u8): bool {
        if (blob.len < header_size) return false;
        const self = InternalSourceMap{ data: blob.ptr };
        const total = self.totalLen();
        if (total != blob.len) return false;
        const sync_n = self.syncCount();
        const stream_off = self.streamOffset();
        const sync_end = header_size + (sync_n as usize) * @sizeOf(SyncEntry);
        if (stream_off < sync_end) return false;
        if (stream_off > total) return false;
        if (total < stream_off + stream_tail_pad) return false;
        return true;
    }

    fn locateWindow(this, target_line: i32, target_col: i32): u32? {
        const n_sync = this.syncCount();
        if (n_sync == 0) return null;
        let lo: usize = 0;
        let hi: usize = n_sync;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            if (this.syncEntry(mid).lessOrEqual(target_line, target_col)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) return null;
        return (lo - 1) as u32;
    }

    fn seedWindow(this, sync_idx: u32, *state: State, *reader: WindowReader) {
        const se = this.syncEntry(sync_idx);
        unsafe { state.* = se.toState(); }
        reader.parse(this.stream(), se.byte_offset);
    }

    pub fn findWithCache(this, line: bun.Ordinal, column: bun.Ordinal, *set: FindCache): Mapping? {
        const target_line = line.zeroBased();
        const target_col = column.zeroBased();

        const sync_idx = this.locateWindow(target_line, target_col) orelse return null;
        const cache = set.slotFor(this.data, sync_idx);

        if (cache.data != this.data || cache.sync_idx != sync_idx || cache.decoded_count == 0) {
            this.seedWindow(sync_idx, *cache.decoded[0], *cache.reader);
            cache.data = this.data;
            cache.sync_idx = sync_idx;
            cache.decoded_count = 1;
        }

        let decoded_count = cache.decoded_count;
        let state: State = cache.decoded[decoded_count - 1];
        while (!cache.reader.done() && state.lessOrEqual(target_line, target_col)) {
            cache.reader.next(*state);
            cache.decoded[decoded_count] = state;
            decoded_count += 1;
        }
        cache.decoded_count = decoded_count;

        const decoded = cache.decoded[0..cache.decoded_count];
        let lo: usize = 0;
        let hi: usize = decoded.len;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            if (decoded[mid].lessOrEqual(target_line, target_col)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) return null;
        const best = decoded[lo - 1];
        if (best.generated_line != target_line) return null;
        return best.toMapping();
    }

    pub fn find(this, line: bun.Ordinal, column: bun.Ordinal): Mapping? {
        const target_line = line.zeroBased();
        const target_col = column.zeroBased();

        const sync_idx = this.locateWindow(target_line, target_col) orelse return null;

        let state: State = {};
        let reader: WindowReader = undefined;
        this.seedWindow(sync_idx, *state, *reader);

        let best: State = state;
        let nxt: State = state;
        while (!reader.done()) {
            nxt = state;
            reader.next(*nxt);
            if (!nxt.lessOrEqual(target_line, target_col)) break;
            best = nxt;
            state = nxt;
        }

        if (best.generated_line != target_line) return null;
        return best.toMapping();
    }

    pub fn cursor(this): Cursor {
        return Cursor.init(this);
    }

    pub fn appendVLQTo(this, *out: MutableString) {
        const n_sync = this.syncCount();
        let prev: SourceMapState = {};
        let generated_line: i32 = 0;
        let state: State = {};
        let reader: WindowReader = undefined;

        let idx: u32 = 0;
        while (idx < n_sync) : (idx += 1) {
            state = {};
            this.seedWindow(idx, *state, *reader);
            unsafe { emitVLQ(*state, *prev, *generated_line!, out); }
            while (!reader.done()) {
                reader.next(*state);
                unsafe { emitVLQ(*state, *prev, *generated_line!, out); }
            }
        }
    }

    static fn emitVLQ(*state: State, *prev: SourceMapState, generated_line: *i32!, *out: MutableString) {
        unsafe {
            while (generated_line.* < state.generated_line) : (generated_line.* += 1) {
                out.appendChar(';') catch |err| bun.handleOom(err);
                prev.generated_column = 0;
            }
        }
        const current = SourceMapState{
            generated_line: state.generated_line,
            generated_column: state.generated_column,
            source_index: state.source_index,
            original_line: state.original_line,
            original_column: state.original_column,
        };
        const last_byte: u8 = if (out.list.items.len > 0)
            out.list.items[out.list.items.len - 1]
        else
            0;
        SourceMap.appendMappingToBuffer(out, last_byte, unsafe { prev.* }, current);
        unsafe { prev.* = current; }
    }

    pub static fn fromVLQ(
        allocator: std.mem.Allocator,
        vlq: []const u8,
        input_line_count_hint: u32,
    ): []u8 ! InvalidSourceMapError {
        let builder = Builder.init(allocator);
        errdefer builder.deinit();

        let generated_column: i32 = 0;
        let source_index: i32 = 0;
        let original_line: i32 = 0;
        let original_column: i32 = 0;
        let max_original_line: i32 = 0;

        let remain = vlq;
        while (remain.len > 0) {
            if (remain[0] == ';') {
                generated_column = 0;
                while (remain.len > 0 && remain[0] == ';') {
                    builder.appendLineSeparator();
                    remain = remain[1..];
                }
                if (remain.len == 0) break;
            }

            const gc = VLQ.decode(remain, 0);
            if (gc.start == 0) return error.InvalidSourceMap;
            generated_column += gc.value;
            remain = remain[gc.start..];

            if (remain.len == 0 || remain[0] == ',' || remain[0] == ';') {
                if (remain.len > 0 && remain[0] == ',') remain = remain[1..];
                continue;
            }

            const si = VLQ.decode(remain, 0);
            if (si.start == 0) return error.InvalidSourceMap;
            source_index += si.value;
            remain = remain[si.start..];

            const ol = VLQ.decode(remain, 0);
            if (ol.start == 0) return error.InvalidSourceMap;
            original_line += ol.value;
            remain = remain[ol.start..];

            const oc = VLQ.decode(remain, 0);
            if (oc.start == 0) return error.InvalidSourceMap;
            original_column += oc.value;
            remain = remain[oc.start..];

            if (remain.len > 0 && remain[0] != ',' && remain[0] != ';') {
                const ni = VLQ.decode(remain, 0);
                if (ni.start == 0) return error.InvalidSourceMap;
                remain = remain[ni.start..];
            }
            if (remain.len > 0 && remain[0] == ',') remain = remain[1..];

            max_original_line = @max(max_original_line, original_line);
            builder.appendMapping(SourceMapState{
                generated_column: generated_column,
                source_index: source_index,
                original_line: original_line,
                original_column: original_column,
            });
        }

        const out = builder.finalize();
        let blob = out.list.items;
        const total_len = blob.len as u64;
        const mapping_count: u64 = builder.count;
        const input_lines: u64 = @max(
            input_line_count_hint as u64,
            (max_original_line as u64) + 1,
        );
        unsafe {
            blob[0..8].* = @bitCast(total_len);
            blob[8..16].* = @bitCast(mapping_count);
            blob[16..24].* = @bitCast(input_lines);
        }

        const owned = out.list.toOwnedSlice(allocator) catch |err| bun.handleOom(err);
        builder.finalized = null;
        return owned;
    }
}
