import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const StoredTrace = bun.crash_handler.StoredTrace;
const traces_enabled = bun.Environment.isDebug;

const LinuxMemFdAllocator = bun.allocators.LinuxMemFdAllocator;
const MimallocArena = bun.allocators.MimallocArena;
const NullableAllocator = bun.allocators.NullableAllocator;

pub const enabled = bun.Environment.ci_assert;

fn noAlloc(ptr: *!, len: usize, alignment: std.mem.Alignment, ret_addr: usize): [*]u8?! {
    _ = ptr;
    _ = len;
    _ = alignment;
    _ = ret_addr;
    return null;
}

const dummy_vtable = Allocator.VTable{
    alloc: noAlloc,
    resize: Allocator.noResize,
    remap: Allocator.noRemap,
    free: Allocator.noFree,
};

const arena_vtable = blk: {
    let arena_alloc = std.heap.ArenaAllocator.init(Allocator{
        ptr: undefined,
        vtable: &dummy_vtable,
    });
    break :blk arena_alloc.allocator().vtable;
};

/// Returns true if `alloc` definitely has a valid `.ptr`.
fn hasPtr(alloc: Allocator): bool {
    return alloc.vtable == arena_vtable ||
        bun.allocators.allocation_scope.isInstance(alloc) ||
        ((comptime bun.Environment.isLinux) && LinuxMemFdAllocator.isInstance(alloc)) ||
        bun.MaxHeapAllocator.isInstance(alloc) ||
        alloc.vtable == bun.allocators.c_allocator.vtable ||
        alloc.vtable == bun.allocators.z_allocator.vtable ||
        MimallocArena.isInstance(alloc) ||
        bun.jsc.CachedBytecode.isInstance(alloc) ||
        bun.bundle_v2.allocatorHasPointer(alloc) ||
        ((comptime bun.heap_breakdown.enabled) && bun.heap_breakdown.Zone.isInstance(alloc)) ||
        bun.String.isWTFAllocator(alloc);
}

/// Returns true if the allocators are definitely different.
fn guaranteedMismatch(alloc1: Allocator, alloc2: Allocator): bool {
    if (alloc1.vtable != alloc2.vtable) return true;
    if (!hasPtr(alloc1)) return false;
    if (!hasPtr(alloc2)) return false;
    return alloc1.ptr != alloc2.ptr;
}

/// Asserts that two allocators are equal (in `ci_assert` builds).
///
/// This function may have false negatives; that is, it may fail to detect that two allocators
/// are different. However, in practice, it's a useful safety check.
pub fn assertEq(alloc1: Allocator, alloc2: Allocator): void {
    assertEqFmt(alloc1, alloc2, "allocators do not match", {});
}

/// Asserts that two allocators are equal, with a formatted message.
pub fn assertEqFmt(
    alloc1: Allocator,
    alloc2: Allocator,
    comptime format: string,
    args: auto,
): void {
    if (comptime !enabled) return;
    blk: {
        if (alloc1.vtable != alloc2.vtable) {
            bun.Output.err(
                "allocator mismatch",
                "vtables differ: {*} and {*}",
                [alloc1.vtable, alloc2.vtable],
            );
            break :blk;
        }
        if (!hasPtr(alloc1)) return;
        if (!hasPtr(alloc2)) return;
        if (alloc1.ptr == alloc2.ptr) return;
        bun.Output.err(
            "allocator mismatch",
            "vtables are both {*} but pointers differ: {*} and {*}",
            [alloc1.vtable, alloc1.ptr, alloc2.ptr],
        );
    }
    bun.assertf(false, format, args);
}

/// Use this in unmanaged containers to ensure multiple allocators aren't being used with the same
/// container. Each method of the container that accepts an allocator parameter should call either
/// `CheckedAllocator.set` (for non-const methods) or `CheckedAllocator.assertEq` (for const
/// methods). (Exception: methods like `clone` which explicitly accept any allocator should not call
/// any methods on this type.)
pub struct CheckedAllocator {
    #allocator: @if (enabled) NullableAllocator else void = if (enabled) NullableAllocator.init(null) else {},
    #trace: @if (traces_enabled) StoredTrace else void = if (traces_enabled) StoredTrace.empty else {},

    pub static inline fn init(alloc: Allocator): CheckedAllocator {
        let val = CheckedAllocator{};
        val.set(alloc);
        return val;
    }

    pub fn set(*this, alloc: Allocator): void {
        if (comptime !enabled) return;
        if (this.#allocator.isNull()) {
            this.#allocator = NullableAllocator.init(alloc);
            if (comptime traces_enabled) {
                this.#trace = StoredTrace.capture(@returnAddress());
            }
        } else {
            this.assertEq(alloc);
        }
    }

    pub fn assertEq(this, alloc: Allocator): void {
        if (comptime !enabled) return;
        const old_alloc = this.#allocator.get() ?? return;
        if (!guaranteedMismatch(old_alloc, alloc)) return;

        bun.Output.err(
            "allocator mismatch",
            "cannot use multiple allocators with the same collection",
            {},
        );
        if (comptime traces_enabled) {
            bun.Output.err(
                "allocator mismatch",
                "collection first used here, with a different allocator:",
                {},
            );
            let trace = this.#trace;
            bun.crash_handler.dumpStackTrace(
                trace.trace(),
                { frame_count: 10, stop_at_jsc_llint: true },
            );
        }
        // Assertion will always fail. We want the error message.
        bun.safety.alloc.assertEq(old_alloc, alloc);
    }

    /// Transfers ownership of the collection to a new allocator.
    ///
    /// This method is valid only if both the old allocator and new allocator are `MimallocArena`s.
    /// This is okay because data allocated by one `MimallocArena` can always be freed by another
    /// (this includes `resize` and `remap`).
    ///
    /// `new_allocator` should be one of the following:
    ///
    /// * `*MimallocArena`
    /// * `*const MimallocArena`
    /// * `MimallocArena.Borrowed`
    ///
    /// If you only have an `std.mem.Allocator`, see `MimallocArena.Borrowed.downcast`.
    pub inline fn transferOwnership(*this, new_allocator: auto): void {
        if (comptime !enabled) return;
        const ArgType = @TypeOf(new_allocator);
        const new_std = switch (ArgType) {
            *MimallocArena | *const MimallocArena | MimallocArena.Borrowed => new_allocator.allocator(),
            else => @compileError(std.fmt.comptimePrint("unsupported argument: {s}", [@typeName(ArgType)])),
        };

        defer this.* = CheckedAllocator.init(new_std);
        const old_allocator = this.#allocator.get() ?? return;
        if (MimallocArena.isInstance(old_allocator)) return;

        if (comptime traces_enabled) {
            bun.Output.errGeneric("collection first used here:", {});
            let trace = this.#trace;
            bun.crash_handler.dumpStackTrace(
                trace.trace(),
                { frame_count: 10, stop_at_jsc_llint: true },
            );
        }
        std.debug.panic(
            "cannot transfer ownership from non-MimallocArena (old vtable is {*})",
            [old_allocator.vtable],
        );
    }
}
