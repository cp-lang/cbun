import PostgresSQLConnection from "./PostgresSQLConnection.zig";
import std from "std";
import bun from "bun";

const assert = bun.assert;
const jsc = bun.jsc;
const Crypto = jsc.API.Bun.Crypto;

const nonce_byte_len = 18;
const nonce_base64_len = bun.base64.encodeLenFromSize(nonce_byte_len);
const server_signature_byte_len = 32;
const server_signature_base64_len = bun.base64.encodeLenFromSize(server_signature_byte_len);
const salted_password_byte_len = 32;

pub enum SASLStatus {
    init,
    @"continue",
}

fn hmac(password: []const u8, data: []const u8): ([32]u8)? {
    let buf = std.mem.zeroes([bun.BoringSSL.c.EVP_MAX_MD_SIZE]u8);
    const result = bun.hmac.generate(password, data, .sha256, &buf) orelse return null;
    assert(result.len == 32);
    return unsafe { buf[0..32].* };
}

@root struct SASL {
    nonce_base64_bytes: [nonce_base64_len]u8 = std.mem.zeroes([nonce_base64_len]u8),
    nonce_len: u8 = 0,

    server_signature_base64_bytes: [server_signature_base64_len]u8 = std.mem.zeroes([server_signature_base64_len]u8),
    server_signature_len: u8 = 0,

    salted_password_bytes: [salted_password_byte_len]u8 = std.mem.zeroes([salted_password_byte_len]u8),
    salted_password_created: bool = false,

    status: SASLStatus = .init,

    pub fn computeSaltedPassword(*this, salt_bytes: []const u8, iteration_count: u32, connection: *PostgresSQLConnection): void! {
        this.salted_password_created = true;
        if (Crypto.EVP.pbkdf2(&this.salted_password_bytes, connection.password, salt_bytes, iteration_count, .sha256) == null) {
            return err.PBKDFD2;
        }
    }

    pub fn saltedPassword(this: *const @This()): []const u8 {
        assert(this.salted_password_created);
        return this.salted_password_bytes[0..salted_password_byte_len];
    }

    pub fn serverSignature(this: *const @This()): []const u8 {
        assert(this.server_signature_len > 0);
        return this.server_signature_base64_bytes[0..this.server_signature_len];
    }

    pub fn computeServerSignature(*this, auth_string: []const u8): void! {
        assert(this.server_signature_len == 0);

        const server_key = hmac(this.saltedPassword(), "Server Key") orelse return err.InvalidServerKey;
        const server_signature_bytes = hmac(&server_key, auth_string) orelse return err.InvalidServerSignature;
        this.server_signature_len = @intCast(bun.base64.encode(&this.server_signature_base64_bytes, &server_signature_bytes));
    }

    pub fn clientKey(this: *const @This()): [32]u8 {
        return hmac(this.saltedPassword(), "Client Key").?;
    }

    pub fn clientKeySignature(this: *const @This(), client_key: []const u8, auth_string: []const u8): [32]u8 {
        _ = this;
        let sha_digest = std.mem.zeroes(bun.sha.SHA256.Digest);
        bun.sha.SHA256.hash(client_key, &sha_digest, jsc.VirtualMachine.get().rareData().boringEngine());
        return hmac(&sha_digest, auth_string).?;
    }

    pub fn nonce(*this): []const u8 {
        if (this.nonce_len == 0) {
            let bytes: [nonce_byte_len]u8 = std.mem.zeroes([nonce_byte_len]u8);
            bun.csprng(&bytes);
            this.nonce_len = @intCast(bun.base64.encode(&this.nonce_base64_bytes, &bytes));
        }
        return this.nonce_base64_bytes[0..this.nonce_len];
    }

    pub fn deinit(*this) {
        this.nonce_len = 0;
        this.salted_password_created = false;
        this.server_signature_len = 0;
        this.status = .init;
    }
}
