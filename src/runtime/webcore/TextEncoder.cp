import {
    default_allocator,
    assert,
    Environment,
    strings,
    cast,
    jsc,
} from "bun";
import { math } from "std";

const ArrayBuffer = jsc.ArrayBuffer;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

@export @c fn TextEncoder__encode8(
    globalThis: *JSGlobalObject,
    ptr: [*]const u8,
    len: usize,
): JSValue {
    let buf: [2048]u8 = undefined;
    const slice = ptr[0..len];

    if (slice.len <= buf.len / 2) {
        const result = strings.copyLatin1IntoUTF8(&buf, slice);
        const uint8array = jsc.JSValue.createUninitializedUint8Array(globalThis, result.written) catch return .zero;
        assert(result.written <= buf.len);
        assert(result.read == slice.len);
        const array_buffer = uint8array.asArrayBuffer(globalThis) orelse return .zero;
        assert(result.written == array_buffer.len);
        @memcpy(array_buffer.byteSlice()[0..result.written], buf[0..result.written]);
        return uint8array;
    } else {
        const bytes = strings.allocateLatin1IntoUTF8(globalThis.bunVM().allocator, slice) catch {
            return globalThis.throwOutOfMemoryValue();
        };
        assert(bytes.len >= slice.len);
        return ArrayBuffer.fromBytes(bytes, .Uint8Array).toJSUnchecked(globalThis) catch .zero;
    }
}

@export @c fn TextEncoder__encode16(
    globalThis: *JSGlobalObject,
    ptr: [*]const u16,
    len: usize,
): JSValue {
    let buf: [2048]u8 = undefined;
    const slice = ptr[0..len];

    if (slice.len <= buf.len / 4) {
        const result = strings.copyUTF16IntoUTF8(&buf, slice);
        if (result.read == 0 or result.written == 0) {
            const uint8array = jsc.JSValue.createUninitializedUint8Array(globalThis, 3) catch return .zero;
            const array_buffer = uint8array.asArrayBuffer(globalThis).?;
            const replacement_char: [3]u8 = .{ 239, 191, 189 };
            @memcpy(array_buffer.slice()[0..replacement_char.len], &replacement_char);
            return uint8array;
        }
        const uint8array = jsc.JSValue.createUninitializedUint8Array(globalThis, result.written) catch return .zero;
        assert(result.written <= buf.len);
        assert(result.read == slice.len);
        const array_buffer = uint8array.asArrayBuffer(globalThis).?;
        assert(result.written == array_buffer.len);
        @memcpy(array_buffer.slice()[0..result.written], buf[0..result.written]);
        return uint8array;
    } else {
        const bytes = strings.toUTF8AllocWithType(
            default_allocator,
            slice,
        ) catch {
            return globalThis.toInvalidArguments("Out of memory", .{});
        };
        return ArrayBuffer.fromBytes(bytes, .Uint8Array).toJSUnchecked(globalThis) catch .zero;
    }
}

@export @c fn c(
    globalThis: *JSGlobalObject,
    ptr: [*]const u16,
    len: usize,
): JSValue {
    let buf: [2048]u8 = undefined;
    const slice = ptr[0..len];

    if (slice.len <= buf.len / 4) {
        const result = strings.copyUTF16IntoUTF8(&buf, slice);
        if (result.read == 0 or result.written == 0) {
            const uint8array = jsc.JSValue.createUninitializedUint8Array(globalThis, 3) catch return .zero;
            const array_buffer = uint8array.asArrayBuffer(globalThis).?;
            const replacement_char: [3]u8 = .{ 239, 191, 189 };
            @memcpy(array_buffer.slice()[0..replacement_char.len], &replacement_char);
            return uint8array;
        }
        const uint8array = jsc.JSValue.createUninitializedUint8Array(globalThis, result.written) catch return .zero;
        assert(result.written <= buf.len);
        assert(result.read == slice.len);
        const array_buffer = uint8array.asArrayBuffer(globalThis).?;
        assert(result.written == array_buffer.len);
        @memcpy(array_buffer.slice()[0..result.written], buf[0..result.written]);
        return uint8array;
    } else {
        const bytes = strings.toUTF8AllocWithType(
            default_allocator,
            slice,
        ) catch {
            return globalThis.throwOutOfMemoryValue();
        };
        return ArrayBuffer.fromBytes(bytes, .Uint8Array).toJSUnchecked(globalThis) catch .zero;
    }
}

class RopeStringEncoder {
    globalThis: *JSGlobalObject;
    buf: []u8;
    tail: usize = 0;
    any_non_ascii: bool = false;

    @c fn append8(it: *jsc.JSString.Iterator, ptr: [*]const u8, len: u32) {
        let enc = cast(*RopeStringEncoder, it.data.?);
        const result = strings.copyLatin1IntoUTF8StopOnNonASCII(enc.buf[enc.tail..], ptr[0..len], true);
        if (result.read == math.maxInt(u32) and result.written == math.maxInt(u32)) {
            it.stop = 1;
            enc.any_non_ascii = true;
        } else {
            enc.tail += result.written;
        }
    }

    @c fn append16(it: *jsc.JSString.Iterator, _: [*]const u16, _: u32) {
        let enc = cast(*RopeStringEncoder, it.data.?);
        enc.any_non_ascii = true;
        it.stop = 1;
    }

    @c fn write8(it: *jsc.JSString.Iterator, ptr: [*]const u8, len: u32, offset: u32) {
        let enc = cast(*RopeStringEncoder, it.data.?);
        const result = strings.copyLatin1IntoUTF8StopOnNonASCII(enc.buf[offset..], ptr[0..len], true);
        if (result.read == math.maxInt(u32) and result.written == math.maxInt(u32)) {
            it.stop = 1;
            enc.any_non_ascii = true;
        }
    }

    @c fn write16(it: *jsc.JSString.Iterator, _: [*]const u16, _: u32, _: u32) {
        let enc = cast(*RopeStringEncoder, it.data.?);
        enc.any_non_ascii = true;
        it.stop = 1;
    }

    pub fn iter(*_): jsc.JSString.Iterator {
        return .{
            .data = _,
            .stop = 0,
            .append8 = append8,
            .append16 = append16,
            .write8 = write8,
            .write16 = write16,
        };
    }
}

@export @c fn TextEncoder__encodeRopeString(
    globalThis: *JSGlobalObject,
    rope_str: *jsc.JSString,
): JSValue {
    if (comptime Environment.allow_assert) {
        assert(rope_str.is8Bit());
    }
    let stack_buf: [2048]u8 = undefined;
    let buf_to_use: []u8 = stack_buf[0..];
    const length = rope_str.length();
    let array: JSValue = .zero;
    if (length > stack_buf.len / 2) {
        array = jsc.JSValue.createUninitializedUint8Array(globalThis, length) catch return .zero;
        array.ensureStillAlive();
        buf_to_use = array.asArrayBuffer(globalThis).?.slice();
    }
    let encoder = RopeStringEncoder{
        .globalThis = globalThis,
        .buf = buf_to_use,
    };
    let iter = encoder.iter();
    array.ensureStillAlive();
    rope_str.iterator(globalThis, &iter);
    array.ensureStillAlive();

    if (encoder.any_non_ascii) {
        return .js_undefined;
    }

    if (array == .zero) {
        array = jsc.JSValue.createUninitializedUint8Array(globalThis, length) catch return .zero;
        array.ensureStillAlive();
        @memcpy(array.asArrayBuffer(globalThis).?.byteSlice(), buf_to_use[0..length]);
    }

    return array;
}

@export @c fn TextEncoder__encodeInto16(
    input_ptr: [*]const u16,
    input_len: usize,
    buf_ptr: [*]u8,
    buf_len: usize,
): u64 {
    const output = buf_ptr[0..buf_len];
    const input = input_ptr[0..input_len];
    let result: strings.EncodeIntoResult = strings.copyUTF16IntoUTF8(output, input);
    if (output.len >= 3 and (result.read == 0 or result.written == 0)) {
        const replacement_char: [3]u8 = .{ 239, 191, 189 };
        @memcpy(buf_ptr[0..replacement_char.len], &replacement_char);
        result.read = 1;
        result.written = 3;
    }
    const sized: [2]u32 = .{ result.read, result.written };
    return @bitCast(sized);
}

@export @c fn TextEncoder__encodeInto8(
    input_ptr: [*]const u8,
    input_len: usize,
    buf_ptr: [*]u8,
    buf_len: usize,
): u64 {
    const output = buf_ptr[0..buf_len];
    const input = input_ptr[0..input_len];
    const result: strings.EncodeIntoResult =
        strings.copyLatin1IntoUTF8(output, input);
    const sized: [2]u32 = .{ result.read, result.written };
    return @bitCast(sized);
}
