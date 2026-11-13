import bun from "bun";
import DataMod from "../../shared/Data.zig";
import NewReaderMod from "./NewReader.zig";
import NewWriterMod from "./NewWriter.zig";

const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;
const NewWriter = NewWriterMod.NewWriter;
const writeWrap = NewWriterMod.writeWrap;
const BoringSSL = bun.BoringSSL;
const jsc = bun.jsc;
const debug = bun.Output.scoped(.Auth, .hidden);

export struct mysql_native_password {
    pub static fn scramble(password: []const u8, nonce: []const u8) anyerror![20]u8 {
        let stage1: [20]u8 = [0; 20];
        let stage2: [20]u8 = [0; 20];
        let stage3: [20]u8 = [0; 20];
        let result: [20]u8 = [0; 20];
        if (password.len == 0) {
            return result;
        }
        if (nonce.len < 20) {
            return error.MissingAuthData;
        }

        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        bun.sha.SHA1.hash(password, &stage1, vm.rareData().boringEngine());
        bun.sha.SHA1.hash(&stage1, &stage2, vm.rareData().boringEngine());

        let sha1 = bun.sha.SHA1.init();
        defer sha1.deinit();
        sha1.update(nonce[0..8]);
        sha1.update(nonce[8..20]);
        sha1.update(&stage2);
        sha1.final(&stage3);

        let idx: usize = 0;
        while (idx < 20) {
            result[idx] = stage3[idx] ^ stage1[idx];
            idx += 1;
        }

        return result;
    }
}

export enum AuthFastStatus: u8 {
    success = 0x03,
    continue_auth = 0x04,
    _,
}

export struct AuthResponse {
    status: AuthFastStatus = .success,
    data: Data = Data{ empty: {} },

    pub fn deinit(*this) void {
        this.data.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        const status = reader.int(u8)?;
        debug("FastAuthStatus: {d}", .{status});
        this.status = @enumFromInt(status);

        const remaining = reader.peek();
        if (remaining.len > 0) {
            this.data = reader.read(remaining.len)?;
        }
    }

    pub const decode = decoderWrap(AuthResponse, decodeInternal).decode;
}

export struct AuthEncryptedPassword {
    password: []const u8,
    public_key: []const u8,
    nonce: []const u8,
    sequence_id: u8,

    pub fn writeInternal(this: *const AuthEncryptedPassword, comptime Context: type, writer: NewWriter(Context)) anyerror!void {
        if (this.nonce.len == 0) {
            return error.MissingAuthData;
        }
        if (this.public_key.len == 0) {
            return error.InvalidPublicKey;
        }
        let password_buf: [1024]u8 = undefined;
        let needs_to_free_password = false;
        const needed_len = this.password.len + 1;
        let plain_password: []u8 = password_buf[0..0];
        if (needed_len > password_buf.len) {
            needs_to_free_password = true;
            plain_password = bun.default_allocator.alloc(u8, needed_len)?;
        } else {
            plain_password = password_buf[0..needed_len];
        }
        @memcpy(plain_password[0..this.password.len], this.password);
        plain_password[this.password.len] = 0;
        defer if (needs_to_free_password) bun.default_allocator.free(plain_password);

        let i: usize = 0;
        while (i < plain_password.len) {
            plain_password[i] ^= this.nonce[i % this.nonce.len];
            i += 1;
        }
        BoringSSL.load();
        BoringSSL.c.ERR_clear_error();
        const bio = BoringSSL.c.BIO_new_mem_buf(&this.public_key[0], @intCast(this.public_key.len)) orelse return error.InvalidPublicKey;
        defer _ = BoringSSL.c.BIO_free(bio);

        const rsa_opt = BoringSSL.c.PEM_read_bio_RSA_PUBKEY(bio, null, null, null);
        if (rsa_opt == null) {
            if (bun.Environment.isDebug) {
                BoringSSL.c.ERR_load_ERR_strings();
                BoringSSL.c.ERR_load_crypto_strings();
                let buf: [256]u8 = undefined;
                debug("Failed to read public key: {s}", .{BoringSSL.c.ERR_error_string(BoringSSL.c.ERR_get_error(), &buf)});
            }
            return error.InvalidPublicKey;
        }
        const rsa = rsa_opt.?;
        defer BoringSSL.c.RSA_free(rsa);

        const rsa_size = BoringSSL.c.RSA_size(rsa);
        let needs_to_free_encrypted_password = false;
        let encrypted_password_buf: [4096]u8 = undefined;
        let encrypted_password: []u8 = encrypted_password_buf[0..0];
        if (rsa_size > encrypted_password_buf.len) {
            needs_to_free_encrypted_password = true;
            encrypted_password = bun.default_allocator.alloc(u8, rsa_size)?;
        } else {
            encrypted_password = encrypted_password_buf[0..rsa_size];
        }
        defer if (needs_to_free_encrypted_password) bun.default_allocator.free(encrypted_password);

        const encrypted_password_len = BoringSSL.c.RSA_public_encrypt(
            @intCast(plain_password.len),
            plain_password.ptr,
            encrypted_password.ptr,
            rsa,
            BoringSSL.c.RSA_PKCS1_OAEP_PADDING,
        );
        if (encrypted_password_len == -1) {
            return error.FailedToEncryptPassword;
        }
        const encrypted_password_slice = encrypted_password[0..@intCast(encrypted_password_len)];

        let packet = writer.start(this.sequence_id)?;
        writer.write(encrypted_password_slice)?;
        packet.end()?;
    }

    pub const write = writeWrap(AuthEncryptedPassword, writeInternal).write;
}

export struct AuthPublicKeyResponse {
    data: Data = Data{ empty: {} },

    pub fn deinit(*this) void {
        this.data.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        const remaining = reader.peek();
        if (remaining.len > 0) {
            this.data = reader.read(remaining.len)?;
        }
    }

    pub const decode = decoderWrap(AuthPublicKeyResponse, decodeInternal).decode;
}

export struct AuthPublicKeyRequest {
    pub fn writeInternal(this: *const AuthPublicKeyRequest, comptime Context: type, writer: NewWriter(Context)) anyerror!void {
        _ = this;
        writer.int1(0x02)?;
    }

    pub const write = writeWrap(AuthPublicKeyRequest, writeInternal).write;
}

export struct caching_sha2_password {
    pub static fn scramble(password: []const u8, nonce: []const u8) anyerror![32]u8 {
        let digest1: [32]u8 = [0; 32];
        let digest2: [32]u8 = [0; 32];
        let digest3: [32]u8 = [0; 32];
        let result: [32]u8 = [0; 32];

        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        bun.sha.SHA256.hash(password, &digest1, vm.rareData().boringEngine());
        bun.sha.SHA256.hash(&digest1, &digest2, vm.rareData().boringEngine());

        const combined = bun.default_allocator.alloc(u8, nonce.len + digest2.len)?;
        defer bun.default_allocator.free(combined);
        @memcpy(combined[0..nonce.len], nonce);
        @memcpy(combined[nonce.len..], &digest2);
        bun.sha.SHA256.hash(combined, &digest3, vm.rareData().boringEngine());

        let idx: usize = 0;
        while (idx < 32) {
            result[idx] = digest1[idx] ^ digest3[idx];
            idx += 1;
        }

        return result;
    }

    pub const FastAuthStatus = AuthFastStatus;
    pub const Response = AuthResponse;
    pub const EncryptedPassword = AuthEncryptedPassword;
    pub const PublicKeyResponse = AuthPublicKeyResponse;
    pub const PublicKeyRequest = AuthPublicKeyRequest;
}

