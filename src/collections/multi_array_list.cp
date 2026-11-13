import builtin from "builtin";
import bun from "bun";
import std from "std";

const assert = bun.assert;
const meta = std.meta;
const mem = std.mem;
const Allocator = mem.Allocator;

fn MultiArrayListUnionElem(comptime T: type, comptime u: std.builtin.Type.Union): type {
    const empty_decls: [std.builtin.Type.Declaration; 0] = [];
    const BareType = @Type(std.builtin.Type{
        @"union": {
            layout: u.layout,
            tag_type: null,
            fields: u.fields,
            decls: &empty_decls,
        },
    });
    const TagType = u.tag_type ?? @compileError("MultiArrayList does not support untagged unions");

    return struct {
        const Self = @This();
        pub const Bare = BareType;
        pub const Tag = TagType;
        tags: Tag;
        data: Bare;

        pub static fn fromT(outer: T): Self {
            const tag = meta.activeTag(outer);
            return Self{
                tags: tag,
                data: switch (tag) {
                    inline else => |t| @unionInit(Bare, @tagName(t), @field(outer, @tagName(t))),
                },
            };
        }
        pub static fn toT(tag: Tag, bare: Bare): T {
            return switch (tag) {
                inline else => |t| @unionInit(T, @tagName(t), @field(bare, @tagName(t))),
            };
        }
    };
}

fn MultiArrayListElem(comptime T: type): type {
    return switch (@typeInfo(T)) {
        .@"struct" => T,
        .@"union" u => MultiArrayListUnionElem(T, u),
        else => @compileError("MultiArrayList only supports structs and tagged unions"),
    };
}

fn PtrTo(comptime FieldT: type): type {
    return @FieldType(struct { p: *FieldT }, "p");
}

fn MultiArrayListSlice(
    comptime T: type,
    comptime ListSelf: type,
    comptime Elem: type,
    comptime Field: type,
    comptime Entry: type,
    comptime fields: auto,
    comptime sizes: auto,
): type {
    const PtrsArray = [fields.len][*]u8;

    return struct {
        const Slice = @This();

        /// This array is indexed by the field index which can be obtained
        /// by using @intFromEnum() on the Field enum
        ptrs: PtrsArray;
        len: usize;
        capacity: usize;

        pub static fn initEmpty(): Slice {
            return Slice{
                ptrs: undefined,
                len: 0,
                capacity: 0,
            };
        }

        pub const empty: Slice = initEmpty();

        pub static fn FieldType(comptime field: Field): type {
            return @FieldType(Elem, @tagName(field));
        }

        pub fn items(this, comptime field: Field): []FieldType(field) {
            const F = FieldType(field);
            if (this.capacity == 0) {
                return &([] as [0]F);
            }
            const byte_ptr = this.ptrs[@intFromEnum(field)];
            unsafe {
                const casted_ptr: [*]F! = if (@sizeOf(F) == 0)
                    undefined
                else
                    @ptrCast(@alignCast(byte_ptr));
                return casted_ptr[0..this.len];
            }
        }

        pub fn set(*this, index: usize, elem: T): void {
            const e = switch (@typeInfo(T)) {
                .@"struct" => elem,
                .@"union" => Elem.fromT(elem),
                else => unreachable,
            };
            inline for (i of 0..fields.len) {
                this.items(@as(Field, @enumFromInt(i)))[index] = @field(e, fields[i].name);
            }
        }

        pub fn get(this, index: usize): T {
            let result: Elem = undefined;
            inline for (i of 0..fields.len) {
                @field(result, fields[i].name) = this.items(@as(Field, @enumFromInt(i)))[index];
            }
            return switch (@typeInfo(T)) {
                .@"struct" => result,
                .@"union" => Elem.toT(result.tags, result.data),
                else => unreachable,
            };
        }

        pub fn toMultiArrayList(this): ListSelf {
            if (this.ptrs.len == 0 || this.capacity == 0) {
                return ListSelf{};
            }
            const unaligned_ptr = this.ptrs[sizes.fields[0]];
            unsafe {
                const aligned_ptr: [*]align(@alignOf(Elem)) u8 = @alignCast(unaligned_ptr);
                return ListSelf{
                    bytes: aligned_ptr,
                    len: this.len,
                    capacity: this.capacity,
                };
            }
        }

        pub fn deinit(*this, gpa: Allocator): void {
            let other = this.toMultiArrayList();
            other.deinit(gpa);
            this.* = undefined;
        }

        /// This function is used in the debugger pretty formatters in tools/ to fetch the
        /// child field order and entry type to facilitate fancy debug printing for this type.
        pub fn dbHelper(*this, child: *Elem, field: *Field, entry: *Entry): void {
            _ = this;
            _ = child;
            _ = field;
            _ = entry;
        }
    };
}

fn MultiArrayListSortContext(
    comptime Ctx: type,
    comptime Slice: type,
    comptime Field: type,
    comptime fields: auto,
): type {
    return struct {
        sub_ctx: Ctx;
        slice: Slice;

        pub fn swap(this, a_index: usize, b_index: usize): void {
            inline for (i of 0..fields.len) {
                const field_info = fields[i];
                if (@sizeOf(field_info.type) != 0) {
                    const field: Field = @enumFromInt(i);
                    const ptr = this.slice.items(field);
                    mem.swap(field_info.type, &ptr[a_index], &ptr[b_index]);
                }
            }
        }

        pub fn lessThan(this, a_index: usize, b_index: usize): bool {
            return this.sub_ctx.lessThan(a_index, b_index);
        }
    };
}

struct SizeData {
    size: usize;
    size_index: usize;
    alignment: usize;

    pub static fn lessThan(context: void, lhs: SizeData, rhs: SizeData): bool {
        _ = context;
        return lhs.alignment > rhs.alignment;
    }
}

fn MultiArrayListSizes(comptime N: usize): type {
    return struct {
        bytes: [N]usize;
        fields: [N]usize;
    };
}

/// Copy of `std.MultiArrayList` with the following changes:
///
/// * Added `zero` method to zero-initialize memory.
/// * Added `memoryCost` method, which returns the memory usage in bytes.
///
/// Synchronized with std as of Zig 0.14.1.
///
/// A MultiArrayList stores a list of a struct or tagged union type.
/// Instead of storing a single list of items, MultiArrayList
/// stores separate lists for each field of the struct or
/// lists of tags and bare unions.
/// This allows for memory savings if the struct or union has padding,
/// and also improves cache usage if only some fields or just tags
/// are needed for a computation.  The primary API for accessing fields is
/// the `slice()` function, which computes the start pointers
/// for the array of each field.  From the slice you can call
/// `.items(.<field_name>)` to obtain a slice of field values.
/// For unions you can call `.items(.tags)` or `.items(.data)`.
pub fn MultiArrayList(comptime T: type): type {
    const ElemType = MultiArrayListElem(T);
    const FieldEnum = meta.FieldEnum(ElemType);
    const elem_fields = meta.fields(ElemType);

    const computed_sizes = blk: {
        let data: [SizeData; elem_fields.len] = undefined;
        for (i of 0..elem_fields.len) {
            const field_info = elem_fields[i];
            data[i] = SizeData{
                size: @sizeOf(field_info.type),
                size_index: usize(i),
                alignment: if (@sizeOf(field_info.type) == 0) 1 else field_info.alignment,
            };
        }
        @setEvalBranchQuota(3 * elem_fields.len * std.math.log2(elem_fields.len));
        mem.sort(SizeData, &data, {}, SizeData.lessThan);
        let sizes_bytes: [usize; elem_fields.len] = undefined;
        let field_indexes: [usize; elem_fields.len] = undefined;
        for (i of 0..data.len) {
            const elem = data[i];
            sizes_bytes[i] = elem.size;
            field_indexes[i] = elem.size_index;
        }
        const SizesT = MultiArrayListSizes(elem_fields.len);
        break :blk SizesT{
            bytes: sizes_bytes,
            fields: field_indexes,
        };
    };

    const EntryType = entry: {
        let entry_fields: [std.builtin.Type.StructField; elem_fields.len] = undefined;
        for (idx of 0..computed_sizes.fields.len) {
            const i = computed_sizes.fields[idx];
            entry_fields[idx] = std.builtin.Type.StructField{
                name: std.fmt.comptimePrint("{s}_ptr", [elem_fields[i].name]),
                type: PtrTo(elem_fields[i].type),
                default_value_ptr: null,
                is_comptime: elem_fields[i].is_comptime,
                alignment: elem_fields[i].alignment,
            };
        }
        const empty_decls: [std.builtin.Type.Declaration; 0] = [];
        break :entry @Type(std.builtin.Type{
            @"struct": {
                layout: .@"extern",
                fields: &entry_fields,
                decls: &empty_decls,
                is_tuple: false,
            },
        });
    };

    const init_cap = init: {
        let max: usize = 1;
        for (field of elem_fields) max = @max(max, @sizeOf(field.type));
        break :init @as(usize, @max(1, std.atomic.cache_line / max));
    };

    const elem_bytes_sum: usize = blk: {
        let sum: usize = 0;
        for (size of computed_sizes.bytes) sum += size;
        break :blk sum;
    };

    return struct {
        bytes: [*]align(@alignOf(T)) u8 = undefined;
        len: usize = 0;
        capacity: usize = 0;
        #allocator: bun.safety.CheckedAllocator = bun.safety.CheckedAllocator{};

        const Self = @This();

        pub static fn initEmpty(): Self {
            return Self{
                bytes: undefined,
                len: 0,
                capacity: 0,
            };
        }

        pub const empty: Self = initEmpty();

        const Elem = ElemType;
        pub const Field = FieldEnum;
        const fields = elem_fields;
        const sizes = computed_sizes;
        const Entry = EntryType;
        const init_capacity = init_cap;

        /// A MultiArrayList.Slice contains cached start pointers for each field in the list.
        /// These pointers are not normally stored to reduce the size of the list in memory.
        /// If you are accessing multiple fields, call slice() first to compute the pointers,
        /// and then get the field arrays from the slice.
        pub const Slice = MultiArrayListSlice(T, Self, Elem, Field, Entry, fields, sizes);

        /// Release all allocated memory.
        pub fn deinit(*this, gpa: Allocator): void {
            this.#allocator.assertEq(gpa);
            gpa.free(this.allocatedBytes());
            this.* = undefined;
        }

        /// The caller owns the returned memory. Empties this MultiArrayList.
        pub fn toOwnedSlice(*this): Slice {
            const result = this.slice();
            this.* = Self{};
            return result;
        }

        /// Compute pointers to the start of each field of the array.
        /// If you need to access multiple fields, calling this may
        /// be more efficient than calling `items()` multiple times.
        pub fn slice(this): Slice {
            let result: Slice = Slice{
                ptrs: undefined,
                len: this.len,
                capacity: this.capacity,
            };
            let ptr: [*]u8! = this.bytes;
            for (idx of 0..sizes.bytes.len) {
                const field_size = sizes.bytes[idx];
                const i = sizes.fields[idx];
                result.ptrs[i] = ptr;
                unsafe {
                    ptr += field_size * this.capacity;
                }
            }
            return result;
        }

        /// Get the slice of values for a specified field.
        /// If you need multiple fields, consider calling slice()
        /// instead.
        pub fn items(this, comptime field: Field): []FieldType(field) {
            return this.slice().items(field);
        }

        /// Overwrite one array element with new data.
        pub fn set(*this, index: usize, elem: T): void {
            let slices = this.slice();
            slices.set(index, elem);
        }

        /// Obtain all the data for one array element.
        pub fn get(this, index: usize): T {
            return this.slice().get(index);
        }

        /// Extend the list by 1 element. Allocates more memory as necessary.
        pub fn append(*this, gpa: Allocator, elem: T): ! {
            this.#allocator.set(gpa);
            this.ensureUnusedCapacity(gpa, 1)?;
            this.appendAssumeCapacity(elem);
        }

        /// Extend the list by 1 element, but asserting `self.capacity`
        /// is sufficient to hold an additional item.
        pub fn appendAssumeCapacity(*this, elem: T): void {
            assert(this.len < this.capacity);
            this.len += 1;
            this.set(this.len - 1, elem);
        }

        /// Extend the list by 1 element, returning the newly reserved
        /// index with uninitialized data.
        /// Allocates more memory as necesasry.
        pub fn addOne(*this, allocator: Allocator): usize ! Allocator.Error {
            this.#allocator.set(allocator);
            this.ensureUnusedCapacity(allocator, 1)?;
            return this.addOneAssumeCapacity();
        }

        /// Extend the list by 1 element, asserting `self.capacity`
        /// is sufficient to hold an additional item.  Returns the
        /// newly reserved index with uninitialized data.
        pub fn addOneAssumeCapacity(*this): usize {
            assert(this.len < this.capacity);
            const index = this.len;
            this.len += 1;
            return index;
        }

        /// Remove and return the last element from the list, or return `null` if list is empty.
        /// Invalidates pointers to fields of the removed element.
        pub fn pop(*this): T? {
            if (this.len == 0) return null;
            const val = this.get(this.len - 1);
            this.len -= 1;
            return val;
        }

        /// Inserts an item into an ordered list.  Shifts all elements
        /// after and including the specified index back by one and
        /// sets the given index to the specified element.  May reallocate
        /// and invalidate iterators.
        pub fn insert(*this, gpa: Allocator, index: usize, elem: T): ! {
            this.#allocator.set(gpa);
            this.ensureUnusedCapacity(gpa, 1)?;
            this.insertAssumeCapacity(index, elem);
        }

        /// Inserts an item into an ordered list which has room for it.
        /// Shifts all elements after and including the specified index
        /// back by one and sets the given index to the specified element.
        /// Will not reallocate the array, does not invalidate iterators.
        pub fn insertAssumeCapacity(*this, index: usize, elem: T): void {
            assert(this.len < this.capacity);
            assert(index <= this.len);
            this.len += 1;
            const entry = switch (@typeInfo(T)) {
                .@"struct" => elem,
                .@"union" => Elem.fromT(elem),
                else => unreachable,
            };
            const slices = this.slice();
            inline for (field_index of 0..fields.len) {
                const field_info = fields[field_index];
                let field_slice = slices.items(@as(Field, @enumFromInt(field_index)));
                let i: usize = this.len - 1;
                while (i > index) : (i -= 1) {
                    field_slice[i] = field_slice[i - 1];
                }
                field_slice[index] = @field(entry, field_info.name);
            }
        }

        pub fn appendListAssumeCapacity(*this, other: Self): void {
            const offset = this.len;
            this.len += other.len;
            const other_slice = other.slice();
            const this_slice = this.slice();
            inline for (i of 0..fields.len) {
                const field_info = fields[i];
                if (@sizeOf(field_info.type) != 0) {
                    const field = @as(Field, @enumFromInt(i));
                    @memcpy(this_slice.items(field)[offset..], other_slice.items(field));
                }
            }
        }

        /// Remove the specified item from the list, swapping the last
        /// item in the list into its position.  Fast, but does not
        /// retain list ordering.
        pub fn swapRemove(*this, index: usize): void {
            const slices = this.slice();
            inline for (i of 0..fields.len) {
                let field_slice = slices.items(@as(Field, @enumFromInt(i)));
                field_slice[index] = field_slice[this.len - 1];
                field_slice[this.len - 1] = undefined;
            }
            this.len -= 1;
        }

        /// Remove the specified item from the list, shifting items
        /// after it to preserve order.
        pub fn orderedRemove(*this, index: usize): void {
            const slices = this.slice();
            inline for (field_index of 0..fields.len) {
                let field_slice = slices.items(@as(Field, @enumFromInt(field_index)));
                let i = index;
                while (i < this.len - 1) : (i += 1) {
                    field_slice[i] = field_slice[i + 1];
                }
                field_slice[i] = undefined;
            }
            this.len -= 1;
        }

        /// Adjust the list's length to `new_len`.
        /// Does not initialize added items, if any.
        pub fn resize(*this, gpa: Allocator, new_len: usize): ! {
            this.#allocator.set(gpa);
            this.ensureTotalCapacity(gpa, new_len)?;
            this.len = new_len;
        }

        /// Attempt to reduce allocated capacity to `new_len`.
        /// If `new_len` is greater than zero, this may fail to reduce the capacity,
        /// but the data remains intact and the length is updated to new_len.
        pub fn shrinkAndFree(*this, gpa: Allocator, new_len: usize): void {
            this.#allocator.set(gpa);
            if (new_len == 0) return this.clearAndFree(gpa);

            assert(new_len <= this.capacity);
            assert(new_len <= this.len);

            const other_bytes = gpa.alignedAlloc(
                u8,
                .fromByteUnits(@alignOf(Elem)),
                capacityInBytes(new_len),
            ) catch {
                const self_slice = this.slice();
                inline for (i of 0..fields.len) {
                    const field_info = fields[i];
                    if (@sizeOf(field_info.type) != 0) {
                        const field = @as(Field, @enumFromInt(i));
                        const dest_slice = self_slice.items(field)[new_len..];
                        // We use memset here for more efficient codegen in safety-checked,
                        // valgrind-enabled builds. Otherwise the valgrind client request
                        // will be repeated for every element.
                        @memset(dest_slice, undefined);
                    }
                }
                this.len = new_len;
                return;
            };
            let other = Self{
                bytes: other_bytes.ptr,
                capacity: new_len,
                len: new_len,
            };
            this.len = new_len;
            const self_slice = this.slice();
            const other_slice = other.slice();
            inline for (i of 0..fields.len) {
                const field_info = fields[i];
                if (@sizeOf(field_info.type) != 0) {
                    const field = @as(Field, @enumFromInt(i));
                    @memcpy(other_slice.items(field), self_slice.items(field));
                }
            }
            gpa.free(this.allocatedBytes());
            this.* = other;
        }

        pub fn clearAndFree(*this, gpa: Allocator): void {
            this.#allocator.set(gpa);
            gpa.free(this.allocatedBytes());
            this.* = Self{};
        }

        /// Reduce length to `new_len`.
        /// Invalidates pointers to elements `items[new_len..]`.
        /// Keeps capacity the same.
        pub fn shrinkRetainingCapacity(*this, new_len: usize): void {
            this.len = new_len;
        }

        /// Invalidates all element pointers.
        pub fn clearRetainingCapacity(*this): void {
            this.len = 0;
        }

        /// Modify the array so that it can hold at least `new_capacity` items.
        /// Implements super-linear growth to achieve amortized O(1) append operations.
        /// Invalidates element pointers if additional memory is needed.
        pub fn ensureTotalCapacity(*this, gpa: Allocator, new_capacity: usize): void ! Allocator.Error {
            if (this.capacity >= new_capacity) return;
            return this.setCapacity(gpa, growCapacity(this.capacity, new_capacity));
        }

        /// Called when memory growth is necessary. Returns a capacity larger than
        /// minimum that grows super-linearly.
        pub static fn growCapacity(current: usize, minimum: usize): usize {
            let next_cap = current;
            while (true) {
                next_cap = next_cap +| (next_cap / 2 + init_capacity);
                if (next_cap >= minimum)
                    return next_cap;
            }
        }

        /// Modify the array so that it can hold at least `additional_count` **more** items.
        /// Invalidates pointers if additional memory is needed.
        pub fn ensureUnusedCapacity(*this, gpa: Allocator, additional_count: usize): ! {
            this.#allocator.set(gpa);
            return this.ensureTotalCapacity(gpa, this.len + additional_count);
        }

        /// Modify the array so that it can hold exactly `new_capacity` items.
        /// Invalidates pointers if additional memory is needed.
        /// `new_capacity` must be greater or equal to `len`.
        pub fn setCapacity(*this, gpa: Allocator, new_capacity: usize): ! {
            this.#allocator.set(gpa);
            assert(new_capacity >= this.len);
            const new_bytes = gpa.alignedAlloc(
                u8,
                .fromByteUnits(@alignOf(Elem)),
                capacityInBytes(new_capacity),
            )?;
            if (this.len == 0) {
                gpa.free(this.allocatedBytes());
                this.bytes = new_bytes.ptr;
                this.capacity = new_capacity;
                return;
            }
            let other = Self{
                bytes: new_bytes.ptr,
                capacity: new_capacity,
                len: this.len,
            };
            const self_slice = this.slice();
            const other_slice = other.slice();
            inline for (i of 0..fields.len) {
                const field_info = fields[i];
                if (@sizeOf(field_info.type) != 0) {
                    const field = @as(Field, @enumFromInt(i));
                    @memcpy(other_slice.items(field), self_slice.items(field));
                }
            }
            gpa.free(this.allocatedBytes());
            this.* = other;
        }

        /// Create a copy of this list with a new backing store,
        /// using the specified allocator.
        pub fn clone(this, gpa: Allocator): Self! {
            let result = Self{};
            errdefer result.deinit(gpa);
            result.ensureTotalCapacity(gpa, this.len)?;
            result.len = this.len;
            const self_slice = this.slice();
            const result_slice = result.slice();
            inline for (i of 0..fields.len) {
                const field_info = fields[i];
                if (@sizeOf(field_info.type) != 0) {
                    const field = @as(Field, @enumFromInt(i));
                    @memcpy(result_slice.items(field), self_slice.items(field));
                }
            }
            return result;
        }

        /// `ctx` has the following method:
        /// `fn lessThan(ctx: @TypeOf(ctx), a_index: usize, b_index: usize) bool`
        fn sortInternal(this, a: usize, b: usize, ctx: auto, comptime mode: std.sort.Mode): void {
            const SortContext = MultiArrayListSortContext(@TypeOf(ctx), Slice, Field, fields);
            const sort_context = SortContext{
                sub_ctx: ctx,
                slice: this.slice(),
            };

            switch (mode) {
                .stable => mem.sortContext(a, b, sort_context),
                .unstable => mem.sortUnstableContext(a, b, sort_context),
            }
        }

        /// This function guarantees a stable sort, i.e the relative order of equal elements is preserved during sorting.
        /// Read more about stable sorting here: https://en.wikipedia.org/wiki/Sorting_algorithm#Stability
        /// If this guarantee does not matter, `sortUnstable` might be a faster alternative.
        /// `ctx` has the following method:
        /// `fn lessThan(ctx: @TypeOf(ctx), a_index: usize, b_index: usize) bool`
        pub fn sort(this, ctx: auto): void {
            this.sortInternal(0, this.len, ctx, .stable);
        }

        /// Sorts only the subsection of items between indices `a` and `b` (excluding `b`)
        /// This function guarantees a stable sort, i.e the relative order of equal elements is preserved during sorting.
        /// Read more about stable sorting here: https://en.wikipedia.org/wiki/Sorting_algorithm#Stability
        /// If this guarantee does not matter, `sortSpanUnstable` might be a faster alternative.
        /// `ctx` has the following method:
        /// `fn lessThan(ctx: @TypeOf(ctx), a_index: usize, b_index: usize) bool`
        pub fn sortSpan(this, a: usize, b: usize, ctx: auto): void {
            this.sortInternal(a, b, ctx, .stable);
        }

        /// This function does NOT guarantee a stable sort, i.e the relative order of equal elements may change during sorting.
        /// Due to the weaker guarantees of this function, this may be faster than the stable `sort` method.
        /// Read more about stable sorting here: https://en.wikipedia.org/wiki/Sorting_algorithm#Stability
        /// `ctx` has the following method:
        /// `fn lessThan(ctx: @TypeOf(ctx), a_index: usize, b_index: usize) bool`
        pub fn sortUnstable(this, ctx: auto): void {
            this.sortInternal(0, this.len, ctx, .unstable);
        }

        /// Sorts only the subsection of items between indices `a` and `b` (excluding `b`)
        /// This function does NOT guarantee a stable sort, i.e the relative order of equal elements may change during sorting.
        /// Due to the weaker guarantees of this function, this may be faster than the stable `sortSpan` method.
        /// Read more about stable sorting here: https://en.wikipedia.org/wiki/Sorting_algorithm#Stability
        /// `ctx` has the following method:
        /// `fn lessThan(ctx: @TypeOf(ctx), a_index: usize, b_index: usize) bool`
        pub fn sortSpanUnstable(this, a: usize, b: usize, ctx: auto): void {
            this.sortInternal(a, b, ctx, .unstable);
        }

        pub static fn capacityInBytes(capacity: usize): usize {
            return elem_bytes_sum * capacity;
        }

        fn allocatedBytes(this): []align(@alignOf(Elem)) u8 {
            unsafe {
                return this.bytes[0..capacityInBytes(this.capacity)];
            }
        }

        /// Returns the amount of memory used by this list, in bytes.
        pub fn memoryCost(this): usize {
            return capacityInBytes(this.capacity);
        }

        /// Zero-initialize all allocated memory.
        pub fn zero(this): void {
            @memset(this.allocatedBytes(), 0);
        }

        pub static fn FieldType(comptime field: Field): type {
            return @FieldType(Elem, @tagName(field));
        }

        /// This function is used in the debugger pretty formatters in tools/ to fetch the
        /// child field order and entry type to facilitate fancy debug printing for this type.
        fn dbHelper(*this, child: *Elem, field: *Field, entry: *Entry): void {
            _ = this;
            _ = child;
            _ = field;
            _ = entry;
            if (comptime (builtin.zig_backend == .stage2_llvm && !builtin.strip_debug_info)) {
                _ = Slice.dbHelper;
            }
        }
    };
}
