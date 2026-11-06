import bun from "bun";

export class Hasher<const digest_size: usize, ContextType, Full, Init, Update, Final> {
    hasher: ContextType,

    pub type Digest = u8[digest_size];
    pub const digest: usize = digest_size;

    pub static fn init(): Hasher<digest_size, ContextType, Full, Init, Update, Final> {
        bun.BoringSSL.load();
        let this = Hasher<digest_size, ContextType, Full, Init, Update, Final> {
            hasher = undefined,
        };
        bun.assert(Init(&this.hasher) == 1);
        return this;
    }

    pub static fn hash(bytes: string, out: *Digest): void {
        Full(bytes.ptr, bytes.len, out);
    }

    pub fn update(*this, data: string): void {
        bun.assert(Update(&this.hasher, data.ptr, data.len) == 1);
    }

    pub fn final(*this, out: *Digest): void {
        bun.assert(Final(out, &this.hasher) == 1);
    }
}

export class EvpHasher<const digest_size: usize, const MDName: string> {
    ctx: bun.BoringSSL.c.EVP_MD_CTX,

    pub type Digest = u8[digest_size];
    pub const digest: usize = digest_size;

    pub static fn init(): EvpHasher<digest_size, MDName> {
        bun.BoringSSL.load();
        let md = reflect.get(bun.BoringSSL.c, MDName)();
        let this = EvpHasher<digest_size, MDName> {
            ctx = undefined,
        };
        bun.BoringSSL.c.EVP_MD_CTX_init(&this.ctx);
        bun.assert(bun.BoringSSL.c.EVP_DigestInit(&this.ctx, md) == 1);
        return this;
    }

    pub static fn hash(bytes: string, out: *Digest, engine: *bun.BoringSSL.c.ENGINE?): void {
        let md = reflect.get(bun.BoringSSL.c, MDName)();
        bun.assert(bun.BoringSSL.c.EVP_Digest(bytes.ptr, bytes.len, out, null, md, engine) == 1);
    }

    pub fn update(*this, data: string): void {
        bun.assert(bun.BoringSSL.c.EVP_DigestUpdate(&this.ctx, data.ptr, data.len) == 1);
    }

    pub fn final(*this, out: *Digest): void {
        bun.assert(bun.BoringSSL.c.EVP_DigestFinal(&this.ctx, out, null) == 1);
    }

    pub fn release(*this): void {
        bun.BoringSSL.c.EVP_MD_CTX_cleanup(&this.ctx);
    }
}

export class EVP {
    pub type SHA1 = EvpHasher<20, "EVP_sha1">;
    pub type MD5 = EvpHasher<16, "EVP_md5">;
    pub type MD4 = EvpHasher<16, "EVP_md4">;
    pub type SHA224 = EvpHasher<28, "EVP_sha224">;
    pub type SHA512 = EvpHasher<64, "EVP_sha512">;
    pub type SHA384 = EvpHasher<48, "EVP_sha384">;
    pub type SHA256 = EvpHasher<32, "EVP_sha256">;
    pub type SHA512_256 = EvpHasher<32, "EVP_sha512_256">;
    pub type MD5_SHA1 = EvpHasher<20, "EVP_md5_sha1">;
    pub type Blake2 = EvpHasher<32, "EVP_blake2b256">;
}

export type SHA1 = EVP.SHA1;
export type MD5 = EVP.MD5;
export type MD4 = EVP.MD4;
export type SHA224 = EVP.SHA224;
export type SHA512 = EVP.SHA512;
export type SHA384 = EVP.SHA384;
export type SHA256 = EVP.SHA256;
export type SHA512_256 = EVP.SHA512_256;
export type MD5_SHA1 = EVP.MD5_SHA1;

export class Hashers {
    pub type SHA1 = Hasher<20, bun.BoringSSL.c.SHA_CTX, bun.BoringSSL.c.SHA1, bun.BoringSSL.c.SHA1_Init, bun.BoringSSL.c.SHA1_Update, bun.BoringSSL.c.SHA1_Final>;
    pub type SHA512 = Hasher<64, bun.BoringSSL.c.SHA512_CTX, bun.BoringSSL.c.SHA512, bun.BoringSSL.c.SHA512_Init, bun.BoringSSL.c.SHA512_Update, bun.BoringSSL.c.SHA512_Final>;
    pub type SHA384 = Hasher<48, bun.BoringSSL.c.SHA512_CTX, bun.BoringSSL.c.SHA384, bun.BoringSSL.c.SHA384_Init, bun.BoringSSL.c.SHA384_Update, bun.BoringSSL.c.SHA384_Final>;
    pub type SHA256 = Hasher<32, bun.BoringSSL.c.SHA256_CTX, bun.BoringSSL.c.SHA256, bun.BoringSSL.c.SHA256_Init, bun.BoringSSL.c.SHA256_Update, bun.BoringSSL.c.SHA256_Final>;
    pub type SHA512_256 = Hasher<32, bun.BoringSSL.c.SHA512_CTX, bun.BoringSSL.c.SHA512_256, bun.BoringSSL.c.SHA512_256_Init, bun.BoringSSL.c.SHA512_256_Update, bun.BoringSSL.c.SHA512_256_Final>;
    pub type RIPEMD160 = Hasher<20, bun.BoringSSL.c.RIPEMD160_CTX, bun.BoringSSL.c.RIPEMD160, bun.BoringSSL.c.RIPEMD160_Init, bun.BoringSSL.c.RIPEMD160_Update, bun.BoringSSL.c.RIPEMD160_Final>;
}
