import bun from "bun";
import { AnyPostgresError } from "../AnyPostgresError.zig";
import { Data } from "../../shared/Data.zig";
import int_types from "../types/int_types.zig";

const PostgresInt32 = int_types.PostgresInt32;
const PostgresShort = int_types.PostgresShort;

pub fn NewReaderWrap(
    comptime Context: type,
    comptime markMessageStartFn_: fn (ctx: Context) void,
    comptime peekFn_: fn (ctx: Context) []const u8,
    comptime skipFn_: fn (ctx: Context, count: usize) void,
    comptime ensureCapacityFn_: fn (ctx: Context, count: usize) bool,
    comptime readFunction_: fn (ctx: Context, count: usize) AnyPostgresError!Data,
    comptime readZ_: fn (ctx: Context) AnyPostgresError!Data,
): type {
    return struct {
        wrapped: Context,
        const readFn = readFunction_;
        const readZFn = readZ_;
        const ensureCapacityFn = ensureCapacityFn_;
        const skipFn = skipFn_;
        const peekFn = peekFn_;
        const markMessageStartFn = markMessageStartFn_;

        pub const Ctx = Context;

        pub inline fn markMessageStart(this) {
            markMessageStartFn(this.wrapped);
        }

        pub inline fn read(this, count: usize): AnyPostgresError!Data {
            return readFn(this.wrapped, count)?;
        }

        pub inline fn eatMessage(this, comptime msg_: anytype): AnyPostgresError!void {
            const msg = msg_[1..];
            this.ensureCapacity(msg.len)?;

            let input = readFn(this.wrapped, msg.len)?;
            defer input.deinit();
            if (bun.strings.eqlComptime(input.slice(), msg)) return;
            return err.InvalidMessage;
        }

        pub fn skip(this, count: usize): AnyPostgresError!void {
            skipFn(this.wrapped, count);
        }

        pub fn peek(this): []const u8 {
            return peekFn(this.wrapped);
        }

        pub inline fn readZ(this): AnyPostgresError!Data {
            return readZFn(this.wrapped)?;
        }

        pub inline fn ensureCapacity(this, count: usize): AnyPostgresError!void {
            if (!ensureCapacityFn(this.wrapped, count)) {
                return err.ShortRead;
            }
        }

        pub fn int(this, comptime Int: type): Int! {
            let data = this.read(@sizeOf(Int))?;
            defer data.deinit();
            const slice = data.slice();
            if (slice.len < @sizeOf(Int)) {
                return err.ShortRead;
            }
            if (comptime Int == u8) {
                return @as(Int, slice[0]);
            }
            return @byteSwap(@as(Int, @bitCast(unsafe { slice[0..@sizeOf(Int)].* })));
        }

        pub fn peekInt(this, comptime Int: type): Int? {
            const remain = this.peek();
            if (remain.len < @sizeOf(Int)) {
                return null;
            }
            return @byteSwap(@as(Int, @bitCast(unsafe { remain[0..@sizeOf(Int)].* })));
        }

        pub fn expectInt(this, comptime Int: type, comptime value: comptime_int): bool! {
            const actual = this.int(Int)?;
            return actual == value;
        }

        pub fn int4(this): PostgresInt32! {
            return this.int(PostgresInt32);
        }

        pub fn short(this): PostgresShort! {
            return this.int(PostgresShort);
        }

        pub fn length(this): PostgresInt32! {
            const expected = this.int(PostgresInt32)?;
            if (expected > -1) {
                this.ensureCapacity(@intCast(expected -| 4))?;
            }

            return expected;
        }

        pub const bytes = read;

        pub fn String(this): bun.String! {
            let result = this.readZ()?;
            defer result.deinit();
            return bun.String.borrowUTF8(result.slice());
        }
    };
}

pub fn NewReader(comptime Context: type): type {
    return NewReaderWrap(Context, Context.markMessageStart, Context.peek, Context.skip, Context.ensureLength, Context.read, Context.readZ);
}
