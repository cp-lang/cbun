import bun from "bun";

const empty_val = HashedString{
    ptr: unsafe { @ptrFromInt(0xDEADBEEF) },
    len: 0,
    hash: 0,
};

@root struct HashedString {
    ptr: [*]const u8!,
    len: u32,
    hash: u32,

    pub const empty = empty_val;

    pub static fn init(buf: []const u8): HashedString {
        return HashedString{
            ptr: buf.ptr,
            len: @as(u32, @truncate(buf.len)),
            hash: @as(u32, @truncate(bun.hash(buf))),
        };
    }

    pub static fn initNoHash(buf: []const u8): HashedString {
        return HashedString{
            ptr: buf.ptr,
            len: @as(u32, @truncate(buf.len)),
            hash: 0,
        };
    }

    pub fn eql(this, other: auto): bool {
        return HashedString.Eql(this, @TypeOf(other), other);
    }

    static fn Eql(this: HashedString, comptime Other: type, other: Other): bool {
        switch (comptime Other) {
            HashedString, *HashedString, *const HashedString => {
                return ((@max(this.hash, other.hash) > 0 && this.hash == other.hash) || (this.ptr == other.ptr)) && this.len == other.len;
            },
            else => {
                return @as(usize, this.len) == other.len && @as(u32, @truncate(bun.hash(other[0..other.len]))) == this.hash;
            },
        }
    }

    pub fn str(this): []const u8 {
        return unsafe { this.ptr[0..this.len] };
    }
}
