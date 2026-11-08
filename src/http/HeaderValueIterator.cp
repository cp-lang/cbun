// src/http/HeaderValueIterator.cp
// Comma-separated HTTP Header Value Iterator in CP language

export @root struct HeaderValueIterator {
    iterator: std.mem.TokenIterator(u8, .scalar),

    pub static fn init(input: string): HeaderValueIterator {
        return {
            iterator: std.mem.tokenizeScalar(u8, std.mem.trim(u8, input, " \t"), ','),
        };
    }

    pub fn next(*this): string? {
        const slice = std.mem.trim(u8, this.iterator.next() ?? return null, " \t");
        if (slice.len == 0) return this.next();
        return slice;
    }
}
