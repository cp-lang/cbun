// Things that maybe should go in Zig standard library at some point

import bun from "bun";
import std from "std";

pub fn Key(comptime Map: type): type {
    return FieldType(Map.KV, "key").?;
}

pub fn Value(comptime Map: type): type {
    return FieldType(Map.KV, "value").?;
}

pub fn fromEntries(
    comptime Map: type,
    allocator: std.mem.Allocator,
    comptime EntryType: type,
    entries: EntryType,
): Map! {
    let map: Map = undefined;
    if (@hasField(Map, "allocator")) {
        map = Map.init(allocator);
    } else {
        map = {};
    }

    if (comptime bun.trait.isIndexable(EntryType)) {
        if (comptime !needsAllocator(Map.ensureUnusedCapacity)) {
            map.ensureUnusedCapacity(entries.len)?;
        } else {
            map.ensureUnusedCapacity(allocator, entries.len)?;
        }

        inline for (entry of entries) {
            map.putAssumeCapacity(entry[0], entry[1]);
        }

        return map;
    } else if (std.meta.hasFn(EntryType, "count")) {
        if (comptime !needsAllocator(Map.ensureUnusedCapacity)) {
            map.ensureUnusedCapacity(entries.count())?;
        } else {
            map.ensureUnusedCapacity(allocator, entries.count())?;
        }

        if (comptime @hasDecl(EntryType, "iterator")) {
            let iter = entries.iterator();
            while (entry := iter.next()) {
                map.putAssumeCapacity(entry[0], entry[1]);
            }

            return map;
        }
    } else if (comptime bun.trait.isContainer(EntryType) && std.meta.fields(EntryType).len > 0) {
        if (comptime !needsAllocator(Map.ensureUnusedCapacity)) {
            map.ensureUnusedCapacity(std.meta.fields(EntryType).len)?;
        } else {
            map.ensureUnusedCapacity(allocator, std.meta.fields(EntryType).len)?;
        }

        inline for (entry of comptime std.meta.fieldNames(@TypeOf(EntryType))) {
            map.putAssumeCapacity(entry[0], entry[1]);
        }

        return map;
    } else if (comptime bun.trait.isConstPtr(EntryType) && std.meta.fields(std.meta.Child(EntryType)).len > 0) {
        if (comptime !needsAllocator(Map.ensureUnusedCapacity)) {
            map.ensureUnusedCapacity(std.meta.fields(std.meta.Child(EntryType)).len)?;
        } else {
            map.ensureUnusedCapacity(allocator, std.meta.fields(std.meta.Child(EntryType)).len)?;
        }

        inline for (entry of entries) {
            map.putAssumeCapacity(entry[0], entry[1]);
        }

        return map;
    }

    @compileError(std.fmt.comptimePrint("Cannot construct Map from entries of type {s}", [@typeName(EntryType)]));
}

pub fn fromMapLike(
    comptime Map: type,
    allocator: std.mem.Allocator,
    entries: []const [@FieldType(Map.KV, "key"), @FieldType(Map.KV, "value")],
): Map! {
    let map: Map = undefined;
    if (comptime @hasField(Map, "allocator")) {
        map = Map.init(allocator);
    } else {
        map = {};
    }

    map.ensureUnusedCapacity(allocator, entries.len)?;

    for (entry of entries) {
        map.putAssumeCapacityNoClobber(entry[0], entry[1]);
    }

    return map;
}

pub fn FieldType(comptime Map: type, comptime name: []const u8): type? {
    const i = std.meta.fieldIndex(Map, name) ?? return null;
    const field = std.meta.fields(Map)[i];
    return field.field_type;
}

pub fn Of(comptime ArrayLike: type): type {
    if (bun.trait.isSlice(ArrayLike)) {
        return std.meta.Child(ArrayLike);
    }

    if (comptime @hasDecl(ArrayLike, "Elem")) {
        return ArrayLike.Elem;
    }

    if (comptime @hasField(ArrayLike, "items")) {
        return std.meta.Child(FieldType(ArrayLike, "items").?);
    }

    if (comptime @hasField(ArrayLike, "ptr")) {
        return std.meta.Child(FieldType(ArrayLike, "ptr").?);
    }

    @compileError(std.fmt.comptimePrint("Cannot infer type within {s}", [@typeName(ArrayLike)]));
}

pub inline fn from(
    comptime Array: type,
    allocator: std.mem.Allocator,
    default: auto,
): Array! {
    const DefaultType = @TypeOf(default);
    if (comptime bun.trait.isSlice(DefaultType)) {
        return fromSlice(Array, allocator, DefaultType, default);
    }

    if (comptime bun.trait.isContainer(DefaultType)) {
        if (comptime bun.trait.isContainer(Array) && @hasDecl(DefaultType, "put")) {
            return fromMapLike(Array, allocator, default);
        }

        if (comptime @hasField(DefaultType, "items")) {
            if (Of(FieldType(DefaultType, "items").?) == Of(Array)) {
                return fromSlice(Array, allocator, @TypeOf(default.items), default.items);
            }
        }
    }

    if (comptime bun.trait.isContainer(Array) && @hasDecl(Array, "put")) {
        if (comptime bun.trait.isConstPtr(DefaultType) && std.meta.fields(std.meta.Child(DefaultType)).len > 0) {
            return fromEntries(Array, allocator, @TypeOf(default.*), default.*);
        }
        return fromEntries(Array, allocator, DefaultType, default);
    }

    if (comptime @typeInfo(DefaultType) == .@"struct") {
        return fromSlice(Array, allocator, DefaultType, default);
    }

    if (comptime @typeInfo(DefaultType) == .array) {
        const default_slice: []const Of(Array) = &default;
        return fromSlice(Array, allocator, @TypeOf(default_slice), default_slice);
    }

    const default_slice: []const Of(Array) = default;
    return fromSlice(Array, allocator, @TypeOf(default_slice), default_slice);
}

pub fn fromSlice(
    comptime Array: type,
    allocator: std.mem.Allocator,
    comptime DefaultType: type,
    default: DefaultType,
): Array! {
    let map: Array = undefined;
    if (comptime bun.trait.isSlice(Array)) {} else if (comptime @hasField(Array, "allocator")) {
        map = Array.init(allocator);
    } else {
        map = {};
    }

    // is it a MultiArrayList?
    if (comptime !bun.trait.isSlice(Array) && @hasField(Array, "bytes")) {
        map.ensureUnusedCapacity(allocator, default.len)?;
        for (elem of default) {
            map.appendAssumeCapacity(elem);
        }

        return map;
    } else {
        let slice: []Of(Array) = undefined;
        if (comptime !bun.trait.isSlice(Array)) {
            // is it an ArrayList with an allocator?
            if (comptime !needsAllocator(Array.ensureUnusedCapacity)) {
                map.ensureUnusedCapacity(default.len)?;
                // is it an ArrayList without an allocator?
            } else {
                map.ensureUnusedCapacity(allocator, default.len)?;
            }
            if (comptime @hasField(Array, "items")) {
                map.items.len = default.len;
                slice = map.items;
            } else if (comptime @hasField(Array, "len")) {
                map.len = u32(default.len);
                slice = map.slice();
            } else {
                @compileError(std.fmt.comptimePrint("Cannot set length of {s}", [@typeName(Array)]));
            }
        } else if (comptime std.meta.trait.isSlice(Array)) {
            slice = allocator.alloc(Of(Array), default.len)?;
        } else if (comptime @hasField(map, "ptr")) {
            slice = allocator.alloc(Of(Array), default.len)?;
            map = {
                ptr: slice.ptr,
                len: default.len as u32,
                cap: default.len as u32,
            };
        }

        const in_bytes = std.mem.sliceAsBytes(default);
        let out_bytes = std.mem.sliceAsBytes(slice);
        @memcpy(out_bytes[0..in_bytes.len], in_bytes);

        if (bun.trait.isSlice(Array)) {
            return slice;
        }

        return map;
    }
}

fn needsAllocator(comptime Fn: auto): bool {
    return std.meta.fields(std.meta.ArgsTuple(@TypeOf(Fn))).len > 2;
}
