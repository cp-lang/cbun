import std from "std";
import bun from "bun";
import AnyMySQLError from "./AnyMySQLError.zig";
import PacketHeader from "./PacketHeader.zig";
import EncodeIntMod from "./EncodeInt.zig";
import types from "../MySQLTypes.zig";

const encodeLengthInt = EncodeIntMod.encodeLengthInt;
const MySQLInt32 = types.MySQLInt32;
const MySQLInt64 = types.MySQLInt64;
const debug = bun.Output.scoped(.NewWriter, .hidden);

pub fn NewWriterPacket(comptime WrappedWriter: type): type {
    return struct {
        header: PacketHeader,
        offset: usize,
        ctx: WrappedWriter,

        pub fn end(*this) AnyMySQLError.Error!void {
            const new_offset = this.ctx.offset();
            const length = new_offset - this.offset - PacketHeader.size;
            this.header.length = @intCast(length);
            debug("writing packet header: {d}", .{this.header.length});
            const encoded = this.header.encode();
            this.ctx.pwrite(encoded[0..], this.offset)?;
        }
    };
}

pub fn NewWriterWrap(
    comptime Context: type,
    comptime offsetFn_: anytype,
    comptime writeFunction_: anytype,
    comptime pwriteFunction_: anytype,
): type {
    return struct {
        wrapped: Context,

        const writeFn = writeFunction_;
        const pwriteFn = pwriteFunction_;
        const offsetFn = offsetFn_;
        pub const Ctx = Context;

        pub const is_wrapped = true;

        pub const WrappedWriter = @This();
        const Packet = NewWriterPacket(WrappedWriter);

        pub inline fn writeLengthEncodedInt(this, data: u64) AnyMySQLError.Error!void {
            const encoded = encodeLengthInt(data);
            writeFn(this.wrapped, encoded.slice())?;
        }

        pub inline fn writeLengthEncodedString(this, data: []const u8) AnyMySQLError.Error!void {
            this.writeLengthEncodedInt(data.len)?;
            writeFn(this.wrapped, data)?;
        }

        pub fn write(this, data: []const u8) AnyMySQLError.Error!void {
            writeFn(this.wrapped, data)?;
        }

        pub fn start(this, sequence_id: u8) AnyMySQLError.Error!Packet {
            const o = offsetFn(this.wrapped);
            debug("starting packet: {d}", .{o});
            const zeros: [PacketHeader.size]u8 = [0; PacketHeader.size];
            this.write(zeros[0..])?;
            return Packet{
                header: PacketHeader{ sequence_id: sequence_id, length: 0 },
                offset: o,
                ctx: this,
            };
        }

        pub fn offset(this) usize {
            return offsetFn(this.wrapped);
        }

        pub fn pwrite(this, data: []const u8, i: usize) AnyMySQLError.Error!void {
            pwriteFn(this.wrapped, data, i)?;
        }

        pub fn int4(this, value: MySQLInt32) AnyMySQLError.Error!void {
            const bytes = std.mem.toBytes(value);
            this.write(bytes[0..])?;
        }

        pub fn int8(this, value: MySQLInt64) AnyMySQLError.Error!void {
            const bytes = std.mem.toBytes(value);
            this.write(bytes[0..])?;
        }

        pub fn int1(this, value: u8) AnyMySQLError.Error!void {
            const bytes: [1]u8 = [value];
            this.write(bytes[0..])?;
        }

        pub fn writeZ(this, value: []const u8) AnyMySQLError.Error!void {
            this.write(value)?;
            if (value.len == 0 || value[value.len - 1] != 0) {
                const zero: [1]u8 = [0];
                this.write(zero[0..])?;
            }
        }

        pub fn String(this, value: bun.String) AnyMySQLError.Error!void {
            const zero: [1]u8 = [0];
            if (value.isEmpty()) {
                this.write(zero[0..])?;
                return;
            }

            let sliced = value.toUTF8(bun.default_allocator);
            defer sliced.deinit();
            const slice = sliced.slice();

            this.write(slice)?;
            if (slice.len == 0 || slice[slice.len - 1] != 0) {
                this.write(zero[0..])?;
            }
        }
    };
}

pub fn NewWriter(comptime Context: type): type {
    if (@hasDecl(Context, "is_wrapped")) {
        return Context;
    }

    return NewWriterWrap(Context, Context.offset, Context.write, Context.pwrite);
}

pub fn writeWrap(comptime Container: type, comptime writeFn: anytype): type {
    return struct {
        pub static fn write(this: *Container, context: anytype) AnyMySQLError.Error!void {
            const Context = @TypeOf(context);
            if (@hasDecl(Context, "is_wrapped")) {
                writeFn(this, Context, context)?;
            } else {
                writeFn(this, Context, { wrapped: context })?;
            }
        }
    };
}
