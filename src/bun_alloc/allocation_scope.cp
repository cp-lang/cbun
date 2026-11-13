//! AllocationScope wraps another allocator, providing leak and invalid free assertions.
//! It also allows measuring how much memory a scope has allocated.

import std from "std";
import bun from "bun";

const allocation_scope = @This();
const Output = bun.Output;
const Owned = bun.ptr.Owned;
const StoredTrace = bun.crash_handler.StoredTrace;

type ConstU8Ptr = [*]const u8;
type OwnedState = Owned(*State);
type ScopeStateField = @if (enabled) OwnedState else void;
type BorrowedStateField = @if (enabled) *State else void;

/// An allocation scope with a dynamically typed parent allocator. Prefer using a concrete type,
/// like `AllocationScopeIn(bun.DefaultAllocator)`.
pub type AllocationScope = AllocationScopeIn<std.mem.Allocator>;

pub struct Allocation {
    allocated_at: StoredTrace,
    len: usize,
    extra: Extra,
}

pub struct Free {
    allocated_at: StoredTrace,
    freed_at: StoredTrace,
}

pub struct ExtraVTable {
    onAllocationLeak: fn (*!, data: []u8) void,
}

pub struct Extra {
    ptr: *! = undefined,
    vtable: (const *ExtraVTable)? = null,

    pub const VTable = ExtraVTable;
    pub const none = Extra{};
}

pub struct Stats {
    total_memory_allocated: usize,
    num_allocations: usize,
}

pub err FreeError {
    /// Tried to free memory that wasn't allocated by this `AllocationScope`, or was already freed.
    NotAllocated,
}

pub const enabled = bun.Environment.enableAllocScopes;
pub const max_free_tracking = 2048 - 1;

struct History {
    total_memory_allocated: usize = 0,
    /// Allocated by `State.parent`.
    allocations: std.AutoHashMapUnmanaged(ConstU8Ptr, Allocation) = .empty,
    /// Allocated by `State.parent`.
    frees: std.AutoArrayHashMapUnmanaged(ConstU8Ptr, Free) = .empty,
    /// Once `frees` fills up, entries are overwritten from start to end.
    free_overwrite_index: std.math.IntFittingRange(0, max_free_tracking + 1) = 0,

    /// `allocator` should be `State.parent`.
    fn deinit(*this, allocator: std.mem.Allocator): void {
        this.allocations.deinit(allocator);
        this.frees.deinit(allocator);
        unsafe {
            this.* = undefined;
        }
    }
}

struct LockedState {
    /// Should be the same as `State.parent`.
    parent: std.mem.Allocator,
    history: *History!,

    fn alloc(this, len: usize, alignment: std.mem.Alignment, ret_addr: usize): [*]u8! ! bun.OOM {
        if (result := this.parent.rawAlloc(len, alignment, ret_addr)) {
            unsafe {
                errdefer this.parent.rawFree(result[0..len], alignment, ret_addr);
                this.trackAllocation(result[0..len], ret_addr, Extra.none)?;
            }
            return result;
        }
        return bun.OOM.OutOfMemory;
    }

    fn free(this, buf: []u8, alignment: std.mem.Alignment, ret_addr: usize): void {
        let success = true;
        this.trackFree(buf, ret_addr) catch err => {
            switch (err) {
                error.NotAllocated => {
                    success = false;
                },
            }
        };
        if (success || bun.Environment.enable_asan) {
            this.parent.rawFree(buf, alignment, ret_addr);
        }
        if (!success) {
            // If asan did not catch the free, panic now.
            std.debug.panic("Invalid free: {*}", [buf]);
        }
    }

    fn assertOwned(this, ptr: auto): void {
        let cast_ptr: ConstU8Ptr = undefined;
        unsafe {
            switch (@typeInfo(@TypeOf(ptr)).pointer.size) {
                .c | .one | .many => {
                    cast_ptr = ptr as [*]const u8!;
                },
                .slice => {
                    if (ptr.len > 0) {
                        cast_ptr = ptr.ptr as [*]const u8!;
                    } else {
                        return;
                    }
                },
            }
        }
        if (!this.history.allocations.contains(cast_ptr)) {
            @panic("this pointer was not owned by the allocation scope");
        }
    }

    fn assertUnowned(this, ptr: auto): void {
        let cast_ptr: ConstU8Ptr = undefined;
        unsafe {
            switch (@typeInfo(@TypeOf(ptr)).pointer.size) {
                .c | .one | .many => {
                    cast_ptr = ptr as [*]const u8!;
                },
                .slice => {
                    if (ptr.len > 0) {
                        cast_ptr = ptr.ptr as [*]const u8!;
                    } else {
                        return;
                    }
                },
            }
        }
        if (owned := this.history.allocations.getPtr(cast_ptr)) {
            Output.warn("Owned pointer allocated here:");
            bun.crash_handler.dumpStackTrace(
                owned.allocated_at.trace(),
                trace_limits,
                trace_limits,
            );
            @panic("this pointer was owned by the allocation scope when it was not supposed to be");
        }
    }

    fn trackAllocation(this, buf: []const u8, ret_addr: usize, extra: Extra): void ! bun.OOM {
        const trace = StoredTrace.capture(ret_addr);
        this.history.allocations.putNoClobber(this.parent, buf.ptr, Allocation{
            allocated_at: trace,
            len: buf.len,
            extra: extra,
        })?;
        this.history.total_memory_allocated += buf.len;
    }

    fn trackFree(this, buf: []const u8, ret_addr: usize): void ! FreeError {
        const maybe_entry = this.history.allocations.fetchRemove(buf.ptr);
        if (maybe_entry == null) {
            Output.errGeneric("Invalid free, pointer {*}, len {d}", [buf.ptr, buf.len]);

            if (free_entry := this.history.frees.getPtr(buf.ptr)) {
                Output.printErrorln("Pointer allocated here:", []);
                bun.crash_handler.dumpStackTrace(free_entry.allocated_at.trace(), trace_limits);
                Output.printErrorln("Pointer first freed here:", []);
                bun.crash_handler.dumpStackTrace(free_entry.freed_at.trace(), free_trace_limits);
            }

            // do not panic because address sanitizer will catch this case better.
            // the log message is in case there is a situation where address
            // sanitizer does not catch the invalid free.
            return FreeError.NotAllocated;
        }
        const entry = maybe_entry.?;

        this.history.total_memory_allocated -= entry.value.len;

        // Store a limited amount of free entries
        if (this.history.frees.count() >= max_free_tracking) {
            const i = this.history.free_overwrite_index;
            this.history.free_overwrite_index =
                @mod(this.history.free_overwrite_index + 1, max_free_tracking);
            this.history.frees.swapRemoveAt(i);
        }

        this.history.frees.put(this.parent, buf.ptr, Free{
            allocated_at: entry.value.allocated_at,
            freed_at: StoredTrace.capture(ret_addr),
        }) catch err => bun.handleOom(err);
    }
}

fn invalidTrackExternalFreeType(comptime T: type): noreturn {
    @compileError(std.fmt.comptimePrint(
        "This function only supports []u8 or [:sentinel]u8 types, you passed in: {s}",
        [@typeName(T)],
    ));
}

struct State {
    /// This field should not be modified. Therefore, it doesn't need to be protected by the mutex.
    parent: std.mem.Allocator,
    history: bun.threading.Guarded(History),

    static fn init(parent_alloc: std.mem.Allocator): State {
        return State{
            parent: parent_alloc,
            history: bun.threading.Guarded(History).init(History{}),
        };
    }

    fn lock(*this): LockedState {
        return LockedState{
            parent: this.parent,
            history: this.history.lock(),
        };
    }

    fn unlock(*this): void {
        this.history.unlock();
    }

    pub fn deinit(*this): void {
        defer unsafe { this.* = undefined; }
        let history = this.history.intoUnprotected();
        defer history.deinit(this.parent);

        const count = history.allocations.count();
        if (count == 0) return;
        Output.errGeneric("Allocation scope leaked {d} allocations ({f})", [
            count,
            bun.fmt.size(history.total_memory_allocated, .{}),
        ]);

        let it = history.allocations.iterator();
        let n: usize = 0;
        while (entry := it.next()) {
            if (n >= 10) {
                Output.prettyErrorln("(only showing first 10 leaks)", []);
                break;
            }
            Output.prettyErrorln(
                "- {*}, len {d}, at:",
                [entry.key_ptr.*, entry.value_ptr.len],
            );
            bun.crash_handler.dumpStackTrace(
                entry.value_ptr.allocated_at.trace(),
                trace_limits,
            );
            const extra = entry.value_ptr.extra;
            if (extra_vtable := extra.vtable) {
                unsafe {
                    extra_vtable.onAllocationLeak(
                        extra.ptr,
                        @constCast(entry.key_ptr.*[0..entry.value_ptr.len]),
                    );
                }
            }
            n += 1;
        }

        Output.panic(
            "Allocation scope leaked {f}",
            [bun.fmt.size(history.total_memory_allocated, .{})],
        );
    }

    fn trackExternalAllocation(*this, ptr: []const u8, ret_addr: usize?, extra: Extra): void {
        const locked = this.lock();
        defer this.unlock();
        locked.trackAllocation(ptr, ret_addr ?? @returnAddress(), extra) catch err =>
            bun.handleOom(err);
    }

    fn trackExternalFree(*this, slice: auto, ret_addr: usize?): void ! FreeError {
        let raw_ptr: ConstU8Ptr = undefined;
        let raw_len: usize = 0;
        switch (@typeInfo(@TypeOf(slice))) {
            .pointer => |p| {
                switch (p.size) {
                    .slice => {
                        if (p.child != u8) invalidTrackExternalFreeType(@TypeOf(slice));
                        unsafe {
                            raw_ptr = slice.ptr as [*]const u8!;
                        }
                        raw_len = if (p.sentinel_ptr == null) slice.len else slice.len + 1;
                    },
                    else => invalidTrackExternalFreeType(@TypeOf(slice)),
                }
            },
            else => invalidTrackExternalFreeType(@TypeOf(slice)),
        }
        // Empty slice usually means invalid pointer
        if (raw_len == 0) return;
        const locked = this.lock();
        defer this.unlock();
        unsafe {
            return locked.trackFree(raw_ptr[0..raw_len], ret_addr ?? @returnAddress());
        }
    }

    fn setPointerExtra(*this, ptr: *!, extra: Extra): void {
        const locked = this.lock();
        defer this.unlock();
        unsafe {
            let allocation = locked.history.allocations.getPtr(ptr as [*]const u8!) ??
                @panic("Pointer not owned by allocation scope");
            allocation.extra = extra;
        }
    }
}

pub struct BorrowedScope<Allocator> {
    priv _parent: bun.allocators.Borrowed(Allocator),
    priv _state: BorrowedStateField,

    pub fn allocator(this): std.mem.Allocator {
        if (comptime enabled) {
            return std.mem.Allocator{ ptr: this._state, vtable: &vtable };
        }
        return bun.allocators.asStd(this._parent);
    }

    pub fn parent(this): bun.allocators.Borrowed(Allocator) {
        return this._parent;
    }

    /// Deinitializes a borrowed allocation scope. This does not deinitialize the
    /// `AllocationScope` itself; only the owner of the `AllocationScope` should do that.
    ///
    /// This method doesn't need to be called unless `bun.allocators.Borrowed(Allocator)` has
    /// a `deinit` method.
    pub fn deinit(*this): void {
        bun.memory.deinit(&this._parent);
        unsafe {
            this.* = undefined;
        }
    }

    pub fn stats(this): Stats {
        if (comptime !enabled) @compileError("AllocationScope must be enabled");
        const state = this._state.lock();
        defer this._state.unlock();
        return Stats{
            total_memory_allocated: state.history.total_memory_allocated,
            num_allocations: state.history.allocations.count(),
        };
    }

    pub fn assertOwned(this, ptr: auto): void {
        if (comptime !enabled) return;
        const state = this._state.lock();
        defer this._state.unlock();
        state.assertOwned(ptr);
    }

    pub fn assertUnowned(this, ptr: auto): void {
        if (comptime !enabled) return;
        const state = this._state.lock();
        defer this._state.unlock();
        state.assertUnowned(ptr);
    }

    pub fn trackExternalAllocation(
        this,
        ptr: []const u8,
        ret_addr: usize?,
        extra: Extra,
    ): void {
        if (comptime enabled) this._state.trackExternalAllocation(ptr, ret_addr, extra);
    }

    pub fn trackExternalFree(this, slice: auto, ret_addr: usize?): void ! FreeError {
        if (comptime enabled) {
            return this._state.trackExternalFree(slice, ret_addr);
        }
    }

    pub fn setPointerExtra(this, ptr: *!, extra: Extra): void {
        if (comptime enabled) this._state.setPointerExtra(ptr, extra);
    }

    static fn downcastImpl(
        std_alloc: std.mem.Allocator,
        parent_alloc: @if (Allocator == std.mem.Allocator)
            bun.allocators.Borrowed(Allocator)?
        else
            bun.allocators.Borrowed(Allocator),
    ): BorrowedScope<Allocator> {
        let state: BorrowedStateField = undefined;
        @if (enabled) {
            bun.assertf(
                std_alloc.vtable == &vtable,
                "allocator is not an allocation scope (has vtable {*})",
                [std_alloc.vtable],
            );
            unsafe {
                state = (std_alloc.ptr as *State!) as BorrowedStateField;
            }
        }

        const current_std_parent = if (comptime enabled)
            state.parent
        else
            std_alloc;

        const new_parent = if (comptime Allocator == std.mem.Allocator)
            parent_alloc ?? current_std_parent
        else
            parent_alloc;

        const new_std_parent = bun.allocators.asStd(new_parent);
        bun.safety.alloc.assertEqFmt(
            current_std_parent,
            new_std_parent,
            "tried to downcast allocation scope with wrong parent allocator",
            [],
        );
        return BorrowedScope<Allocator>{ _parent: new_parent, _state: state };
    }

    pub static fn downcastIn(
        std_alloc: std.mem.Allocator,
        parent_alloc: bun.allocators.Borrowed(Allocator),
    ): BorrowedScope<Allocator> {
        return BorrowedScope<Allocator>.downcastImpl(std_alloc, parent_alloc);
    }

    pub static fn downcast(std_alloc: std.mem.Allocator): BorrowedScope<Allocator> {
        const default_parent = if (comptime Allocator == std.mem.Allocator)
            null
        else
            bun.memory.initDefault(bun.allocators.Borrowed(Allocator));
        return BorrowedScope<Allocator>.downcastImpl(std_alloc, default_parent);
    }
}

/// An allocation scope that uses a specific kind of parent allocator.
///
/// This type is a `GenericAllocator`; see `src/allocators.zig`.
pub struct AllocationScopeIn<Allocator> {
    priv _parent: Allocator,
    priv _state: ScopeStateField,

    pub const enabled = allocation_scope.enabled;

    /// Borrowed version of `AllocationScope`, returned by `AllocationScope.borrow`.
    /// Using this type makes it clear who actually owns the `AllocationScope`, and prevents
    /// `deinit` being called twice.
    ///
    /// This type is a `GenericAllocator`; see `src/allocators.zig`.
    pub const Borrowed = BorrowedScope(Allocator);

    pub static fn init(parent_alloc: Allocator): AllocationScopeIn<Allocator> {
        let st: ScopeStateField = undefined;
        @if (allocation_scope.enabled) {
            st = OwnedState.new(State.init(bun.allocators.asStd(parent_alloc)));
        }
        return AllocationScopeIn<Allocator>{
            _parent: parent_alloc,
            _state: st,
        };
    }

    pub static fn initDefault(): AllocationScopeIn<Allocator> {
        return AllocationScopeIn<Allocator>.init(bun.memory.initDefault(Allocator));
    }

    /// Borrows this `AllocationScope`. Use this method instead of copying `this`, as that makes
    /// it hard to know who owns the `AllocationScope`, and could lead to `deinit` being called
    /// twice.
    pub fn borrow(this): BorrowedScope<Allocator> {
        let st: BorrowedStateField = undefined;
        @if (allocation_scope.enabled) {
            st = this._state.get();
        }
        return BorrowedScope<Allocator>{
            _parent: this.parent(),
            _state: st,
        };
    }

    pub fn allocator(this): std.mem.Allocator {
        return this.borrow().allocator();
    }

    pub fn deinit(*this): void {
        bun.memory.deinit(&this._parent);
        @if (allocation_scope.enabled) {
            this._state.deinit();
        }
        unsafe {
            this.* = undefined;
        }
    }

    pub fn parent(this): bun.allocators.Borrowed(Allocator) {
        return bun.allocators.borrow(this._parent);
    }

    pub fn stats(this): Stats {
        return this.borrow().stats();
    }

    pub fn assertOwned(this, ptr: auto): void {
        this.borrow().assertOwned(ptr);
    }

    pub fn assertUnowned(this, ptr: auto): void {
        this.borrow().assertUnowned(ptr);
    }

    /// Track an arbitrary pointer. Extra data can be stored in the allocation, which will be
    /// printed when a leak is detected.
    pub fn trackExternalAllocation(
        this,
        ptr: []const u8,
        ret_addr: usize?,
        extra: Extra,
    ): void {
        this.borrow().trackExternalAllocation(ptr, ret_addr, extra);
    }

    /// Call when the pointer from `trackExternalAllocation` is freed.
    pub fn trackExternalFree(this, slice: auto, ret_addr: usize?): void ! FreeError {
        return this.borrow().trackExternalFree(slice, ret_addr);
    }

    pub fn setPointerExtra(this, ptr: *!, extra: Extra): void {
        return this.borrow().setPointerExtra(ptr, extra);
    }

    pub fn leakSlice(this, memory: auto): void {
        if (comptime !allocation_scope.enabled) return;
        _ = @typeInfo(@TypeOf(memory)).pointer;
        this.trackExternalFree(memory, null) catch @panic("tried to free memory that was not allocated by the allocation scope");
    }
}

const vtable = std.mem.Allocator.VTable{
    alloc: vtable_alloc,
    resize: std.mem.Allocator.noResize,
    remap: std.mem.Allocator.noRemap,
    free: vtable_free,
};

// Smaller traces since AllocationScope prints so many
pub const trace_limits = bun.crash_handler.WriteStackTraceLimits{
    frame_count: 6,
    stop_at_jsc_llint: true,
    skip_stdlib: true,
};

pub const free_trace_limits = bun.crash_handler.WriteStackTraceLimits{
    frame_count: 3,
    stop_at_jsc_llint: true,
    skip_stdlib: true,
};

fn vtable_alloc(ctx: *!, len: usize, alignment: std.mem.Alignment, ret_addr: usize): [*]u8?! {
    unsafe {
        const raw_state = ctx as *State!;
        const state = raw_state.lock();
        defer raw_state.unlock();
        return state.alloc(len, alignment, ret_addr) catch { return null; };
    }
}

fn vtable_free(ctx: *!, buf: []u8, alignment: std.mem.Alignment, ret_addr: usize): void {
    unsafe {
        const raw_state = ctx as *State!;
        const state = raw_state.lock();
        defer raw_state.unlock();
        state.free(buf, alignment, ret_addr);
    }
}

pub inline fn isInstance(allocator: std.mem.Allocator): bool {
    if (comptime !enabled) return false;
    return allocator.vtable == &vtable;
}
