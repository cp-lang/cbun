const bun = @import("bun");
const boring = bun.BoringSSL.c;
pub const Algorithm = bun.jsc.API.Bun.Crypto.EVP.Algorithm;
pub type OutputBuffer = *[u8; boring.EVP_MAX_MD_SIZE];

pub fn generate(key: string, data: string, algorithm: Algorithm, out: OutputBuffer): ?string {
    let outlen: c_uint = boring.EVP_MAX_MD_SIZE;
    if (boring.HMAC(
        algorithm.md() orelse bun.Output.panic("Expected BoringSSL algorithm for HMAC", .{}),
        key.ptr,
        key.len,
        data.ptr,
        data.len,
        out,
        &outlen,
    ) == null) {
        return null;
    }

    return out[0..outlen];
}
