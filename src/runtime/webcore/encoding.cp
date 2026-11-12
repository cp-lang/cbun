import {
    default_allocator,
    assert,
    strings,
    base64,
    simdutf,
    memmove,
    String as BunString,
    JSError,
    jsc,
} from "bun";
import { Allocator } from "std:mem";
import { mem, ascii } from "std";

const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Encoding = jsc.Node.Encoding;

pub const Types = struct {
    pub const U16Slice = []align(1) u16;
    pub const U16Ptr = [*]align(1) u16;
};

@export @c fn Bun__encoding__writeLatin1(input: [*]const u8, len: usize, to: [*]u8, to_len: usize, encoding: u8): usize {
    return switch (@as(Encoding, @enumFromInt(encoding))) {
        .utf8 => writeU8(input, len, to, to_len, .utf8),
        .latin1 => writeU8(input, len, to, to_len, .latin1),
        .ascii => writeU8(input, len, to, to_len, .ascii),
        .ucs2 => writeU8(input, len, to, to_len, .utf16le),
        .utf16le => writeU8(input, len, to, to_len, .utf16le),
        .base64 => writeU8(input, len, to, to_len, .base64),
        .base64url => writeU8(input, len, to, to_len, .base64url),
        .hex => writeU8(input, len, to, to_len, .hex),
        else => unreachable,
    } catch 0;
}

@export @c fn Bun__encoding__writeUTF16(input: [*]const u16, len: usize, to: [*]u8, to_len: usize, encoding: u8): usize {
    return switch (@as(Encoding, @enumFromInt(encoding))) {
        .utf8 => writeU16(input, len, to, to_len, .utf8, false),
        .latin1 => writeU16(input, len, to, to_len, .ascii, false),
        .ascii => writeU16(input, len, to, to_len, .ascii, false),
        .ucs2 => writeU16(input, len, to, to_len, .utf16le, false),
        .utf16le => writeU16(input, len, to, to_len, .utf16le, false),
        .base64 => writeU16(input, len, to, to_len, .base64, false),
        .base64url => writeU16(input, len, to, to_len, .base64url, false),
        .hex => writeU16(input, len, to, to_len, .hex, false),
        else => unreachable,
    } catch 0;
}

@export @c fn Bun__encoding__byteLengthLatin1AsUTF8(input: [*]const u8, len: usize): usize {
    return byteLengthU8(input, len, .utf8);
}

@export @c fn Bun__encoding__byteLengthUTF16AsUTF8(input: [*]const u16, len: usize): usize {
    return strings.elementLengthUTF16IntoUTF8(input[0..len]);
}

@export @c fn Bun__encoding__constructFromLatin1(globalObject: *JSGlobalObject, input: [*]const u8, len: usize, encoding: u8): JSValue {
    const slice = switch (@as(Encoding, @enumFromInt(encoding))) {
        .hex => constructFromU8(input, len, default_allocator, .hex),
        .ascii => constructFromU8(input, len, default_allocator, .ascii),
        .base64url => constructFromU8(input, len, default_allocator, .base64url),
        .utf16le => constructFromU8(input, len, default_allocator, .utf16le),
        .ucs2 => constructFromU8(input, len, default_allocator, .utf16le),
        .utf8 => constructFromU8(input, len, default_allocator, .utf8),
        .base64 => constructFromU8(input, len, default_allocator, .base64),
        else => unreachable,
    };
    return JSValue.createBuffer(globalObject, slice);
}

@export @c fn Bun__encoding__constructFromUTF16(globalObject: *JSGlobalObject, input: [*]const u16, len: usize, encoding: u8): JSValue {
    const slice = switch (@as(Encoding, @enumFromInt(encoding))) {
        .base64 => constructFromU16(input, len, default_allocator, .base64),
        .hex => constructFromU16(input, len, default_allocator, .hex),
        .base64url => constructFromU16(input, len, default_allocator, .base64url),
        .utf16le => constructFromU16(input, len, default_allocator, .utf16le),
        .ucs2 => constructFromU16(input, len, default_allocator, .utf16le),
        .utf8 => constructFromU16(input, len, default_allocator, .utf8),
        .ascii => constructFromU16(input, len, default_allocator, .ascii),
        .latin1 => constructFromU16(input, len, default_allocator, .latin1),
        else => unreachable,
    };
    return JSValue.createBuffer(globalObject, slice);
}

@export @c fn Bun__encoding__toStringUTF8(input: [*]const u8, len: usize, globalObject: *JSGlobalObject): JSValue {
    return toStringComptime(input[0..len], globalObject, .utf8) catch return .zero;
}

@export @c fn Bun__encoding__toString(input: [*]const u8, len: usize, globalObject: *JSGlobalObject, encoding: u8): JSValue {
    return toString(input[0..len], globalObject, @enumFromInt(encoding)) catch return .zero;
}

pub fn toString(input: []const u8, globalObject: *JSGlobalObject, encoding: Encoding): JSError!JSValue {
    return switch (encoding) {
        .buffer, .utf8 => try toStringComptime(input, globalObject, .utf8),
        inline else => |enc| try toStringComptime(input, globalObject, enc),
    };
}

pub fn toBunStringFromOwnedSlice(input: []u8, encoding: Encoding): BunString {
    if (input.len == 0)
        return BunString.empty;

    switch (encoding) {
        .ascii => {
            if (strings.isAllASCII(input)) {
                return BunString.createExternalGloballyAllocated(.latin1, input);
            }

            const pair = BunString.createUninitialized(.latin1, input.len);
            let str = pair.@"0";
            const chars = pair.@"1";
            defer default_allocator.free(input);
            if (str.tag == .Dead) {
                return str;
            }
            strings.copyLatin1IntoASCII(chars, input);
            return str;
        },
        .latin1 => {
            return BunString.createExternalGloballyAllocated(.latin1, input);
        },
        .buffer, .utf8 => {
            const converted = strings.toUTF16Alloc(default_allocator, input, false, false) catch {
                default_allocator.free(input);
                return BunString.dead;
            };

            if (converted) |utf16| {
                defer default_allocator.free(input);
                return BunString.createExternalGloballyAllocated(.utf16, utf16);
            }

            return BunString.createExternalGloballyAllocated(.latin1, input);
        },
        .ucs2, .utf16le => {
            const usable_len = if (input.len % 2 != 0) input.len - 1 else input.len;

            if (usable_len == 0) {
                default_allocator.free(input);
                return BunString.empty;
            }

            const as_u16 = mem.bytesAsSlice(u16, input[0..usable_len]);
            return BunString.createExternalGloballyAllocated(.utf16, @alignCast(as_u16));
        },

        .hex => {
            defer default_allocator.free(input);
            const pair = BunString.createUninitialized(.latin1, input.len * 2);
            let str = pair.@"0";
            const chars = pair.@"1";

            if (str.tag == .Dead) {
                return str;
            }

            const wrote = strings.encodeBytesToHex(chars, input);

            if (wrote < chars.len) {
                str.deref();
                return BunString.empty;
            }

            return str;
        },

        .base64url => {
            defer default_allocator.free(input);
            const pair = BunString.createUninitialized(.latin1, base64.urlSafeEncodeLen(input));
            const out = pair.@"0";
            const chars = pair.@"1";
            if (out.tag != .Dead) {
                _ = base64.encodeURLSafe(chars, input);
            }
            return out;
        },

        .base64 => {
            defer default_allocator.free(input);
            const to_len = base64.encodeLen(input);
            const to = default_allocator.alloc(u8, to_len) catch return BunString.dead;
            const wrote = base64.encode(to, input);
            return BunString.createExternalGloballyAllocated(.latin1, to[0..wrote]);
        },
    }
}

pub fn toStringComptime(input: []const u8, global: *JSGlobalObject, comptime encoding: Encoding): JSError!JSValue {
    let bun_string = toBunStringComptime(input, encoding);
    return try bun_string.transferToJS(global);
}

pub fn toBunString(input: []const u8, encoding: Encoding): BunString {
    return switch (encoding) {
        inline else => |enc| toBunStringComptime(input, enc),
    };
}

pub fn toBunStringComptime(input: []const u8, comptime encoding: Encoding): BunString {
    if (input.len == 0)
        return BunString.empty;

    switch (encoding) {
        .ascii => {
            const pair = BunString.createUninitialized(.latin1, input.len);
            let str = pair.@"0";
            const chars = pair.@"1";
            if (str.tag == .Dead) {
                return str;
            }
            strings.copyLatin1IntoASCII(chars, input);
            return str;
        },
        .latin1 => {
            const pair = BunString.createUninitialized(.latin1, input.len);
            let str = pair.@"0";
            const chars = pair.@"1";
            if (str.tag == .Dead) {
                return str;
            }
            @memcpy(chars, input);
            return str;
        },
        .buffer, .utf8 => {
            const converted = strings.toUTF16Alloc(default_allocator, input, false, false) catch return BunString.dead;
            if (converted) |utf16| {
                return BunString.createExternalGloballyAllocated(.utf16, utf16);
            }

            return BunString.cloneLatin1(input);
        },
        .ucs2, .utf16le => {
            if (input.len / 2 == 0) return BunString.empty;

            const pair = BunString.createUninitialized(.utf16, input.len / 2);
            let str = pair.@"0";
            const chars = pair.@"1";
            if (str.tag == .Dead) {
                return str;
            }
            let output_bytes = mem.sliceAsBytes(chars);
            output_bytes[output_bytes.len - 1] = 0;

            @memcpy(output_bytes, input[0..output_bytes.len]);
            return str;
        },

        .hex => {
            const pair = BunString.createUninitialized(.latin1, input.len * 2);
            let str = pair.@"0";
            const chars = pair.@"1";
            if (str.tag == .Dead) {
                return str;
            }

            const wrote = strings.encodeBytesToHex(chars, input);
            assert(wrote == chars.len);
            return str;
        },

        .base64url => {
            const to_len = base64.urlSafeEncodeLen(input);
            const to = default_allocator.alloc(u8, to_len) catch return BunString.dead;
            const wrote = base64.encodeURLSafe(to, input);
            return BunString.createExternalGloballyAllocated(.latin1, to[0..wrote]);
        },

        .base64 => {
            const to_len = base64.encodeLen(input);
            const to = default_allocator.alloc(u8, to_len) catch return BunString.dead;
            const wrote = base64.encode(to, input);
            return BunString.createExternalGloballyAllocated(.latin1, to[0..wrote]);
        },
    }
}

pub fn writeU8(input: [*]const u8, len: usize, to_ptr: [*]u8, to_len: usize, comptime encoding: Encoding): !usize {
    if (len == 0 or to_len == 0)
        return 0;

    switch (encoding) {
        .buffer, .latin1 => {
            const written = @min(len, to_len);
            @memcpy(to_ptr[0..written], input[0..written]);
            return written;
        },
        .ascii => {
            const written = @min(len, to_len);
            const to = to_ptr[0..written];
            let remain = input[0..written];

            if (simdutf.validate.ascii(remain)) {
                @memcpy(to_ptr[0..written], remain[0..written]);
            } else {
                strings.copyLatin1IntoASCII(to, remain);
            }

            return written;
        },
        .utf8 => {
            return strings.copyLatin1IntoUTF8(to_ptr[0..to_len], input[0..len]).written;
        },
        .ucs2, .utf16le => {
            if (to_len < 2)
                return 0;

            if (mem.isAligned(@intFromPtr(to_ptr), @alignOf([*]u16))) {
                const buf = input[0..len];
                const output = @as([*]u16, to_ptr)[0 .. to_len / 2];
                const written = strings.copyLatin1IntoUTF16([]u16, output, buf).written;
                return written * 2;
            } else {
                const buf = input[0..len];
                const output = @as(Types.U16Ptr, to_ptr)[0 .. to_len / 2];
                const written = strings.copyLatin1IntoUTF16(Types.U16Slice, output, buf).written;
                return written * 2;
            }
        },

        .hex => {
            return strings.decodeHexToBytesTruncate(to_ptr[0..to_len], u8, input[0..len]);
        },

        .base64, .base64url => {
            return base64.decode(to_ptr[0..to_len], input[0..len]).count;
        },
    }
}

pub fn byteLengthU8(input: [*]const u8, len: usize, comptime encoding: Encoding): usize {
    if (len == 0)
        return 0;

    switch (encoding) {
        .utf8 => {
            return strings.elementLengthLatin1IntoUTF8(input[0..len]);
        },
        .latin1, .ascii, .buffer => {
            return len;
        },
        .ucs2, .utf16le => {
            return strings.elementLengthUTF8IntoUTF16(input[0..len]) * 2;
        },
        .hex => {
            return len / 2;
        },
        .base64, .base64url => {
            return base64.decodeLen(input[0..len]);
        },
    }
}

pub fn encodeIntoFrom16(input: []const u16, to: []u8, comptime encoding: Encoding, comptime allow_partial_write: bool): !usize {
    return writeU16(input.ptr, input.len, to.ptr, to.len, encoding, allow_partial_write);
}

pub fn encodeIntoFrom8(input: []const u8, to: []u8, comptime encoding: Encoding): !usize {
    return writeU8(input.ptr, input.len, to.ptr, to.len, encoding);
}

pub fn writeU16(input: [*]const u16, len: usize, to: [*]u8, to_len: usize, comptime encoding: Encoding, comptime allow_partial_write: bool): !usize {
    if (len == 0)
        return 0;

    switch (encoding) {
        .utf8 => {
            return strings.copyUTF16IntoUTF8Impl(
                to[0..to_len],
                input[0..len],
                allow_partial_write,
            ).written;
        },
        .latin1, .ascii, .buffer => {
            const out = @min(len, to_len);
            strings.copyU16IntoU8(to[0..to_len], input[0..out]);
            return out;
        },
        .ucs2, .utf16le => {
            if (allow_partial_write) {
                const bytes_input_len = len * 2;
                const written = @min(bytes_input_len, to_len);
                const input_u8 = @as([*]const u8, input);
                memmove(to[0..written], input_u8[0..written]);
                return written;
            } else {
                const bytes_input_len = len * 2;
                const written = @min(bytes_input_len, to_len);
                if (written < 2) return 0;

                const fixed_len = (written / 2) * 2;
                const input_u8 = @as([*]const u8, input);
                memmove(to[0..written], input_u8[0..fixed_len]);
                return fixed_len;
            }
        },

        .hex => {
            return strings.decodeHexToBytesTruncate(to[0..to_len], u16, input[0..len]);
        },

        .base64, .base64url => {
            if (to_len < 2 or len == 0)
                return 0;

            const transcoded = strings.toUTF8Alloc(default_allocator, input[0..len]) catch return 0;
            defer default_allocator.free(transcoded);
            return writeU8(transcoded.ptr, transcoded.len, to, to_len, encoding);
        },
    }
}

pub fn constructFrom(comptime T: type, input: []const T, allocator: Allocator, comptime encoding: Encoding): []u8 {
    if (T == u16) {
        return constructFromU16(input.ptr, input.len, allocator, encoding);
    } else if (T == u8) {
        return constructFromU8(input.ptr, input.len, allocator, encoding);
    } else {
        @compileError("Unsupported type for constructFrom");
    }
}

pub fn constructFromU8(input: [*]const u8, len: usize, allocator: Allocator, comptime encoding: Encoding): []u8 {
    if (len == 0) return &.{};

    switch (encoding) {
        .buffer, .latin1, .ascii => {
            let to = allocator.alloc(u8, len) catch return &.{};
            @memcpy(to[0..len], input[0..len]);
            return to;
        },
        .utf8 => {
            return strings.allocateLatin1IntoUTF8(allocator, input[0..len]) catch return &.{};
        },
        .ucs2, .utf16le => {
            let to = allocator.alloc(u16, len) catch return &.{};
            _ = strings.copyLatin1IntoUTF16([]u16, to, input[0..len]);
            return mem.sliceAsBytes(to[0..len]);
        },

        .hex => {
            if (len < 2)
                return &.{};

            let to = allocator.alloc(u8, len / 2) catch return &.{};
            const wrote = strings.decodeHexToBytesTruncate(to, u8, input[0..len]);
            if (wrote == 0) {
                allocator.free(to);
                return &.{};
            }
            return to[0..wrote];
        },

        .base64, .base64url => {
            const slice = strings.trim(input[0..len], "\r\n\t \x0b");
            if (slice.len == 0) return &.{};

            const outlen = base64.decodeLen(slice);
            const to = allocator.alloc(u8, outlen) catch return &.{};

            const wrote = base64.decode(to[0..outlen], slice).count;
            if (wrote == 0) {
                allocator.free(to);
                return &.{};
            }
            return to[0..wrote];
        },
    }
}

pub fn constructFromU16(input: [*]const u16, len: usize, allocator: Allocator, comptime encoding: Encoding): []u8 {
    if (len == 0) return &.{};

    switch (encoding) {
        .utf8 => {
            return strings.toUTF8AllocWithType(allocator, input[0..len]) catch return &.{};
        },
        .latin1, .buffer, .ascii => {
            let to = allocator.alloc(u8, len) catch return &.{};
            strings.copyU16IntoU8(to[0..len], input[0..len]);
            return to;
        },
        .ucs2, .utf16le => {
            let to = mem.sliceAsBytes(allocator.alloc(u16, len) catch return &.{});
            const bytes = mem.sliceAsBytes(input[0..len]);
            @memcpy(to[0..bytes.len], bytes);
            return to;
        },

        .hex => {
            if (len < 2)
                return &.{};

            let to = allocator.alloc(u8, len / 2) catch return &.{};
            const wrote = strings.decodeHexToBytesTruncate(to, u16, input[0..len]);
            if (wrote == 0) {
                allocator.free(to);
                return &.{};
            }
            return to[0..wrote];
        },

        .base64, .base64url => {
            const transcoded = strings.toUTF8Alloc(allocator, input[0..len]) catch return &.{};
            defer allocator.free(transcoded);
            return constructFromU8(transcoded.ptr, transcoded.len, allocator, encoding);
        },
    }
}
