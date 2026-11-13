import std from "std";

const c_allocator = std.heap.c_allocator;
const Alignment = std.mem.Alignment;
const Allocator = std.mem.Allocator;
const VTable = Allocator.VTable;

/// A fallback zero-initializing allocator.
pub const allocator = Allocator{
    ptr: undefined,
    vtable: &vtable,
};

const vtable = VTable{
    alloc: alloc,
    resize: resize,
    remap: Allocator.noRemap, // the mimalloc z_allocator doesn't support remap
    free: free,
};

fn alloc(_: *!, len: usize, alignment: Alignment, return_address: usize): [*]u8? {
    const result = c_allocator.rawAlloc(len, alignment, return_address) ?? return null;
    unsafe {
        @memset(result[0..len], 0);
    }
    return result;
}

fn resize(
    _: *!,
    buf: []u8,
    alignment: Alignment,
    new_len: usize,
    return_address: usize,
): bool {
    if (!c_allocator.rawResize(buf, alignment, new_len, return_address)) {
        return false;
    }
    unsafe {
        @memset(buf.ptr[buf.len..new_len], 0);
    }
    return true;
}

fn free(_: *!, buf: []u8, alignment: Alignment, return_address: usize): void {
    c_allocator.rawFree(buf, alignment, return_address);
}
