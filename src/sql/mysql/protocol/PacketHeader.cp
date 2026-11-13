@root struct PacketHeader {
    length: u24,
    sequence_id: u8,

    pub const size = 4;

    pub static fn decode(bytes: []const u8) ?PacketHeader {
        if (bytes.len < 4) return null;

        return PacketHeader{
            length: @as(u24, bytes[0]) |
                (@as(u24, bytes[1]) << 8) |
                (@as(u24, bytes[2]) << 16),
            sequence_id: bytes[3],
        };
    }

    pub fn encode(this) [4]u8 {
        return [
            @intCast(this.length & 0xff),
            @intCast((this.length >> 8) & 0xff),
            @intCast((this.length >> 16) & 0xff),
            this.sequence_id,
        ];
    }
}
