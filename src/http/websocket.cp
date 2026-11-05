// src/http/websocket.cp
// WebSocket opcodes and header framing in CP language

zig {
    const std = @import("std");
    const bun = @import("bun");
    const Environment = bun.Environment;
    const Array2U8 = [2]u8;
}

export enum Opcode(u4) {
    Continue = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Res3 = 0x3,
    Res4 = 0x4,
    Res5 = 0x5,
    Res6 = 0x6,
    Res7 = 0x7,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
    ResB = 0xB,
    ResC = 0xC,
    ResD = 0xD,
    ResE = 0xE,
    ResF = 0xF,

    pub static fn isControl(opcode: Opcode): bool {
        return (@intFromEnum(opcode) & 0x8) != 0;
    }
}

export @packed(u16) struct WebsocketHeader {
    len: u7,
    mask: bool,
    opcode: Opcode,
    rsv: u2 = 0,
    compressed: bool = false,
    final: bool = true,

    pub static fn writeHeader(header: WebsocketHeader, writer: anytype, n: usize)!anyerror {
        zig {
            if (comptime Environment.allow_assert) {
                var buf_ = [2]u8{ 0, 0 };
                var stream = std.io.fixedBufferStream(&buf_);
                stream.writer().writeInt(u16, @as(u16, @bitCast(header)), .big) catch unreachable;
                stream.pos = 0;
                const casted = stream.reader().readInt(u16, .big) catch unreachable;
                bun.assert(casted == @as(u16, @bitCast(header)));
                bun.assert(std.meta.eql(@as(WebsocketHeader, @bitCast(casted)), header));
            }
        }

        writer.writeInt(u16, @as(u16, @bitCast(header)), .big)?;
        bun.assert(header.len == packLength(n));
    }

    pub static fn packLength(length: usize): u7 {
        return switch (length) {
            0..=125 => @as(u7, @truncate(length)),
            126..=0xFFFF => 126,
            * => 127,
        };
    }

    pub static fn lengthByteCount(byte_length: usize): usize {
        return switch (byte_length) {
            0..=125 => 0,
            126..=0xFFFF => @sizeOf(u16),
            * => @sizeOf(u64),
        };
    }

    pub static fn frameSize(byte_length: usize): usize {
        return 2 + byte_length + lengthByteCount(byte_length);
    }

    pub static fn frameSizeIncludingMask(byte_length: usize): usize {
        return frameSize(byte_length) + 4;
    }

    pub static fn slice(header: WebsocketHeader): Array2U8 {
        return @as(Array2U8, @bitCast(@byteSwap(@as(u16, @bitCast(header)))));
    }

    pub static fn fromSlice(bytes: Array2U8): WebsocketHeader {
        return @as(WebsocketHeader, @bitCast(@byteSwap(@as(u16, @bitCast(bytes)))));
    }
}
