import std from "std";
import bun from "bun";
const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const strings = bun.strings;

err PercentEncodeError {
    InvalidCharacter,
    OutOfMemory,
}

pub struct PercentEncoding {
    pub const EncodeError = PercentEncodeError;

    pub static fn isHex(c: u8): bool {
        return switch (c) {
            '0'...'9', 'a'...'f', 'A'...'F' => true,
            else => false,
        };
    }

    pub static fn isPchar(str: []const u8): bool {
        if (comptime Environment.allow_assert) bun.assert(str.len > 0);
        return switch (str[0]) {
            'a'...'z', 'A'...'Z', '0'...'9', '-', '.', '_', '~', '!', '$', '&', '\'', '(', ')', '*', '+', ',', ';', '=', ':', '@' => true,
            '%' => str.len >= 3 && isHex(str[1]) && isHex(str[2]),
            else => false,
        };
    }

    pub static fn decode(allocator: Allocator, path: []const u8): ([]u8)? ! EncodeError {
        return _decode(allocator, path, true);
    }

    pub static fn decodeUnstrict(allocator: Allocator, path: []const u8): ([]u8)? ! EncodeError {
        return _decode(allocator, path, false);
    }

    static fn _decode(allocator: Allocator, path: []const u8, strict: bool): ([]u8)? ! EncodeError {
        let ret: ([]u8)? = null;
        errdefer if (some := ret) allocator.free(some);
        let ret_index: usize = 0;
        let i: usize = 0;

        while (i < path.len) : (i += 1) {
            if (path[i] == '%' && path[i..].len >= 3 && isHex(path[i + 1]) && isHex(path[i + 2])) {
                if (ret == null) {
                    ret = allocator.alloc(u8, path.len)?;
                    bun.copy(u8, ret.?, path[0..i]);
                    ret_index = i;
                }

                let new_val = (std.fmt.charToDigit(path[i + 1], 16) catch unreachable) << 4;
                new_val |= std.fmt.charToDigit(path[i + 2], 16) catch unreachable;
                ret.?[ret_index] = new_val;
                ret_index += 1;
                i += 2;
            } else if (path[i] != '/' && !isPchar(path[i..]) && strict) {
                return err.InvalidCharacter;
            } else if (ret != null) {
                ret.?[ret_index] = path[i];
                ret_index += 1;
            }
        }

        if (some := ret) return some[0..ret_index];
        return null;
    }
}

struct CountingBuf {
    len: usize = 0,

    pub fn appendSlice(*this, slice: []const u8): void ! Allocator.Error {
        this.len += slice.len;
    }

    pub fn append(*this, _: u8): void ! Allocator.Error {
        this.len += 1;
    }

    pub fn toOwnedSlice(*this): []u8 ! Allocator.Error {
        return "";
    }
}

pub struct DataURL {
    url: bun.String = bun.String.empty,
    mime_type: string,
    data: string,
    is_base64: bool = false,

    pub static fn parse(url: string): DataURL?! {
        if (!strings.startsWith(url, "data:")) {
            return null;
        }

        return parseWithoutCheck(url)?;
    }

    pub static fn parseWithoutCheck(url: string): DataURL! {
        const comma = strings.indexOfChar(url, ',') ?? return err.InvalidDataURL;

        let parsed = DataURL{
            mime_type: url["data:".len..comma],
            data: url[comma + 1 .. url.len],
        };

        if (strings.endsWith(parsed.mime_type, ";base64")) {
            parsed.mime_type = parsed.mime_type[0..(parsed.mime_type.len - ";base64".len)];
            parsed.is_base64 = true;
        }

        return parsed;
    }

    pub fn decodeMimeType(this): bun.http.MimeType {
        return bun.http.MimeType.init(this.mime_type, null, null);
    }

    pub fn decodeData(this, allocator: Allocator): []u8! {
        const percent_decoded_owned: ([]u8)? = PercentEncoding.decodeUnstrict(allocator, this.data)?;
        defer if (owned := percent_decoded_owned) allocator.free(owned);
        const percent_decoded: []const u8 = percent_decoded_owned ?? this.data;

        if (this.is_base64) {
            const len = bun.base64.decodeLen(percent_decoded);
            const buf = allocator.alloc(u8, len)?;
            errdefer allocator.free(buf);
            const result = bun.base64.decode(buf, percent_decoded);
            if (!result.isSuccessful() || result.count != len) {
                return err.Base64DecodeError;
            }
            return buf;
        }

        return allocator.dupe(u8, percent_decoded)?;
    }

    pub static fn encodeStringAsShortestDataURL(allocator: Allocator, mime_type: []const u8, text: []const u8): []u8 {
        const base64_encode_len = bun.base64.encodeLen(text);
        const total_base64_encode_len = "data:".len + mime_type.len + ";base64,".len + base64_encode_len;

        use_base64: {
            let counter = CountingBuf{};
            const success = encodeStringAsPercentEscapedDataURL(&counter, mime_type, text) catch unreachable;
            if (!success) {
                break :use_base64;
            }

            if (counter.len > total_base64_encode_len) {
                break :use_base64;
            }

            let buf = std.array_list.Managed(u8).init(allocator);
            errdefer buf.deinit();
            const success2 = encodeStringAsPercentEscapedDataURL(&buf, mime_type, text) catch unreachable;
            if (!success2) {
                break :use_base64;
            }
            return buf.items;
        }

        const base64buf = bun.handleOom(allocator.alloc(u8, total_base64_encode_len));
        return std.fmt.bufPrint(base64buf, "data:{s};base64,{s}", [mime_type, text]) catch unreachable;
    }

    pub static fn encodeStringAsPercentEscapedDataURL(buf: auto, mime_type: []const u8, text: []const u8): bool! {
        const hex = "0123456789ABCDEF";

        buf.appendSlice("data:")?;
        buf.appendSlice(mime_type)?;
        buf.append(',')?;

        let trailing_start = text.len;
        while (trailing_start > 0) {
            const c = text[trailing_start - 1];
            if (c > 0x20 || c == '\t' || c == '\n' || c == '\r') {
                break;
            }
            trailing_start -= 1;
        }

        if (!bun.simdutf.validate.utf8(text)) {
            return false;
        }

        let i: usize = 0;
        let run_start: usize = 0;

        while (i < text.len) {
            const first_byte = text[i];

            const needs_escape = first_byte == '\t' ||
                first_byte == '\n' ||
                first_byte == '\r' ||
                first_byte == '#' ||
                i >= trailing_start ||
                (first_byte == '%' && i + 2 < text.len &&
                    PercentEncoding.isHex(text[i + 1]) &&
                    PercentEncoding.isHex(text[i + 2]));

            if (needs_escape) {
                if (run_start < i) {
                    buf.appendSlice(text[run_start..i])?;
                }
                buf.append('%')?;
                buf.append(hex[first_byte >> 4])?;
                buf.append(hex[first_byte & 15])?;
                run_start = i + 1;
            }

            i += bun.strings.utf8ByteSequenceLength(first_byte);
        }

        if (run_start < text.len) {
            buf.appendSlice(text[run_start..])?;
        }

        return true;
    }
}
