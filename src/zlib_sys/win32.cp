//  zig translate-c -I${VCPKG_ROOT}/installed/x64-windows/include/  ${VCPKG_ROOT}/current/installed/x64-windows/include/zlib.h -target x86_64-windows-msvc -lc > src/zlib_sys/win32.zig
import std from "std";
import { DataType, FlushValue, ReturnCode } from "./shared.zig";

export type rsize_t = usize;
export type _ino_t = c_ushort;
export type ino_t = _ino_t;
export type _dev_t = c_uint;
export type dev_t = _dev_t;
export type _off_t = c_long;
export type off_t = _off_t;
type voidpf = *!;
type Bytef = [*]u8!;
type uInt = c_uint;
// zlib-ng compat (and stock zlib) typedef uLong as `unsigned long` — 4 bytes on
// Windows LLP64. cloudflare/zlib used uint64_t, which is why this was u64. With
// the wrong width, sizeof(z_stream) mismatches and inflateInit_/deflateInit_
// return Z_VERSION_ERROR.
type uLong = c_ulong;
type z_size_t = usize;
type uLongf = uLong;
type voidpc = *const !?;
type voidp = *!?;
export type alloc_func = (@c fn (voidpf, uInt, uInt): voidpf)?;
export type free_func = (@c fn (voidpf, voidpf): void)?;
export type z_alloc_func = alloc_func;
export type z_free_func = free_func;
export @opaque struct struct_internal_state {}
export @c struct struct_z_stream_s {
    next_in: ([*]const u8)?,
    avail_in: uInt,
    total_in: uLong,
    next_out: ([*]u8)?,
    avail_out: uInt,
    total_out: uLong,
    err_msg: cstr?,
    internal_state: *struct_internal_state!?,
    alloc_func: alloc_func,
    free_func: free_func,
    user_data: voidpf,
    data_type: DataType,
    adler: uLong,
    reserved: uLong,
}
export type z_stream = struct_z_stream_s;
export type z_streamp = *z_stream!?;
export @c struct struct_gz_header_s {
    text: c_int,
    time: uLong,
    xflags: c_int,
    os: c_int,
    extra: [*c]Bytef,
    extra_len: uInt,
    extra_max: uInt,
    name: [*c]Bytef,
    name_max: uInt,
    comment: [*c]Bytef,
    comm_max: uInt,
    hcrc: c_int,
    done: c_int,
}
export type gz_header = struct_gz_header_s;
export type gz_headerp = [*c]gz_header;
pub extern fn zlibVersion(): [*c]const u8;
pub extern fn deflate(strm: z_streamp, flush: FlushValue): ReturnCode;
pub extern fn deflateEnd(strm: z_streamp): ReturnCode;
pub extern fn inflate(strm: z_streamp, flush: FlushValue): ReturnCode;
pub extern fn inflateEnd(strm: z_streamp): ReturnCode;
pub extern fn deflateSetDictionary(strm: z_streamp, dictionary: [*c]const Bytef, dictLength: uInt): ReturnCode;
pub extern fn deflateGetDictionary(strm: z_streamp, dictionary: [*c]Bytef, dictLength: [*c]uInt): ReturnCode;
pub extern fn deflateCopy(dest: z_streamp, source: z_streamp): ReturnCode;
pub extern fn deflateReset(strm: z_streamp): ReturnCode;
pub extern fn deflateParams(strm: z_streamp, level: c_int, strategy: c_int): ReturnCode;
pub extern fn deflateTune(strm: z_streamp, good_length: c_int, max_lazy: c_int, nice_length: c_int, max_chain: c_int): ReturnCode;
pub extern fn deflateBound(strm: z_streamp, sourceLen: uLong): uLong;
pub extern fn deflatePending(strm: z_streamp, pending: [*c]c_uint, bits: [*c]c_int): ReturnCode;
pub extern fn deflatePrime(strm: z_streamp, bits: c_int, value: c_int): ReturnCode;
pub extern fn deflateSetHeader(strm: z_streamp, head: gz_headerp): ReturnCode;
pub extern fn inflateSetDictionary(strm: z_streamp, dictionary: [*c]const Bytef, dictLength: uInt): ReturnCode;
pub extern fn inflateGetDictionary(strm: z_streamp, dictionary: [*c]Bytef, dictLength: [*c]uInt): ReturnCode;
pub extern fn inflateSync(strm: z_streamp): ReturnCode;
pub extern fn inflateCopy(dest: z_streamp, source: z_streamp): ReturnCode;
pub extern fn inflateReset(strm: z_streamp): ReturnCode;
pub extern fn inflateReset2(strm: z_streamp, windowBits: c_int): ReturnCode;
pub extern fn inflatePrime(strm: z_streamp, bits: c_int, value: c_int): ReturnCode;
pub extern fn inflateMark(strm: z_streamp): c_long;
pub extern fn inflateGetHeader(strm: z_streamp, head: gz_headerp): ReturnCode;
export type in_func = (@c fn (*!?, [*c][*c]u8): c_uint)?;
export type out_func = (@c fn (*!?, [*c]u8, c_uint): ReturnCode)?;
pub extern fn inflateBack(strm: z_streamp, in_fn: in_func, in_desc: *!?, out_fn: out_func, out_desc: *!?): ReturnCode;
pub extern fn inflateBackEnd(strm: z_streamp): ReturnCode;
pub extern fn zlibCompileFlags(): uLong;
pub extern fn compress(dest: [*c]Bytef, destLen: [*c]uLongf, source: [*c]const Bytef, sourceLen: uLong): ReturnCode;
pub extern fn compress2(dest: [*c]Bytef, destLen: [*c]uLongf, source: [*c]const Bytef, sourceLen: uLong, level: c_int): ReturnCode;
pub extern fn compressBound(sourceLen: uLong): uLong;
pub extern fn uncompress(dest: [*c]Bytef, destLen: [*c]uLongf, source: [*c]const Bytef, sourceLen: uLong): ReturnCode;
pub extern fn uncompress2(dest: [*c]Bytef, destLen: [*c]uLongf, source: [*c]const Bytef, sourceLen: [*c]uLong): ReturnCode;
export @c struct struct_gzFile_s {
    have: c_uint,
    next: [*c]u8,
    pos: c_longlong,
}
export type gzFile = [*c]struct_gzFile_s;
pub extern fn gzdopen(fd: c_int, mode: [*c]const u8): gzFile;
pub extern fn gzbuffer(file: gzFile, size: c_uint): ReturnCode;
pub extern fn gzsetparams(file: gzFile, level: c_int, strategy: c_int): ReturnCode;
pub extern fn gzread(file: gzFile, buf: voidp, len: c_uint): ReturnCode;
pub extern fn gzfread(buf: voidp, size: z_size_t, nitems: z_size_t, file: gzFile): z_size_t;
pub extern fn gzwrite(file: gzFile, buf: voidpc, len: c_uint): ReturnCode;
pub extern fn gzfwrite(buf: voidpc, size: z_size_t, nitems: z_size_t, file: gzFile): z_size_t;
pub extern fn gzprintf(file: gzFile, format: [*c]const u8, ...): ReturnCode;
pub extern fn gzputs(file: gzFile, s: [*c]const u8): ReturnCode;
pub extern fn gzgets(file: gzFile, buf: [*c]u8, len: c_int): [*c]u8;
pub extern fn gzputc(file: gzFile, c: c_int): ReturnCode;
pub extern fn gzgetc(file: gzFile): ReturnCode;
pub extern fn gzungetc(c: c_int, file: gzFile): ReturnCode;
pub extern fn gzflush(file: gzFile, flush: FlushValue): ReturnCode;
pub extern fn gzrewind(file: gzFile): ReturnCode;
pub extern fn gzeof(file: gzFile): ReturnCode;
pub extern fn gzdirect(file: gzFile): ReturnCode;
pub extern fn gzclose(file: gzFile): ReturnCode;
pub extern fn gzclose_r(file: gzFile): ReturnCode;
pub extern fn gzclose_w(file: gzFile): ReturnCode;
pub extern fn gzerror(file: gzFile, errnum: [*c]c_int): [*c]const u8;
pub extern fn gzclearerr(file: gzFile): void;
pub extern fn adler32(adler: uLong, buf: [*c]const Bytef, len: uInt): uLong;
pub extern fn adler32_z(adler: uLong, buf: [*c]const Bytef, len: z_size_t): uLong;
pub extern fn crc32(crc: uLong, buf: [*c]const Bytef, len: uInt): uLong;
pub extern fn crc32_z(crc: uLong, buf: [*c]const Bytef, len: z_size_t): uLong;
pub extern fn crc32_combine_op(crc1: uLong, crc2: uLong, op: uLong): uLong;
pub extern fn deflateInit_(strm: z_streamp, level: c_int, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn inflateInit_(strm: z_streamp, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn deflateInit2_(strm: z_streamp, level: c_int, method: c_int, windowBits: c_int, memLevel: c_int, strategy: c_int, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn inflateInit2_(strm: z_streamp, windowBits: c_int, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn inflateBackInit_(strm: z_streamp, windowBits: c_int, window: [*c]u8, version: [*c]const u8, stream_size: c_int): ReturnCode;
pub extern fn gzgetc_(file: gzFile): ReturnCode;
pub extern fn gzopen(path: [*c]const u8, mode: [*c]const u8): gzFile;
pub extern fn gzseek(file: gzFile, offset: c_long, whence: c_int): c_long;
pub extern fn gztell(file: gzFile): c_long;
pub extern fn gzoffset(file: gzFile): c_long;
pub extern fn adler32_combine(adler1: uLong, adler2: uLong, len2: c_long): uLong;
pub extern fn crc32_combine(crc1: uLong, crc2: uLong, len2: c_long): uLong;
pub extern fn crc32_combine_gen(len2: c_long): uLong;
pub extern fn zError(errnum: c_int): [*c]const u8;
pub extern fn inflateSyncPoint(strm: z_streamp): ReturnCode;
// pub extern fn get_crc_table() [*c]const z_crc_t;
pub extern fn inflateUndermine(strm: z_streamp, subvert: c_int): ReturnCode;
pub extern fn inflateValidate(strm: z_streamp, check: c_int): ReturnCode;
pub extern fn inflateCodesUsed(strm: z_streamp): c_ulong;
pub extern fn inflateResetKeep(strm: z_streamp): ReturnCode;
pub extern fn deflateResetKeep(strm: z_streamp): ReturnCode;

export const z_off_t = c_long;
export const Z_NO_FLUSH: c_int = 0;
export const Z_PARTIAL_FLUSH: c_int = 1;
export const Z_SYNC_FLUSH: c_int = 2;
export const Z_FULL_FLUSH: c_int = 3;
export const Z_FINISH: c_int = 4;
export const Z_BLOCK: c_int = 5;
export const Z_TREES: c_int = 6;
export const Z_OK: c_int = 0;
export const Z_STREAM_END: c_int = 1;
export const Z_NEED_DICT: c_int = 2;
export const Z_ERRNO: c_int = -1;
export const Z_STREAM_ERROR: c_int = -2;
export const Z_DATA_ERROR: c_int = -3;
export const Z_MEM_ERROR: c_int = -4;
export const Z_BUF_ERROR: c_int = -5;
export const Z_VERSION_ERROR: c_int = -6;
export const Z_NO_COMPRESSION: c_int = 0;
export const Z_BEST_SPEED: c_int = 1;
export const Z_BEST_COMPRESSION: c_int = 9;
export const Z_DEFAULT_COMPRESSION: c_int = -1;
export const Z_FILTERED: c_int = 1;
export const Z_HUFFMAN_ONLY: c_int = 2;
export const Z_RLE: c_int = 3;
export const Z_FIXED: c_int = 4;
export const Z_DEFAULT_STRATEGY: c_int = 0;
export const Z_BINARY: c_int = 0;
export const Z_TEXT: c_int = 1;
export const Z_ASCII = Z_TEXT;
export const Z_UNKNOWN: c_int = 2;
export const Z_DEFLATED: c_int = 8;
export const Z_NULL: c_int = 0;

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
export type internal_state = struct_internal_state;
export type z_stream_s = struct_z_stream_s;
export type zStream_struct = struct_z_stream_s;
export type gz_header_s = struct_gz_header_s;
export type gzFile_s = struct_gzFile_s;
