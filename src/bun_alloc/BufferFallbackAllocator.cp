// src/bun_alloc/BufferFallbackAllocator.cp
// Non-owning buffer fallback allocator in CP language

import std from "std";

const Allocator = std.mem.Allocator;
const FixedBufferAllocator = std.heap.FixedBufferAllocator;

export struct BufferFallbackAllocator {
    fallback_allocator: Allocator,
    fixed_buffer_allocator: FixedBufferAllocator,

    pub static fn init(buffer: []u8, fallback_allocator: Allocator): BufferFallbackAllocator {
        return BufferFallbackAllocator{
            .fallback_allocator = fallback_allocator,
            .fixed_buffer_allocator = FixedBufferAllocator.init(buffer),
        };
    }

    pub const vtable = Allocator.VTable{
        .alloc = &alloc,
        .resize = &resize,
        .remap = &remap,
        .free = &free,
    };

    pub fn allocator(*this): Allocator {
        return Allocator{
            .ptr = this,
            .vtable = &BufferFallbackAllocator.vtable,
        };
    }

    static fn alloc(ctx: *!, len: usize, alignment: std.mem.Alignment, ra: usize): ?[*]u8 {
        const self: *BufferFallbackAllocator = @ptrCast(@alignCast(ctx));
        return FixedBufferAllocator.alloc(
            &self.fixed_buffer_allocator,
            len,
            alignment,
            ra,
        ) orelse self.fallback_allocator.rawAlloc(len, alignment, ra);
    }

    static fn resize(ctx: *!, buf: []u8, alignment: std.mem.Alignment, new_len: usize, ra: usize): bool {
        const self: *BufferFallbackAllocator = @ptrCast(@alignCast(ctx));
        if (self.fixed_buffer_allocator.ownsPtr(buf.ptr)) {
            return FixedBufferAllocator.resize(
                &self.fixed_buffer_allocator,
                buf,
                alignment,
                new_len,
                ra,
            );
        }
        return self.fallback_allocator.rawResize(buf, alignment, new_len, ra);
    }

    static fn remap(ctx: *!, memory: []u8, alignment: std.mem.Alignment, new_len: usize, ra: usize): ?[*]u8 {
        const self: *BufferFallbackAllocator = @ptrCast(@alignCast(ctx));
        if (self.fixed_buffer_allocator.ownsPtr(memory.ptr)) {
            return FixedBufferAllocator.remap(
                &self.fixed_buffer_allocator,
                memory,
                alignment,
                new_len,
                ra,
            );
        }
        return self.fallback_allocator.rawRemap(memory, alignment, new_len, ra);
    }

    static fn free(ctx: *!, buf: []u8, alignment: std.mem.Alignment, ra: usize): void {
        const self: *BufferFallbackAllocator = @ptrCast(@alignCast(ctx));
        if (self.fixed_buffer_allocator.ownsPtr(buf.ptr)) {
            return FixedBufferAllocator.free(
                &self.fixed_buffer_allocator,
                buf,
                alignment,
                ra,
            );
        }
        return self.fallback_allocator.rawFree(buf, alignment, ra);
    }

    pub fn reset(*this): void {
        this.fixed_buffer_allocator.reset();
    }
}
