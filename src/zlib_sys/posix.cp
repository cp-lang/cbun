import std from "std";
import { DataType, FlushValue, ReturnCode } from "./shared.zig";

type uInt = c_uint;
type uLong = c_ulong;

export @c struct struct_internal_state {
    dummy: c_int,
}

// https://zlib.net/manual.html#Stream
type voidpf = *!?;

// typedef voidpf (*alloc_func) OF((voidpf opaque, uInt items, uInt size));
// typedef void   (*free_func)  OF((voidpf opaque, voidpf address));

export type z_alloc_fn = (@c fn (*!, uInt, uInt): voidpf)?;
export type z_free_fn = (@c fn (*!, *!): void)?;

export @c struct zStream_struct {
    /// next input byte
    next_in: [*c]const u8,
    /// number of bytes available at next_in
    avail_in: uInt,
    /// total number of input bytes read so far
    total_in: uLong,

    /// next output byte will go here
    next_out: [*c]u8,
    /// remaining free space at next_out
    avail_out: uInt,
    /// total number of bytes output so far
    total_out: uLong,

    /// last error message, NULL if no error
    err_msg: cstr?,
    /// not visible by applications
    internal_state: *struct_internal_state!?,

    /// used to allocate the internal state
    alloc_func: z_alloc_fn,
    /// used to free the internal state
    free_func: z_free_fn,
    /// private data object passed to zalloc and zfree
    user_data: *!,

    /// best guess about the data type: binary or text for deflate, or the decoding state for inflate
    data_type: DataType,

    ///Adler-32 or CRC-32 value of the uncompressed data
    adler: uLong,
    /// reserved for future use
    reserved: uLong,
}

export type z_stream = zStream_struct;
export type z_streamp = *z_stream!;

pub extern fn zlibVersion(): [*c]const u8;

pub extern fn deflateInit_(strm: z_streamp, level: c_int, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn inflateInit_(strm: z_streamp, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn deflateInit2_(strm: z_streamp, level: c_int, method: c_int, windowBits: c_int, memLevel: c_int, strategy: c_int, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn inflateInit2_(strm: z_streamp, windowBits: c_int, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn inflateBackInit_(strm: z_streamp, windowBits: c_int, window: [*c]u8, version: [*c]const u8, stream_size: c_int): ReturnCode;

pub inline fn deflateInit(strm: auto, level: auto): ReturnCode {
    unsafe {
        return deflateInit_(strm, level, zlibVersion(), @sizeOf(z_stream) as c_int);
    }
}
pub inline fn inflateInit(strm: auto): ReturnCode {
    unsafe {
        return inflateInit_(strm, zlibVersion(), @sizeOf(z_stream) as c_int);
    }
}
pub inline fn deflateInit2(strm: auto, level: auto, method: auto, windowBits: auto, memLevel: auto, strategy: auto): ReturnCode {
    unsafe {
        return deflateInit2_(strm, level, method, windowBits, memLevel, strategy, zlibVersion(), @sizeOf(z_stream) as c_int);
    }
}
pub inline fn inflateInit2(strm: auto, windowBits: auto): ReturnCode {
    unsafe {
        return inflateInit2_(strm, windowBits, zlibVersion(), @sizeOf(z_stream) as c_int);
    }
}
pub inline fn inflateBackInit(strm: auto, windowBits: auto, window: auto): ReturnCode {
    unsafe {
        return inflateBackInit_(strm, windowBits, window, zlibVersion(), @sizeOf(z_stream) as c_int);
    }
}
