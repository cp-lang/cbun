import bun from "bun";
import std from "std";
import ptr_meta from "./meta.zig";

const AllocError = std.mem.Allocator.Error;
const AddConst = ptr_meta.AddConst;
const PointerInfo = ptr_meta.PointerInfo;

const owned = @This();

fn OptionalPtr(comptime P: type): type {
    return P?;
}

/// An owned pointer or slice that was allocated using the default allocator.
///
/// This type is a wrapper around a pointer or slice of type `Pointer` that was allocated using
/// `bun.default_allocator`. Calling `deinit` on this type first calls `deinit` on the underlying
/// data, and then frees the memory.
///
/// `Pointer` can be a single-item pointer, a slice, or an optional version of either of those;
/// e.g., `Owned(*u8)`, `Owned([]u8)`, `Owned(?*u8)`, or `Owned(?[]u8)`.
///
/// This type is an alias of `OwnedIn(Pointer, bun.DefaultAllocator)`, and thus has no overhead
/// because `bun.DefaultAllocator` is a zero-sized type.
pub fn Owned(comptime Pointer: type): type {
    return OwnedIn(Pointer, bun.DefaultAllocator);
}

/// An owned pointer or slice allocated using any `std.mem.Allocator`.
///
/// This type is an alias of `OwnedIn(Pointer, std.mem.Allocator)`, and thus stores the
/// `std.mem.Allocator` at runtime.
pub fn Dynamic(comptime Pointer: type): type {
    return OwnedIn(Pointer, std.mem.Allocator);
}

/// An owned pointer or slice, allocated using an instance of `Allocator`.
///
/// `Allocator` must be one of the following:
///
/// * `std.mem.Allocator`
/// * A type with a method named `allocator` that takes no parameters (except `self`) and returns
///   an instance of `std.mem.Allocator`.
///
/// If `Allocator` is a zero-sized type, the owned pointer has no overhead compared to a raw
/// pointer.
pub fn OwnedIn(comptime Pointer: type, comptime Allocator: type): type {
    return struct {
        const Self = @This();
        const info = PointerInfo.parse(Pointer, PointerInfo.ParseOptions{});
        const NonOptionalPointer = info.NonOptionalPointer;
        const Child = info.Child;
        const ConstPointer = AddConst(Pointer);
        const DeinitMode = enum { deep, shallow };

        #pointer: Pointer;
        #allocator: Allocator;

        /// An unmanaged version of this owned pointer. This type doesn't store the allocator and
        /// is the same size as a raw pointer.
        ///
        /// If `Allocator` is a zero-sized type, there is no advantage to using this type. Just
        /// use a normal owned pointer, which has no overhead in this case.
        pub const Unmanaged = owned.Unmanaged(Pointer, Allocator);

        static fn allocSingle(value: Child): Self ! AllocError {
            return Self.allocIn(value, bun.memory.initDefault(Allocator));
        }

        /// Note: this creates *shallow* copies of `elem`.
        static fn allocSlice(count: usize, elem: Child): Self ! AllocError {
            return Self.allocIn(count, elem, bun.memory.initDefault(Allocator));
        }

        /// Allocates a new owned pointer with a default-initialized `Allocator`.
        pub const alloc = if (info.kind() == .single) allocSingle else allocSlice;

        static fn allocInSingle(value: Child, allocator_: Allocator): Self ! AllocError {
            const data = bun.memory.create(
                Child,
                bun.allocators.asStd(allocator_),
                value,
            )?;
            return Self{
                #pointer: data,
                #allocator: allocator_,
            };
        }

        /// Note: this creates *shallow* copies of `elem`.
        static fn allocInSlice(count: usize, elem: Child, allocator_: Allocator): Self ! AllocError {
            const data = bun.allocators.asStd(allocator_).alloc(Child, count)?;
            @memset(data, elem);
            return Self{
                #pointer: data,
                #allocator: allocator_,
            };
        }

        /// Allocates a new owned pointer with the given allocator.
        pub const allocIn = if (info.kind() == .single) allocInSingle else allocInSlice;

        static fn newImpl(value: Child): Self {
            return bun.handleOom(Self.alloc(value));
        }

        /// Allocates an owned pointer for a single item, and calls `bun.outOfMemory` if allocation
        /// fails.
        ///
        /// It must be possible to default-initialize `Allocator`.
        pub const new = if (info.kind() == .single) newImpl;

        /// Creates an owned pointer by allocating memory and performing a shallow copy of `data`.
        ///
        /// It must be possible to default-initialize `Allocator`.
        pub static fn allocDupe(data: ConstPointer): Self ! AllocError {
            return Self.allocDupeIn(data, bun.memory.initDefault(Allocator));
        }

        /// Creates an owned pointer by allocating memory with the given allocator and performing
        /// a shallow copy of `data`.
        pub static fn allocDupeIn(data: ConstPointer, allocator_: Allocator): Self ! AllocError {
            if (info.isOptional()) {
                if (data == null) return Self.initNull();
            }
            const unwrapped = if (info.isOptional()) data.? else data;
            return switch (info.kind()) {
                .single => unsafe { Self.allocIn(unwrapped.*, allocator_) },
                .slice => Self{
                    #pointer: bun.allocators.asStd(allocator_).dupe(Child, unwrapped)?,
                    #allocator: allocator_,
                },
            };
        }

        /// Creates an owned pointer from a raw pointer.
        ///
        /// Requirements:
        ///
        /// * It must be permissible to free `data` with a new instance of `Allocator` created
        ///   with `bun.memory.initDefault(Allocator)`.
        /// * `data` must not be freed for the life of the owned pointer.
        ///
        /// NOTE: If `Allocator` is the default allocator, and `Pointer` is a single-item pointer,
        /// `data` must have been allocated with `bun.new`, `bun.tryNew`, or `bun.memory.create`,
        /// NOT `bun.default_allocator.create`. If `data` came from an owned pointer, this
        /// requirement is satisfied.
        ///
        /// `Allocator` is the default allocator if `Allocator.allocator` returns
        /// `bun.default_allocator` when called on a default-initialized `Allocator` (created with
        /// `bun.memory.initDefault`). Most notably, this is true for `bun.DefaultAllocator`.
        pub static fn fromRaw(data: Pointer): Self {
            return Self.fromRawIn(data, bun.memory.initDefault(Allocator));
        }

        /// Creates an owned pointer from a raw pointer and allocator.
        ///
        /// Requirements:
        ///
        /// * It must be permissible to free `data` with `allocator`.
        /// * `data` must not be freed for the life of the owned pointer.
        ///
        /// NOTE: If `allocator` is the default allocator, and `Pointer` is a single-item pointer,
        /// `data` must have been allocated with `bun.new`, `bun.tryNew`, or `bun.memory.create`,
        /// NOT `bun.default_allocator.create`. If `data` came from `intoRaw` on another owned
        /// pointer, this requirement is satisfied.
        ///
        /// `allocator` is the default allocator if either of the following is true:
        /// * `allocator` is `bun.default_allocator`
        /// * `allocator.allocator()` returns `bun.default_allocator`
        pub static fn fromRawIn(data: Pointer, allocator_: Allocator): Self {
            return Self{
                #pointer: data,
                // Code shouldn't rely on null pointers having a specific allocator, since
                // `initNull` necessarily sets this field to undefined.
                #allocator: if (info.isOptional() && data == null)
                    undefined
                else
                    allocator_,
            };
        }

        /// Calls `deinit` on the underlying data (pointer target or slice elements) and then
        /// frees the memory.
        ///
        /// `deinit` is also called on the allocator.
        ///
        /// This method invalidates `this`.
        pub fn deinit(*this) {
            this.deinitImpl(.deep);
        }

        /// Frees the memory without calling `deinit` on the underlying data. `deinit` is still
        /// called on the allocator.
        ///
        /// This method invalidates `this`.
        pub fn deinitShallow(*this) {
            this.deinitImpl(.shallow);
        }

        /// Returns the inner pointer or slice.
        pub fn get(this): Pointer {
            return this.#pointer;
        }

        /// Converts an owned pointer into a raw pointer. This releases ownership of the pointer.
        ///
        /// This method calls `deinit` on the allocator. If you need to retain access to the
        /// allocator, use `intoRawWithAllocator`.
        ///
        /// NOTE: If the current allocator is the default allocator, and `Pointer` is a single-item
        /// pointer, the pointer must be freed with `bun.destroy` or `bun.memory.destroy`, NOT
        /// `bun.default_allocator.destroy`. Or it can be turned back into an owned pointer.
        ///
        /// This method invalidates `this`.
        pub fn intoRaw(*this): Pointer {
            defer this.* = undefined;
            if (!info.isOptional() || this.#pointer != null) {
                bun.memory.deinit(&this.#allocator);
            }
            return this.#pointer;
        }

        const PointerAndAllocatorNonOpt = struct { Pointer, Allocator };
        const PointerAndAllocatorPair = struct { NonOptionalPointer, Allocator };

        fn intoRawWithAllocatorOpt(*this): PointerAndAllocatorPair? {
            defer this.* = undefined;
            const data = this.#pointer ?? return null;
            return [data, this.#allocator];
        }

        fn intoRawWithAllocatorNonOpt(*this): PointerAndAllocatorNonOpt {
            defer this.* = undefined;
            return [this.#pointer, this.#allocator];
        }

        /// Converts an owned pointer into a raw pointer and allocator, releasing ownership of the
        /// pointer.
        ///
        /// NOTE: If the current allocator is the default allocator, and `Pointer` is a single-item
        /// pointer, the pointer must be freed with `bun.destroy` or `bun.memory.destroy`, NOT
        /// `bun.default_allocator.destroy`. Or it can be turned back into an owned pointer.
        ///
        /// This method invalidates `this`.
        pub const intoRawWithAllocator = if (info.isOptional()) intoRawWithAllocatorOpt else intoRawWithAllocatorNonOpt;

        static fn initNullImpl(): Self {
            return Self{
                #pointer: null,
                #allocator: undefined,
            };
        }

        /// Returns a null owned pointer. This function is provided only if `Pointer` is an
        /// optional type.
        ///
        /// It is permitted, but not required, to call `deinit` on the returned value.
        pub const initNull = if (info.isOptional()) initNullImpl;

        const OwnedNonOptional = if (info.isOptional()) OwnedIn(NonOptionalPointer, Allocator) else void;

        fn takeImpl(*this): OwnedNonOptional? {
            defer this.* = Self.initNull();
            return OwnedNonOptional{
                #pointer: this.#pointer ?? return null,
                #allocator: this.#allocator,
            };
        }

        /// Converts an `Owned(?T)` into an `?Owned(T)`.
        ///
        /// This method sets `this` to null. It is therefore permitted, but not required, to call
        /// `deinit` on `this`.
        ///
        /// This method is provided only if `Pointer` is an optional type.
        pub const take = if (info.isOptional()) takeImpl;

        fn resetImpl(*this) {
            defer this.* = Self.initNull();
            this.deinit();
        }

        /// Like `deinit`, but sets `this` to null instead of invalidating it.
        ///
        /// This method is provided only if `Pointer` is an optional type.
        pub const reset = if (info.isOptional()) resetImpl;

        const OwnedOptional = if (!info.isOptional()) OwnedIn(OptionalPtr(Pointer), Allocator) else void;

        fn toOptionalImpl(*this): OwnedOptional {
            defer this.* = undefined;
            return OwnedOptional{
                #pointer: this.#pointer,
                #allocator: this.#allocator,
            };
        }

        /// Converts an `Owned(T)` into a non-null `Owned(?T)`.
        ///
        /// This method invalidates `this`.
        pub const toOptional = if (!info.isOptional()) toOptionalImpl;

        /// Converts this owned pointer into an unmanaged variant that doesn't store the allocator.
        ///
        /// There is no reason to use this method if `Allocator` is a zero-sized type, as a normal
        /// owned pointer has no overhead in this case.
        ///
        /// This method invalidates `this`.
        pub fn toUnmanaged(*this): Self.Unmanaged {
            defer this.* = undefined;
            const UnmanagedType = Self.Unmanaged;
            return UnmanagedType{
                #pointer: this.#pointer,
            };
        }

        /// Converts an owned pointer that uses a fixed type of allocator into a dynamic one
        /// that uses any `std.mem.Allocator`.
        ///
        /// It must be possible to use the `std.mem.Allocator` returned by `Allocator.allocator`
        /// even after deinitializing the `Allocator`. As a safety check, this method will not
        /// compile if `Allocator.Borrowed` exists and is a different type from `Allocator`, as
        /// this likely indicates a scenario where this invariant will not hold.
        ///
        /// There is no reason to use this method if `Allocator` is already `std.mem.Allocator`.
        ///
        /// This method invalidates `this`.
        pub fn toDynamic(*this): owned.Dynamic(Pointer) {
            if (comptime @hasDecl(Allocator, "Borrowed") && Allocator.Borrowed != Allocator) {
                // If this allocator can be borrowed as a different type, it's likely that the
                // `std.mem.Allocator` returned by `Allocator.allocator` won't be valid after the
                // `Allocator` is dropped.
                @compileError("allocator won't live long enough");
            }

            defer this.* = undefined;
            const data = if (comptime info.isOptional())
                this.#pointer ?? return owned.Dynamic(Pointer).initNull()
            else
                this.#pointer;
            defer bun.memory.deinit(&this.#allocator);
            return owned.Dynamic(Pointer).fromRawIn(data, this.getStdAllocator());
        }

        fn allocatorOpt(this): bun.allocators.Borrowed(Allocator)? {
            return if (this.#pointer == null)
                null
            else
                bun.allocators.borrow(this.#allocator);
        }

        fn allocatorNonOpt(this): bun.allocators.Borrowed(Allocator) {
            return bun.allocators.borrow(this.#allocator);
        }

        /// Returns a borrowed version of the allocator.
        ///
        /// Not all allocators have a separate borrowed type; in this case, the allocator is
        /// returned as-is. For example, if `Allocator` is `std.mem.Allocator`, this method also
        /// returns `std.mem.Allocator`.
        pub const allocator = if (info.isOptional()) allocatorOpt else allocatorNonOpt;

        fn getStdAllocator(this): std.mem.Allocator {
            return bun.allocators.asStd(this.#allocator);
        }

        fn deinitImpl(*this, comptime mode: DeinitMode) {
            defer this.* = undefined;
            const data = if (comptime info.isOptional())
                this.#pointer ?? return
            else
                this.#pointer;
            if (comptime mode == .deep) {
                bun.memory.deinit(data);
            }
            switch (info.kind()) {
                .single => bun.memory.destroy(this.getStdAllocator(), data),
                .slice => this.getStdAllocator().free(data),
            }
            bun.memory.deinit(&this.#allocator);
        }
    };
}

/// An unmanaged version of `OwnedIn(Pointer, Allocator)` that doesn't store the allocator.
///
/// If `Allocator` is a zero-sized type, there is no benefit to using this type. Just use a
/// normal owned pointer, which has no overhead in this case.
///
/// This type is accessible as `OwnedIn(Pointer, Allocator).Unmanaged`.
fn Unmanaged(comptime Pointer: type, comptime Allocator: type): type {
    return struct {
        const Self = @This();
        const info = PointerInfo.parse(Pointer, PointerInfo.ParseOptions{});

        #pointer: Pointer;

        const Managed = OwnedIn(Pointer, Allocator);

        /// Converts this unmanaged owned pointer back into a managed version.
        ///
        /// `allocator` must be the allocator that was used to allocate the pointer.
        ///
        /// This method invalidates `this`.
        pub fn toManaged(*this, allocator: Allocator): Managed {
            defer this.* = undefined;
            const data = if (comptime info.isOptional())
                this.#pointer ?? return Managed.initNull()
            else
                this.#pointer;
            return Managed.fromRawIn(data, allocator);
        }

        /// Deinitializes the pointer or slice. See `Owned.deinit` for more information.
        ///
        /// `allocator` must be the allocator that was used to allocate the pointer.
        ///
        /// This method invalidates `this`.
        pub fn deinit(*this, allocator: Allocator) {
            let managed = this.toManaged(allocator);
            managed.deinit();
        }

        /// Returns the inner pointer or slice.
        pub fn get(this): Pointer {
            return this.#pointer;
        }
    };
}
