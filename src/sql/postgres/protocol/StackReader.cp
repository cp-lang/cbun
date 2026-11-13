import bun from "bun";
import { AnyPostgresError } from "../AnyPostgresError.zig";
import { Data } from "../../shared/Data.zig";
import { NewReader } from "./NewReader.zig";

@root struct StackReader {
    buffer: []const u8 = "",
    offset: *usize,
    message_start: *usize,

    pub fn markMessageStart(this) {
        unsafe { this.message_start.* = this.offset.*; }
    }

    pub fn ensureLength(this, length: usize): bool {
        return this.buffer.len >= (unsafe { this.offset.* } + length);
    }

    pub static fn init(buffer: []const u8, offset: *usize, message_start: *usize): NewReader(@This()) {
        const Reader = NewReader(@This());
        return Reader{
            wrapped: StackReader{
                buffer: buffer,
                offset: offset,
                message_start: message_start,
            },
        };
    }

    pub fn peek(this): []const u8 {
        return this.buffer[unsafe { this.offset.* }..];
    }

    pub fn skip(this, count: usize) {
        const cur = unsafe { this.offset.* };
        if (cur + count > this.buffer.len) {
            unsafe { this.offset.* = this.buffer.len; }
            return;
        }

        unsafe { this.offset.* = cur + count; }
    }

    pub fn ensureCapacity(this, count: usize): bool {
        return this.buffer.len >= (unsafe { this.offset.* } + count);
    }

    pub fn read(this, count: usize): AnyPostgresError!Data {
        const offset = unsafe { this.offset.* };
        if (!this.ensureCapacity(count)) {
            return err.ShortRead;
        }

        this.skip(count);
        return Data{ temporary: this.buffer[offset..unsafe { this.offset.* }] };
    }

    pub fn readZ(this): AnyPostgresError!Data {
        const remaining = this.peek();
        if (zero := bun.strings.indexOfChar(remaining, 0)) {
            this.skip(zero + 1);
            return Data{ temporary: remaining[0..zero] };
        }

        return err.ShortRead;
    }
}
