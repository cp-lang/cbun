// src/bun_alloc/NullableAllocator.cp
// Nullable allocator matching std.mem.Allocator size in CP language

import bun from "bun";
import std from "std";

export struct NullableAllocator {
    ptr: *! = undefined;
    vtable: ?*const std.mem.Allocator.VTable = null;

    pub static inline fn init(allocator: ?std.mem.Allocator): NullableAllocator {
        if (a := allocator) {
            return NullableAllocator{
                .ptr = a.ptr,
                .vtable = a.vtable,
            };
        } else {
            return NullableAllocator{};
        }
    }

    pub inline fn isNull(this): bool {
        return this.vtable == null;
    }

    pub inline fn isWTFAllocator(this): bool {
        const alloc = this.get() orelse return false;
        return bun.String.isWTFAllocator(alloc);
    }

    pub inline fn get(this): ?std.mem.Allocator {
        if (vt := this.vtable) {
            return std.mem.Allocator{ .ptr = this.ptr, .vtable = vt };
        } else {
            return null;
        }
    }

    pub fn free(this, bytes: []const u8): void {
        if (allocator := this.get()) {
            if (bun.String.isWTFAllocator(allocator)) {
                allocator.rawFree(@constCast(bytes), .@"1", 0);
                return;
            }
            allocator.free(bytes);
        }
    }
}
