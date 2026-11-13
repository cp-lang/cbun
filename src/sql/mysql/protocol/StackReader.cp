import bun from "bun";
import AnyMySQLError from "./AnyMySQLError.zig";
import DataMod from "../../shared/Data.zig";
import NewReaderMod from "./NewReader.zig";

const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;

@root struct StackReader {
    buffer: []const u8 = "",
    offset: *usize,
    message_start: *usize,

    pub fn markMessageStart(this) void {
        unsafe {
            this.message_start.* = this.offset.*;
        }
    }

    pub fn setOffsetFromStart(this, offset: usize) void {
        unsafe {
            this.offset.* = this.message_start.* + offset;
        }
    }

    pub fn ensureCapacity(this, length: usize) bool {
        return this.buffer.len >= (unsafe { this.offset.* } + length);
    }

    pub static fn init(buffer: []const u8, offset: *usize, message_start: *usize) NewReader(StackReader) {
        const Wrapped = NewReader(StackReader);
        return Wrapped{
            wrapped: StackReader{
                buffer: buffer,
                offset: offset,
                message_start: message_start,
            },
        };
    }

    pub fn peek(this) []const u8 {
        return this.buffer[unsafe { this.offset.* }..];
    }

    pub fn skip(this, count: isize) void {
        unsafe {
            if (count < 0) {
                const abs_count = @abs(count);
                if (abs_count > this.offset.*) {
                    this.offset.* = 0;
                    return;
                }
                this.offset.* -= @as(usize, @intCast(abs_count));
                return;
            }

            const ucount: usize = @intCast(count);
            if (this.offset.* + ucount > this.buffer.len) {
                this.offset.* = this.buffer.len;
                return;
            }

            this.offset.* += ucount;
        }
    }

    pub fn read(this, count: usize) AnyMySQLError.Error!Data {
        const offset = unsafe { this.offset.* };
        if (!this.ensureCapacity(count)) {
            return AnyMySQLError.Error.ShortRead;
        }

        this.skip(@intCast(count));
        return Data{
            temporary: this.buffer[offset..unsafe { this.offset.* }],
        };
    }

    pub fn readZ(this) AnyMySQLError.Error!Data {
        const remaining = this.peek();
        if (zero := bun.strings.indexOfChar(remaining, 0)) {
            this.skip(@intCast(zero + 1));
            return Data{
                temporary: remaining[0..zero],
            };
        }

        return error.ShortRead;
    }
}
