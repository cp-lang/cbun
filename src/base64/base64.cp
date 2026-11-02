// src/base64/base64.cp
// Bun Base64 module ported to CP (Concurrent Plus) language

export struct Base64Encoder {
    alphabet_chars: [64]u8,
    pad_char: u8?,

    static fn init(alphabet_chars: [64]u8, pad_char: u8?): Base64Encoder {
        return Base64Encoder {
            alphabet_chars: alphabet_chars,
            pad_char: pad_char,
        };
    }

    fn calcSize(source_len: usize): usize {
        if (pad_char != null) {
            return ((source_len + 2) / 3) * 4;
        } else {
            let leftover = source_len % 3;
            return (source_len / 3) * 4 + ((leftover * 4 + 2) / 3);
        }
    }

    fn encodeWithoutSizeCheck(*dest: u8[], source: string): usize {
        let acc: u32 = 0;
        let acc_len: u32 = 0;
        let out_idx = 0;
        for (v in source) {
            acc = (acc << 8) + (v as u32);
            acc_len += 8;
            while (acc_len >= 6) {
                acc_len -= 6;
                let idx: usize = ((acc >> acc_len) & 0x3F) as usize;
                dest[out_idx] = alphabet_chars[idx];
                out_idx += 1;
            }
        }
        if (acc_len > 0) {
            let shift: u32 = (6 as u32) - acc_len;
            let idx: usize = ((acc << shift) & 0x3F) as usize;
            dest[out_idx] = alphabet_chars[idx];
            out_idx += 1;
        }
        return out_idx;
    }

    fn encode(*dest: u8[], source: string): string {
        let out_len = calcSize(source.len);
        let out_idx = encodeWithoutSizeCheck(dest, source);
        if (pad_char != null) {
            let pad = pad_char ?? 0;
            let i = out_idx;
            while (i < out_len) {
                dest[i] = pad;
                i += 1;
            }
        }
        return dest[0..out_len];
    }
}

export struct Base64Decoder {
    char_to_index: [256]u8,
    pad_char: u8?,

    static fn init(alphabet_chars: [64]u8, pad_char: u8?): Base64Decoder {
        let result = Base64Decoder {
            char_to_index: [255 as u8; 256],
            pad_char: pad_char,
        };
        let i = 0;
        for (c in alphabet_chars) {
            result.char_to_index[c as usize] = i as u8;
            i += 1;
        }
        return result;
    }

    fn calcSizeUpperBound(source_len: usize): !usize {
        let result = (source_len / 4) * 3;
        let leftover = source_len % 4;
        if (pad_char != null) {
            if (leftover % 4 != 0) {
                return err.InvalidPadding;
            }
        } else {
            if (leftover % 4 == 1) {
                return err.InvalidPadding;
            }
            result += (leftover * 3) / 4;
        }
        return result;
    }

    fn calcSizeForSlice(source: string): !usize {
        let source_len = source.len;
        let result = calcSizeUpperBound(source_len)?;
        if (pad_char != null) {
            let pad = pad_char ?? 0;
            if (source_len >= 1 && source[source_len - 1] == pad) {
                result -= 1;
            }
            if (source_len >= 2 && source[source_len - 2] == pad) {
                result -= 1;
            }
        }
        return result;
    }

    fn decode(*dest: u8[], source: string): !void {
        if (pad_char != null && source.len % 4 != 0) {
            return err.InvalidPadding;
        }
        let acc: u32 = 0;
        let acc_len: u32 = 0;
        let dest_idx: usize = 0;
        let leftover_idx: usize? = null;
        let src_idx: usize = 0;
        for (c in source) {
            let d = char_to_index[c as usize];
            if (d == 255) {
                if (pad_char == null || c != (pad_char ?? 0)) {
                    return err.InvalidCharacter;
                }
                leftover_idx = src_idx;
                break;
            }
            acc = (acc << 6) + (d as u32);
            acc_len += 6;
            if (acc_len >= 8) {
                acc_len -= 8;
                let val: u8 = ((acc >> acc_len) & 0xFF) as u8;
                dest[dest_idx] = val;
                dest_idx += 1;
            }
            src_idx += 1;
        }
        if (acc_len > 4 || (acc & (((1 as u32) << acc_len) - 1)) != 0) {
            return err.InvalidPadding;
        }
        if (leftover_idx == null) {
            return;
        }
        let l_idx = leftover_idx ?? 0;
        let leftover = source[l_idx..source.len];
        if (pad_char != null) {
            let pad = pad_char ?? 0;
            let padding_len: u32 = acc_len / 2;
            let padding_chars: u32 = 0;
            for (c in leftover) {
                if (c != pad) {
                    if (c == 255) {
                        return err.InvalidCharacter;
                    } else {
                        return err.InvalidPadding;
                    }
                }
                padding_chars += 1;
            }
            if (padding_chars != padding_len) {
                return err.InvalidPadding;
            }
        }
    }
}

export struct Base64DecoderWithIgnore {
    decoder: Base64Decoder,
    char_is_ignored: [256]bool,

    static fn init(alphabet_chars: [64]u8, pad_char: u8?, ignore_chars: string): Base64DecoderWithIgnore {
        let result = Base64DecoderWithIgnore {
            decoder: Base64Decoder.init(alphabet_chars, pad_char),
            char_is_ignored: [false; 256],
        };
        for (c in ignore_chars) {
            result.char_is_ignored[c as usize] = true;
        }
        return result;
    }

    fn calcSizeUpperBound(source_len: usize): usize {
        let result = (source_len / 4) * 3;
        if (decoder.pad_char == null) {
            let leftover = source_len % 4;
            result += (leftover * 3) / 4;
        }
        return result;
    }

    fn decode(*dest: u8[], source: string, *wrote: usize): !void {
        let acc: u32 = 0;
        let acc_len: u32 = 0;
        let dest_idx: usize = 0;
        let leftover_idx: usize? = null;
        let src_idx: usize = 0;

        defer {
            wrote.* = dest_idx;
        }

        for (c in source) {
            if (char_is_ignored[c as usize]) {
                src_idx += 1;
                continue;
            }
            let d = decoder.char_to_index[c as usize];
            if (d == 255) {
                if (decoder.pad_char != null) {
                    let pad = decoder.pad_char ?? 0;
                    if (c == pad) {
                        leftover_idx = src_idx;
                        break;
                    }
                }
                if (char_is_ignored[255]) {
                    src_idx += 1;
                    continue;
                }
                return err.InvalidCharacter;
            }
            acc = (acc << 6) + (d as u32);
            acc_len += 6;
            if (acc_len >= 8) {
                if (dest_idx == dest.len) {
                    return err.NoSpaceLeft;
                }
                acc_len -= 8;
                let val: u8 = ((acc >> acc_len) & 0xFF) as u8;
                dest[dest_idx] = val;
                dest_idx += 1;
            }
            src_idx += 1;
        }

        if (acc_len > 4 || (acc & (((1 as u32) << acc_len) - 1)) != 0) {
            return err.InvalidPadding;
        }

        if (decoder.pad_char != null) {
            let pad = decoder.pad_char ?? 0;
            let padding_len: u32 = acc_len / 2;

            if (leftover_idx != null) {
                let l_idx = leftover_idx ?? 0;
                let leftover = source[l_idx..source.len];
                let padding_chars: u32 = 0;
                for (c in leftover) {
                    if (char_is_ignored[c as usize]) {
                        continue;
                    }
                    if (c != pad) {
                        if (c == 255) {
                            return err.InvalidCharacter;
                        } else {
                            return err.InvalidPadding;
                        }
                    }
                    padding_chars += 1;
                }
                if (padding_chars != padding_len) {
                    return err.InvalidPadding;
                }
            } else if (padding_len > 0) {
                return err.InvalidPadding;
            }
        }
    }
}

zig {
    const bun = @import("bun");
    const assert = bun.assert;
    const testing = std.testing;

    extern fn WTF__base64URLEncode(input: [*]const u8, input_len: usize, output: [*]u8, output_len: usize) usize;

    const decoderWithIgnoreProto = *const fn (ignore: []const u8) Base64DecoderWithIgnore;

    pub const Codecs = struct {
        alphabet_chars: [64]u8,
        pad_char: ?u8,
        decoderWithIgnore: decoderWithIgnoreProto,
        Encoder: Base64Encoder,
        Decoder: Base64Decoder,
    };

    pub const standard_alphabet_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".*;
    fn standardBase64DecoderWithIgnore(ignore: []const u8) Base64DecoderWithIgnore {
        return Base64DecoderWithIgnore.init(standard_alphabet_chars, '=', ignore);
    }

    pub const standard = Codecs{
        .alphabet_chars = standard_alphabet_chars,
        .pad_char = '=',
        .decoderWithIgnore = standardBase64DecoderWithIgnore,
        .Encoder = Base64Encoder.init(standard_alphabet_chars, '='),
        .Decoder = Base64Decoder.init(standard_alphabet_chars, '='),
    };

    pub const standard_no_pad = Codecs{
        .alphabet_chars = standard_alphabet_chars,
        .pad_char = null,
        .decoderWithIgnore = standardBase64DecoderWithIgnore,
        .Encoder = Base64Encoder.init(standard_alphabet_chars, null),
        .Decoder = Base64Decoder.init(standard_alphabet_chars, null),
    };

    pub const url_safe_alphabet_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".*;
    fn urlSafeBase64DecoderWithIgnore(ignore: []const u8) Base64DecoderWithIgnore {
        return Base64DecoderWithIgnore.init(url_safe_alphabet_chars, '=', ignore);
    }

    pub const url_safe = Codecs{
        .alphabet_chars = url_safe_alphabet_chars,
        .pad_char = '=',
        .decoderWithIgnore = urlSafeBase64DecoderWithIgnore,
        .Encoder = Base64Encoder.init(url_safe_alphabet_chars, '='),
        .Decoder = Base64Decoder.init(url_safe_alphabet_chars, '='),
    };

    pub const url_safe_no_pad = Codecs{
        .alphabet_chars = url_safe_alphabet_chars,
        .pad_char = null,
        .decoderWithIgnore = urlSafeBase64DecoderWithIgnore,
        .Encoder = Base64Encoder.init(url_safe_alphabet_chars, null),
        .Decoder = Base64Decoder.init(url_safe_alphabet_chars, null),
    };

    const mixed_decoder = brk: {
        var decoder = standard.decoderWithIgnore("\xff \t\r\n" ++ [_]u8{
            std.ascii.control_code.vt,
            std.ascii.control_code.ff,
        });

        for (url_safe_alphabet_chars[62..], 62..) |c, i| {
            decoder.decoder.char_to_index[c] = @as(u8, @intCast(i));
        }

        break :brk decoder;
    };

    pub fn decode(destination: []u8, source: []const u8) bun.simdutf.SIMDUTFResult {
        const result = bun.simdutf.base64.decode(source, destination, false);

        if (!result.isSuccessful()) {
            var wrote: usize = 0;
            mixed_decoder.decode(destination, source, &wrote) catch {
                return .{
                    .count = wrote,
                    .status = .invalid_base64_character,
                };
            };
            return .{ .count = wrote, .status = .success };
        }

        return result;
    }

    pub fn decodeAlloc(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
        var dest = try allocator.alloc(u8, decodeLen(input));
        errdefer allocator.free(dest);
        const result = decode(dest, input);
        if (!result.isSuccessful()) {
            return err.DecodingFailed;
        }
        return dest[0..result.count];
    }

    pub fn encode(destination: []u8, source: []const u8) usize {
        return bun.simdutf.base64.encode(source, destination, false);
    }

    pub fn encodeAlloc(allocator: std.mem.Allocator, source: []const u8) !bun.ByteList {
        const len = encodeLen(source);
        const destination = try allocator.alloc(u8, len);
        errdefer allocator.free(destination);
        const encoded_len = encode(destination, source);
        return .{
            .ptr = destination.ptr,
            .len = @truncate(encoded_len),
            .cap = @truncate(len),
        };
    }

    pub fn simdutfEncodeLenUrlSafe(source_len: usize) usize {
        return bun.simdutf.base64.encode_len(source_len, true);
    }

    pub fn simdutfEncodeUrlSafe(destination: []u8, source: []const u8) usize {
        return bun.simdutf.base64.encode(source, destination, true);
    }

    pub fn decodeLenUpperBound(len: usize) usize {
        return standard.Decoder.calcSizeUpperBound(len) catch {
            return len / 4 * 3;
        };
    }

    pub fn decodeLen(source: anytype) usize {
        return standard.Decoder.calcSizeForSlice(source) catch {
            return source.len / 4 * 3 + 2;
        };
    }

    pub fn encodeLen(source: anytype) usize {
        return encodeLenFromSize(source.len);
    }

    pub fn encodeLenFromSize(source: usize) usize {
        return standard.Encoder.calcSize(source);
    }

    pub fn urlSafeEncodeLen(source: anytype) usize {
        return ((source.len * 4) + 2) / 3;
    }

    pub fn encodeURLSafe(dest: []u8, source: []const u8) usize {
        bun.jsc.markBinding(@src());
        return WTF__base64URLEncode(source.ptr, source.len, dest.ptr, dest.len);
    }
}
