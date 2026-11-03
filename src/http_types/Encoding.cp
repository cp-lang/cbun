export enum Encoding {
    identity,
    gzip,
    deflate,
    brotli,
    zstd,
    chunked,

    fn canUseLibDeflate(this): bool {
        return switch (this) {
            .gzip, .deflate => true,
            else => false,
        };
    }

    fn isCompressed(this): bool {
        return switch (this) {
            .brotli, .gzip, .deflate, .zstd => true,
            else => false,
        };
    }
}
