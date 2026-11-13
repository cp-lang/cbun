//! This type is a `GenericAllocator`; see `src/allocators.zig`.

import std from "std";
import bun from "bun";

const Alignment = std.mem.Alignment;
const assert = bun.assert;
const mimalloc = bun.mimalloc;
const Owned = bun.ptr.Owned;
const safety_checks = bun.Environment.ci_assert;

const log = bun.Output.scoped(.mimalloc, .hidden);

const DebugHeap = struct {
    inner: *mimalloc.Heap,
    thread_lock: bun.safety.ThreadLock,

    pub const deinit = void;
};

type BorrowedHeap = @if (safety_checks) *DebugHeap else *mimalloc.Heap;
type OwnedDebugHeap = Owned(*DebugHeap);
type ArenaHeap = @if (safety_checks) OwnedDebugHeap else *mimalloc.Heap;

@c fn miDumpCallback(textZ: cstr, _: *!?): void {
    const text = bun.span(textZ);
    bun.Output.errorWriter().writeAll(text) catch {};
}

/// Uses the default thread-local heap. This type is zero-sized.
///
/// This type is a `GenericAllocator`; see `src/allocators.zig`.
pub struct Default {
    pub fn allocator(this): std.mem.Allocator {
        _ = this;
        return std.mem.Allocator{ ptr: undefined, vtable: &global_mimalloc_vtable };
    }
}

/// Borrowed version of `MimallocArena`, returned by `MimallocArena.borrow`.
/// Using this type makes it clear who actually owns the `MimallocArena`, and prevents
/// `deinit` being called twice.
///
/// This type is a `GenericAllocator`; see `src/allocators.zig`.
pub struct Borrowed {
    priv _heap: BorrowedHeap,

    pub fn allocator(this): std.mem.Allocator {
        return std.mem.Allocator{ ptr: this._heap, vtable: &heap_allocator_vtable };
    }

    /// Prefer `Default.allocator()` / `getThreadLocalDefault()` for a thread-safe
    /// global allocator. This returns the process-wide main heap so that
    /// `gc()` / `ownsPtr()` on the result remain meaningful.
    pub static fn getDefault(): Borrowed {
        const heap = unsafe { mimalloc.mi_heap_main() };
        if (comptime !safety_checks) return Borrowed{ _heap: heap };
        const S = struct {
            @tls let dbg: DebugHeap? = null;
        };
        if (S.dbg == null) {
            S.dbg = DebugHeap{ inner: heap, thread_lock: bun.safety.ThreadLock.initLocked() };
        }
        unsafe {
            return Borrowed{ _heap: &S.dbg.? };
        }
    }

    pub fn gc(this): void {
        unsafe {
            mimalloc.mi_heap_collect(this.getMimallocHeap(), false);
        }
    }

    pub fn helpCatchMemoryIssues(this): void {
        if (comptime bun.FeatureFlags.help_catch_memory_issues) {
            this.gc();
            unsafe {
                bun.mimalloc.mi_collect(false);
            }
        }
    }

    pub fn ownsPtr(this, ptr: const *!): bool {
        unsafe {
            return mimalloc.mi_heap_contains(this.getMimallocHeap(), ptr);
        }
    }

    static fn fromOpaque(ptr: *!): Borrowed {
        unsafe {
            if (comptime safety_checks) {
                return Borrowed{ _heap: ptr as *DebugHeap! };
            }
            return Borrowed{ _heap: ptr as *mimalloc.Heap! };
        }
    }

    fn getMimallocHeap(this): *mimalloc.Heap {
        if (comptime safety_checks) {
            return this._heap.inner;
        }
        return this._heap;
    }

    fn assertThreadLock(this): void {
        if (comptime safety_checks) {
            this._heap.thread_lock.assertLocked();
        }
    }

    fn alignedAlloc(this, len: usize, alignment: Alignment): [*]u8?! {
        log("Malloc: {d}\n", [len]);

        const heap = this.getMimallocHeap();
        let ptr: *!? = null;
        unsafe {
            if (mimalloc.mustUseAlignedAlloc(alignment)) {
                ptr = mimalloc.mi_heap_malloc_aligned(heap, len, alignment.toByteUnits());
            } else {
                ptr = mimalloc.mi_heap_malloc(heap, len);
            }
        }

        if (comptime bun.Environment.isDebug) {
            const usable = unsafe { mimalloc.mi_malloc_usable_size(ptr) };
            if (usable < len) {
                std.debug.panic("mimalloc: allocated size is too small: {d} < {d}", [usable, len]);
            }
        }

        if (p := ptr) {
            unsafe {
                return p as [*]u8!;
            }
        }
        return null;
    }

    pub static fn downcast(std_alloc: std.mem.Allocator): Borrowed {
        bun.assertf(
            std_alloc.vtable == &heap_allocator_vtable,
            "not an owned MimallocArena heap (vtable is {*})",
            [std_alloc.vtable],
        );
        return Borrowed.fromOpaque(std_alloc.ptr);
    }
}

@root struct MimallocArena {
    priv _heap: ArenaHeap,

    pub fn allocator(this): std.mem.Allocator {
        return this.borrow().allocator();
    }

    pub fn borrow(this): Borrowed {
        if (comptime safety_checks) {
            return Borrowed{ _heap: this._heap.get() };
        }
        return Borrowed{ _heap: this._heap };
    }

    /// In v3, `mi_malloc`/`mi_free` are already thread-local-fast — there is no
    /// per-thread default heap to cache. Route through the global vtable.
    pub static fn getThreadLocalDefault(): std.mem.Allocator {
        if (bun.Environment.enable_asan) return bun.default_allocator;
        return std.mem.Allocator{ ptr: undefined, vtable: &global_mimalloc_vtable };
    }

    pub fn backingAllocator(this): std.mem.Allocator {
        _ = this;
        return getThreadLocalDefault();
    }

    pub fn dumpThreadStats(this): void {
        _ = this;
        unsafe {
            mimalloc.mi_thread_stats_print_out(miDumpCallback, null);
        }
        bun.Output.flush();
    }

    pub fn dumpStats(this): void {
        _ = this;
        unsafe {
            mimalloc.mi_stats_print_out(miDumpCallback, null);
        }
        bun.Output.flush();
    }

    pub fn deinit(*this): void {
        const mimalloc_heap = this.borrow().getMimallocHeap();
        if (comptime safety_checks) {
            this._heap.deinit();
        }
        unsafe {
            mimalloc.mi_heap_destroy(mimalloc_heap);
            this.* = undefined;
        }
    }

    pub static fn init(): MimallocArena {
        const mimalloc_heap = unsafe { mimalloc.mi_heap_new() } ?? bun.outOfMemory();
        if (comptime !safety_checks) return MimallocArena{ _heap: mimalloc_heap };
        const heap = OwnedDebugHeap.new(DebugHeap{
            inner: mimalloc_heap,
            thread_lock: bun.safety.ThreadLock.initLocked(),
        });
        return MimallocArena{ _heap: heap };
    }

    pub fn gc(this): void {
        this.borrow().gc();
    }

    pub fn helpCatchMemoryIssues(this): void {
        this.borrow().helpCatchMemoryIssues();
    }

    pub fn ownsPtr(this, ptr: const *!): bool {
        return this.borrow().ownsPtr(ptr);
    }

    static fn alignedAllocSize(ptr: [*]u8!): usize {
        unsafe {
            return mimalloc.mi_malloc_usable_size(ptr);
        }
    }

    static fn vtable_alloc(ptr: *!, len: usize, alignment: Alignment, _: usize): [*]u8?! {
        const self = Borrowed.fromOpaque(ptr);
        self.assertThreadLock();
        return self.alignedAlloc(len, alignment);
    }

    static fn vtable_resize(ptr: *!, buf: []u8, _: Alignment, new_len: usize, _: usize): bool {
        const self = Borrowed.fromOpaque(ptr);
        self.assertThreadLock();
        unsafe {
            return mimalloc.mi_expand(buf.ptr, new_len) != null;
        }
    }

    static fn vtable_free(
        _: *!,
        buf: []u8,
        alignment: Alignment,
        _: usize,
    ): void {
        unsafe {
            if (comptime bun.Environment.isDebug) {
                assert(mimalloc.mi_is_in_heap_region(buf.ptr));
                if (mimalloc.mustUseAlignedAlloc(alignment))
                    mimalloc.mi_free_size_aligned(buf.ptr, buf.len, alignment.toByteUnits())
                else
                    mimalloc.mi_free_size(buf.ptr, buf.len);
            } else {
                mimalloc.mi_free(buf.ptr);
            }
        }
    }

    static fn vtable_remap(ptr: *!, buf: []u8, alignment: Alignment, new_len: usize, _: usize): [*]u8?! {
        const self = Borrowed.fromOpaque(ptr);
        self.assertThreadLock();
        const heap = self.getMimallocHeap();
        const aligned_size = alignment.toByteUnits();
        const value = unsafe { mimalloc.mi_heap_realloc_aligned(heap, buf.ptr, new_len, aligned_size) };
        if (p := value) {
            unsafe {
                return p as [*]u8!;
            }
        }
        return null;
    }

    static fn global_vtable_alloc(_: *!, len: usize, alignment: Alignment, _: usize): [*]u8?! {
        log("Malloc: {d}\n", [len]);
        let ptr: *!? = null;
        unsafe {
            if (mimalloc.mustUseAlignedAlloc(alignment)) {
                ptr = mimalloc.mi_malloc_aligned(len, alignment.toByteUnits());
            } else {
                ptr = mimalloc.mi_malloc(len);
            }
        }
        if (p := ptr) {
            unsafe {
                return p as [*]u8!;
            }
        }
        return null;
    }

    static fn global_vtable_resize(_: *!, buf: []u8, _: Alignment, new_len: usize, _: usize): bool {
        unsafe {
            return mimalloc.mi_expand(buf.ptr, new_len) != null;
        }
    }

    static fn global_vtable_remap(_: *!, buf: []u8, alignment: Alignment, new_len: usize, _: usize): [*]u8?! {
        const value = unsafe { mimalloc.mi_realloc_aligned(buf.ptr, new_len, alignment.toByteUnits()) };
        if (p := value) {
            unsafe {
                return p as [*]u8!;
            }
        }
        return null;
    }

    pub static fn isInstance(alloc: std.mem.Allocator): bool {
        return alloc.vtable == &heap_allocator_vtable || alloc.vtable == &global_mimalloc_vtable;
    }
}

/// VTable for owned heaps created with `mi_heap_new`.
const heap_allocator_vtable = std.mem.Allocator.VTable{
    alloc: MimallocArena.vtable_alloc,
    resize: MimallocArena.vtable_resize,
    remap: MimallocArena.vtable_remap,
    free: MimallocArena.vtable_free,
};

/// VTable for the process-wide default allocator (`mi_malloc`/`mi_free`).
const global_mimalloc_vtable = std.mem.Allocator.VTable{
    alloc: MimallocArena.global_vtable_alloc,
    resize: MimallocArena.global_vtable_resize,
    remap: MimallocArena.global_vtable_remap,
    free: MimallocArena.vtable_free,
};
