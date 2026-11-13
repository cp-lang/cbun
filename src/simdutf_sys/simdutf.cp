import std from "std";
import bun from "bun";
const jsc = bun.jsc;

pub enum SIMDUTFResultStatus: i32 {
    success = 0,
    header_bits = 1,
    too_short = 2,
    too_long = 3,
    overlong = 4,
    too_large = 5,
    surrogate = 6,
    invalid_base64_character = 7,
    base64_input_remainder = 8,
    output_buffer_too_small = 9,
    _,
}

pub @c struct SIMDUTFResult {
    status: SIMDUTFResultStatus,
    count: usize = 0,

    pub fn isSuccessful(this): bool {
        return this.status == Status.success;
    }

    pub const Status = SIMDUTFResultStatus;
}

pub extern fn simdutf__detect_encodings(input: [*]const u8, length: usize): c_int;
pub extern fn simdutf__validate_utf8(buf: [*]const u8, len: usize): bool;
pub extern fn simdutf__validate_utf8_with_errors(buf: [*]const u8, len: usize): SIMDUTFResult;
pub extern fn simdutf__validate_ascii(buf: [*]const u8, len: usize): bool;
pub extern fn simdutf__validate_ascii_with_errors(buf: [*]const u8, len: usize): SIMDUTFResult;
pub extern fn simdutf__validate_utf16le(buf: [*]const u16, len: usize): bool;
pub extern fn simdutf__validate_utf16be(buf: [*]const u16, len: usize): bool;
pub extern fn simdutf__validate_utf16le_with_errors(buf: [*]const u16, len: usize): SIMDUTFResult;
pub extern fn simdutf__validate_utf16be_with_errors(buf: [*]const u16, len: usize): SIMDUTFResult;
pub extern fn simdutf__validate_utf32(buf: [*c]const c_uint, len: usize): bool;
pub extern fn simdutf__validate_utf32_with_errors(buf: [*c]const c_uint, len: usize): SIMDUTFResult;
pub extern fn simdutf__convert_utf8_to_utf16le(buf: [*]const u8, len: usize, utf16_output: [*]u16): usize;
pub extern fn simdutf__convert_utf8_to_utf16be(buf: [*]const u8, len: usize, utf16_output: [*]u16): usize;
pub extern fn simdutf__convert_utf8_to_utf16le_with_errors(buf: [*]const u8, len: usize, utf16_output: [*]u16): SIMDUTFResult;
pub extern fn simdutf__convert_utf8_to_utf16be_with_errors(buf: [*]const u8, len: usize, utf16_output: [*]u16): SIMDUTFResult;
pub extern fn simdutf__convert_valid_utf8_to_utf16be(buf: [*]const u8, len: usize, utf16_buffer: [*]u16): usize;
pub extern fn simdutf__convert_utf8_to_utf32(buf: [*]const u8, len: usize, utf32_output: [*]u32): usize;
pub extern fn simdutf__convert_utf8_to_utf32_with_errors(buf: [*]const u8, len: usize, utf32_output: [*]u32): SIMDUTFResult;
pub extern fn simdutf__convert_valid_utf8_to_utf32(buf: [*]const u8, len: usize, utf32_buffer: [*]u32): usize;
pub extern fn simdutf__convert_utf16le_to_utf8(buf: [*]const u16, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__convert_utf16be_to_utf8(buf: [*]const u16, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__convert_utf16le_to_utf8_with_errors(buf: [*]const u16, len: usize, utf8_buffer: [*]u8): SIMDUTFResult;
pub extern fn simdutf__convert_utf16be_to_utf8_with_errors(buf: [*]const u16, len: usize, utf8_buffer: [*]u8): SIMDUTFResult;
pub extern fn simdutf__convert_valid_utf16le_to_utf8(buf: [*]const u16, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__convert_valid_utf16be_to_utf8(buf: [*]const u16, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__convert_utf32_to_utf8(buf: [*c]const c_uint, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__convert_utf32_to_utf8_with_errors(buf: [*c]const c_uint, len: usize, utf8_buffer: [*]u8): SIMDUTFResult;
pub extern fn simdutf__convert_valid_utf32_to_utf8(buf: [*c]const c_uint, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__convert_utf32_to_utf16le(buf: [*c]const c_uint, len: usize, utf16_buffer: [*]u16): usize;
pub extern fn simdutf__convert_utf32_to_utf16be(buf: [*c]const c_uint, len: usize, utf16_buffer: [*]u16): usize;
pub extern fn simdutf__convert_utf32_to_utf16le_with_errors(buf: [*c]const c_uint, len: usize, utf16_buffer: [*]u16): SIMDUTFResult;
pub extern fn simdutf__convert_utf32_to_utf16be_with_errors(buf: [*c]const c_uint, len: usize, utf16_buffer: [*]u16): SIMDUTFResult;
pub extern fn simdutf__convert_valid_utf32_to_utf16le(buf: [*c]const c_uint, len: usize, utf16_buffer: [*]u16): usize;
pub extern fn simdutf__convert_valid_utf32_to_utf16be(buf: [*c]const c_uint, len: usize, utf16_buffer: [*]u16): usize;
pub extern fn simdutf__convert_utf16le_to_utf32(buf: [*]const u16, len: usize, utf32_buffer: [*]u32): usize;
pub extern fn simdutf__convert_utf16be_to_utf32(buf: [*]const u16, len: usize, utf32_buffer: [*]u32): usize;
pub extern fn simdutf__convert_utf16le_to_utf32_with_errors(buf: [*]const u16, len: usize, utf32_buffer: [*]u32): SIMDUTFResult;
pub extern fn simdutf__convert_utf16be_to_utf32_with_errors(buf: [*]const u16, len: usize, utf32_buffer: [*]u32): SIMDUTFResult;
pub extern fn simdutf__convert_valid_utf16le_to_utf32(buf: [*]const u16, len: usize, utf32_buffer: [*]u32): usize;
pub extern fn simdutf__convert_valid_utf16be_to_utf32(buf: [*]const u16, len: usize, utf32_buffer: [*]u32): usize;
pub extern fn simdutf__convert_latin1_to_utf8(buf: [*]const u8, len: usize, utf8_buffer: [*]u8): usize;
pub extern fn simdutf__change_endianness_utf16(buf: [*]const u16, length: usize, output: [*]u16): void;
pub extern fn simdutf__count_utf16le(buf: [*]const u16, length: usize): usize;
pub extern fn simdutf__count_utf16be(buf: [*]const u16, length: usize): usize;
pub extern fn simdutf__count_utf8(buf: [*]const u8, length: usize): usize;
pub extern fn simdutf__utf8_length_from_utf16le(input: [*]const u16, length: usize): usize;
pub extern fn simdutf__utf8_length_from_utf16be(input: [*]const u16, length: usize): usize;
pub extern fn simdutf__utf32_length_from_utf16le(input: [*]const u16, length: usize): usize;
pub extern fn simdutf__utf32_length_from_utf16be(input: [*]const u16, length: usize): usize;
pub extern fn simdutf__utf16_length_from_utf8(input: [*]const u8, length: usize): usize;
pub extern fn simdutf__utf8_length_from_utf32(input: [*c]const c_uint, length: usize): usize;
pub extern fn simdutf__utf16_length_from_utf32(input: [*c]const c_uint, length: usize): usize;
pub extern fn simdutf__utf32_length_from_utf8(input: [*]const u8, length: usize): usize;
pub extern fn simdutf__utf8_length_from_latin1(input: [*]const u8, length: usize): usize;
pub extern fn simdutf__utf16_length_from_latin1(input: [*]const u8, length: usize): usize;

extern fn simdutf__base64_encode(input: [*]const u8, length: usize, output: [*]u8, is_urlsafe: c_int): usize;
extern fn simdutf__base64_decode_from_binary(input: [*]const u8, length: usize, output: [*]u8, outlen: usize, is_urlsafe: c_int): SIMDUTFResult;
extern fn simdutf__base64_decode_from_binary16(input: [*]const u16, length: usize, output: [*]u8, outlen: usize, is_urlsafe: c_int): SIMDUTFResult;
extern fn simdutf__base64_length_from_binary(length: usize, options: c_int): usize;

static struct validate_with_errors {
    pub fn utf8(input: []const u8): SIMDUTFResult {
        unsafe { return simdutf__validate_utf8_with_errors(input.ptr, input.len); }
    }
    pub fn ascii(input: []const u8): SIMDUTFResult {
        unsafe { return simdutf__validate_ascii_with_errors(input.ptr, input.len); }
    }
    pub fn utf16le(input: []const u16): SIMDUTFResult {
        unsafe { return simdutf__validate_utf16le_with_errors(input.ptr, input.len); }
    }
    pub fn utf16be(input: []const u16): SIMDUTFResult {
        unsafe { return simdutf__validate_utf16be_with_errors(input.ptr, input.len); }
    }
}

pub static struct validate {
    pub const with_errors = validate_with_errors;
    pub fn utf8(input: []const u8): bool {
        unsafe { return simdutf__validate_utf8(input.ptr, input.len); }
    }
    pub fn ascii(input: []const u8): bool {
        jsc.markBinding(@src());
        unsafe { return simdutf__validate_ascii(input.ptr, input.len); }
    }
    pub fn utf16le(input: []const u16): bool {
        unsafe { return simdutf__validate_utf16le(input.ptr, input.len); }
    }
    pub fn utf16be(input: []const u16): bool {
        unsafe { return simdutf__validate_utf16be(input.ptr, input.len); }
    }
}

static struct convert_latin1_to {
    pub fn utf8(input: []const u8, output: []u8): usize {
        unsafe { return simdutf__convert_latin1_to_utf8(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_latin1 {
    pub const to = convert_latin1_to;
}

static struct convert_utf8_to_utf16_with_errors {
    pub fn le(input: []const u8, output: []u16): SIMDUTFResult {
        unsafe { return simdutf__convert_utf8_to_utf16le_with_errors(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u8, output: []u16): SIMDUTFResult {
        unsafe { return simdutf__convert_utf8_to_utf16be_with_errors(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf8_to_utf16 {
    pub const with_errors = convert_utf8_to_utf16_with_errors;
    pub fn le(input: []const u8, output: []u16): usize {
        unsafe { return simdutf__convert_utf8_to_utf16le(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u8, output: []u16): usize {
        unsafe { return simdutf__convert_utf8_to_utf16be(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf8_to_utf32_with_errors {
    pub fn le(input: []const u8, output: []u32): SIMDUTFResult {
        unsafe { return simdutf__convert_utf8_to_utf32_with_errors(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u8, output: []u32): SIMDUTFResult {
        unsafe { return simdutf__convert_utf8_to_utf32_with_errors(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf8_to_utf32 {
    pub const with_errors = convert_utf8_to_utf32_with_errors;
    pub fn le(input: []const u8, output: []u32): usize {
        unsafe { return simdutf__convert_valid_utf8_to_utf32(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u8, output: []u32): usize {
        unsafe { return simdutf__convert_valid_utf8_to_utf32(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf8_to {
    pub const utf16 = convert_utf8_to_utf16;
    pub const utf32 = convert_utf8_to_utf32;
}
static struct convert_utf8 {
    pub const to = convert_utf8_to;
}

static struct convert_utf16_to_utf8_with_errors {
    pub fn le(input: []const u16, output: []u8): SIMDUTFResult {
        unsafe { return simdutf__convert_utf16le_to_utf8_with_errors(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u16, output: []u8): SIMDUTFResult {
        unsafe { return simdutf__convert_utf16be_to_utf8_with_errors(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf16_to_utf8 {
    pub const with_errors = convert_utf16_to_utf8_with_errors;
    pub fn le(input: []const u16, output: []u8): usize {
        unsafe { return simdutf__convert_valid_utf16le_to_utf8(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u16, output: []u8): usize {
        unsafe { return simdutf__convert_valid_utf16be_to_utf8(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf16_to_utf32_with_errors {
    pub fn le(input: []const u16, output: []u32): SIMDUTFResult {
        unsafe { return simdutf__convert_utf16le_to_utf32_with_errors(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u16, output: []u32): SIMDUTFResult {
        unsafe { return simdutf__convert_utf16be_to_utf32_with_errors(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf16_to_utf32 {
    pub const with_errors = convert_utf16_to_utf32_with_errors;
    pub fn le(input: []const u16, output: []u32): usize {
        unsafe { return simdutf__convert_valid_utf16le_to_utf32(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u16, output: []u32): usize {
        unsafe { return simdutf__convert_valid_utf16be_to_utf32(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf16_to {
    pub const utf8 = convert_utf16_to_utf8;
    pub const utf32 = convert_utf16_to_utf32;
}
static struct convert_utf16 {
    pub const to = convert_utf16_to;
}

static struct convert_utf32_to_utf8_with_errors {
    pub fn le(input: []const u32, output: []u8): SIMDUTFResult {
        unsafe { return simdutf__convert_utf32_to_utf8_with_errors(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u32, output: []u8): SIMDUTFResult {
        unsafe { return simdutf__convert_utf32_to_utf8_with_errors(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf32_to_utf8 {
    pub const with_errors = convert_utf32_to_utf8_with_errors;
    pub fn le(input: []const u32, output: []u8): usize {
        unsafe { return simdutf__convert_valid_utf32_to_utf8(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u32, output: []u8): usize {
        unsafe { return simdutf__convert_valid_utf32_to_utf8(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf32_to_utf16_with_errors {
    pub fn le(input: []const u32, output: []u16): SIMDUTFResult {
        unsafe { return simdutf__convert_utf32_to_utf16le_with_errors(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u32, output: []u16): SIMDUTFResult {
        unsafe { return simdutf__convert_utf32_to_utf16be_with_errors(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf32_to_utf16 {
    pub const with_errors = convert_utf32_to_utf16_with_errors;
    pub fn le(input: []const u32, output: []u16): usize {
        unsafe { return simdutf__convert_valid_utf32_to_utf16le(input.ptr, input.len, output.ptr); }
    }
    pub fn be(input: []const u32, output: []u16): usize {
        unsafe { return simdutf__convert_valid_utf32_to_utf16be(input.ptr, input.len, output.ptr); }
    }
}
static struct convert_utf32_to {
    pub const utf8 = convert_utf32_to_utf8;
    pub const utf16 = convert_utf32_to_utf16;
}
static struct convert_utf32 {
    pub const to = convert_utf32_to;
}

pub static struct convert {
    pub const latin1 = convert_latin1;
    pub const utf8 = convert_utf8;
    pub const utf16 = convert_utf16;
    pub const utf32 = convert_utf32;
}

static struct length_utf8_from_utf16 {
    pub fn le(input: []const u16): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf8_length_from_utf16le(input.ptr, input.len); }
    }
    pub fn be(input: []const u16): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf8_length_from_utf16be(input.ptr, input.len); }
    }
}
static struct length_utf8_from {
    pub const utf16 = length_utf8_from_utf16;
    pub fn latin1(input: []const u8): usize {
        unsafe { return simdutf__utf8_length_from_latin1(input.ptr, input.len); }
    }
    pub fn utf32(input: []const u32): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf8_length_from_utf32(input.ptr, input.len); }
    }
}
static struct length_utf8 {
    pub const from = length_utf8_from;
}

static struct length_utf16_from {
    pub fn utf8(input: []const u8): usize {
        if (@inComptime()) {
            return std.unicode.utf8CountCodepoints(input) catch @compileError("Invalid UTF-8");
        }
        unsafe { return simdutf__utf16_length_from_utf8(input.ptr, input.len); }
    }
    pub fn utf32(input: []const u32): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf16_length_from_utf32(input.ptr, input.len); }
    }
    pub fn latin1(input: []const u8): usize {
        unsafe { return simdutf__utf16_length_from_latin1(input.ptr, input.len); }
    }
}
static struct length_utf16 {
    pub const from = length_utf16_from;
}

static struct length_utf32_from_utf8 {
    pub fn le(input: []const u8): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf32_length_from_utf8(input.ptr, input.len); }
    }
    pub fn be(input: []const u8): usize {
        unsafe { return simdutf__utf32_length_from_utf8(input.ptr, input.len); }
    }
}
static struct length_utf32_from_utf16 {
    pub fn le(input: []const u16): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf32_length_from_utf16le(input.ptr, input.len); }
    }
    pub fn be(input: []const u16): usize {
        jsc.markBinding(@src());
        unsafe { return simdutf__utf32_length_from_utf16be(input.ptr, input.len); }
    }
}
static struct length_utf32_from {
    pub const utf8 = length_utf32_from_utf8;
    pub const utf16 = length_utf32_from_utf16;
}
static struct length_utf32 {
    pub const from = length_utf32_from;
}

pub static struct length {
    pub const utf8 = length_utf8;
    pub const utf16 = length_utf16;
    pub const utf32 = length_utf32;
}

pub static struct trim {
    pub fn utf8_len(buf: []const u8): usize {
        const len = buf.len;
        if (len < 3) {
            switch (len) {
                2 => {
                    if (buf[len - 1] >= 0b11000000) {
                        return len - 1;
                    }
                    if (buf[len - 2] >= 0b11100000) {
                        return len - 2;
                    }
                    return len;
                },
                1 => {
                    if (buf[len - 1] >= 0b11000000) {
                        return len - 1;
                    }
                    return len;
                },
                0 => return len,
                else => unreachable,
            }
        }
        if (buf[len - 1] >= 0b11000000) {
            return len - 1;
        }
        if (buf[len - 2] >= 0b11100000) {
            return len - 2;
        }
        if (buf[len - 3] >= 0b11110000) {
            return len - 3;
        }
        return len;
    }

    pub fn utf16_len(buf: []const u16): usize {
        const len = buf.len;
        if (len == 0) {
            return 0;
        }
        if ((buf[len - 1] >= 0xD800) && (buf[len - 1] <= 0xDBFF)) {
            return len - 1;
        }
        return len;
    }

    pub fn utf16(buf: []const u16): []const u16 {
        return buf[0..utf16_len(buf)];
    }

    pub fn utf8(buf: []const u8): []const u8 {
        return buf[0..utf8_len(buf)];
    }
}

pub static struct base64 {
    pub fn encode(input: []const u8, output: []u8, is_urlsafe: bool): usize {
        unsafe { return simdutf__base64_encode(input.ptr, input.len, output.ptr, @intFromBool(is_urlsafe)); }
    }

    pub fn encode_len(input: usize, is_urlsafe: bool): usize {
        unsafe { return simdutf__base64_length_from_binary(input, @intFromBool(is_urlsafe)); }
    }

    pub fn decode(input: []const u8, output: []u8, is_urlsafe: bool): SIMDUTFResult {
        unsafe { return simdutf__base64_decode_from_binary(input.ptr, input.len, output.ptr, output.len, @intFromBool(is_urlsafe)); }
    }

    pub fn decode16(input: []const u16, output: []u8, is_urlsafe: bool): SIMDUTFResult {
        unsafe { return simdutf__base64_decode_from_binary16(input.ptr, input.len, output.ptr, output.len, @intFromBool(is_urlsafe)); }
    }
}
