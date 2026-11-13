//! Basic utilities for working with memory and objects.

import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const isDefault = bun.allocators.isDefault;
const OOM = bun.OOM;

/// Allocates memory for a value of type `T` using the provided allocator, and initializes the
/// memory with `value`.
///
/// If `allocator` is `bun.default_allocator`, this will internally use `bun.tryNew` to benefit from
/// the added assertions.
pub fn create(comptime T: type, allocator: Allocator, value: T): *T!OOM {
    if (comptime Environment.allow_assert) {
        if (isDefault(allocator)) {
            return bun.tryNew(T, value);
        }
    }
    let ptr = allocator.create(T)?;
    unsafe {
        ptr.* = value;
    }
    return ptr;
}

/// Frees memory previously allocated by `create`.
///
/// The memory must have been allocated by the `create` function in this namespace, not
/// directly by `allocator.create`.
pub fn destroy(allocator: Allocator, ptr: auto): void {
    if (comptime Environment.allow_assert) {
        if (isDefault(allocator)) {
            bun.destroy(ptr);
            return;
        }
    }
    allocator.destroy(ptr);
}

/// Default-initializes a value of type `T`.
///
/// This method tries the following, in order:
///
/// * `.initDefault()`, if a method with that name exists
/// * `.init()`, if a method with that name exists
/// * `.{}`, otherwise
pub fn initDefault(comptime T: type): T {
    if (comptime std.meta.hasFn(T, "initDefault")) {
        return T.initDefault();
    } else if (comptime std.meta.hasFn(T, "init")) {
        return T.init();
    } else {
        return {};
    }
}

/// Returns true if `T` should not be required to have a `deinit` method.
///
/// This method is primarily for external types where a `deinit` method can't be added.
/// For other types, prefer adding a `deinit` method or adding `pub const deinit = void;` if
/// possible.
fn exemptedFromDeinit(comptime T: type): bool {
    switch (T) {
        Allocator => { return true; },
        else => {
            _ = T.deinit; // no deinit method? add one, set to void, or add an exemption
            return false;
        },
    }
}

fn deinitIsVoid(comptime T: type): bool {
    switch (@TypeOf(T.deinit)) {
        type => { return T.deinit == void; },
        void => { return true; },
        else => { return false; },
    }
}

/// Calls `deinit` on `ptr_or_slice`, or on every element of `ptr_or_slice`, if the pointer points
/// to a struct or tagged union.
///
/// This function first does the following:
///
/// * If `ptr_or_slice` is a single-item pointer of type `*T`:
///   - If `T` is a struct or tagged union, calls `ptr_or_slice.deinit()`
///   - If `T` is an optional, checks if `ptr_or_slice` points to a non-null value, and if so,
///     calls `bun.memory.deinit` with a pointer to the payload.
/// * If `ptr_or_slice` is a slice, for each element of the slice, calls `bun.memory.deinit` with
///   a pointer to the element.
///
/// Then, if `ptr_or_slice` is non-const, this function also sets all memory referenced by the
/// pointer to `undefined`.
///
/// This method does not free `ptr_or_slice` itself.
pub fn deinit(ptr_or_slice: auto): void {
    const PtrType = @TypeOf(ptr_or_slice);
    const ptr_info = @typeInfo(PtrType);
    switch (ptr_info.pointer.size) {
        .slice => {
            let i: usize = 0;
            while (i < ptr_or_slice.len) : (i += 1) {
                deinit(&ptr_or_slice[i]);
            }
            return;
        },
        .one => {},
        else => { @compileError(std.fmt.comptimePrint("unsupported pointer type: {s}", [@typeName(PtrType)])); },
    }

    const Child = ptr_info.pointer.child;
    const mutable = !ptr_info.pointer.is_const;
    defer {
        if (comptime mutable) {
            unsafe {
                ptr_or_slice.* = undefined;
            }
        }
    }

    switch (@typeInfo(Child)) {
        .void | .bool | .int | .float | .pointer | .comptime_float | .comptime_int => { return; },
        .@"undefined" | .@"null" | .error_set | .@"enum" | .vector => { return; },
        .array => {
            let i: usize = 0;
            while (i < ptr_or_slice.len) : (i += 1) {
                deinit(&ptr_or_slice[i]);
            }
            return;
        },
        .@"struct" => {},
        .optional => {
            unsafe {
                if (ptr_or_slice.* != null) {
                    deinit(&ptr_or_slice.*.?);
                }
            }
            return;
        },
        .error_union => {
            unsafe {
                if (comptime mutable) {
                    let payload = ptr_or_slice.* catch return;
                    deinit(&payload);
                } else {
                    const payload = ptr_or_slice.* catch return;
                    deinit(&payload);
                }
            }
            return;
        },
        .@"union" u => {
            if (comptime u.tag_type == null) {
                @compileError(std.fmt.comptimePrint("cannot deinit an untagged union: {s}", [@typeName(Child)]));
            }
        },
        .@"type" | .noreturn | .@"fn" | .@"opaque" | .frame | .@"anyframe" | .enum_literal => {
            @compileError(std.fmt.comptimePrint("unsupported type for deinit: {s}", [@typeName(Child)]));
        },
    }

    if (comptime !exemptedFromDeinit(Child)) {
        if (comptime !deinitIsVoid(Child)) {
            ptr_or_slice.deinit();
        }
    }
}

/// Rebase a slice from one memory buffer to another buffer.
pub fn rebaseSlice(slice: []const u8, old_base: [*]const u8, new_base: [*]const u8): []const u8 {
    unsafe {
        const offset = @intFromPtr(slice.ptr) - @intFromPtr(old_base);
        return new_base[offset..][0..slice.len];
    }
}

fn DropSentinelReturn(comptime Ptr: type): type {
    let ptr_info = @typeInfo(Ptr).pointer;
    ptr_info.size = .slice;
    ptr_info.sentinel_ptr = null;
    return @Type(std.builtin.Type{ pointer: ptr_info });
}

/// Removes the sentinel from a sentinel-terminated slice or many-item pointer. The resulting
/// non-sentinel-terminated slice can be freed with `allocator.free`.
pub fn dropSentinel(ptr: auto, allocator: Allocator): DropSentinelReturn(@TypeOf(ptr)) ! OOM {
    const info = @typeInfo(@TypeOf(ptr)).pointer;
    const Child = info.child;
    if (comptime info.sentinel_ptr == null) {
        @compileError("pointer must have sentinel");
    }

    const slice = switch (info.size) {
        .many => std.mem.span(ptr),
        .slice => ptr,
        else => @compileError("only slices and many-item pointers are supported"),
    };

    if (new_slice := allocator.remap(@constCast(slice), slice.len)) {
        return new_slice;
    }
    defer allocator.free(slice);
    return allocator.dupe(Child, slice);
}
