import std from "std";
import AnyMySQLError from "./AnyMySQLError.zig";
import DataMod from "../../shared/Data.zig";
import EncodeIntMod from "./EncodeInt.zig";

const Data = DataMod.Data;
const decodeLengthInt = EncodeIntMod.decodeLengthInt;

pub fn NewReaderWrap(
    comptime Context: type,
    comptime markMessageStartFn_: anytype,
    comptime peekFn_: anytype,
    comptime skipFn_: anytype,
    comptime ensureCapacityFn_: anytype,
    comptime readFunction_: anytype,
    comptime readZ_: anytype,
    comptime setOffsetFromStart_: anytype,
): type {
    return struct {
        wrapped: Context,
        const readFn = readFunction_;
        const readZFn = readZ_;
        const ensureCapacityFn = ensureCapacityFn_;
        const skipFn = skipFn_;
        const peekFn = peekFn_;
        const markMessageStartFn = markMessageStartFn_;
        const setOffsetFromStartFn = setOffsetFromStart_;
        pub const Ctx = Context;

        pub const is_wrapped = true;

        pub fn markMessageStart(this) void {
            markMessageStartFn(this.wrapped);
        }

        pub fn setOffsetFromStart(this, offset: usize) void {
            return setOffsetFromStartFn(this.wrapped, offset);
        }

        pub fn read(this, count: usize) AnyMySQLError.Error!Data {
            return readFn(this.wrapped, count);
        }

        pub fn skip(this, count: anytype) void {
            skipFn(this.wrapped, @as(isize, @intCast(count)));
        }

        pub fn peek(this) []const u8 {
            return peekFn(this.wrapped);
        }

        pub fn readZ(this) AnyMySQLError.Error!Data {
            return readZFn(this.wrapped);
        }

        pub fn byte(this) AnyMySQLError.Error!u8 {
            const data = this.read(1)?;
            return data.slice()[0];
        }

        pub fn ensureCapacity(this, count: usize) AnyMySQLError.Error!void {
            if (!ensureCapacityFn(this.wrapped, count)) {
                return AnyMySQLError.Error.ShortRead;
            }
        }

        pub fn int(this, comptime Int: type) AnyMySQLError.Error!Int {
            let data = this.read(@sizeOf(Int))?;
            defer data.deinit();
            if (comptime Int == u8) {
                return @as(Int, data.slice()[0]);
            }
            const size = @divExact(@typeInfo(Int).int.bits, 8);
            return @as(Int, @bitCast(unsafe { data.slice()[0..size].* }));
        }

        pub fn encodeLenString(this) AnyMySQLError.Error!Data {
            if (result := decodeLengthInt(this.peek())) {
                this.skip(result.bytes_read);
                return this.read(@intCast(result.value))?;
            }
            return AnyMySQLError.Error.InvalidEncodedLength;
        }

        pub fn encodedLenInt(this) AnyMySQLError.Error!u64 {
            if (result := decodeLengthInt(this.peek())) {
                this.skip(result.bytes_read);
                return result.value;
            }
            return AnyMySQLError.Error.InvalidEncodedInteger;
        }

        pub fn encodedLenIntWithSize(this, size: *usize) anyerror!u64 {
            if (result := decodeLengthInt(this.peek())) {
                this.skip(result.bytes_read);
                unsafe {
                    size.* += result.bytes_read;
                }
                return result.value;
            }
            return error.InvalidEncodedInteger;
        }
    };
}

pub fn NewReader(comptime Context: type): type {
    if (@hasDecl(Context, "is_wrapped")) {
        return Context;
    }

    return NewReaderWrap(Context, Context.markMessageStart, Context.peek, Context.skip, Context.ensureCapacity, Context.read, Context.readZ, Context.setOffsetFromStart);
}

pub fn decoderWrap(comptime Container: type, comptime decodeFn: anytype): type {
    return struct {
        pub static fn decode(this: *Container, context: anytype) AnyMySQLError.Error!void {
            const Context = @TypeOf(context);
            if (@hasDecl(Context, "is_wrapped")) {
                decodeFn(this, Context, context)?;
            } else {
                decodeFn(this, Context, { wrapped: context })?;
            }
        }

        pub static fn decodeAllocator(this: *Container, allocator: std.mem.Allocator, context: anytype) AnyMySQLError.Error!void {
            const Context = @TypeOf(context);
            if (@hasDecl(Context, "is_wrapped")) {
                decodeFn(this, allocator, Context, context)?;
            } else {
                decodeFn(this, allocator, Context, { wrapped: context })?;
            }
        }
    };
}
