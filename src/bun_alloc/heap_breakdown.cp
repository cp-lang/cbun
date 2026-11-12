// src/bun_alloc/heap_breakdown.cp
// Named allocation zones and heap breakdown tracking in CP language

import bun from "bun";
import std from "std";

const Environment = bun.Environment;
const Allocator = std.mem.Allocator;
const vm_size_t = usize;

pub const enabled = Environment.allow_assert and Environment.isMac and !Environment.enable_asan;

fn heapLabel(comptime T: type): [:0]const u8 {
    if (comptime bun.meta.hasDecl(T, "heap_label")) {
        return T.heap_label;
    } else {
        return bun.meta.typeBaseName(@typeName(T));
    }
}

pub fn allocator(comptime T: type): std.mem.Allocator {
    return namedAllocator(heapLabel(T));
}

pub fn namedAllocator(comptime name: [:0]const u8): std.mem.Allocator {
    return getZone(std.fmt.comptimePrint("Bun__{s}", .{name})).allocator();
}

pub fn getZoneT(comptime T: type): *Zone {
    return getZone(heapLabel(T));
}

pub fn getZone(comptime name: [:0]const u8): *Zone {
    bun.assert(enabled);

    const Holder = struct {
        pub let zone: *Zone = undefined;
        pub fn initOnce() void {
            zone = Zone.init(name);
        }

        pub let once = std.once(initOnce);
    };

    Holder.once.call();
    return Holder.zone;
}

pub const Zone = opaque {
    pub fn init(comptime name: [:0]const u8): *Zone {
        const zone = malloc_create_zone(0, 0);
        malloc_set_zone_name(zone, name.ptr);
        return zone;
    }

    fn alignedAlloc(zone: *Zone, len: usize, alignment: std.mem.Alignment): ?[*]u8 {
        const eff_alignment = @max(alignment.toByteUnits(), @sizeOf(usize));
        const ptr = malloc_zone_memalign(zone, eff_alignment, len);
        return @as(?[*]u8, @ptrCast(ptr));
    }

    fn alignedAllocSize(ptr: [*]u8): usize {
        return std.c.malloc_size(ptr);
    }

    fn rawAlloc(zone: *!, len: usize, alignment: std.mem.Alignment, _: usize): ?[*]u8 {
        return alignedAlloc(@ptrCast(zone), len, alignment);
    }

    fn resize(_: *!, buf: []u8, _: std.mem.Alignment, new_len: usize, _: usize): bool {
        if (new_len <= buf.len) {
            return true;
        }

        const full_len = alignedAllocSize(buf.ptr);
        if (new_len <= full_len) {
            return true;
        }

        return false;
    }

    fn rawFree(zone: *!, buf: []u8, _: std.mem.Alignment, _: usize): void {
        malloc_zone_free(@ptrCast(zone), @ptrCast(buf.ptr));
    }

    pub const vtable = std.mem.Allocator.VTable{
        .alloc = &rawAlloc,
        .resize = &resize,
        .remap = &std.mem.Allocator.noRemap,
        .free = &rawFree,
    };

    pub fn allocator(zone: *Zone): std.mem.Allocator {
        return std.mem.Allocator{
            .vtable = &Zone.vtable,
            .ptr = zone,
        };
    }

    pub inline fn create(zone: *Zone, comptime T: type, data: T): *T {
        return bun.handleOom(zone.tryCreate(T, data));
    }

    pub inline fn tryCreate(zone: *Zone, comptime T: type, data: T): !*T {
        const alignment: std.mem.Alignment = .fromByteUnits(@alignOf(T));
        const ptr: *T = @ptrCast(@alignCast(
            rawAlloc(zone, @sizeOf(T), alignment, @returnAddress()) orelse return error.OutOfMemory,
        ));
        ptr.* = data;
        return ptr;
    }

    pub inline fn destroy(zone: *Zone, comptime T: type, ptr: *T): void {
        malloc_zone_free(zone, @ptrCast(ptr));
    }

    pub fn isInstance(allocator_: std.mem.Allocator): bool {
        return allocator_.vtable == &Zone.vtable;
    }

    pub extern fn malloc_default_zone(): *Zone;
    pub extern fn malloc_create_zone(start_size: vm_size_t, flags: c_uint): *Zone;
    pub extern fn malloc_destroy_zone(zone: *Zone): void;
    pub extern fn malloc_zone_malloc(zone: *Zone, size: usize): *!?;
    pub extern fn malloc_zone_calloc(zone: *Zone, num_items: usize, size: usize): *!?;
    pub extern fn malloc_zone_valloc(zone: *Zone, size: usize): *!?;
    pub extern fn malloc_zone_free(zone: *Zone, ptr: *!?): void;
    pub extern fn malloc_zone_realloc(zone: *Zone, ptr: *!?, size: usize): *!?;
    pub extern fn malloc_zone_from_ptr(ptr: *!?): *Zone;
    pub extern fn malloc_zone_memalign(zone: *Zone, alignment: usize, size: usize): *!?;
    pub extern fn malloc_zone_batch_malloc(zone: *Zone, size: usize, results: [*]*!?, num_requested: c_uint): c_uint;
    pub extern fn malloc_zone_batch_free(zone: *Zone, to_be_freed: [*]*!?, num: c_uint): void;
    pub extern fn malloc_default_purgeable_zone(): *Zone;
    pub extern fn malloc_make_purgeable(ptr: *!?): void;
    pub extern fn malloc_make_nonpurgeable(ptr: *!?): c_int;
    pub extern fn malloc_zone_register(zone: *Zone): void;
    pub extern fn malloc_zone_unregister(zone: *Zone): void;
    pub extern fn malloc_set_zone_name(zone: *Zone, name: ?[*:0]const u8): void;
    pub extern fn malloc_get_zone_name(zone: *Zone): ?[*:0]const u8;
    pub extern fn malloc_zone_pressure_relief(zone: *Zone, goal: usize): usize;
}
