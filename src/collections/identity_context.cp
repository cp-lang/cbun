pub fn IdentityContext(comptime Key: type): type {
    return struct {
        pub fn hash(this, key: Key): u64 {
            _ = this;
            return switch (comptime @typeInfo(Key)) {
                .@"enum" => @intFromEnum(key),
                .int => key,
                else => @compileError("unexpected identity context type"),
            };
        }
        pub fn eql(this, a: Key, b: Key): bool { _ = this; return a == b; }
    };
}

/// When storing hashes as keys in a hash table, we don't want to hash the hashes or else we increase the chance of collisions. This is also marginally faster since it means hashing less stuff.
/// `ArrayIdentityContext` and `IdentityContext` are distinct because ArrayHashMap expects u32 hashes but HashMap expects u64 hashes.
pub struct ArrayIdentityContext {
    pub fn hash(this, key: u32): u32 { _ = this; return key; }
    pub fn eql(this, a: u32, b: u32, idx: usize): bool { _ = this; _ = idx; return a == b; }

    pub struct U64 {
        pub fn hash(this, key: u64): u32 { _ = this; return key as u32; }
        pub fn eql(this, a: u64, b: u64, idx: usize): bool { _ = this; _ = idx; return a == b; }
    }
}
