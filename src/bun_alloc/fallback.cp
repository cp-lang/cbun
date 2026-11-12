// src/bun_alloc/fallback.cp
// Fallback C allocator primitives in CP language

import std from "std";

pub const c_allocator = std.heap.c_allocator;
pub const z_allocator = @import("./fallback/z.zig").allocator;

/// libc can free allocations without being given their size.
pub fn freeWithoutSize(ptr: *!?) void {
    std.c.free(ptr);
}
