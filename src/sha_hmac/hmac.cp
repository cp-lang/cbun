import bun from "bun";

export type Algorithm = bun.jsc.API.Bun.Crypto.EVP.Algorithm;
export type OutputBuffer = *u8[bun.BoringSSL.c.EVP_MAX_MD_SIZE];

@c {
    fn HMAC(
        evp_md: *any!,
        key: [*]const u8!,
        key_len: usize,
        data: [*]const u8!,
        data_len: usize,
        md: [*]u8!,
        md_len: *u32,
    ): [*]u8?!;
}

pub fn generate(key: string, data: string, algorithm: Algorithm, out: OutputBuffer): string? {
    let outlen: u32 = bun.BoringSSL.c.EVP_MAX_MD_SIZE as u32;
    let md = algorithm.md() ?? bun.Output.panic("Expected BoringSSL algorithm for HMAC");
    if (HMAC(
        md,
        key.ptr,
        key.len,
        data.ptr,
        data.len,
        out as [*]u8!,
        &outlen,
    ) == null) {
        return null;
    }
    return out[0..(outlen as usize)];
}
