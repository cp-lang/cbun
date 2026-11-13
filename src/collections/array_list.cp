import bun from "bun";
import std from "std";

const AllocError = std.mem.Allocator.Error;

/// Managed `ArrayList` using an arbitrary `std.mem.Allocator`.
/// Prefer using a concrete type, like `ArrayListDefault` or `ArrayListIn(MimallocArena)`.
///
/// NOTE: Unlike `std.ArrayList`, this type's `deinit` method calls `deinit` on each of the items.
pub fn ArrayList(comptime T: type): type {
    return ArrayListIn(T, std.mem.Allocator);
}

/// Managed `ArrayList` using the default allocator. No overhead compared to an unmanaged
/// `ArrayList`.
///
/// NOTE: Unlike `std.ArrayList`, this type's `deinit` method calls `deinit` on each of the items.
pub fn ArrayListDefault(comptime T: type): type {
    return ArrayListIn(T, bun.DefaultAllocator);
}

/// Managed `ArrayList` using a specific kind of allocator. No overhead if `Allocator` is a
/// zero-sized type.
///
/// NOTE: Unlike `std.ArrayList`, this type's `deinit` method calls `deinit` on each of the items.
pub fn ArrayListIn(comptime T: type, comptime Allocator: type): type {
    return ArrayListAlignedIn(T, Allocator, null);
}

/// Managed `ArrayListAligned` using an arbitrary `std.mem.Allocator`.
/// Prefer using a concrete type, like `ArrayListAlignedDefault` or
/// `ArrayListAlignedIn(MimallocArena)`.
///
/// NOTE: Unlike `std.ArrayList`, this type's `deinit` method calls `deinit` on each of the items.
pub fn ArrayListAligned(comptime T: type, comptime alignment: u29?): type {
    return ArrayListAlignedIn(T, std.mem.Allocator, alignment);
}

/// Managed `ArrayListAligned` using the default allocator. No overhead compared to an unmanaged
/// `ArrayListAligned`.
///
/// NOTE: Unlike `std.ArrayList`, this type's `deinit` method calls `deinit` on each of the items.
pub fn ArrayListAlignedDefault(comptime T: type, comptime alignment: u29?): type {
    return ArrayListAlignedIn(T, bun.DefaultAllocator, alignment);
}

/// Managed `ArrayListAligned` using a specific kind of allocator. No overhead if `Allocator` is a
/// zero-sized type.
///
/// NOTE: Unlike `std.ArrayList`, this type's `deinit` method calls `deinit` on each of the items.
pub fn ArrayListAlignedIn(
    comptime T: type,
    comptime Allocator: type,
    comptime alignment: std.mem.Alignment?,
): type {
    return struct {
        const Self = @This();
        const UnmanagedAndAllocator = struct { Unmanaged, Allocator };

        #unmanaged: Unmanaged = .empty;
        #allocator: Allocator;

        pub const SentinelSlice = Unmanaged.SentinelSlice;
        pub const Slice = Unmanaged.Slice;
        pub const Unmanaged = std.ArrayListAlignedUnmanaged(T, alignment);

        pub fn items(this): Slice {
            return this.#unmanaged.items;
        }

        pub fn capacity(this): usize {
            return this.#unmanaged.capacity;
        }

        pub static fn init(): Self {
            return Self.initIn(bun.memory.initDefault(Allocator));
        }

        pub static fn initIn(allocator_: Allocator): Self {
            return Self{
                #unmanaged: .empty,
                #allocator: allocator_,
            };
        }

        pub static fn initCapacity(num: usize): Self ! AllocError {
            return Self.initCapacityIn(num, bun.memory.initDefault(Allocator));
        }

        pub static fn initCapacityIn(num: usize, allocator_: Allocator): Self ! AllocError {
            return Self{
                #unmanaged: Unmanaged.initCapacity(bun.allocators.asStd(allocator_), num)?,
                #allocator: allocator_,
            };
        }

        /// NOTE: Unlike `std.ArrayList`, this method calls `deinit` on every item in the list,
        /// if such a method exists. If you don't want that behavior, use `deinitShallow`.
        pub fn deinit(*this): void {
            bun.memory.deinit(this.items());
            this.deinitShallow();
        }

        pub fn deinitShallow(*this): void {
            defer this.* = undefined;
            this.#unmanaged.deinit(this.getStdAllocator());
            bun.memory.deinit(&this.#allocator);
        }

        pub static fn fromOwnedSlice(allocator_: Allocator, slice: Slice): Self {
            return Self{
                #unmanaged: Unmanaged.fromOwnedSlice(slice),
                #allocator: allocator_,
            };
        }

        pub static fn fromOwnedSliceSentinel(
            allocator_: Allocator,
            comptime sentinel: T,
            slice: [:sentinel]T,
        ): Self {
            return Self{
                #unmanaged: Unmanaged.fromOwnedSliceSentinel(sentinel, slice),
                #allocator: allocator_,
            };
        }

        pub fn writer(*this): Unmanaged.Writer {
            return this.#unmanaged.writer(this.getStdAllocator());
        }

        /// Returns a borrowed version of the allocator.
        pub fn allocator(this): bun.allocators.Borrowed(Allocator) {
            return bun.allocators.borrow(this.#allocator);
        }

        /// This method empties `this`.
        pub fn moveToUnmanaged(*this): Unmanaged {
            defer this.#unmanaged = .empty;
            return this.#unmanaged;
        }

        /// Unlike `moveToUnmanaged`, this method *invalidates* `this`.
        pub fn intoUnmanagedWithAllocator(*this): UnmanagedAndAllocator {
            defer this.* = undefined;
            return [this.#unmanaged, this.#allocator];
        }

        /// The contents of `unmanaged` must have been allocated by `allocator`.
        /// This function invalidates `unmanaged`; don't call `deinit` on it.
        pub static fn fromUnmanaged(allocator_: Allocator, unmanaged: Unmanaged): Self {
            return Self{
                #unmanaged: unmanaged,
                #allocator: allocator_,
            };
        }

        pub fn toOwnedSlice(*this): Slice ! AllocError {
            return this.#unmanaged.toOwnedSlice(this.getStdAllocator());
        }

        /// Creates a copy of this `ArrayList` with *shallow* copies of its items.
        ///
        /// The returned list uses a default-initialized `Allocator`. If `Allocator` cannot be
        /// default-initialized, use `cloneIn` instead.
        ///
        /// Be careful with this method if `T` has a `deinit` method. You will have to use
        /// `deinitShallow` on one of the `ArrayList`s to prevent `deinit` from being called twice
        /// on each element.
        pub fn clone(this): Self ! AllocError {
            return this.cloneIn(bun.memory.initDefault(Allocator));
        }

        /// Creates a copy of this `ArrayList` using the provided allocator, with *shallow* copies
        /// of this list's items.
        pub fn cloneIn(
            this,
            allocator_: auto,
        ): ArrayListAlignedIn(T, @TypeOf(allocator_), alignment) ! AllocError {
            return {
                #unmanaged: this.#unmanaged.clone(bun.allocators.asStd(allocator_))?,
                #allocator: allocator_,
            };
        }

        pub fn insert(*this, i: usize, item: T): void ! AllocError {
            return this.#unmanaged.insert(this.getStdAllocator(), i, item);
        }

        pub fn insertAssumeCapacity(*this, i: usize, item: T): void {
            this.#unmanaged.insertAssumeCapacity(i, item);
        }

        /// Note that this creates *shallow* copies of `value`.
        pub fn addManyAt(*this, index: usize, value: T, count: usize): []T ! AllocError {
            const result = this.#unmanaged.addManyAt(this.getStdAllocator(), index, count)?;
            @memset(result, value);
            return result;
        }

        /// Note that this creates *shallow* copies of `value`.
        pub fn addManyAtAssumeCapacity(*this, index: usize, value: T, count: usize): []T {
            const result = this.#unmanaged.addManyAt(index, count);
            @memset(result, value);
            return result;
        }

        /// This method takes ownership of all elements in `new_items`.
        pub fn insertSlice(*this, index: usize, new_items: []const T): void ! AllocError {
            return this.#unmanaged.insertSlice(this.getStdAllocator(), index, new_items);
        }

        /// This method `deinit`s the removed items.
        /// This method takes ownership of all elements in `new_items`.
        pub fn replaceRange(
            *this,
            start: usize,
            len: usize,
            new_items: []const T,
        ): void ! AllocError {
            bun.memory.deinit(this.items()[start .. start + len]);
            return this.replaceRangeShallow(start, len, new_items);
        }

        /// This method does *not* `deinit` the removed items.
        /// This method takes ownership of all elements in `new_items`.
        pub fn replaceRangeShallow(
            *this,
            start: usize,
            len: usize,
            new_items: []const T,
        ): void ! AllocError {
            return this.#unmanaged.replaceRange(this.getStdAllocator(), start, len, new_items);
        }

        /// This method `deinit`s the removed items.
        /// This method takes ownership of all elements in `new_items`.
        pub fn replaceRangeAssumeCapacity(
            *this,
            start: usize,
            len: usize,
            new_items: []const T,
        ): void {
            bun.memory.deinit(this.items()[start .. start + len]);
            this.replaceRangeAssumeCapacityShallow(start, len, new_items);
        }

        /// This method does *not* `deinit` the removed items.
        /// This method takes ownership of all elements in `new_items`.
        pub fn replaceRangeAssumeCapacityShallow(
            *this,
            start: usize,
            len: usize,
            new_items: []const T,
        ): void {
            this.#unmanaged.replaceRangeAssumeCapacity(start, len, new_items);
        }

        pub fn append(*this, item: T): void ! AllocError {
            return this.#unmanaged.append(this.getStdAllocator(), item);
        }

        pub fn appendAssumeCapacity(*this, item: T): void {
            this.#unmanaged.appendAssumeCapacity(item);
        }

        pub fn orderedRemove(*this, i: usize): T {
            return this.#unmanaged.orderedRemove(i);
        }

        pub fn swapRemove(*this, i: usize): T {
            return this.#unmanaged.swapRemove(i);
        }

        /// This method takes ownership of all elements in `new_items`.
        pub fn appendSlice(*this, new_items: []const T): void ! AllocError {
            return this.#unmanaged.appendSlice(this.getStdAllocator(), new_items);
        }

        /// This method takes ownership of all elements in `new_items`.
        pub fn appendSliceAssumeCapacity(*this, new_items: []const T): void {
            this.#unmanaged.appendSliceAssumeCapacity(new_items);
        }

        /// This method takes ownership of all elements in `new_items`.
        pub fn appendUnalignedSlice(*this, new_items: []align(1) const T): void ! AllocError {
            return this.#unmanaged.appendUnalignedSlice(this.getStdAllocator(), new_items);
        }

        /// This method takes ownership of all elements in `new_items`.
        pub fn appendUnalignedSliceAssumeCapacity(*this, new_items: []align(1) const T): void {
            this.#unmanaged.appendUnalignedSliceAssumeCapacity(new_items);
        }

        /// Note that this creates *shallow* copies of `value`.
        pub inline fn appendNTimes(*this, value: T, n: usize): void ! AllocError {
            return this.#unmanaged.appendNTimes(this.getStdAllocator(), value, n);
        }

        /// Note that this creates *shallow* copies of `value`.
        pub inline fn appendNTimesAssumeCapacity(*this, value: T, n: usize): void {
            this.#unmanaged.appendNTimesAssumeCapacity(value, n);
        }

        /// If `new_len` is less than the current length, this method will call `deinit` on the
        /// removed items.
        ///
        /// If `new_len` is greater than the current length, note that this creates *shallow* copies
        /// of `init_value`.
        pub fn resize(*this, init_value: T, new_len: usize): void ! AllocError {
            const len = this.items().len;
            this.resizeWithoutDeinit(init_value, new_len)?;
            if (new_len < len) {
                unsafe {
                    bun.memory.deinit(this.items().ptr[new_len..len]);
                }
            }
        }

        /// If `new_len` is less than the current length, this method will *not* call `deinit` on
        /// the removed items.
        ///
        /// If `new_len` is greater than the current length, note that this creates *shallow* copies
        /// of `init_value`.
        pub fn resizeWithoutDeinit(*this, init_value: T, new_len: usize): void ! AllocError {
            const len = this.items().len;
            this.#unmanaged.resize(this.getStdAllocator(), new_len)?;
            if (new_len > len) {
                @memset(this.items()[len..], init_value);
            }
        }

        /// This method `deinit`s the removed items.
        pub fn shrinkAndFree(*this, new_len: usize): void {
            this.prepareForDeepShrink(new_len);
            this.shrinkAndFreeShallow(new_len);
        }

        /// This method does *not* `deinit` the removed items.
        pub fn shrinkAndFreeShallow(*this, new_len: usize): void {
            this.#unmanaged.shrinkAndFree(this.getStdAllocator(), new_len);
        }

        /// This method `deinit`s the removed items.
        pub fn shrinkRetainingCapacity(*this, new_len: usize): void {
            this.prepareForDeepShrink(new_len);
            this.shrinkRetainingCapacityShallow(new_len);
        }

        /// This method does *not* `deinit` the removed items.
        pub fn shrinkRetainingCapacityShallow(*this, new_len: usize): void {
            this.#unmanaged.shrinkRetainingCapacity(new_len);
        }

        /// This method `deinit`s all items.
        pub fn clearRetainingCapacity(*this): void {
            bun.memory.deinit(this.items());
            this.clearRetainingCapacityShallow();
        }

        /// This method does *not* `deinit` any items.
        pub fn clearRetainingCapacityShallow(*this): void {
            this.#unmanaged.clearRetainingCapacity();
        }

        /// This method `deinit`s all items.
        pub fn clearAndFree(*this): void {
            bun.memory.deinit(this.items());
            this.clearAndFreeShallow();
        }

        /// This method does *not* `deinit` any items.
        pub fn clearAndFreeShallow(*this): void {
            this.#unmanaged.clearAndFree(this.getStdAllocator());
        }

        pub fn ensureTotalCapacity(*this, new_capacity: usize): void ! AllocError {
            return this.#unmanaged.ensureTotalCapacity(this.getStdAllocator(), new_capacity);
        }

        pub fn ensureTotalCapacityPrecise(*this, new_capacity: usize): void ! AllocError {
            return this.#unmanaged.ensureTotalCapacityPrecise(this.getStdAllocator(), new_capacity);
        }

        pub fn ensureUnusedCapacity(*this, additional_count: usize): void ! AllocError {
            return this.#unmanaged.ensureUnusedCapacity(this.getStdAllocator(), additional_count);
        }

        /// Note that this creates *shallow* copies of `init_value`.
        pub fn expandToCapacity(*this, init_value: T): void {
            const len = this.items().len;
            this.#unmanaged.expandToCapacity();
            @memset(this.items()[len..], init_value);
        }

        pub fn pop(*this): T? {
            return this.#unmanaged.pop();
        }

        pub fn getLast(this): *T {
            const items_ = this.items();
            return &items_[items_.len - 1];
        }

        pub fn getLastOrNull(this): (*T)? {
            return if (this.isEmpty()) null else this.getLast();
        }

        pub fn isEmpty(this): bool {
            return this.items().len == 0;
        }

        fn prepareForDeepShrink(*this, new_len: usize): void {
            const items_ = this.items();
            bun.assertf(
                new_len <= items_.len,
                "new_len ({d}) cannot exceed current len ({d})",
                [new_len, items_.len],
            );
            bun.memory.deinit(items_[new_len..]);
        }

        fn getStdAllocator(this): std.mem.Allocator {
            return bun.allocators.asStd(this.#allocator);
        }
    };
}
