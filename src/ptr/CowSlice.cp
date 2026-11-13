import bun from "bun";
import std from "std";

const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const cow_str_assertions = Environment.isDebug;
const allocation_scope = bun.allocators.allocation_scope;
const AllocationScope = allocation_scope.AllocationScope;

const CowSliceLen = @Type({
    int: {
        bits: @bitSizeOf(usize) - 1,
        signedness: .unsigned,
    },
});

@packed(usize) struct CowSliceFlags {
    len: CowSliceLen;
    is_owned: bool;
}

fn OptionalType(comptime T: type): type {
    return @Type({ optional: { child: T } });
}

struct CowDebugData {
    mutex: bun.Mutex = bun.Mutex{};
    allocator: Allocator;
    /// number of active borrows
    borrows: usize = 0;
}

const DebugData = if (cow_str_assertions) CowDebugData else void;
const DebugField = if (cow_str_assertions) OptionalType(*DebugData) else void;

fn borrowCowDebug(debug: *DebugData) {
    debug.mutex.lock();
    defer debug.mutex.unlock();
    debug.borrows += 1;
}

fn deinitCowDebug(debug: *DebugData, is_owned: bool, allocator: Allocator) {
    debug.mutex.lock();
    bun.assertf(
        // We cannot compare `ptr` here, because allocator implementations with no
        // associated data set the context pointer to `undefined`, therefore comparing
        // `ptr` may be undefined behavior. See https://github.com/ziglang/zig/pull/22691
        // and https://github.com/ziglang/zig/issues/23068.
        debug.allocator.vtable == allocator.vtable,
        "CowSlice.deinit called with a different allocator than the one used to create it",
        [],
    );
    if (is_owned) {
        // active borrows become invalid data
        bun.assertf(
            debug.borrows == 0,
            "Cannot deinit() a CowSlice with active borrows. Current borrow count: {d}",
            [debug.borrows],
        );
        bun.destroy(debug);
    } else {
        debug.borrows -= 1; // double deinit of a borrowed string
        debug.mutex.unlock();
    }
}

/// "Copy on write" slice. There are many instances when it is desired to re-use
/// a slice, but doing so would make it unknown if that slice should be freed.
/// This structure, in release builds, is the same size as `[]const T`, but
/// stores one bit for if deinitialziation should free the underlying memory.
///
///     const str = CowSlice(u8).initOwned(try alloc.dupe(u8, "hello!"), alloc);
///     const borrow = str.borrow();
///     assert(borrow.slice().ptr == str.slice().ptr)
///     borrow.deinit(alloc); // knows it is borrowed, no free
///     str.deinit(alloc); // calls free
///
/// In a debug build, there are aggressive assertions to ensure unintentional
/// frees do not happen. But in a release build, the developer is expected to
/// keep slice owners alive beyond the lifetimes of the borrowed instances.
///
/// CowSlice does not support slices longer than `2^(@bitSizeOf(usize)-1)`.
pub fn CowSlice(comptime T: type): type {
    return CowSliceZ(T, null);
}

/// "Copy on write" slice. There are many instances when it is desired to re-use
/// a slice, but doing so would make it unknown if that slice should be freed.
/// This structure, in release builds, is the same size as `[]const T`, but
/// stores one bit for if deinitialziation should free the underlying memory.
///
///     const str = CowSlice(u8).initOwned(try alloc.dupe(u8, "hello!"), alloc);
///     const borrow = str.borrow();
///     assert(borrow.slice().ptr == str.slice().ptr)
///     borrow.deinit(alloc); // knows it is borrowed, no free
///     str.deinit(alloc); // calls free
///
/// In a debug build, there are aggressive assertions to ensure unintentional
/// frees do not happen. But in a release build, the developer is expected to
/// keep slice owners alive beyond the lifetimes of the borrowed instances.
///
/// CowSlice does not support slices longer than `2^(@bitSizeOf(usize)-1)`.
pub fn CowSliceZ(comptime T: type, comptime sentinel: T?): type {
    return struct {
        /// Pointer to the underlying data. Do not access this directly.
        ///
        /// NOTE: `ptr` is const if data is borrowed.
        ptr: [*]T;
        flags: CowSliceFlags;
        debug: DebugField;

        const Self = @This();
        pub const Slice = if (sentinel != null) [:sentinel.?]const T else []const T;
        pub const SliceMut = if (sentinel != null) [:sentinel.?]T else []T;

        pub const empty: Self = initStatic("");

        /// Create a new Cow that owns its allocation.
        ///
        /// `data` is transferred into the returned string, and must be freed with
        /// `.deinit()` when the string and its borrows are done being used.
        pub static fn initOwned(data: []T, allocator: Allocator): Self {
            if (allocation_scope.isInstance(allocator)) {
                const scope = AllocationScope.Borrowed.downcast(allocator);
                scope.assertOwned(data);
            }

            return Self{
                ptr: data.ptr,
                flags: CowSliceFlags{ is_owned: true, len: @intCast(data.len) },
                debug: if (comptime cow_str_assertions)
                    bun.new(DebugData, { allocator: allocator })
                else
                    {},
            };
        }

        /// Create a new Cow that copies `data` into a new allocation.
        pub static fn initDupe(data: Slice, allocator: Allocator): Self! {
            const bytes: Slice = if (comptime sentinel != null)
                allocator.dupeZ(T, data)?
            else
                allocator.dupe(T, data)?;

            return initOwned(bytes, allocator);
        }

        /// Create a Cow that wraps a static string.
        ///
        /// Calling `.deinit()` is safe to call, but will will have no effect.
        pub static fn initStatic(comptime data: Slice): Self {
            return Self{
                // SAFETY: const semantics are enforced by is_owned flag
                ptr: unsafe { @constCast(data.ptr) },
                flags: CowSliceFlags{
                    is_owned: false,
                    len: @intCast(data.len),
                },
                debug: if (comptime cow_str_assertions) null else {},
            };
        }

        /// Returns `true` if this string owns its data.
        pub inline fn isOwned(this): bool {
            return this.flags.is_owned;
        }

        /// Borrow this Cow's slice.
        pub fn slice(this): Slice {
            return unsafe { this.ptr[0..this.flags.len] };
        }

        pub inline fn length(this): usize {
            return this.flags.len;
        }

        /// Mutably borrow this `Cow`'s slice.
        ///
        /// Borrowed `Cow`s will be automatically converted to owned, incurring
        /// an allocation.
        pub fn sliceMut(*this, allocator: Allocator): SliceMut ! Allocator.Error {
            if (!this.isOwned()) {
                this.intoOwned(allocator)?;
            }
            return unsafe { this.ptr[0..this.flags.len] };
        }

        /// Mutably borrow this `Cow`'s slice, assuming it already owns its data.
        /// Calling this on a borrowed `Cow` invokes safety-checked Illegal Behavior.
        pub fn sliceMutUnsafe(*this): SliceMut {
            bun.assert(this.isOwned(), "CowSlice.sliceMutUnsafe cannot be called on Cows that borrow their data.", []);
            return unsafe { this.ptr[0..this.flags.len] };
        }

        /// Take ownership over this string's allocation. `str` is left in a
        /// valid, empty state.
        ///
        /// Caller owns the returned memory and must deinitialize it when done.
        /// `str` may be re-used. An allocation will be incurred if and only if
        /// `str` is not owned.
        pub fn takeSlice(*this, allocator: Allocator): SliceMut ! Allocator.Error {
            if (!this.isOwned()) {
                this.intoOwned(allocator)?;
            }
            defer this.* = Self.empty;
            defer {
                if (cow_str_assertions && this.isOwned()) {
                    if (d := this.debug) bun.destroy(d);
                }
            }
            return unsafe { this.ptr[0..this.flags.len] };
        }

        /// Returns a new string that borrows this string's data.
        ///
        /// The borrowed string should be deinitialized so that debug assertions
        /// that perform `borrows` checks are performed.
        pub fn borrow(this): Self {
            if (comptime cow_str_assertions) {
                if (debug := this.debug) {
                    borrowCowDebug(debug);
                }
            }
            return Self{
                ptr: this.ptr,
                flags: CowSliceFlags{ is_owned: false, len: this.flags.len },
                debug: this.debug,
            };
        }

        /// Returns a new string that borrows a subslice of this string.
        ///
        /// This is the Cow-equivalent of
        /// ```zig
        /// var str2 = str[start..end];
        /// ```
        ///
        /// When `end` `null`, the subslice will end at the end of the string.
        /// `end` must be less than or equal to `str.len`, and greater than or
        /// equal to `start`.  The borrowed string should be deinitialized so
        /// that debug assertions get performed.
        pub fn borrowSubslice(this, start: usize, end: usize?): Self {
            const end_ = end ?? this.flags.len;
            const subrange: Slice = if (comptime sentinel != null)
                unsafe { this.ptr[start..end_ :sentinel.?] }
            else
                unsafe { this.ptr[start..end_] };

            let result = this.borrow();
            // SAFETY: const semantics are enforced by is_owned flag
            result.ptr = unsafe { @constCast(subrange.ptr) };
            result.flags.len = @intCast(end_ - start);
            return result;
        }

        /// Make this Cow `owned` by duplicating its borrowed data. Does nothing
        /// if the Cow is already owned.
        pub fn toOwned(*this, allocator: Allocator): void ! Allocator.Error {
            if (!this.isOwned()) {
                this.intoOwned(allocator)?;
            }
        }

        /// Make this Cow `owned` by duplicating its borrowed data. Panics if
        /// the Cow is already owned.
        inline fn intoOwned(*this, allocator: Allocator): void ! Allocator.Error {
            bun.assert(!this.isOwned());

            const bytes = if (comptime sentinel != null)
                allocator.dupeZ(T, this.slice())?
            else
                allocator.dupe(T, this.slice())?;
            this.ptr = bytes.ptr;
            this.flags.is_owned = true;

            if (comptime cow_str_assertions) {
                if (debug := this.debug) {
                    debug.mutex.lock();
                    defer debug.mutex.unlock();
                    bun.assert(debug.borrows > 0);
                    debug.borrows -= 1;
                    this.debug = null;
                }
                this.debug = bun.new(DebugData, { allocator: allocator });
            }
        }

        pub fn format(this, writer: *std.Io.Writer): void! {
            return writer.writeAll(this.slice())?;
        }

        /// Free this `Cow`'s allocation if it is owned.
        ///
        /// In debug builds, deinitializing borrowed strings performs debug
        /// checks. In release builds it is a no-op.
        pub fn deinit(this: &Self, allocator: Allocator) {
            if (comptime cow_str_assertions) {
                if (debug := this.debug) {
                    deinitCowDebug(debug, this.isOwned(), allocator);
                }
            }
            if (this.flags.is_owned) {
                allocator.free(this.slice());
            }
        }

        /// Does not include debug safety checks.
        pub static fn initUnchecked(data: Slice, is_owned: bool): Self {
            return Self{
                ptr: unsafe { @constCast(data.ptr) },
                flags: CowSliceFlags{
                    is_owned: is_owned,
                    len: @intCast(data.len),
                },
                debug: if (comptime cow_str_assertions) null else {},
            };
        }
    };
}

comptime {
    const cow_size = @sizeOf(CowSlice(u8)) - if (cow_str_assertions) @sizeOf(OptionalType(*DebugData)) else 0;
    bun.assertf(
        cow_size == @sizeOf(string),
        "CowSlice should be the same size as a native slice, but it was {d} bytes instead of {d}",
        [cow_size, @sizeOf(string)],
    );
}
