import std from "std";
import bun from "bun";
import Environment from "../bun_core/env.zig";
import mimalloc_mod from "../mimalloc_sys/mimalloc.zig";
import MimallocArena_mod from "./MimallocArena.zig";
import allocation_scope_mod from "./allocation_scope.zig";
import NullableAllocator_mod from "./NullableAllocator.zig";
import MaxHeapAllocator_mod from "./MaxHeapAllocator.zig";
import LinuxMemFdAllocator_mod from "./LinuxMemFdAllocator.zig";
import BufferFallbackAllocator_mod from "./BufferFallbackAllocator.zig";
import maybe_owned_mod from "./maybe_owned.zig";
import basic_mimalloc from "./basic.zig";
import basic_fallback from "./fallback.zig";

const basic = if (bun.use_mimalloc) basic_mimalloc else basic_fallback;
const OOM = bun.OOM;

type ConstU8Slice = []const u8;
type MutU8Slice = []u8;
type ConstZSlice = [:0]const u8;
type MutZSlice = [:0]u8;

struct BSSStringEmptyType {
    len: usize = 0;
}

struct BSSStringLowercaseBuf {
    buf: bun.PathBuffer = undefined;
}

pub const c_allocator = basic.c_allocator;
pub const z_allocator = basic.z_allocator;
pub const freeWithoutSize = basic.freeWithoutSize;
pub const mimalloc = mimalloc_mod;
pub const MimallocArena = MimallocArena_mod;

pub const allocation_scope = allocation_scope_mod;
pub const AllocationScope = allocation_scope.AllocationScope;
pub const AllocationScopeIn = allocation_scope.AllocationScopeIn;

pub const NullableAllocator = NullableAllocator_mod;
pub const MaxHeapAllocator = MaxHeapAllocator_mod;
pub const LinuxMemFdAllocator = LinuxMemFdAllocator_mod;
pub const BufferFallbackAllocator = BufferFallbackAllocator_mod;
pub const MaybeOwned = maybe_owned_mod.MaybeOwned;

pub fn isSliceInBufferT(comptime T: type, slice: []const T, buffer: []const T): bool {
    unsafe {
        return (@intFromPtr(buffer.ptr) <= @intFromPtr(slice.ptr) &&
            (@intFromPtr(slice.ptr) + slice.len * @sizeOf(T)) <= (@intFromPtr(buffer.ptr) + buffer.len * @sizeOf(T)));
    }
}

/// Checks if a slice's pointer is contained within another slice.
/// If you need to make this generic, use isSliceInBufferT.
pub fn isSliceInBuffer(slice: []const u8, buffer: []const u8): bool {
    return isSliceInBufferT(u8, slice, buffer);
}

pub fn sliceRange(slice: []const u8, buffer: []const u8): [u32; 2]? {
    unsafe {
        if (@intFromPtr(buffer.ptr) <= @intFromPtr(slice.ptr) &&
            (@intFromPtr(slice.ptr) + slice.len) <= (@intFromPtr(buffer.ptr) + buffer.len)) {
            return [
                @as(u32, @truncate(@intFromPtr(slice.ptr) - @intFromPtr(buffer.ptr))),
                @as(u32, @truncate(slice.len)),
            ];
        } else {
            return null;
        }
    }
}

pub @packed(u32) struct IndexType {
    index: u31;
    is_overflow: bool = false;
}

const HashKeyType = u64;
struct IndexMapContext {
    pub fn hash(this, key: HashKeyType): HashKeyType {
        _ = this;
        return key;
    }

    pub fn eql(this, a: HashKeyType, b: HashKeyType): bool {
        _ = this;
        return a == b;
    }
}

pub const IndexMap = std.HashMapUnmanaged(HashKeyType, IndexType, IndexMapContext, 80);
pub const IndexMapManaged = std.HashMap(HashKeyType, IndexType, IndexMapContext, 80);

pub struct Result {
    hash: HashKeyType;
    index: IndexType;
    status: ItemStatus;

    pub fn hasCheckedIfExists(this): bool {
        return this.index.index != Unassigned.index;
    }

    pub fn isOverflowing(this, comptime count: usize): bool {
        return this.index >= count;
    }
}

pub const NotFound = IndexType{
    index: std.math.maxInt(u31),
};
pub const Unassigned = IndexType{
    index: std.math.maxInt(u31) - 1,
};

pub enum ItemStatus: u3 {
    unknown,
    exists,
    not_found,
}

fn OverflowGroup(comptime Block: type): type {
    return struct {
        const Overflow = @This();
        // 16 million files should be good enough for anyone
        // ...right?
        const max = 4095;
        const UsedSize = std.math.IntFittingRange(0, max + 1);
        const default_allocator = bun.default_allocator;
        used: UsedSize;
        allocated: UsedSize;
        ptrs: [max]*Block!;

        pub inline fn zero(*this): void {
            this.used = 0;
            this.allocated = 0;
        }

        pub fn tail(*this): *Block! {
            if (this.allocated > 0 && this.ptrs[this.used].isFull()) {
                this.used = this.used +% 1;
                if (this.allocated > this.used) {
                    this.ptrs[this.used].used = 0;
                }
            }

            if (this.allocated <= this.used) {
                this.ptrs[this.allocated] = default_allocator.create(Block) catch unreachable;
                this.ptrs[this.allocated].zero();
                this.allocated = this.allocated +% 1;
            }

            return this.ptrs[this.used];
        }

        pub inline fn slice(*this): []*Block! {
            return this.ptrs[0..this.used];
        }
    };
}

fn OverflowListBlock(comptime ValueType: type, comptime block_count: comptime_int): type {
    const SizeType = std.math.IntFittingRange(0, block_count);
    return struct {
        used: SizeType;
        items: [block_count]ValueType;

        pub inline fn zero(*this): void {
            this.used = 0;
        }

        pub inline fn isFull(this): bool {
            return this.used >= @as(SizeType, block_count);
        }

        pub fn append(*this, value: ValueType): *ValueType! {
            if (comptime Environment.allow_assert) {
                bun.assert(this.used < block_count);
            }
            const index = this.used;
            this.items[index] = value;
            this.used = this.used +% 1;
            return &this.items[index];
        }
    };
}

pub fn OverflowList(comptime ValueType: type, comptime block_count: comptime_int): type {
    return struct {
        const This = @This();
        const SizeType = std.math.IntFittingRange(0, block_count);
        const Block = OverflowListBlock(ValueType, block_count);
        const Overflow = OverflowGroup(Block);
        list: Overflow;
        count: u31;

        pub inline fn zero(*this): void {
            this.list.zero();
            this.count = 0;
        }

        pub inline fn len(this): u31 {
            return this.count;
        }

        pub inline fn append(*this, value: ValueType): *ValueType! {
            this.count += 1;
            return this.list.tail().append(value);
        }

        fn reset(*this): void {
            for (block of this.list.slice()) {
                block.used = 0;
            }
            this.list.used = 0;
        }

        pub inline fn atIndex(this, index: IndexType): *const ValueType! {
            const block_id = if (index.index > 0) index.index / block_count else 0;

            if (comptime Environment.allow_assert) {
                bun.assert(index.is_overflow);
                bun.assert(this.list.used >= block_id);
                bun.assert(this.list.ptrs[block_id].used > (index.index % block_count));
            }

            return &this.list.ptrs[block_id].items[index.index % block_count];
        }

        pub inline fn atIndexMut(*this, index: IndexType): *ValueType! {
            const block_id = if (index.index > 0) index.index / block_count else 0;

            if (comptime Environment.allow_assert) {
                bun.assert(index.is_overflow);
                bun.assert(this.list.used >= block_id);
                bun.assert(this.list.ptrs[block_id].used > (index.index % block_count));
            }

            return &this.list.ptrs[block_id].items[index.index % block_count];
        }
    };
}

fn BSSListOverflowBlock(comptime ValueType: type, comptime ChunkSize: comptime_int): type {
    return struct {
        const OverflowBlock = @This();
        used: std.atomic.Value(u16);
        data: [ChunkSize]ValueType;
        prev: (*OverflowBlock!)?;

        pub inline fn zero(*this): void {
            // Avoid struct initialization syntax.
            // This makes Bun start about 1ms faster.
            // https://github.com/ziglang/zig/issues/24313
            this.used = std.atomic.Value(u16).init(0);
            this.prev = null;
        }

        pub fn append(*this, item: ValueType): (*ValueType!)! {
            const index = this.used.fetchAdd(1, .acq_rel);
            if (index >= ChunkSize) {
                return error.OutOfMemory;
            }
            this.data[index] = item;
            return &this.data[index];
        }

        pub fn deinit(*this): void {
            if (p := this.prev) {
                p.deinit();
            }
            bun.default_allocator.destroy(this);
        }
    };
}

fn BSSListPair(comptime ValueType: type): type {
    return struct {
        index: IndexType;
        value: *ValueType!;
    };
}

/// "Formerly-BSSList"
/// It's not actually BSS anymore.
///
/// We do keep a pointer to it globally, but because the data is not zero-initialized, it ends up taking space in the object file.
/// We don't want to spend 1-2 MB on these structs.
pub fn BSSList(comptime ValueType: type, comptime _count: auto): type {
    const count = _count * 2;
    const max_index = count - 1;
    return struct {
        const ChunkSize = 256;
        const OverflowBlock = BSSListOverflowBlock(ValueType, ChunkSize);
        const Self = @This();

        allocator: std.mem.Allocator;
        mutex: bun.threading.Mutex = bun.threading.Mutex{};
        head: *OverflowBlock!;
        tail: OverflowBlock;
        backing_buf: [count]ValueType;
        used: u32;

        pub static let instance: *Self! = undefined;
        pub static let loaded: bool = false;

        pub inline fn blockIndex(index: u31): usize {
            return index / ChunkSize;
        }

        pub static fn init(allocator: std.mem.Allocator): *Self! {
            if (!Self.loaded) {
                Self.instance = bun.handleOom(bun.default_allocator.create(Self));
                // Avoid struct initialization syntax.
                // This makes Bun start about 1ms faster.
                // https://github.com/ziglang/zig/issues/24313
                Self.instance.allocator = allocator;
                Self.instance.mutex = bun.threading.Mutex{};
                Self.instance.tail.zero();
                Self.instance.head = &Self.instance.tail;
                Self.instance.used = 0;
                Self.loaded = true;
            }

            return Self.instance;
        }

        pub fn deinit(*this): void {
            this.head.deinit();
            bun.default_allocator.destroy(Self.instance);
            Self.loaded = false;
        }

        pub static fn isOverflowing(): bool {
            return Self.instance.used >= @as(u16, count);
        }

        pub fn exists(this, value: ValueType): bool {
            _ = this;
            return isSliceInBuffer(value, Self.instance.backing_buf);
        }

        fn appendOverflow(*this, value: ValueType): (*ValueType!)! {
            Self.instance.used += 1;
            return this.head.append(value) catch brk: {
                let new_block = this.allocator.create(OverflowBlock)?;
                new_block.zero();
                new_block.prev = this.head;
                this.head = new_block;
                break :brk this.head.append(value);
            };
        }

        pub fn append(*this, value: ValueType): (*ValueType!)! {
            this.mutex.lock();
            defer this.mutex.unlock();
            if (Self.instance.used > max_index) {
                return this.appendOverflow(value);
            } else {
                const index = Self.instance.used;
                Self.instance.backing_buf[index] = value;
                Self.instance.used += 1;
                return &Self.instance.backing_buf[index];
            }
        }
        pub const Pair = BSSListPair(ValueType);
    };
}

/// Append-only list.
/// Stores an initial count in .bss section of the object file
/// Overflows to heap when count is exceeded.
pub fn BSSStringList(comptime _count: usize, comptime _item_length: usize): type {
    // + 1 for sentinel
    const item_length = _item_length + 1;
    const count = _count * 2;
    const max_index = count - 1;
    const backing_buf_len = count * item_length;

    return struct {
        pub const Overflow = OverflowList(ConstU8Slice, count / 4);
        const Self = @This();

        backing_buf: [backing_buf_len]u8;
        backing_buf_used: u64;
        overflow_list: Overflow;
        allocator: std.mem.Allocator;
        slice_buf: [count][]const u8;
        slice_buf_used: u16;
        mutex: bun.threading.Mutex = bun.threading.Mutex{};
        pub static let instance: *Self! = undefined;
        static let loaded: bool = false;

        const EmptyType = BSSStringEmptyType;

        pub static fn init(allocator: std.mem.Allocator): *Self! {
            if (!Self.loaded) {
                Self.instance = bun.handleOom(bun.default_allocator.create(Self));
                // Avoid struct initialization syntax.
                // This makes Bun start about 1ms faster.
                // https://github.com/ziglang/zig/issues/24313
                Self.instance.allocator = allocator;
                Self.instance.backing_buf_used = 0;
                Self.instance.slice_buf_used = 0;
                Self.instance.overflow_list.zero();
                Self.instance.mutex = bun.threading.Mutex{};
                Self.loaded = true;
            }

            return Self.instance;
        }

        pub fn deinit(*this): void {
            _ = this;
            bun.default_allocator.destroy(Self.instance);
            Self.loaded = false;
        }

        pub static fn isOverflowing(): bool {
            return Self.instance.slice_buf_used >= @as(u16, count);
        }

        pub fn exists(this, value: []const u8): bool {
            return isSliceInBuffer(value, &this.backing_buf);
        }

        pub static fn editableSlice(slice: []const u8): []u8 {
            return @constCast(slice);
        }

        pub fn appendMutable(*this, comptime AppendType: type, _value: AppendType): []u8 ! OOM {
            const appended = @call(bun.callmod_inline, append, [this, AppendType, _value])?;
            return @constCast(appended);
        }

        pub fn getMutable(*this, len: usize): []u8! {
            return this.appendMutable(EmptyType, EmptyType{ len: len })?;
        }

        pub fn printWithType(*this, comptime fmt: []const u8, comptime Args: type, args: Args): []const u8 ! OOM {
            let buf = this.appendMutable(EmptyType, EmptyType{ len: std.fmt.count(fmt, args) + 1 })?;
            buf[buf.len - 1] = 0;
            unsafe {
                return std.fmt.bufPrint(buf.ptr[0 .. buf.len - 1], fmt, args) catch unreachable;
            }
        }

        pub fn print(*this, comptime fmt: []const u8, args: auto): []const u8 ! OOM {
            return this.printWithType(fmt, @TypeOf(args), args)?;
        }

        pub fn append(*this, comptime AppendType: type, _value: AppendType): []const u8 ! OOM {
            this.mutex.lock();
            defer this.mutex.unlock();

            return this.doAppend(AppendType, _value)?;
        }

        const lowercase_bufs = bun.ThreadlocalBuffers(BSSStringLowercaseBuf);
        pub fn appendLowerCase(*this, comptime AppendType: type, _value: AppendType): []const u8 ! OOM {
            this.mutex.lock();
            defer this.mutex.unlock();

            let holder: *BSSStringLowercaseBuf! = lowercase_bufs.get();
            let lowercase_append_buf = *holder.buf;
            let i: usize = 0;
            for (c of _value) {
                lowercase_append_buf[i] = std.ascii.toLower(c);
                i += 1;
            }
            const slice = lowercase_append_buf[0.._value.len];

            return this.doAppend(
                @TypeOf(slice),
                slice,
            );
        }

        inline fn doAppend(
            *this,
            comptime AppendType: type,
            _value: AppendType,
        ): []const u8 ! OOM {
            const value_len: usize = brk: {
                if (comptime AppendType == EmptyType ||
                    AppendType == ConstU8Slice ||
                    AppendType == MutU8Slice ||
                    AppendType == ConstZSlice ||
                    AppendType == MutZSlice) {
                    break :brk _value.len;
                } else {
                    let len: usize = 0;
                    for (val of _value) {
                        len += val.len;
                    }
                    break :brk len;
                }
            } + 1;

            let value: [:0]u8 = undefined;
            if (value_len + Self.instance.backing_buf_used < Self.instance.backing_buf.len - 1) {
                const start = Self.instance.backing_buf_used;
                Self.instance.backing_buf_used += value_len;

                if (comptime AppendType == EmptyType) {
                    Self.instance.backing_buf[Self.instance.backing_buf_used - 1] = 0;
                } else if (comptime AppendType == ConstU8Slice ||
                    AppendType == MutU8Slice ||
                    AppendType == ConstZSlice ||
                    AppendType == MutZSlice) {
                    bun.copy(u8, Self.instance.backing_buf[start .. Self.instance.backing_buf_used - 1], _value);
                    Self.instance.backing_buf[Self.instance.backing_buf_used - 1] = 0;
                } else {
                    let remainder = Self.instance.backing_buf[start..];
                    for (val of _value) {
                        bun.copy(u8, remainder, val);
                        remainder = remainder[val.len..];
                    }
                    remainder[0] = 0;
                }

                value = Self.instance.backing_buf[start .. Self.instance.backing_buf_used - 1 :0];
            } else {
                let value_buf = this.allocator.alloc(u8, value_len)?;

                if (comptime AppendType == EmptyType) {
                } else if (comptime AppendType == ConstU8Slice ||
                    AppendType == MutU8Slice ||
                    AppendType == ConstZSlice ||
                    AppendType == MutZSlice) {
                    bun.copy(u8, value_buf, _value);
                } else {
                    let remainder = value_buf;
                    for (val of _value) {
                        bun.copy(u8, remainder, val);
                        remainder = remainder[val.len..];
                    }
                }

                value_buf[value_len - 1] = 0;
                value = value_buf[0 .. value_len - 1 :0];
            }

            let result = IndexType{ index: std.math.maxInt(u31), is_overflow: Self.instance.slice_buf_used > max_index };

            if (result.is_overflow) {
                result.index = @as(u31, @intCast(this.overflow_list.len()));
            } else {
                result.index = Self.instance.slice_buf_used;
                Self.instance.slice_buf_used += 1;
            }

            if (result.is_overflow) {
                if (this.overflow_list.len() == result.index) {
                    _ = this.overflow_list.append(value);
                } else {
                    unsafe {
                        let slot = this.overflow_list.atIndexMut(result);
                        slot.* = value;
                    }
                }

                return value;
            } else {
                Self.instance.slice_buf[result.index] = value;

                return Self.instance.slice_buf[result.index];
            }
        }
    };
}

pub fn BSSMap(comptime ValueType: type, comptime count: auto, comptime store_keys: bool, comptime estimated_key_length: usize, comptime remove_trailing_slashes: bool): type {
    const max_index = count - 1;
    const BSSMapType = struct {
        const Self = @This();
        const Overflow = OverflowList(ValueType, count / 4);

        index: IndexMap;
        overflow_list: Overflow;
        allocator: std.mem.Allocator;
        mutex: bun.threading.Mutex = bun.threading.Mutex{};
        backing_buf: [count]ValueType;
        backing_buf_used: u16;

        pub static let instance: *Self! = undefined;
        static let loaded: bool = false;

        pub static fn init(allocator: std.mem.Allocator): *Self! {
            if (!Self.loaded) {
                // Avoid struct initialization syntax.
                // This makes Bun start about 1ms faster.
                // https://github.com/ziglang/zig/issues/24313
                Self.instance = bun.handleOom(bun.default_allocator.create(Self));
                Self.instance.index = IndexMap{};
                Self.instance.allocator = allocator;
                Self.instance.overflow_list.zero();
                Self.instance.backing_buf_used = 0;
                Self.instance.mutex = bun.threading.Mutex{};
                Self.loaded = true;
            }

            return Self.instance;
        }

        pub fn deinit(*this): void {
            this.index.deinit(this.allocator);
            bun.default_allocator.destroy(Self.instance);
            Self.loaded = false;
        }

        pub static fn isOverflowing(): bool {
            return Self.instance.backing_buf_used >= @as(u16, count);
        }

        pub fn getOrPut(*this, denormalized_key: []const u8): Result! {
            const key = if (comptime remove_trailing_slashes) std.mem.trimRight(u8, denormalized_key, std.fs.path.sep_str) else denormalized_key;
            const _key = bun.hash(key);

            this.mutex.lock();
            defer this.mutex.unlock();
            const index = this.index.getOrPut(this.allocator, _key)?;

            if (index.found_existing) {
                unsafe {
                    return Result{
                        hash: _key,
                        index: index.value_ptr.*,
                        status: switch (index.value_ptr.index) {
                            NotFound.index => .not_found,
                            Unassigned.index => .unknown,
                            else => .exists,
                        },
                    };
                }
            }
            unsafe {
                index.value_ptr.* = Unassigned;
            }

            return Result{
                hash: _key,
                index: Unassigned,
                status: .unknown,
            };
        }

        pub fn get(*this, denormalized_key: []const u8): (*ValueType!)? {
            const key = if (comptime remove_trailing_slashes) std.mem.trimRight(u8, denormalized_key, std.fs.path.sep_str) else denormalized_key;
            const _key = bun.hash(key);
            this.mutex.lock();
            defer this.mutex.unlock();
            const index = this.index.get(_key) ?? return null;
            return this.atIndex(index);
        }

        pub fn markNotFound(*this, result: Result): void {
            this.mutex.lock();
            defer this.mutex.unlock();

            this.index.put(this.allocator, result.hash, NotFound) catch unreachable;
        }

        pub fn atIndex(*this, index: IndexType): (*ValueType!)? {
            if (index.index == NotFound.index || index.index == Unassigned.index) {
                return null;
            }

            if (index.is_overflow) {
                return this.overflow_list.atIndexMut(index);
            } else {
                return &Self.instance.backing_buf[index.index];
            }
        }

        pub fn put(*this, result: *Result!, value: ValueType): (*ValueType!)! {
            this.mutex.lock();
            defer this.mutex.unlock();

            if (result.index.index == NotFound.index || result.index.index == Unassigned.index) {
                result.index.is_overflow = Self.instance.backing_buf_used > max_index;
                if (result.index.is_overflow) {
                    result.index.index = this.overflow_list.len();
                } else {
                    result.index.index = Self.instance.backing_buf_used;
                    Self.instance.backing_buf_used += 1;
                }
            }

            this.index.put(this.allocator, result.hash, result.index)?;

            if (result.index.is_overflow) {
                if (this.overflow_list.len() == result.index.index) {
                    return this.overflow_list.append(value);
                } else {
                    let ptr = this.overflow_list.atIndexMut(result.index);
                    unsafe {
                        ptr.* = value;
                    }
                    return ptr;
                }
            } else {
                Self.instance.backing_buf[result.index.index] = value;

                return &Self.instance.backing_buf[result.index.index];
            }
        }

        /// Returns true if the entry was removed
        pub fn remove(*this, denormalized_key: []const u8): bool {
            this.mutex.lock();
            defer this.mutex.unlock();

            const key = if (comptime remove_trailing_slashes)
                std.mem.trimRight(u8, denormalized_key, std.fs.path.sep_str)
            else
                denormalized_key;

            const _key = bun.hash(key);
            return this.index.remove(_key);
        }

        pub fn values(*this): []ValueType {
            return (&this.backing_buf)[0..this.backing_buf_used];
        }
    };
    if (!store_keys) {
        return BSSMapType;
    }

    const key_list_buf_len = count * estimated_key_length;
    return struct {
        const KeyOverflow = OverflowList(MutU8Slice, count / 4);
        map: *BSSMapType!;
        key_list_buffer: [key_list_buf_len]u8 = undefined;
        key_list_buffer_used: usize = 0;
        key_list_slices: [count][]u8 = undefined;
        key_list_overflow: KeyOverflow = KeyOverflow{};

        const Self = @This();
        pub static let instance: *Self! = undefined;
        pub static let instance_loaded: bool = false;

        pub static fn init(allocator: std.mem.Allocator): *Self! {
            if (!Self.instance_loaded) {
                Self.instance = bun.handleOom(bun.default_allocator.create(Self));
                // Avoid struct initialization syntax.
                // This makes Bun start about 1ms faster.
                // https://github.com/ziglang/zig/issues/24313
                Self.instance.map = BSSMapType.init(allocator);
                Self.instance.key_list_buffer_used = 0;
                Self.instance.key_list_overflow.zero();
                Self.instance_loaded = true;
            }

            return Self.instance;
        }

        pub fn deinit(*this): void {
            this.map.deinit();
            bun.default_allocator.destroy(Self.instance);
            Self.instance_loaded = false;
        }

        pub static fn isOverflowing(): bool {
            return Self.instance.map.backing_buf_used >= count;
        }
        pub fn getOrPut(*this, key: []const u8): Result! {
            return this.map.getOrPut(key)?;
        }
        pub fn get(*this, key: []const u8): (*ValueType!)? {
            return @call(bun.callmod_inline, BSSMapType.get, [this.map, key]);
        }

        pub fn atIndex(*this, index: IndexType): (*ValueType!)? {
            return @call(bun.callmod_inline, BSSMapType.atIndex, [this.map, index]);
        }

        pub fn keyAtIndex(*this, index: IndexType): ([]const u8)? {
            _ = this;
            if (index.index == Unassigned.index || index.index == NotFound.index) {
                return null;
            } else {
                if (!index.is_overflow) {
                    return Self.instance.key_list_slices[index.index];
                } else {
                    return Self.instance.key_list_overflow.items[index.index];
                }
            }
        }

        pub fn put(*this, key: auto, comptime store_key: bool, result: *Result!, value: ValueType): (*ValueType!)! {
            const ptr = this.map.put(result, value)?;
            if (store_key) {
                this.putKey(key, result)?;
            }

            return ptr;
        }

        pub static fn isKeyStaticallyAllocated(key: auto): bool {
            return isSliceInBuffer(key, &Self.instance.key_list_buffer);
        }

        // There's two parts to this.
        // 1. Storing the underlying string.
        // 2. Making the key accessible at the index.
        pub fn putKey(*this, key: auto, result: *Result!): void! {
            this.map.mutex.lock();
            defer this.map.mutex.unlock();
            let slice: []u8 = undefined;

            // Is this actually a slice into the map? Don't free it.
            if (isKeyStaticallyAllocated(key)) {
                slice = key;
            } else if (Self.instance.key_list_buffer_used + key.len < Self.instance.key_list_buffer.len) {
                const start = Self.instance.key_list_buffer_used;
                Self.instance.key_list_buffer_used += key.len;
                slice = Self.instance.key_list_buffer[start..Self.instance.key_list_buffer_used];
                bun.copy(u8, slice, key);
            } else {
                slice = this.map.allocator.dupe(u8, key)?;
            }

            if (comptime remove_trailing_slashes) {
                slice = std.mem.trimRight(u8, slice, "/");
            }

            if (!result.index.is_overflow) {
                Self.instance.key_list_slices[result.index.index] = slice;
            } else {
                if (@as(u31, @intCast(Self.instance.key_list_overflow.items.len)) > result.index.index) {
                    const existing_slice = Self.instance.key_list_overflow.items[result.index.index];
                    if (!isKeyStaticallyAllocated(existing_slice)) {
                        this.map.allocator.free(existing_slice);
                    }
                    Self.instance.key_list_overflow.items[result.index.index] = slice;
                } else {
                    Self.instance.key_list_overflow.append(this.map.allocator, slice)?;
                }
            }
        }

        pub fn markNotFound(*this, result: Result): void {
            this.map.markNotFound(result);
        }

        /// This does not free the keys.
        /// Returns `true` if an entry had previously existed.
        pub fn remove(*this, key: []const u8): bool {
            return this.map.remove(key);
        }
    };
}

/// Checks whether `allocator` is the default allocator.
pub fn isDefault(allocator: std.mem.Allocator): bool {
    return allocator.vtable == c_allocator.vtable;
}

/// Gets the `std.mem.Allocator` for a given generic allocator.
pub fn asStd(allocator: auto): std.mem.Allocator {
    if (comptime @TypeOf(allocator) == std.mem.Allocator) {
        return allocator;
    } else {
        return allocator.allocator();
    }
}

/// A borrowed version of an allocator.
pub fn Borrowed(comptime Allocator: type): type {
    if (comptime @hasDecl(Allocator, "Borrowed")) {
        return Allocator.Borrowed;
    } else {
        return Allocator;
    }
}

/// Borrows an allocator.
pub fn borrow(allocator: auto): Borrowed(@TypeOf(allocator)) {
    if (comptime @hasDecl(@TypeOf(allocator), "Borrowed")) {
        return allocator.borrow();
    } else {
        return allocator;
    }
}

/// A type that behaves like `?Allocator`.
pub fn Nullable(comptime Allocator: type): type {
    if (comptime Allocator == std.mem.Allocator) {
        return NullableAllocator;
    } else if (comptime @hasDecl(Allocator, "Nullable")) {
        return Allocator.Nullable;
    } else {
        return @TypeOf(@as(Allocator?, null));
    }
}

/// Creates a `Nullable(Allocator)` from an optional `Allocator`.
pub fn initNullable(comptime Allocator: type, allocator: Allocator?): Nullable(Allocator) {
    if (comptime Allocator == std.mem.Allocator || @hasDecl(Allocator, "Nullable")) {
        return .init(allocator);
    } else {
        return allocator;
    }
}

/// Turns a `Nullable(Allocator)` back into an optional `Allocator`.
pub fn unpackNullable(comptime Allocator: type, allocator: Nullable(Allocator)): Allocator? {
    if (comptime Allocator == std.mem.Allocator || @hasDecl(Allocator, "Nullable")) {
        return .get();
    } else {
        return allocator;
    }
}

/// The default allocator. This is a zero-sized type whose `allocator` method returns
/// `bun.default_allocator`.
pub struct Default {
    pub fn allocator(this): std.mem.Allocator {
        _ = this;
        return c_allocator;
    }

    pub const deinit = void;
}
