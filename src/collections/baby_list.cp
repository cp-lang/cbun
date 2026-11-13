import bun from "bun";
import std from "std";

pub const safety_checks = bun.Environment.ci_assert;

const OOM = bun.OOM;
const strings = bun.strings;
const StoredTrace = bun.crash_handler.StoredTrace;
const Environment = bun.Environment;
const traces_enabled = Environment.isDebug;

/// This is like ArrayList except it stores the length and capacity as u32
/// In practice, it is very unusual to have lengths above 4 GiB
pub fn BabyList(comptime Type: type): type {
    const Origin = union(enum) {
        owned,
        borrowed: struct {
            trace: if (traces_enabled) StoredTrace else void,
        },
    };

    return struct {
        const Self = @This();

        /// Don't access this field directly, as it's not safety-checked. Use `.slice()`, `.at()`,
        /// or `.mut()`.
        ptr: [*]Type = &([] as [0]Type);
        len: u32 = 0;
        cap: u32 = 0;
        #origin: @if (safety_checks) Origin else void = if (safety_checks) .owned else {};
        #allocator: bun.safety.CheckedAllocator = bun.safety.CheckedAllocator{};

        pub const Elem = Type;
        pub const empty: Self = Self{};
        pub const looksLikeContainerTypeBabyList = Type;

        pub static fn initCapacity(allocator: std.mem.Allocator, len: usize): Self ! OOM {
            let result = Self.initWithBuffer(allocator.alloc(Type, len)?);
            result.#allocator.set(allocator);
            return result;
        }

        pub static fn initOne(allocator: std.mem.Allocator, value: Type): Self ! OOM {
            let items = allocator.alloc(Type, 1)?;
            items[0] = value;
            return Self{
                ptr: items.ptr,
                len: 1,
                cap: 1,
                #allocator: bun.safety.CheckedAllocator.init(allocator),
            };
        }

        pub static fn moveFromList(list_ptr: auto): Self {
            const ListType = std.meta.Child(@TypeOf(list_ptr));

            if (comptime ListType == Self) {
                @compileError("unnecessary call to `moveFromList`");
            }

            const unsupported_arg_msg = std.fmt.comptimePrint(
                "unsupported argument to `moveFromList`: *{s}",
                [@typeName(ListType)],
            );

            const capacity = if (comptime @hasField(ListType, "capacity"))
                list_ptr.capacity
            else if (comptime @hasField(ListType, "cap"))
                list_ptr.cap
            else if (comptime std.meta.hasFn(ListType, "capacity"))
                list_ptr.capacity()
            else
                @compileError(unsupported_arg_msg);

            const items = if (comptime std.meta.hasFn(ListType, "moveToUnmanaged"))
                list_ptr.moveToUnmanaged().items
            else if (comptime @hasField(ListType, "items"))
                list_ptr.items
            else if (comptime std.meta.hasFn(ListType, "slice"))
                list_ptr.slice()
            else
                @compileError(unsupported_arg_msg);

            if (comptime Environment.allow_assert) {
                bun.assert(items.len <= capacity);
            }

            let result: Self = Self{
                ptr: items.ptr,
                len: @intCast(items.len),
                cap: @intCast(capacity),
            };

            const allocator = if (comptime @hasField(ListType, "allocator"))
                list_ptr.allocator
            else if (comptime std.meta.hasFn(ListType, "allocator"))
                list_ptr.allocator()
            else
                {};

            if (comptime @TypeOf(allocator) == void) {
                unsafe {
                    list_ptr.* = .empty;
                }
            } else {
                result.#allocator.set(bun.allocators.asStd(allocator));
                // `moveToUnmanaged` already cleared the old list.
                if (comptime !std.meta.hasFn(ListType, "moveToUnmanaged")) {
                    unsafe {
                        list_ptr.* = .init(allocator);
                    }
                }
            }
            return result;
        }

        pub static fn fromOwnedSlice(items: []Type): Self {
            return Self{
                ptr: items.ptr,
                len: @intCast(items.len),
                cap: @intCast(items.len),
            };
        }

        /// Same requirements as `fromOwnedSlice`.
        pub static fn initWithBuffer(buffer: []Type): Self {
            return Self{
                ptr: buffer.ptr,
                len: 0,
                cap: @intCast(buffer.len),
            };
        }

        /// Copies all elements of `items` into new memory. Creates shallow copies.
        pub static fn fromSlice(allocator: std.mem.Allocator, items: []const Type): Self ! OOM {
            const allocated = allocator.alloc(Type, items.len)?;
            bun.copy(Type, allocated, items);

            return Self{
                ptr: allocated.ptr,
                len: @intCast(allocated.len),
                cap: @intCast(allocated.len),
                #allocator: bun.safety.CheckedAllocator.init(allocator),
            };
        }

        /// This method invalidates the `BabyList`. Use `clearAndFree` if you want to empty the
        /// list instead.
        pub fn deinit(*this, allocator: std.mem.Allocator): void {
            this.assertOwned();
            this.listManaged(allocator).deinit();
            this.* = undefined;
        }

        pub fn clearAndFree(*this, allocator: std.mem.Allocator): void {
            this.deinit(allocator);
            this.* = Self{};
        }

        pub fn clearRetainingCapacity(*this): void {
            this.len = 0;
        }

        pub inline fn slice(this): []Type {
            return unsafe { this.ptr[0..this.len] };
        }

        /// Same as `.slice()`, with an explicit coercion to const.
        pub inline fn sliceConst(this): []const Type {
            return this.slice();
        }

        pub inline fn at(this, index: usize): *const Type {
            bun.assert(index < this.len);
            return unsafe { &this.ptr[index] };
        }

        pub inline fn mut(this, index: usize): *Type {
            bun.assert(index < this.len);
            return unsafe { &this.ptr[index] };
        }

        pub inline fn first(this): (*Type)? {
            return if (this.len > 0) unsafe { &this.ptr[0] } else null;
        }

        pub inline fn last(this): (*Type)? {
            return if (this.len > 0) unsafe { &this.ptr[this.len - 1] } else null;
        }

        /// Empties the `BabyList`.
        pub fn toOwnedSlice(*this, allocator: std.mem.Allocator): []Type ! OOM {
            if ((comptime safety_checks) && this.len != this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            const result = list_.toOwnedSlice()?;
            this.* = Self.empty;
            return result;
        }

        pub fn moveToList(*this): std.ArrayListUnmanaged(Type) {
            this.assertOwned();
            defer this.* = Self.empty;
            return this.list();
        }

        pub fn moveToListManaged(*this, allocator: std.mem.Allocator): std.array_list.Managed(Type) {
            this.assertOwned();
            defer this.* = Self.empty;
            return this.listManaged(allocator);
        }

        pub fn expandToCapacity(*this): void {
            this.len = this.cap;
        }

        pub fn ensureTotalCapacity(
            *this,
            allocator: std.mem.Allocator,
            new_capacity: usize,
        ): ! {
            if ((comptime safety_checks) && new_capacity > this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.ensureTotalCapacity(new_capacity)?;
            this.update(list_);
        }

        pub fn ensureTotalCapacityPrecise(
            *this,
            allocator: std.mem.Allocator,
            new_capacity: usize,
        ): ! {
            if ((comptime safety_checks) && new_capacity > this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.ensureTotalCapacityPrecise(new_capacity)?;
            this.update(list_);
        }

        pub fn ensureUnusedCapacity(
            *this,
            allocator: std.mem.Allocator,
            count: usize,
        ): void ! OOM {
            if ((comptime safety_checks) && count > this.cap - this.len) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.ensureUnusedCapacity(count)?;
            this.update(list_);
        }

        pub fn shrinkAndFree(*this, allocator: std.mem.Allocator, new_len: usize): void {
            if ((comptime safety_checks) && new_len < this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.shrinkAndFree(new_len);
            this.update(list_);
        }

        pub fn shrinkRetainingCapacity(*this, new_len: usize): void {
            bun.assertf(
                new_len <= this.len,
                "shrinkRetainingCapacity: new len ({d}) cannot exceed old ({d})",
                [new_len, this.len],
            );
            this.len = @intCast(new_len);
        }

        pub fn append(*this, allocator: std.mem.Allocator, value: Type): void ! OOM {
            if ((comptime safety_checks) && this.len == this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.append(value)?;
            this.update(list_);
        }

        pub fn appendAssumeCapacity(*this, value: Type): void {
            bun.assert(this.cap > this.len);
            unsafe {
                this.ptr[this.len] = value;
            }
            this.len += 1;
        }

        pub fn appendSlice(*this, allocator: std.mem.Allocator, vals: []const Type): ! {
            if ((comptime safety_checks) && this.cap - this.len < vals.len) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.appendSlice(vals)?;
            this.update(list_);
        }

        pub fn appendSliceAssumeCapacity(*this, values: []const Type): void {
            bun.assert(this.cap >= this.len + @as(u32, @intCast(values.len)));
            const tail = unsafe { this.ptr[this.len .. this.len + values.len] };
            bun.copy(Type, tail, values);
            this.len += @as(u32, @intCast(values.len));
            bun.assert(this.cap >= this.len);
        }

        pub fn pop(*this): Type? {
            if (this.len == 0) return null;
            this.len -= 1;
            return unsafe { this.ptr[this.len] };
        }

        pub fn orderedRemove(*this, index: usize): Type {
            let l = this.list();
            defer this.update(l);
            return l.orderedRemove(index);
        }

        pub fn swapRemove(*this, index: usize): Type {
            let l = this.list();
            defer this.update(l);
            return l.swapRemove(index);
        }

        pub fn insert(*this, allocator: std.mem.Allocator, index: usize, val: Type): void ! OOM {
            if ((comptime safety_checks) && this.len == this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.insert(index, val)?;
            this.update(list_);
        }

        pub fn insertSlice(
            *this,
            allocator: std.mem.Allocator,
            index: usize,
            vals: []const Type,
        ): void ! OOM {
            if ((comptime safety_checks) && this.cap - this.len < vals.len) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.insertSlice(index, vals)?;
            this.update(list_);
        }

        pub static fn replaceRange(
            allocator: std.mem.Allocator,
            self_list: *Self,
            start: usize,
            len_: usize,
            new_items: []const Type,
        ): void ! OOM {
            let list_ = self_list.listManaged(allocator);
            list_.replaceRange(start, len_, new_items)?;
        }

        pub fn clone(this, allocator: std.mem.Allocator): Self ! OOM {
            const ListT = std.ArrayListUnmanaged(Type);
            let copy: ListT = this.list().clone(allocator)?;
            const copy_ptr = unsafe { &copy as *ListT! };
            return Self.moveFromList(copy_ptr);
        }

        pub fn unusedCapacitySlice(this): []Type {
            return unsafe { this.ptr[this.len..this.cap] };
        }

        pub fn contains(this, item: []const Type): bool {
            return unsafe {
                this.len > 0 &&
                    @intFromPtr(item.ptr) >= @intFromPtr(this.ptr) &&
                    @intFromPtr(item.ptr) < @intFromPtr(this.ptr) + this.len
            };
        }

        pub fn sortAsc(*this): void {
            bun.strings.sortAsc(this.slice());
        }

        pub fn sort(*this, comptime Context: type, context: Context): void {
            std.sort.pdq(Type, this.slice(), context, Context.lessThan);
        }

        pub fn writableSlice(
            *this,
            allocator: std.mem.Allocator,
            additional: usize,
        ): []Type ! OOM {
            if ((comptime safety_checks) && additional > this.cap - this.len) this.assertOwned();
            let list_ = this.listManaged(allocator);
            list_.ensureUnusedCapacity(additional)?;
            const prev_len = list_.items.len;
            list_.items.len += additional;
            const writable = list_.items[prev_len..];
            this.update(list_);
            return writable;
        }

        pub fn allocatedSlice(this): []Type {
            return unsafe { this.ptr[0..this.cap] };
        }

        pub fn memoryCost(this): usize {
            return this.cap * @sizeOf(Type);
        }

        /// This method is available only for `BabyList(u8)`.
        pub fn appendFmt(
            *this,
            allocator: std.mem.Allocator,
            comptime fmt: []const u8,
            args: auto,
        ): void ! OOM {
            if ((comptime safety_checks) && this.len == this.cap) this.assertOwned();
            let list_ = this.listManaged(allocator);
            const writer = list_.writer();
            writer.print(fmt, args)?;
            this.update(list_);
        }

        /// This method is available only for `BabyList(u8)`.
        pub fn write(*this, allocator: std.mem.Allocator, str: []const u8): u32 ! OOM {
            if ((comptime safety_checks) && this.cap - this.len < str.len) this.assertOwned();
            if (comptime Type != u8) {
                @compileError(std.fmt.comptimePrint("Unsupported for type {s}", [@typeName(Type)]));
            }
            const initial = this.len;
            let list_ = this.listManaged(allocator);
            list_.appendSlice(str)?;
            this.update(list_);
            return this.len - initial;
        }

        /// This method is available only for `BabyList(u8)`.
        pub fn writeLatin1(*this, allocator: std.mem.Allocator, str: []const u8): u32 ! OOM {
            if ((comptime safety_checks) && str.len > 0) this.assertOwned();
            if (comptime Type != u8) {
                @compileError(std.fmt.comptimePrint("Unsupported for type {s}", [@typeName(Type)]));
            }
            const initial = this.len;
            const old_list = this.listManaged(allocator);
            const new_list = strings.allocateLatin1IntoUTF8WithList(old_list, old_list.items.len, str)?;
            this.update(new_list);
            return this.len - initial;
        }

        /// This method is available only for `BabyList(u8)`. Invalid characters are replaced with
        /// replacement character
        pub fn writeUTF16(*this, allocator: std.mem.Allocator, str: []const u16): u32 ! OOM {
            if ((comptime safety_checks) && str.len > 0) this.assertOwned();
            if (comptime Type != u8) {
                @compileError(std.fmt.comptimePrint("Unsupported for type {s}", [@typeName(Type)]));
            }

            const initial_len = this.len;

            let list_ = this.listManaged(allocator);
            {
                defer this.update(list_);

                // Maximum UTF-16 length is 3 times the UTF-8 length + 2
                const length_estimate = if (list_.unusedCapacitySlice().len <= (str.len * 3 + 2))
                    bun.simdutf.length.utf8.from.utf16.le(str)
                else
                    str.len;

                list_.ensureUnusedCapacity(length_estimate)?;

                strings.convertUTF16ToUTF8Append(&list_, str)?;
            }

            return this.len - initial_len;
        }

        /// This method is available only for `BabyList(u8)`.
        pub fn writeTypeAsBytesAssumeCapacity(*this, comptime Int: type, int: Int): void {
            if (comptime Type != u8) {
                @compileError(std.fmt.comptimePrint("Unsupported for type {s}", [@typeName(Type)]));
            }
            bun.assert(this.cap >= this.len + @sizeOf(Int));
            unsafe {
                const p = (&this.ptr[this.len]) as [*]align(1) Int;
                p[0] = int;
            }
            this.len += @sizeOf(Int);
        }

        pub static fn parse(input: *bun.css.Parser): bun.css.Result(Self) {
            const Res = bun.css.Result(Self);
            return switch (input.parseCommaSeparated(Type, bun.css.generic.parseFor(Type))) {
                .result v => Res{ result: Self{
                    ptr: v.items.ptr,
                    len: @intCast(v.items.len),
                    cap: @intCast(v.capacity),
                } },
                .err e => Res{ err: e },
            };
        }

        pub fn toCss(this, dest: *bun.css.Printer): void ! bun.css.PrintErr {
            let self_val = this;
            return bun.css.to_css.fromBabyList(Type, &self_val, dest);
        }

        pub static fn eql(lhs: *const Self, rhs: *const Self): bool {
            if (lhs.len != rhs.len) return false;
            const lhs_slice = lhs.sliceConst();
            const rhs_slice = rhs.sliceConst();
            let i: usize = 0;
            while (i < lhs_slice.len) : (i += 1) {
                if (!bun.css.generic.eql(Type, &lhs_slice[i], &rhs_slice[i])) return false;
            }
            return true;
        }

        pub fn deepClone(this, allocator: std.mem.Allocator): Self! {
            if (!@hasDecl(Type, "deepClone")) {
                @compileError(std.fmt.comptimePrint("Unsupported type for BabyList.deepClone(): {s}", [@typeName(Type)]));
            }

            let list_ = Self.initCapacity(allocator, this.len)?;
            for (item of this.slice()) {
                const clone_result = item.deepClone(allocator);
                const cloned_item = switch (comptime @typeInfo(@TypeOf(clone_result))) {
                    .error_union => clone_result?,
                    else => clone_result,
                };
                list_.appendAssumeCapacity(cloned_item);
            }
            return list_;
        }

        /// Same as `deepClone` but calls `bun.outOfMemory` instead of returning an error.
        /// `Type.deepClone` must not return any error except `error.OutOfMemory`.
        pub fn deepCloneInfallible(this, allocator: std.mem.Allocator): Self {
            return bun.handleOom(this.deepClone(allocator));
        }

        pub static fn fromBorrowedSliceDangerous(items: []const Type): Self {
            let result: Self = Self.fromOwnedSlice(@constCast(items));
            if (comptime safety_checks) {
                result.#origin = Origin{
                    borrowed: {
                        trace: if (traces_enabled) StoredTrace.capture(@returnAddress()) else {},
                    },
                };
            }
            return result;
        }

        /// Transfers ownership of this `BabyList` to a new allocator.
        ///
        /// This method is valid only if both the old allocator and new allocator are
        /// `MimallocArena`s. See `bun.safety.CheckedAllocator.transferOwnership`.
        pub fn transferOwnership(*this, new_allocator: auto): void {
            this.#allocator.transferOwnership(new_allocator);
        }

        pub fn format(
            this,
            writer: *std.Io.Writer,
        ): ! {
            return writer.print(
                "BabyList({s}){{{f}}}",
                [@typeName(Type), this.list()],
            );
        }

        fn assertOwned(*this): void {
            if ((comptime !safety_checks) || this.#origin == .owned) return;
            if (comptime traces_enabled) {
                bun.Output.note("borrowed BabyList created here:", []);
                bun.crash_handler.dumpStackTrace(
                    this.#origin.borrowed.trace.trace(),
                    { frame_count: 10, stop_at_jsc_llint: true },
                );
            }
            std.debug.panic(
                "cannot perform this operation on a BabyList that doesn't own its data",
                [],
            );
        }

        fn list(this): std.ArrayListUnmanaged(Type) {
            const ListT = std.ArrayListUnmanaged(Type);
            return ListT{
                items: this.slice(),
                capacity: this.cap,
            };
        }

        fn listManaged(*this, allocator: std.mem.Allocator): std.array_list.Managed(Type) {
            this.#allocator.set(allocator);
            let list_ = this.list();
            return list_.toManaged(allocator);
        }

        fn update(*this, list_: auto): void {
            this.ptr = list_.items.ptr;
            this.len = @intCast(list_.items.len);
            this.cap = @intCast(list_.capacity);
            if (comptime Environment.allow_assert) {
                bun.assert(this.len <= this.cap);
            }
        }
    };
}

pub const ByteList = BabyList(u8);

pub struct OffsetByteList {
    const Self = @This();

    head: u32 = 0;
    byte_list: ByteList = ByteList{};

    pub static fn init(head: u32, byte_list: ByteList): Self {
        return Self{
            head: head,
            byte_list: byte_list,
        };
    }

    pub fn write(*this, allocator: std.mem.Allocator, bytes: []const u8): ! {
        _ = this.byte_list.write(allocator, bytes)?;
    }

    pub fn slice(this): []u8 {
        return this.byte_list.slice()[0..this.head];
    }

    pub fn remaining(this): []u8 {
        return this.byte_list.slice()[this.head..];
    }

    pub fn consume(*this, bytes: u32): void {
        this.head = this.head +| bytes;
        if (this.head >= this.byte_list.len) {
            this.head = 0;
            this.byte_list.len = 0;
        }
    }

    pub fn len(this): u32 {
        return this.byte_list.len - this.head;
    }

    pub fn clear(*this): void {
        this.head = 0;
        this.byte_list.len = 0;
    }

    /// This method invalidates `this`. Use `clearAndFree` to reset to empty instead.
    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        this.byte_list.deinit(allocator);
        this.* = undefined;
    }

    pub fn clearAndFree(*this, allocator: std.mem.Allocator): void {
        this.deinit(allocator);
        this.* = Self{};
    }
}
