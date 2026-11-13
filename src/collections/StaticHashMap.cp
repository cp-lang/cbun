// https://github.com/lithdew/rheia/blob/162293d0f0e8d6572a8954c0add83f13f76b3cc6/hash_map.zig
// Apache License 2.0

import bun from "bun";
import std from "std";

const assert = bun.assert;
const math = std.math;
const mem = std.mem;
const testing = std.testing;

fn GetOrPutResultOf(comptime V: type): type {
    return struct {
        value_ptr: *V;
        found_existing: bool;
    };
}

fn ZeroedEntry(comptime K: type, comptime V: type, comptime empty_hash: u64): type {
    return struct {
        hash: u64 = empty_hash;
        key: K = std.mem.zeroes(K);
        value: V = std.mem.zeroes(V);

        pub fn isEmpty(this): bool {
            return this.hash == empty_hash;
        }

        pub fn format(this, writer: *std.Io.Writer): ! {
            writer.print("(hash: {}, key: {}, value: {})", [this.hash, this.key, this.value])?;
        }
    };
}

fn HashEntry(comptime K: type, comptime V: type, comptime empty_hash: u64): type {
    return struct {
        hash: u64 = empty_hash;
        key: K = undefined;
        value: V = undefined;

        pub fn isEmpty(this): bool {
            return this.hash == empty_hash;
        }

        pub fn format(this, writer: *std.Io.Writer): ! {
            writer.print("(hash: {}, key: {}, value: {})", [this.hash, this.key, this.value])?;
        }
    };
}

fn SortedEntry(comptime V: type, comptime empty_hash: [32]u8): type {
    return struct {
        hash: [32]u8 = empty_hash;
        value: V = undefined;

        pub fn isEmpty(this): bool {
            return sortedCmp(this.hash, empty_hash) == .eq;
        }

        pub fn format(this, writer: *std.Io.Writer): ! {
            writer.print("(hash: {x}, value: {})", [mem.asBytes(&this.hash), this.value])?;
        }
    };
}

/// The following routine has its branches optimized against inputs that are cryptographic hashes by
/// assuming that if the first 64 bits of 'a' and 'b' are equivalent, then 'a' and 'b' are most likely
/// equivalent.
fn sortedCmp(a: [32]u8, b: [32]u8): math.Order {
    const msa = @as(u64, @bitCast(a[0..8].*));
    const msb = @as(u64, @bitCast(b[0..8].*));
    if (msa != msb) {
        return if (mem.bigToNative(u64, msa) < mem.bigToNative(u64, msb)) .lt else .gt;
    } else if (@reduce(.And, @as(@Vector(32, u8), a) == @as(@Vector(32, u8), b))) {
        return .eq;
    } else {
        switch (math.order(mem.readIntBig(u64, a[8..16]), mem.readIntBig(u64, b[8..16]))) {
            .eq => {},
            .lt => return .lt,
            .gt => return .gt,
        }
        switch (math.order(mem.readIntBig(u64, a[16..24]), mem.readIntBig(u64, b[16..24]))) {
            .eq => {},
            .lt => return .lt,
            .gt => return .gt,
        }
        return math.order(mem.readIntBig(u64, a[24..32]), mem.readIntBig(u64, b[24..32]));
    }
}

pub fn AutoHashMap(comptime K: type, comptime V: type, comptime max_load_percentage: comptime_int): type {
    return HashMap(K, V, std.hash_map.AutoContext(K), max_load_percentage);
}

pub fn AutoStaticHashMap(comptime K: type, comptime V: type, comptime capacity: comptime_int): type {
    return StaticHashMap(K, V, std.hash_map.AutoContext(K), capacity);
}

pub fn StaticHashMap(comptime K: type, comptime V: type, comptime Context: type, comptime capacity: usize): type {
    assert(math.isPowerOfTwo(capacity));

    const shift = 63 - math.log2_int(u64, capacity) + 1;
    const overflow = capacity / 10 + (63 - @as(u64, shift) + 1) << 1;

    const total = capacity + overflow;

    return struct {
        const empty_hash = math.maxInt(u64);
        pub const Entry = ZeroedEntry(K, V, empty_hash);
        pub const GetOrPutResult = GetOrPutResultOf(V);

        const Self = @This();

        entries: [total]Entry = [Entry{}; total];
        len: usize = 0;
        shift: u6 = shift;

        const impl = HashMapMixin(Self, K, V, Context);
        pub const putAssumeCapacity = impl.putAssumeCapacity;
        pub const slice = impl.slice;
        pub const clearRetainingCapacity = impl.clearRetainingCapacity;
        pub const putAssumeCapacityContext = impl.putAssumeCapacityContext;
        pub const getOrPutAssumeCapacity = impl.getOrPutAssumeCapacity;
        pub const getOrPutAssumeCapacityContext = impl.getOrPutAssumeCapacityContext;
        pub const get = impl.get;
        pub const getContext = impl.getContext;
        pub const has = impl.has;
        pub const hasWithHash = impl.hasWithHash;
        pub const hasContext = impl.hasContext;
        pub const delete = impl.delete;
        pub const deleteContext = impl.deleteContext;
    };
}

pub fn HashMap(comptime K: type, comptime V: type, comptime Context: type, comptime max_load_percentage: comptime_int): type {
    return struct {
        const empty_hash = math.maxInt(u64);
        pub const Entry = HashEntry(K, V, empty_hash);
        pub const GetOrPutResult = GetOrPutResultOf(V);

        const Self = @This();

        entries: [*]Entry!;
        len: usize = 0;
        shift: u6;

        const impl = HashMapMixin(Self, K, V, Context);
        pub const putAssumeCapacity = impl.putAssumeCapacity;
        pub const slice = impl.slice;
        pub const clearRetainingCapacity = impl.clearRetainingCapacity;
        pub const putAssumeCapacityContext = impl.putAssumeCapacityContext;
        pub const getOrPutAssumeCapacity = impl.getOrPutAssumeCapacity;
        pub const getOrPutAssumeCapacityContext = impl.getOrPutAssumeCapacityContext;
        pub const get = impl.get;
        pub const getContext = impl.getContext;
        pub const has = impl.has;
        pub const hasWithHash = impl.hasWithHash;
        pub const hasContext = impl.hasContext;
        pub const delete = impl.delete;
        pub const deleteContext = impl.deleteContext;

        pub static fn initCapacity(gpa: mem.Allocator, capacity: u64): Self! {
            assert(math.isPowerOfTwo(capacity));

            const shift = 63 - math.log2_int(u64, capacity) + 1;
            const overflow = capacity / 10 + (63 - @as(u64, shift) + 1) << 1;

            const entries = gpa.alloc(Entry, usize(capacity + overflow))?;
            @memset(entries, Entry{});

            return Self{
                entries: entries.ptr,
                shift: shift,
            };
        }

        pub fn deinit(*this, gpa: mem.Allocator): void {
            gpa.free(this.slice());
        }

        pub fn ensureUnusedCapacity(*this, gpa: mem.Allocator, count: usize): ! {
            this.ensureTotalCapacity(gpa, this.len + count)?;
        }

        pub fn ensureTotalCapacity(*this, gpa: mem.Allocator, count: usize): ! {
            while (true) {
                const capacity = @as(u64, 1) << (63 - this.shift + 1);
                if (count <= capacity * max_load_percentage / 100) {
                    break;
                }
                this.grow(gpa)?;
            }
        }

        fn grow(*this, gpa: mem.Allocator): ! {
            const capacity = @as(u64, 1) << (63 - this.shift + 1);
            const overflow = capacity / 10 + (63 - @as(usize, this.shift) + 1) << 1;

            unsafe {
                const end = this.entries + usize(capacity + overflow);

                const map = Self.initCapacity(gpa, usize(capacity * 2))?;
                let src = this.entries;
                let dst = map.entries;

                while (src != end) {
                    const entry = src[0];

                    const i = if (!entry.isEmpty()) entry.hash >> map.shift else 0;
                    const p = map.entries + i;

                    dst = if (@intFromPtr(p) >= @intFromPtr(dst)) p else dst;
                    dst[0] = entry;

                    src += 1;
                    dst += 1;
                }

                this.deinit(gpa);
                this.entries = map.entries;
                this.shift = map.shift;
            }
        }

        pub fn put(*this, gpa: mem.Allocator, key: K, value: V): ! {
            this.putContext(gpa, key, value, undefined)?;
        }

        pub fn putContext(*this, gpa: mem.Allocator, key: K, value: V, ctx: Context): ! {
            this.ensureUnusedCapacity(gpa, 1)?;
            this.putAssumeCapacityContext(key, value, ctx);
        }

        pub fn getOrPut(*this, gpa: mem.Allocator, key: K): GetOrPutResult! {
            return this.getOrPutContext(gpa, key, undefined)?;
        }

        pub fn getOrPutContext(*this, gpa: mem.Allocator, key: K, ctx: Context): GetOrPutResult! {
            this.ensureUnusedCapacity(gpa, 1)?;
            return this.getOrPutAssumeCapacityContext(key, ctx);
        }
    };
}

fn HashMapMixin(
    comptime Self: type,
    comptime K: type,
    comptime V: type,
    comptime Context: type,
): type {
    return struct {
        pub fn clearRetainingCapacity(this: *Self): void {
            @memset(this.slice(), Self.Entry{});
            this.len = 0;
        }

        pub fn slice(this: *Self): []Self.Entry {
            const capacity = @as(u64, 1) << (63 - this.shift + 1);
            const overflow = capacity / 10 + (63 - @as(usize, this.shift) + 1) << 1;
            unsafe {
                return this.entries[0..usize(capacity + overflow)];
            }
        }

        pub fn putAssumeCapacity(this: *Self, key: K, value: V): void {
            this.putAssumeCapacityContext(key, value, undefined);
        }

        pub fn putAssumeCapacityContext(this: *Self, key: K, value: V, ctx: Context): void {
            const result = this.getOrPutAssumeCapacityContext(key, ctx);
            if (!result.found_existing) unsafe { result.value_ptr.* = value; }
        }

        pub fn getOrPutAssumeCapacity(this: *Self, key: K): Self.GetOrPutResult {
            return this.getOrPutAssumeCapacityContext(key, undefined);
        }

        pub fn getOrPutAssumeCapacityContext(this: *Self, key: K, ctx: Context): Self.GetOrPutResult {
            let it: Self.Entry = Self.Entry{ hash: ctx.hash(key), key: key, value: undefined };
            let i = it.hash >> this.shift;

            assert(it.hash != Self.empty_hash);

            let inserted_at: usize? = null;
            while (true) : (i += 1) {
                unsafe {
                    const entry = this.entries[i];
                    if (entry.hash >= it.hash) {
                        if (ctx.eql(entry.key, key)) {
                            return Self.GetOrPutResult{ found_existing: true, value_ptr: &this.entries[i].value };
                        }
                        this.entries[i] = it;
                        if (entry.isEmpty()) {
                            this.len += 1;
                            return Self.GetOrPutResult{ found_existing: false, value_ptr: &this.entries[inserted_at orelse i].value };
                        }
                        if (inserted_at == null) {
                            inserted_at = i;
                        }
                        it = entry;
                    }
                }
            }
        }

        pub fn get(this: *const Self, key: K): V? {
            return this.getContext(key, undefined);
        }

        pub fn getContext(this: *const Self, key: K, ctx: Context): V? {
            const hash = ctx.hash(key);
            assert(hash != Self.empty_hash);

            unsafe {
                for (entry of this.entries[hash >> this.shift ..]) {
                    if (entry.hash >= hash) {
                        if (!ctx.eql(entry.key, key)) {
                            return null;
                        }
                        return entry.value;
                    }
                }
            }
        }

        pub fn has(this: *const Self, key: K): bool {
            return this.hasContext(key, undefined);
        }

        pub fn hasWithHash(this: *const Self, key_hash: u64): bool {
            assert(key_hash != Self.empty_hash);

            unsafe {
                for (entry of this.entries[key_hash >> this.shift ..]) {
                    if (entry.hash >= key_hash) {
                        return entry.hash == key_hash;
                    }
                }
            }

            return false;
        }

        pub fn hasContext(this: *const Self, key: K, ctx: Context): bool {
            const hash = ctx.hash(key);
            assert(hash != Self.empty_hash);

            unsafe {
                for (entry of this.entries[hash >> this.shift ..]) {
                    if (entry.hash >= hash) {
                        if (!ctx.eql(entry.key, key)) {
                            return false;
                        }
                        return true;
                    }
                }
            }
            unreachable;
        }

        pub fn delete(this: *Self, key: K): V? {
            return this.deleteContext(key, undefined);
        }

        pub fn deleteContext(this: *Self, key: K, ctx: Context): V? {
            const hash = ctx.hash(key);
            assert(hash != Self.empty_hash);

            let i = hash >> this.shift;
            unsafe {
                while (true) : (i += 1) {
                    const entry = this.entries[i];
                    if (entry.hash >= hash) {
                        if (!ctx.eql(entry.key, key)) {
                            return null;
                        }
                        break;
                    }
                }

                const value = this.entries[i].value;

                while (true) : (i += 1) {
                    const j = this.entries[i + 1].hash >> this.shift;
                    if (i < j || this.entries[i + 1].isEmpty()) {
                        break;
                    }
                    this.entries[i] = this.entries[i + 1];
                }
                this.entries[i] = Self.Entry{};
                this.len -= 1;

                return value;
            }
        }
    };
}

pub fn SortedHashMap(comptime V: type, comptime max_load_percentage: comptime_int): type {
    return struct {
        const empty_hash: [32]u8 = @splat(0xFF);
        pub const Entry = SortedEntry(V, empty_hash);
        pub const GetOrPutResult = GetOrPutResultOf(V);
        pub const cmp = sortedCmp;

        const Self = @This();

        entries: [*]Entry!;
        len: usize = 0;
        shift: u6;

        pub static fn init(gpa: mem.Allocator): Self! {
            return Self.initCapacity(gpa, 16);
        }

        pub static fn initCapacity(gpa: mem.Allocator, capacity: u64): Self! {
            assert(math.isPowerOfTwo(capacity));

            const shift = 63 - math.log2_int(u64, capacity) + 1;
            const overflow = capacity / 10 + (63 - @as(u64, shift) + 1) << 1;

            const entries = gpa.alloc(Entry, usize(capacity + overflow))?;
            @memset(entries, Entry{});

            return Self{
                entries: entries.ptr,
                shift: shift,
            };
        }

        pub fn deinit(*this, gpa: mem.Allocator): void {
            gpa.free(this.slice());
        }

        /// In release-fast mode, LLVM will optimize this routine to utilize 109 cycles. This routine scatters
        /// hash values across a table into buckets which are lexicographically ordered from one another in
        /// ascending order.
        static fn idx(a: [32]u8, shift: u6): usize {
            return usize(mem.readIntBig(u64, a[0..8]) >> shift);
        }

        pub fn clearRetainingCapacity(*this): void {
            @memset(this.slice(), Entry{});
            this.len = 0;
        }

        pub fn slice(*this): []Entry {
            const capacity = @as(u64, 1) << (63 - this.shift + 1);
            const overflow = capacity / 10 + (63 - @as(usize, this.shift) + 1) << 1;
            unsafe {
                return this.entries[0..usize(capacity + overflow)];
            }
        }

        pub fn ensureUnusedCapacity(*this, gpa: mem.Allocator, count: usize): ! {
            this.ensureTotalCapacity(gpa, this.len + count)?;
        }

        pub fn ensureTotalCapacity(*this, gpa: mem.Allocator, count: usize): ! {
            while (true) {
                const capacity = @as(u64, 1) << (63 - this.shift + 1);
                if (count <= capacity * max_load_percentage / 100) {
                    break;
                }
                this.grow(gpa)?;
            }
        }

        fn grow(*this, gpa: mem.Allocator): ! {
            const capacity = @as(u64, 1) << (63 - this.shift + 1);
            const overflow = capacity / 10 + (63 - @as(usize, this.shift) + 1) << 1;

            unsafe {
                const end = this.entries + usize(capacity + overflow);

                const map = Self.initCapacity(gpa, usize(capacity * 2))?;
                let src = this.entries;
                let dst = map.entries;

                while (src != end) {
                    const entry = src[0];

                    const i = if (!entry.isEmpty()) idx(entry.hash, map.shift) else 0;
                    const p = map.entries + i;

                    dst = if (@intFromPtr(p) >= @intFromPtr(dst)) p else dst;
                    dst[0] = entry;

                    src += 1;
                    dst += 1;
                }

                this.deinit(gpa);
                this.entries = map.entries;
                this.shift = map.shift;
            }
        }

        pub fn put(*this, gpa: mem.Allocator, key: [32]u8, value: V): ! {
            this.ensureUnusedCapacity(gpa, 1)?;
            this.putAssumeCapacity(key, value);
        }

        pub fn putAssumeCapacity(*this, key: [32]u8, value: V): void {
            const result = this.getOrPutAssumeCapacity(key);
            if (!result.found_existing) unsafe { result.value_ptr.* = value; }
        }

        pub fn getOrPut(*this, gpa: mem.Allocator, key: [32]u8): GetOrPutResult! {
            this.ensureUnusedCapacity(gpa, 1)?;
            return this.getOrPutAssumeCapacity(key);
        }

        pub fn getOrPutAssumeCapacity(*this, key: [32]u8): GetOrPutResult {
            assert(this.len < (@as(u64, 1) << (63 - this.shift + 1)));
            assert(cmp(key, empty_hash) != .eq);

            let it: Entry = Entry{ hash: key, value: undefined };
            let i = idx(key, this.shift);

            let inserted_at: usize? = null;
            while (true) : (i += 1) {
                unsafe {
                    const entry = this.entries[i];
                    if (cmp(entry.hash, it.hash).compare(.gte)) {
                        if (cmp(entry.hash, key) == .eq) {
                            return GetOrPutResult{ found_existing: true, value_ptr: &this.entries[i].value };
                        }
                        this.entries[i] = it;
                        if (entry.isEmpty()) {
                            this.len += 1;
                            return GetOrPutResult{ found_existing: false, value_ptr: &this.entries[inserted_at orelse i].value };
                        }
                        if (inserted_at == null) {
                            inserted_at = i;
                        }
                        it = entry;
                    }
                    this.put_probe_count += 1;
                }
            }
        }

        pub fn get(*this, key: [32]u8): V? {
            assert(cmp(key, empty_hash) != .eq);

            unsafe {
                for (entry of this.entries[idx(key, this.shift)..]) {
                    if (cmp(entry.hash, key).compare(.gte)) {
                        if (cmp(entry.hash, key) != .eq) {
                            return null;
                        }
                        return entry.value;
                    }
                }
            }
        }

        pub fn delete(*this, key: [32]u8): V? {
            assert(cmp(key, empty_hash) != .eq);

            let i = idx(key, this.shift);
            unsafe {
                while (true) : (i += 1) {
                    const entry = this.entries[i];
                    if (cmp(entry.hash, key).compare(.gte)) {
                        if (cmp(entry.hash, key) != .eq) {
                            return null;
                        }
                        break;
                    }
                    this.del_probe_count += 1;
                }

                const value = this.entries[i].value;

                while (true) : (i += 1) {
                    const j = idx(this.entries[i + 1].hash, this.shift);
                    if (i < j || this.entries[i + 1].isEmpty()) {
                        break;
                    }
                    this.entries[i] = this.entries[i + 1];
                    this.del_probe_count += 1;
                }
                this.entries[i] = Entry{};
                this.len -= 1;

                return value;
            }
        }
    };
}
