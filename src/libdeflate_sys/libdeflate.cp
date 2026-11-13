import std from "std";
import bun from "bun";

export @c struct Options {
    sizeof_options: usize = @sizeOf(Options),
    malloc_func: (@c fn (usize): *!?)? = null,
    free_func: (@c fn (*!?): void)? = null,
}

pub extern fn libdeflate_alloc_compressor(compression_level: c_int): *Compressor!?;
pub extern fn libdeflate_alloc_compressor_ex(compression_level: c_int, options: (*const Options)?): *Compressor!?;
pub extern fn libdeflate_deflate_compress(compressor: *Compressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize): usize;
pub extern fn libdeflate_deflate_compress_bound(compressor: *Compressor!, in_nbytes: usize): usize;
pub extern fn libdeflate_zlib_compress(compressor: *Compressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize): usize;
pub extern fn libdeflate_zlib_compress_bound(compressor: *Compressor!, in_nbytes: usize): usize;
pub extern fn libdeflate_gzip_compress(compressor: *Compressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize): usize;
pub extern fn libdeflate_gzip_compress_bound(compressor: *Compressor!, in_nbytes: usize): usize;
pub extern fn libdeflate_free_compressor(compressor: *Compressor!): void;

fn load_once(): void {
    unsafe {
        libdeflate_set_memory_allocator(bun.mimalloc.mi_malloc, bun.mimalloc.mi_free);
    }
}

let loaded_once = std.once(load_once);

pub fn load(): void {
    loaded_once.call();
}

export @opaque struct Compressor {
    pub static fn alloc(compression_level: c_int): *Compressor!? {
        unsafe {
            return libdeflate_alloc_compressor(compression_level);
        }
    }

    pub static fn alloc_ex(compression_level: c_int, options: (*const Options)?): *Compressor!? {
        unsafe {
            return libdeflate_alloc_compressor_ex(compression_level, options);
        }
    }

    pub fn deinit(*this): void {
        unsafe {
            return libdeflate_free_compressor(this);
        }
    }

    /// Compresses `input` into `output` and returns the number of bytes written.
    pub fn inflate(*this, input: []const u8, output: []u8): Result {
        unsafe {
            const written = libdeflate_deflate_compress(this, input.ptr, input.len, output.ptr, output.len);
            return Result{ read: input.len, written: written, status: Status.success };
        }
    }

    pub fn maxBytesNeeded(*this, input: []const u8, encoding: Encoding): usize {
        unsafe {
            return switch (encoding) {
                Encoding.deflate => libdeflate_deflate_compress_bound(this, input.len),
                Encoding.zlib => libdeflate_zlib_compress_bound(this, input.len),
                Encoding.gzip => libdeflate_gzip_compress_bound(this, input.len),
            };
        }
    }

    pub fn compress(*this, input: []const u8, output: []u8, encoding: Encoding): Result {
        switch (encoding) {
            Encoding.deflate => return this.inflate(input, output),
            Encoding.zlib => return this.zlib(input, output),
            Encoding.gzip => return this.gzip(input, output),
        }
    }

    pub fn zlib(*this, input: []const u8, output: []u8): Result {
        unsafe {
            const result = libdeflate_zlib_compress(this, input.ptr, input.len, output.ptr, output.len);
            return Result{ read: input.len, written: result, status: Status.success };
        }
    }

    pub fn gzip(*this, input: []const u8, output: []u8): Result {
        unsafe {
            const result = libdeflate_gzip_compress(this, input.ptr, input.len, output.ptr, output.len);
            return Result{ read: input.len, written: result, status: Status.success };
        }
    }
}

export @opaque struct Decompressor {
    pub static fn alloc(): *Decompressor!? {
        unsafe {
            return libdeflate_alloc_decompressor();
        }
    }

    pub fn deinit(*this): void {
        unsafe {
            return libdeflate_free_decompressor(this);
        }
    }

    pub fn deflate(*this, input: []const u8, output: []u8): Result {
        let actual_in_bytes_ret: usize = input.len;
        let actual_out_bytes_ret: usize = output.len;
        unsafe {
            const result = libdeflate_deflate_decompress_ex(this, input.ptr, input.len, output.ptr, output.len, *actual_in_bytes_ret!, *actual_out_bytes_ret!);
            return Result{ read: actual_in_bytes_ret, written: actual_out_bytes_ret, status: result };
        }
    }

    pub fn zlib(*this, input: []const u8, output: []u8): Result {
        let actual_in_bytes_ret: usize = input.len;
        let actual_out_bytes_ret: usize = output.len;
        unsafe {
            const result = libdeflate_zlib_decompress_ex(this, input.ptr, input.len, output.ptr, output.len, *actual_in_bytes_ret!, *actual_out_bytes_ret!);
            return Result{ read: actual_in_bytes_ret, written: actual_out_bytes_ret, status: result };
        }
    }

    pub fn gzip(*this, input: []const u8, output: []u8): Result {
        let actual_in_bytes_ret: usize = input.len;
        let actual_out_bytes_ret: usize = output.len;
        unsafe {
            const result = libdeflate_gzip_decompress_ex(this, input.ptr, input.len, output.ptr, output.len, *actual_in_bytes_ret!, *actual_out_bytes_ret!);
            return Result{ read: actual_in_bytes_ret, written: actual_out_bytes_ret, status: result };
        }
    }

    pub fn decompress(*this, input: []const u8, output: []u8, encoding: Encoding): Result {
        switch (encoding) {
            Encoding.deflate => return this.deflate(input, output),
            Encoding.zlib => return this.zlib(input, output),
            Encoding.gzip => return this.gzip(input, output),
        }
    }
}

export struct Result {
    read: usize,
    written: usize,
    status: Status,
}

export enum Encoding {
    deflate,
    zlib,
    gzip,
}

pub extern fn libdeflate_alloc_decompressor(): *Decompressor!?;
pub extern fn libdeflate_alloc_decompressor_ex(options: (*const Options)?): *Decompressor!?;
export const LIBDEFLATE_SUCCESS = 0;
export const LIBDEFLATE_BAD_DATA = 1;
export const LIBDEFLATE_SHORT_OUTPUT = 2;
export const LIBDEFLATE_INSUFFICIENT_SPACE = 3;
export enum Status: c_uint {
    success = LIBDEFLATE_SUCCESS,
    bad_data = LIBDEFLATE_BAD_DATA,
    short_output = LIBDEFLATE_SHORT_OUTPUT,
    insufficient_space = LIBDEFLATE_INSUFFICIENT_SPACE,
}
pub extern fn libdeflate_deflate_decompress(decompressor: *Decompressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize, actual_out_nbytes_ret: *usize!): Status;
pub extern fn libdeflate_deflate_decompress_ex(decompressor: *Decompressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize, actual_in_nbytes_ret: *usize!, actual_out_nbytes_ret: *usize!): Status;
pub extern fn libdeflate_zlib_decompress(decompressor: *Decompressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize, actual_out_nbytes_ret: *usize!): Status;
pub extern fn libdeflate_zlib_decompress_ex(decompressor: *Decompressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize, actual_in_nbytes_ret: *usize!, actual_out_nbytes_ret: *usize!): Status;
pub extern fn libdeflate_gzip_decompress(decompressor: *Decompressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize, actual_out_nbytes_ret: *usize!): Status;
pub extern fn libdeflate_gzip_decompress_ex(decompressor: *Decompressor!, in_buf: const *!?, in_nbytes: usize, out: *!?, out_nbytes_avail: usize, actual_in_nbytes_ret: *usize!, actual_out_nbytes_ret: *usize!): Status;
pub extern fn libdeflate_free_decompressor(decompressor: *Decompressor!): void;
pub extern fn libdeflate_adler32(adler: u32, buffer: const *!?, len: usize): u32;
pub extern fn libdeflate_crc32(crc: u32, buffer: const *!?, len: usize): u32;
pub extern fn libdeflate_set_memory_allocator(malloc_func: (@c fn (usize): *!?)?, free_func: (@c fn (*!?): void)?): void;
export type libdeflate_compressor = Compressor;
export type libdeflate_options = Options;
export type libdeflate_decompressor = Decompressor;
