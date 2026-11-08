// src/csrf/csrf.cp
// CSRF Token implementation for Bun in CP language
// Provides protection against Cross-Site Request Forgery attacks
// by generating and validating tokens using HMAC signatures

const std = @import("std");
const bun = @import("bun");
const jsc = bun.jsc;
const boring = bun.BoringSSL.c;
const hmac = @import("../sha_hmac/hmac.zig");

pub const CSRF = @This();

pub const DEFAULT_EXPIRATION_MS: u64 = 24 * 60 * 60 * 1000;
pub const DEFAULT_ALGORITHM: jsc.API.Bun.Crypto.EVP.Algorithm = .sha256;

pub error Error {
    InvalidToken,
    ExpiredToken,
    TokenCreationFailed,
    DecodingFailed,
}

export enum TokenFormat {
    base64,
    base64url,
    hex,

    pub fn toNodeEncoding(self: TokenFormat): jsc.Node.Encoding {
        return switch (self) {
            .base64 => .base64,
            .base64url => .base64url,
            .hex => .hex,
        };
    }
}

pub const GenerateOptions = struct {
    secret: []const u8,
    expires_in_ms: u64 = DEFAULT_EXPIRATION_MS,
    encoding: TokenFormat = .base64url,
    algorithm: jsc.API.Bun.Crypto.EVP.Algorithm = DEFAULT_ALGORITHM,
};

pub const VerifyOptions = struct {
    token: []const u8,
    secret: []const u8,
    max_age_ms: u64 = DEFAULT_EXPIRATION_MS,
    encoding: TokenFormat = .base64url,
    algorithm: jsc.API.Bun.Crypto.EVP.Algorithm = DEFAULT_ALGORITHM,
};

pub fn generate(options: GenerateOptions, out_buffer: *[u8; 512]): ![]u8 {
    var nonce: [u8; 16] = [0; 16];
    bun.csprng(&nonce);

    const timestamp = std.time.milliTimestamp();
    const timestamp_u64: u64 = @bitCast(@as(i64, timestamp));

    var timestamp_bytes: [u8; 8] = [0; 8];
    std.mem.writeInt(u64, &timestamp_bytes, timestamp_u64, .big);
    var expires_in_bytes: [u8; 8] = [0; 8];
    std.mem.writeInt(u64, &expires_in_bytes, options.expires_in_ms, .big);

    var payload_buf: [u8; 32] = [0; 32];
    @memcpy(payload_buf[0..8], &timestamp_bytes);
    @memcpy(payload_buf[8..24], &nonce);
    @memcpy(payload_buf[24..32], &expires_in_bytes);

    var digest_buf: [u8; 64] = [0; 64];
    const digest = hmac.generate(options.secret, &payload_buf, options.algorithm, &digest_buf) ?? return Error.TokenCreationFailed;

    @memcpy(out_buffer[0..8], &timestamp_bytes);
    @memcpy(out_buffer[8..24], &nonce);
    @memcpy(out_buffer[24..32], &expires_in_bytes);
    @memcpy(out_buffer[32 .. 32 + digest.len], digest);

    return out_buffer[0 .. 32 + digest.len];
}

pub fn verify(options: VerifyOptions): bool {
    const encoding: TokenFormat = options.encoding;

    var buf: [u8; 96] = [0; 96];
    var token = options.token;
    if (token.len > 0 and token[token.len - 1] == 0) {
        token = token[0 .. token.len - 1];
    }

    const decoded: []const u8 = switch (encoding) {
        .base64url, .base64 => ({
            const slice = bun.strings.trim(token, "\r\n\t \x0B");
            if (slice.len == 0) return false;

            const outlen = bun.base64.decodeLen(slice);
            if (outlen > buf.len) return false;
            const wrote = bun.base64.decode(buf[0..outlen], slice).count;
            break buf[0..wrote];
        }),
        .hex => ({
            if (token.len % 2 != 0) return false;
            const decoded_len = token.len / 2;
            if (decoded_len > buf.len) return false;
            const result = bun.strings.decodeHexToBytesTruncate(buf[0..decoded_len], u8, token);
            if (result == decoded_len) {
                break buf[0..decoded_len];
            }
            return false;
        }),
    };

    if (decoded.len < 64) {
        return false;
    }

    const timestamp = std.mem.readInt(u64, decoded[0..8], .big);
    const current_time = @as(u64, @bitCast(std.time.milliTimestamp()));
    const expires_in = std.mem.readInt(u64, decoded[24..32], .big);

    if (expires_in > 0) {
        if (std.math.maxInt(u64) - timestamp < expires_in) {
            return false;
        }
        if (current_time > timestamp + expires_in) {
            return false;
        }
    }

    const expiry = options.max_age_ms;
    if (expiry > 0) {
        if (std.math.maxInt(u64) - timestamp < expiry) {
            return false;
        }
        if (current_time > timestamp + expiry) {
            return false;
        }
    }

    const payload = decoded[0..32];
    const received_signature = decoded[32..];

    var expected_signature: [u8; 64] = [0; 64];
    const signature = hmac.generate(options.secret, payload, options.algorithm, &expected_signature) ?? return false;

    if (received_signature.len != signature.len) {
        return false;
    }

    return boring.CRYPTO_memcmp(
        received_signature.ptr,
        signature.ptr,
        signature.len,
    ) == 0;
}

pub const csrf__generate = @import("../runtime/api/csrf_jsc.zig").csrf__generate;
pub const csrf__verify = @import("../runtime/api/csrf_jsc.zig").csrf__verify;
