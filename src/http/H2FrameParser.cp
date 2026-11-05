// src/http/H2FrameParser.cp
// HTTP/2 wire-format types for fetch() HTTP/2 client in CP language

zig {
    const bun = @import("bun");
    const std = @import("std");
}

export const client_preface: string = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

export const MAX_WINDOW_SIZE = std.math.maxInt(i32);
export const MAX_HEADER_TABLE_SIZE = std.math.maxInt(u32);
export const MAX_STREAM_ID = std.math.maxInt(i32);
export const MAX_FRAME_SIZE = std.math.maxInt(u24);
export const DEFAULT_WINDOW_SIZE = std.math.maxInt(u16);
export const DEFAULT_MAX_FRAME_SIZE: u24 = 16384;

export enum FrameType(u8) {
    HTTP_FRAME_DATA = 0x00,
    HTTP_FRAME_HEADERS = 0x01,
    HTTP_FRAME_PRIORITY = 0x02,
    HTTP_FRAME_RST_STREAM = 0x03,
    HTTP_FRAME_SETTINGS = 0x04,
    HTTP_FRAME_PUSH_PROMISE = 0x05,
    HTTP_FRAME_PING = 0x06,
    HTTP_FRAME_GOAWAY = 0x07,
    HTTP_FRAME_WINDOW_UPDATE = 0x08,
    HTTP_FRAME_CONTINUATION = 0x09,
    HTTP_FRAME_ALTSVC = 0x0A,
    HTTP_FRAME_ORIGIN = 0x0C,
    _,
}

export enum PingFrameFlags(u8) {
    ACK = 0x1,
}

export enum DataFrameFlags(u8) {
    END_STREAM = 0x1,
    PADDED = 0x8,
}

export enum HeadersFrameFlags(u8) {
    END_STREAM = 0x1,
    END_HEADERS = 0x4,
    PADDED = 0x8,
    PRIORITY = 0x20,
}

export enum SettingsFlags(u8) {
    ACK = 0x1,
}

export enum ErrorCode(u32) {
    NO_ERROR = 0x0,
    PROTOCOL_ERROR = 0x1,
    INTERNAL_ERROR = 0x2,
    FLOW_CONTROL_ERROR = 0x3,
    SETTINGS_TIMEOUT = 0x4,
    STREAM_CLOSED = 0x5,
    FRAME_SIZE_ERROR = 0x6,
    REFUSED_STREAM = 0x7,
    CANCEL = 0x8,
    COMPRESSION_ERROR = 0x9,
    CONNECT_ERROR = 0xa,
    ENHANCE_YOUR_CALM = 0xb,
    INADEQUATE_SECURITY = 0xc,
    HTTP_1_1_REQUIRED = 0xd,
    MAX_PENDING_SETTINGS_ACK = 0xe,
    _,
}

export enum SettingsType(u16) {
    SETTINGS_HEADER_TABLE_SIZE = 0x1,
    SETTINGS_ENABLE_PUSH = 0x2,
    SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    SETTINGS_MAX_FRAME_SIZE = 0x5,
    SETTINGS_MAX_HEADER_LIST_SIZE = 0x6,
    SETTINGS_ENABLE_CONNECT_PROTOCOL = 0x8,
    SETTINGS_NO_RFC7540_PRIORITIES = 0x9,
    _,
}

@inline
pub fn u32FromBytes(src: string): u32 {
    bun.debugAssert(src.len == 4);
    return std.mem.readInt(u32, src[0..4], .big);
}

export @packed(u32) struct UInt31WithReserved {
    reserved: bool = false,
    uint31: u31 = 0,

    @inline
    pub static fn from(value: u32): UInt31WithReserved {
        return { uint31: @truncate(value & 0x7fffffff), reserved: (value & 0x80000000) != 0 };
    }

    @inline
    pub static fn init(value: u31, reserved: bool): UInt31WithReserved {
        return { uint31: value, reserved: reserved };
    }

    @inline
    pub static fn toUInt32(value: UInt31WithReserved): u32 {
        return @bitCast(value);
    }

    @inline
    pub static fn fromBytes(src: string): UInt31WithReserved {
        const value: u32 = u32FromBytes(src);
        return { uint31: @truncate(value & 0x7fffffff), reserved: (value & 0x80000000) != 0 };
    }
}

export @packed(u40) struct StreamPriority {
    streamIdentifier: u32 = 0,
    weight: u8 = 0,

    zig {
        pub const byteSize: usize = 5;
    }

    @inline
    pub static fn from(dst: *StreamPriority, src: string): void {
        zig {
            @memcpy(@as(*[StreamPriority.byteSize]u8, @ptrCast(dst)), src);
            std.mem.byteSwapAllFields(StreamPriority, dst);
        }
    }
}

export @packed(u72) struct FrameHeader {
    length: u24 = 0,
    type: u8 = @intFromEnum(FrameType.HTTP_FRAME_SETTINGS),
    flags: u8 = 0,
    streamIdentifier: u32 = 0,

    zig {
        pub const byteSize: usize = 9;
    }

    @inline
    pub static fn from(dst: *FrameHeader, src: string, offset: usize, comptime end: bool): void {
        zig {
            @memcpy(@as(*[FrameHeader.byteSize]u8, @ptrCast(dst))[offset .. src.len + offset], src);
            if (comptime end) {
                std.mem.byteSwapAllFields(FrameHeader, dst);
            }
        }
    }
}

export @packed(u48) struct SettingsPayloadUnit {
    type: u16,
    value: u32,

    zig {
        pub const byteSize: usize = 6;
    }

    @inline
    pub static fn from(dst: *SettingsPayloadUnit, src: string, offset: usize, comptime end: bool): void {
        zig {
            @memcpy(@as(*[SettingsPayloadUnit.byteSize]u8, @ptrCast(dst))[offset .. src.len + offset], src);
            if (comptime end) {
                std.mem.byteSwapAllFields(SettingsPayloadUnit, dst);
            }
        }
    }

    @inline
    pub static fn encode(dst: anytype, setting: SettingsType, value: u32): void {
        zig {
            std.mem.writeInt(u16, dst[0..2], @intFromEnum(setting), .big);
            std.mem.writeInt(u32, dst[2..6], value, .big);
        }
    }
}
