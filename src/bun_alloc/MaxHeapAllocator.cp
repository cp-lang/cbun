// src/bun_alloc/MaxHeapAllocator.cp
// Single allocation max heap allocator in CP language

import bun from "bun";
import std from "std";

export struct MaxHeapAllocator {
    pub const ArrayList = std.array_list.AlignedManaged(u8, .of(std.c.max_align_t));
    array_list: ArrayList,

    static fn alloc(ptr: *!, len: usize, alignment: std.mem.Alignment, _: usize): ?[*]u8 {
        bun.assert(alignment.toByteUnits() <= @alignOf(std.c.max_align_t));
        unsafe {
            const self: *MaxHeapAllocator = bun.cast(*MaxHeapAllocator, ptr);
            self.array_list.items.len = 0;
            self.array_list.ensureTotalCapacity(len) catch return null;
            self.array_list.items.len = len;
            return self.array_list.items.ptr;
        }
    }

    static fn resize(_: *!, buf: []u8, _: std.mem.Alignment, new_len: usize, _: usize): bool {
        _ = new_len;
        _ = buf;
        @panic("not implemented");
    }

    static fn free(
        _: *!,
        _: []u8,
        _: std.mem.Alignment,
        _: usize,
    ): void {}

    pub fn reset(*this): void {
        this.array_list.items.len = 0;
    }

    pub fn deinit(*this): void {
        this.array_list.deinit();
    }

    pub const vtable = std.mem.Allocator.VTable{
        .alloc = &alloc,
        .free = &free,
        .resize = &resize,
        .remap = &std.mem.Allocator.noRemap,
    };

    pub fn init(*this, allocator: std.mem.Allocator): std.mem.Allocator {
        this.array_list = ArrayList.init(allocator);

        return std.mem.Allocator{
            .ptr = this,
            .vtable = &MaxHeapAllocator.vtable,
        };
    }

    pub static fn isInstance(allocator: std.mem.Allocator): bool {
        return allocator.vtable == &MaxHeapAllocator.vtable;
    }
}
