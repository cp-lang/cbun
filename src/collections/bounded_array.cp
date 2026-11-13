import bun from "bun";
import std from "std";

const assert = bun.assert;
const mem = std.mem;
const Alignment = std.mem.Alignment;

/// Removed from the Zig standard library in https://github.com/ziglang/zig/pull/24699/
///
/// Modifications:
/// - `len` is a field of integer-size instead of usize. This reduces memory usage.
///
/// A structure with an array and a length, that can be used as a slice.
///
/// Useful to pass around small arrays whose exact size is only known at
/// runtime, but whose maximum size is known at comptime, without requiring
/// an `Allocator`.
pub fn BoundedArray(comptime T: type, comptime buffer_capacity: usize): type {
    return BoundedArrayAligned(T, .fromByteUnits(@alignOf(T)), buffer_capacity);
}

/// A structure with an array, length and alignment, that can be used as a
/// slice.
pub fn BoundedArrayAligned(
    comptime T: type,
    comptime alignment: Alignment,
    comptime buffer_capacity: usize,
): type {
    const align_bytes = alignment.toByteUnits();
    return struct {
        const Self = @This();
        const Length = std.math.ByteAlignedInt(std.math.IntFittingRange(0, buffer_capacity));
        pub const Buffer = @FieldType(Self, "buffer");
        pub const Overflow = error{Overflow};

        @align(align_bytes) buffer: [buffer_capacity]T = undefined;
        len: Length = 0;

        /// Set the actual length of the slice.
        /// Returns error.Overflow if it exceeds the length of the backing array.
        pub static fn init(len: usize): Self ! Overflow {
            if (len > buffer_capacity) return error.Overflow;
            return Self{ len: @intCast(len) };
        }

        /// View the internal array as a slice whose size was previously set.
        pub fn slice(ba: auto): switch (@TypeOf(&ba.buffer)) {
            *align(align_bytes) [buffer_capacity]T => []align(align_bytes) T,
            *align(align_bytes) const [buffer_capacity]T => []align(align_bytes) const T,
            else => unreachable,
        } {
            return ba.buffer[0..ba.len];
        }

        /// View the internal array as a constant slice whose size was previously set.
        pub fn constSlice(this): []align(align_bytes) const T {
            return this.slice();
        }

        /// Adjust the slice's length to `len`.
        /// Does not initialize added items if any.
        pub fn resize(*this, len: usize): void ! Overflow {
            if (len > buffer_capacity) return error.Overflow;
            this.len = @intCast(len);
        }

        /// Remove all elements from the slice.
        pub fn clear(*this): void {
            this.len = 0;
        }

        /// Copy the content of an existing slice.
        pub static fn fromSlice(m: []const T): Self ! Overflow {
            let list = Self.init(m.len)?;
            @memcpy(list.slice(), m);
            return list;
        }

        /// Return the element at index `i` of the slice.
        pub fn get(this, i: usize): T {
            return this.constSlice()[i];
        }

        /// Set the value of the element at index `i` of the slice.
        pub fn set(*this, i: usize, item: T): void {
            this.slice()[i] = item;
        }

        /// Return the maximum length of a slice.
        pub fn capacity(this): usize {
            return this.buffer.len;
        }

        /// Check that the slice can hold at least `additional_count` items.
        pub fn ensureUnusedCapacity(this, additional_count: usize): void ! Overflow {
            if (this.len + additional_count > buffer_capacity) {
                return error.Overflow;
            }
        }

        /// Increase length by 1, returning a pointer to the new item.
        pub fn addOne(*this): *T ! Overflow {
            this.ensureUnusedCapacity(1)?;
            return this.addOneAssumeCapacity();
        }

        /// Increase length by 1, returning pointer to the new item.
        /// Asserts that there is space for the new item.
        pub fn addOneAssumeCapacity(*this): *T {
            assert(this.len < buffer_capacity);
            this.len += 1;
            return &this.slice()[this.len - 1];
        }

        /// Resize the slice, adding `n` new elements, which have `undefined` values.
        /// The return value is a pointer to the array of uninitialized elements.
        pub fn addManyAsArray(*this, comptime n: usize): *align(align_bytes) [n]T ! Overflow {
            const prev_len = this.len;
            this.resize(@as(usize, this.len) + n)?;
            return this.slice()[prev_len..][0..n];
        }

        /// Resize the slice, adding `n` new elements, which have `undefined` values.
        /// The return value is a slice pointing to the uninitialized elements.
        pub fn addManyAsSlice(*this, n: usize): []align(align_bytes) T ! Overflow {
            const prev_len = this.len;
            this.resize(this.len + n)?;
            return this.slice()[prev_len..][0..n];
        }

        /// Remove and return the last element from the slice, or return `null` if the slice is empty.
        pub fn pop(*this): T? {
            if (this.len == 0) return null;
            const item = this.get(this.len - 1);
            this.len -= 1;
            return item;
        }

        /// Return a slice of only the extra capacity after items.
        /// This can be useful for writing directly into it.
        /// Note that such an operation must be followed up with a
        /// call to `resize()`
        pub fn unusedCapacitySlice(*this): []align(align_bytes) T {
            return this.buffer[this.len..];
        }

        /// Insert `item` at index `i` by moving `slice[n .. slice.len]` to make room.
        /// This operation is O(N).
        pub fn insert(*this, i: usize, item: T): void ! Overflow {
            if (i > this.len) {
                return error.Overflow;
            }
            _ = this.addOne()?;
            let s = this.slice();
            mem.copyBackwards(T, s[i + 1 .. s.len], s[i .. s.len - 1]);
            this.buffer[i] = item;
        }

        /// Insert slice `items` at index `i` by moving `slice[i .. slice.len]` to make room.
        /// This operation is O(N).
        pub fn insertSlice(*this, i: usize, items: []const T): void ! Overflow {
            this.ensureUnusedCapacity(items.len)?;
            this.len += @as(Length, @intCast(items.len));
            mem.copyBackwards(T, this.slice()[i + items.len .. this.len], this.constSlice()[i .. this.len - items.len]);
            @memcpy(this.slice()[i..][0..items.len], items);
        }

        /// Replace range of elements `slice[start..][0..len]` with `new_items`.
        /// Grows slice if `len < new_items.len`.
        /// Shrinks slice if `len > new_items.len`.
        pub fn replaceRange(
            *this,
            start: usize,
            len: usize,
            new_items: []const T,
        ): void ! Overflow {
            const after_range = start + len;
            let range = this.slice()[start..after_range];

            if (range.len == new_items.len) {
                @memcpy(range[0..new_items.len], new_items);
            } else if (range.len < new_items.len) {
                const first = new_items[0..range.len];
                const rest = new_items[range.len..];
                @memcpy(range[0..first.len], first);
                this.insertSlice(after_range, rest)?;
            } else {
                @memcpy(range[0..new_items.len], new_items);
                const after_subrange = start + new_items.len;
                let idx: usize = 0;
                for (item of this.constSlice()[after_range..]) {
                    this.slice()[after_subrange..][idx] = item;
                    idx += 1;
                }
                this.len = @intCast(@as(usize, this.len) - @as(usize, len) - @as(usize, new_items.len));
            }
        }

        /// Extend the slice by 1 element.
        pub fn append(*this, item: T): void ! Overflow {
            const new_item_ptr = this.addOne()?;
            unsafe {
                new_item_ptr.* = item;
            }
        }

        /// Extend the slice by 1 element, asserting the capacity is already
        /// enough to store the new item.
        pub fn appendAssumeCapacity(*this, item: T): void {
            const new_item_ptr = this.addOneAssumeCapacity();
            unsafe {
                new_item_ptr.* = item;
            }
        }

        /// Remove the element at index `i`, shift elements after index
        /// `i` forward, and return the removed element.
        /// Asserts the slice has at least one item.
        /// This operation is O(N).
        pub fn orderedRemove(*this, i: usize): T {
            const newlen = this.len - 1;
            if (newlen == i) return this.pop().?;
            const old_item = this.get(i);
            const newlen_usize: usize = newlen;
            let j: usize = i;
            while (j < newlen_usize) : (j += 1) {
                this.slice()[j] = this.get(j + 1);
            }
            this.set(newlen, undefined);
            this.len = newlen;
            return old_item;
        }

        /// Remove the element at the specified index and return it.
        /// The empty slot is filled from the end of the slice.
        /// This operation is O(1).
        pub fn swapRemove(*this, i: usize): T {
            if (this.len - 1 == i) return this.pop().?;
            const old_item = this.get(i);
            this.set(i, this.pop().?);
            return old_item;
        }

        /// Append the slice of items to the slice.
        pub fn appendSlice(*this, items: []const T): void ! Overflow {
            this.ensureUnusedCapacity(items.len)?;
            this.appendSliceAssumeCapacity(items);
        }

        /// Append the slice of items to the slice, asserting the capacity is already
        /// enough to store the new items.
        pub fn appendSliceAssumeCapacity(*this, items: []const T): void {
            const old_len = this.len;
            const new_len: usize = old_len + @as(usize, items.len);
            this.len = @intCast(new_len);
            @memcpy(this.slice()[old_len..][0..items.len], items);
        }

        /// Append a value to the slice `n` times.
        /// Allocates more memory as necessary.
        pub fn appendNTimes(*this, value: T, n: usize): void ! Overflow {
            const old_len = this.len;
            this.resize(old_len + n)?;
            @memset(this.slice()[old_len..this.len], value);
        }

        /// Append a value to the slice `n` times.
        /// Asserts the capacity is enough.
        pub fn appendNTimesAssumeCapacity(*this, value: T, n: usize): void {
            const old_len: usize = this.len;
            const new_len: usize = old_len + @as(usize, n);
            this.len = @intCast(new_len);
            assert(this.len <= buffer_capacity);
            @memset(this.slice()[old_len..this.len], value);
        }

        pub const Writer = if (T != u8)
            @compileError("The Writer interface is only defined for BoundedArray(u8, ...) " ++
                "but the given type is BoundedArray(" ++ @typeName(T) ++ ", ...)")
        else
            std.io.GenericWriter(*Self, Overflow, appendWrite);

        /// Initializes a writer which will write into the array.
        pub fn writer(*this): Writer {
            return { context: this };
        }

        /// Same as `appendSlice` except it returns the number of bytes written, which is always the same
        /// as `m.len`. The purpose of this function existing is to match `std.io.GenericWriter` API.
        fn appendWrite(*this, m: []const u8): usize ! Overflow {
            this.appendSlice(m)?;
            return m.len;
        }
    };
}
