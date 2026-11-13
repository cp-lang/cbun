import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const libdeflate = bun.libdeflate;
const zlib = bun.zlib;

const Z_DEFAULT_COMPRESSION = 6;
const Z_DEFLATED = 8;
const Z_DEFAULT_STRATEGY = 0;
const Z_DEFAULT_MEM_LEVEL = 8;
const COMPRESSION_BUFFER_SIZE = 4096;
const MAX_DECOMPRESSED_SIZE: usize = 128 * 1024 * 1024;
const DEFLATE_TRAILER: [4]u8 = [0x00, 0x00, 0xff, 0xff];

export @c struct Params {
    server_max_window_bits: u8 = 15,
    client_max_window_bits: u8 = 15,
    server_no_context_takeover: u8 = 0,
    client_no_context_takeover: u8 = 0,

    pub const MAX_WINDOW_BITS: u8 = 15;
    pub const MIN_WINDOW_BITS: u8 = 8;
}

const stack_buf_size = 128 * 1024;
const StackFallback = std.heap.StackFallbackAllocator(stack_buf_size);

export struct RareData {
    libdeflate_compressor: (*libdeflate.Compressor)? = null,
    libdeflate_decompressor: (*libdeflate.Decompressor)? = null,
    stack_fallback: StackFallback = undefined,

    pub const stack_buffer_size = stack_buf_size;

    pub fn arrayList(*this) std.array_list.Managed(u8) {
        let list = std.array_list.Managed(u8).init(this.allocator());
        list.items = &this.stack_fallback.buffer;
        list.items.len = 0;
        list.capacity = this.stack_fallback.buffer.len;
        this.stack_fallback.fixed_buffer_allocator.end_index = this.stack_fallback.buffer.len;
        return list;
    }

    pub fn deinit(*this) void {
        if (c := this.libdeflate_compressor) {
            c.deinit();
        }
        if (d := this.libdeflate_decompressor) {
            d.deinit();
        }
        bun.destroy(this);
    }

    pub fn allocator(*this) std.mem.Allocator {
        this.stack_fallback = StackFallback{
            buffer: undefined,
            fallback_allocator: bun.default_allocator,
            fixed_buffer_allocator: undefined,
        };
        return this.stack_fallback.get();
    }

    pub fn decompressor(*this) *libdeflate.Decompressor {
        if (d := this.libdeflate_decompressor) {
            return d;
        }
        this.libdeflate_decompressor = libdeflate.Decompressor.alloc();
        return this.libdeflate_decompressor.?;
    }

    pub fn compressor(*this) *libdeflate.Compressor {
        if (c := this.libdeflate_compressor) {
            return c;
        }
        this.libdeflate_compressor = libdeflate.Compressor.alloc();
        return this.libdeflate_compressor.?;
    }
}

pub err DecompressError { InflateFailed, OutOfMemory, TooLarge }
pub err CompressError { DeflateFailed, OutOfMemory }

@root struct PerMessageDeflate {
    allocator: std.mem.Allocator,
    compress_stream: zlib.z_stream,
    decompress_stream: zlib.z_stream,
    params: Params,
    rare_data: *RareData,

    pub static fn init(allocator: std.mem.Allocator, params: Params, rare_data: *jsc.RareData) anyerror!*PerMessageDeflate {
        let inst = allocator.create(PerMessageDeflate)?;
        unsafe {
            inst.* = PerMessageDeflate{
                allocator: allocator,
                params: params,
                compress_stream: std.mem.zeroes(zlib.z_stream),
                decompress_stream: std.mem.zeroes(zlib.z_stream),
                rare_data: rare_data.websocketDeflate(),
            };
        }

        const compress_err = zlib.deflateInit2_(
            &inst.compress_stream,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            -@as(c_int, inst.params.client_max_window_bits),
            Z_DEFAULT_MEM_LEVEL,
            Z_DEFAULT_STRATEGY,
            zlib.zlibVersion(),
            @sizeOf(zlib.z_stream),
        );
        if (compress_err != .Ok) {
            allocator.destroy(inst);
            return error.DeflateInitFailed;
        }

        const decompress_err = zlib.inflateInit2_(
            &inst.decompress_stream,
            -@as(c_int, inst.params.server_max_window_bits),
            zlib.zlibVersion(),
            @sizeOf(zlib.z_stream),
        );
        if (decompress_err != .Ok) {
            _ = zlib.deflateEnd(&inst.compress_stream);
            allocator.destroy(inst);
            return error.InflateInitFailed;
        }

        return inst;
    }

    pub fn deinit(*this) void {
        _ = zlib.deflateEnd(&this.compress_stream);
        _ = zlib.inflateEnd(&this.decompress_stream);
        this.allocator.destroy(this);
    }

    pub static fn canUseLibDeflate(len: usize) bool {
        if (bun.feature_flag.BUN_FEATURE_FLAG_NO_LIBDEFLATE.get()) {
            return false;
        }
        return len < RareData.stack_buffer_size;
    }

    pub fn decompress(*this, in_buf: []const u8, out: *std.array_list.Managed(u8)) DecompressError!void {
        const initial_len = out.items.len;

        if (canUseLibDeflate(in_buf.len)) {
            const result = this.rare_data.decompressor().deflate(in_buf, out.unusedCapacitySlice());
            if (result.status == .success) {
                out.items.len += result.written;
                if (out.items.len - initial_len > MAX_DECOMPRESSED_SIZE) {
                    return error.TooLarge;
                }
                return;
            }
        }

        let in_with_trailer = std.array_list.Managed(u8).init(this.allocator);
        defer in_with_trailer.deinit();
        in_with_trailer.appendSlice(in_buf)?;
        in_with_trailer.appendSlice(DEFLATE_TRAILER[0..])?;

        this.decompress_stream.next_in = in_with_trailer.items.ptr;
        this.decompress_stream.avail_in = @intCast(in_with_trailer.items.len);

        while (true) {
            out.ensureUnusedCapacity(COMPRESSION_BUFFER_SIZE)?;
            this.decompress_stream.next_out = out.unusedCapacitySlice().ptr;
            this.decompress_stream.avail_out = @intCast(out.unusedCapacitySlice().len);

            const res = zlib.inflate(&this.decompress_stream, zlib.FlushValue.NoFlush);
            out.items.len += out.unusedCapacitySlice().len - this.decompress_stream.avail_out;

            if (out.items.len - initial_len > MAX_DECOMPRESSED_SIZE) {
                return error.TooLarge;
            }

            if (res == .StreamEnd) {
                break;
            }
            if (res != .Ok) {
                return error.InflateFailed;
            }
            if (this.decompress_stream.avail_out == 0 && this.decompress_stream.avail_in != 0) {
                continue;
            }
            if (this.decompress_stream.avail_in == 0) {
                break;
            }
        }

        if (this.params.server_no_context_takeover == 1) {
            _ = zlib.inflateReset(&this.decompress_stream);
        }
    }

    pub fn compress(*this, in_buf: []const u8, out: *std.array_list.Managed(u8)) CompressError!void {
        this.compress_stream.next_in = in_buf.ptr;
        this.compress_stream.avail_in = @intCast(in_buf.len);

        while (true) {
            out.ensureUnusedCapacity(COMPRESSION_BUFFER_SIZE)?;
            this.compress_stream.next_out = out.unusedCapacitySlice().ptr;
            this.compress_stream.avail_out = @intCast(out.unusedCapacitySlice().len);

            const res = zlib.deflate(&this.compress_stream, zlib.FlushValue.SyncFlush);
            out.items.len += out.unusedCapacitySlice().len - this.compress_stream.avail_out;
            if (res != .Ok)
                return error.DeflateFailed;

            if (this.compress_stream.avail_in == 0 && this.compress_stream.avail_out != 0) {
                break;
            }
        }

        if (out.items.len >= 4 &&
            std.mem.eql(u8, out.items[out.items.len - 4 ..], DEFLATE_TRAILER[0..]))
        {
            out.shrinkRetainingCapacity(out.items.len - 4);
        }

        if (this.params.client_no_context_takeover == 1) {
            _ = zlib.deflateReset(&this.compress_stream);
        }
    }
}
