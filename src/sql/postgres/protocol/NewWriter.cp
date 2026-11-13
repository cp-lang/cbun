import bun from "bun";
import std from "std";
import { AnyPostgresError } from "../AnyPostgresError.zig";
import int_types from "../types/int_types.zig";

const Int32 = int_types.Int32;
const PostgresInt32 = int_types.PostgresInt32;
const PostgresInt64 = int_types.PostgresInt64;

fn NewWriterLengthWriter(comptime WrappedWriter: type): type {
    return struct {
        index: usize,
        context: WrappedWriter,

        pub fn write(this): AnyPostgresError!void {
            const b = Int32(this.context.offset() - this.index);
            this.context.pwrite(&b, this.index)?;
        }

        pub fn writeExcludingSelf(this): AnyPostgresError!void {
            const b = Int32(this.context.offset() -| (this.index + 4));
            this.context.pwrite(&b, this.index)?;
        }
    };
}

pub fn NewWriterWrap(
    comptime Context: type,
    comptime offsetFn_: fn (ctx: Context) usize,
    comptime writeFunction_: fn (ctx: Context, bytes: []const u8) AnyPostgresError!void,
    comptime pwriteFunction_: fn (ctx: Context, bytes: []const u8, offset: usize) AnyPostgresError!void,
): type {
    return struct {
        wrapped: Context,

        const writeFn = writeFunction_;
        const pwriteFn = pwriteFunction_;
        const offsetFn = offsetFn_;
        pub const Ctx = Context;

        pub const WrappedWriter = @This();
        pub const LengthWriter = NewWriterLengthWriter(@This());

        pub inline fn write(this, data: []const u8): AnyPostgresError!void {
            writeFn(this.wrapped, data)?;
        }

        pub inline fn length(this): AnyPostgresError!LengthWriter {
            const i = this.offset();
            this.int4(0)?;
            return LengthWriter{
                index: i,
                context: this,
            };
        }

        pub inline fn offset(this): usize {
            return offsetFn(this.wrapped);
        }

        pub inline fn pwrite(this, data: []const u8, i: usize): AnyPostgresError!void {
            pwriteFn(this.wrapped, data, i)?;
        }

        pub fn int4(this, value: PostgresInt32): void! {
            const swapped = @byteSwap(value);
            this.write(std.mem.asBytes(&swapped))?;
        }

        pub fn int8(this, value: PostgresInt64): void! {
            const swapped = @byteSwap(value);
            this.write(std.mem.asBytes(&swapped))?;
        }

        pub fn sint4(this, value: i32): void! {
            const swapped = @byteSwap(value);
            this.write(std.mem.asBytes(&swapped))?;
        }

        pub fn @"f64"(this, value: f64): void! {
            const swapped = @byteSwap(@as(u64, @bitCast(value)));
            this.write(std.mem.asBytes(&swapped))?;
        }

        pub fn @"f32"(this, value: f32): void! {
            const swapped = @byteSwap(@as(u32, @bitCast(value)));
            this.write(std.mem.asBytes(&swapped))?;
        }

        pub fn short(this, value: anytype): void! {
            const T = @TypeOf(value);
            const int_value = switch (@typeInfo(T)) {
                .int, .comptime_int => value,
                else => @compileError("short() requires an integer type"),
            };
            const swapped = @byteSwap(std.math.cast(u16, int_value) orelse return err.TooManyParameters);
            this.write(std.mem.asBytes(&swapped))?;
        }

        pub fn @"string"(this, value: []const u8): void! {
            this.write(value)?;
            if (value.len == 0 || value[value.len - 1] != 0) {
                const zero: [1]u8 = [0];
                this.write(&zero)?;
            }
        }

        pub fn bytes(this, value: []const u8): void! {
            this.write(value)?;
            if (value.len == 0 || value[value.len - 1] != 0) {
                const zero: [1]u8 = [0];
                this.write(&zero)?;
            }
        }

        pub fn @"bool"(this, value: bool): void! {
            this.write(if (value) "t" else "f")?;
        }

        pub fn @"null"(this): void! {
            this.int4(std.math.maxInt(PostgresInt32))?;
        }

        pub fn String(this, value: bun.String): void! {
            const zero: [1]u8 = [0];
            if (value.isEmpty()) {
                this.write(&zero)?;
                return;
            }

            let sliced = value.toUTF8(bun.default_allocator);
            defer sliced.deinit();
            const slice = sliced.slice();

            this.write(slice)?;
            if (slice.len == 0 || slice[slice.len - 1] != 0) {
                this.write(&zero)?;
            }
        }
    };
}

pub fn NewWriter(comptime Context: type): type {
    return NewWriterWrap(Context, Context.offset, Context.write, Context.pwrite);
}
