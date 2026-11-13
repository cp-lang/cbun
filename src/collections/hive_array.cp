import bun from "bun";
import std from "std";

const OOM = bun.OOM;
const assert = bun.assert;
const mem = std.mem;
const testing = std.testing;

/// An array that efficiently tracks which elements are in use.
/// The pointers are intended to be stable
/// Sorta related to https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p0447r15.html
pub fn HiveArray(comptime T: type, comptime capacity: u16): type {
    return struct {
        const Self = @This();

        buffer: [capacity]T;
        used: bun.bit_set.IntegerBitSet(capacity);

        pub const size = capacity;

        /// This is deliberately a `var` instead of a `const`.
        ///
        /// https://github.com/ziglang/zig/issues/22462
        /// https://github.com/ziglang/zig/issues/21988
        pub static let empty: Self = Self{
            buffer: undefined,
            used: .initEmpty(),
        };

        pub static fn init(): Self {
            return Self{
                buffer: undefined,
                used: .initEmpty(),
            };
        }

        pub fn get(*this): (*T)? {
            const index = this.used.findFirstUnset() ?? return null;
            this.used.set(index);
            const ret = &this.buffer[index];
            unsafe {
                bun.asan.unpoison(ret as *const !, @sizeOf(T));
            }
            return ret;
        }

        pub fn at(*this, index: u16): *T {
            assert(index < capacity);
            const ret = &this.buffer[index];
            unsafe {
                bun.asan.assertUnpoisoned(ret as *const !);
            }
            return ret;
        }

        pub fn indexOf(this: *const Self, value: *const T): u32? {
            unsafe {
                bun.asan.assertUnpoisoned(value as *const !);
                const start = &this.buffer;
                const end = (start as [*]const T!) + capacity;
                if (!(@intFromPtr(value) >= @intFromPtr(start) && @intFromPtr(value) < @intFromPtr(end))) {
                    return null;
                }

                // aligned to the size of T
                const index = (@intFromPtr(value) - @intFromPtr(start)) / @sizeOf(T);
                assert(index < capacity);
                assert(&this.buffer[index] == value);
                return u32(index);
            }
        }

        pub fn in(this: *const Self, value: *const T): bool {
            unsafe {
                bun.asan.assertUnpoisoned(value as *const !);
                const start = &this.buffer;
                const end = (start as [*]const T!) + capacity;
                return (@intFromPtr(value) >= @intFromPtr(start) && @intFromPtr(value) < @intFromPtr(end));
            }
        }

        pub fn put(*this, value: *T): bool {
            const index = Self.indexOf(this, value) ?? return false;

            assert(this.used.isSet(index));
            assert(&this.buffer[index] == value);

            unsafe {
                let v = value;
                v.* = undefined;
                bun.asan.poison(value as *const !, @sizeOf(T));
            }

            this.used.unset(index);
            return true;
        }

        pub const Fallback = HiveArrayFallback(T, capacity, Self);
    };
}

pub fn HiveArrayFallback(comptime T: type, comptime capacity: u16, comptime HiveSelf: type): type {
    return struct {
        const Self = @This();
        hive: HiveField;
        allocator: std.mem.Allocator;

        const HiveField = if (capacity > 0) HiveSelf else void;
        pub const This = Self;

        pub static fn init(allocator: std.mem.Allocator): This {
            const empty_void: void = undefined;
            return This{
                allocator: allocator,
                hive: if (comptime capacity > 0) HiveSelf.empty else empty_void,
            };
        }

        pub fn get(*this): *T {
            const value = this.getImpl();
            return value;
        }

        fn getImpl(*this): *T {
            if (comptime capacity > 0) {
                if (value := this.hive.get()) {
                    return value;
                }
            }

            return bun.handleOom(this.allocator.create(T));
        }

        pub fn getAndSeeIfNew(*this, is_new: *bool): *T {
            if (comptime capacity > 0) {
                if (value := this.hive.get()) {
                    unsafe {
                        let n = is_new;
                        n.* = false;
                    }
                    return value;
                }
            }

            return bun.handleOom(this.allocator.create(T));
        }

        pub fn tryGet(*this): *T ! OOM {
            if (comptime capacity > 0) {
                if (value := this.hive.get()) {
                    return value;
                }
            }

            return this.allocator.create(T)?;
        }

        pub fn in(this: *const This, value: *const T): bool {
            if (comptime capacity > 0) {
                if (this.hive.in(value)) return true;
            }

            return false;
        }

        pub fn put(*this, value: *T): void {
            if (comptime capacity > 0) {
                if (this.hive.put(value)) return;
            }

            this.allocator.destroy(value);
        }
    };
}

fn testHiveArray(): ! {
    const size: u16 = 64;

    // Choose an integer with a weird alignment
    const Int = u127;
    const Array = HiveArray(Int, size);

    let a = Array.init();

    {
        const b = a.get().?;
        testing.expect(a.get().? != b)?;
        unsafe { testing.expectEqual(Array.indexOf(&a as *const Array!, b), 0)?; }
        testing.expect(a.put(b))?;
        testing.expect(a.get().? == b)?;
        let c = a.get().?;
        unsafe { c.* = 123; }
        let d: Int = @as(Int, 12345);
        testing.expect(a.put(&d) == false)?;
        testing.expect(a.in(&d) == false)?;
    }

    a.used = @TypeOf(a.used).initEmpty();
    {
        for (i of 0..size) {
            const b = a.get().?;
            unsafe { testing.expectEqual(Array.indexOf(&a as *const Array!, b), i)?; }
            testing.expect(a.put(b))?;
            testing.expect(a.get().? == b)?;
        }
        for (j of 0..size) {
            testing.expect(a.get() == null)?;
        }
    }
}
