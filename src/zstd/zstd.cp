import std from "std";
import bun from "bun";

const c = bun.c;

// -----------------------------------

/// ZSTD_compress() :
///  Compresses `src` content as a single zstd compressed frame into already allocated `dst`.
///  NOTE: Providing `dstCapacity >= ZSTD_compressBound(srcSize)` guarantees that zstd will have
///        enough space to successfully compress the data.
///  @return : compressed size written into `dst` (<= `dstCapacity),
///            or an error code if it fails (which can be tested using ZSTD_isError()). */
pub fn compress(dest: []u8, src: []const u8, level: i32?): Result {
    const result = c.ZSTD_compress(dest.ptr, dest.len, src.ptr, src.len, level ?? c.ZSTD_defaultCLevel());
    if (c.ZSTD_isError(result) != 0) return { err: bun.sliceTo(c.ZSTD_getErrorName(result), 0) };
    return { success: result };
}

pub fn compressBound(srcSize: usize): usize {
    return c.ZSTD_compressBound(srcSize);
}

/// ZSTD_decompress() :
/// `compressedSize` : must be the _exact_ size of some number of compressed and/or skippable frames.
/// `dstCapacity` is an upper bound of originalSize to regenerate.
/// If user cannot imply a maximum upper bound, it's better to use streaming mode to decompress data.
/// @return : the number of bytes decompressed into `dst` (<= `dstCapacity`),
///           or an errorCode if it fails (which can be tested using ZSTD_isError()). */
pub fn decompress(dest: []u8, src: []const u8): Result {
    const result = c.ZSTD_decompress(dest.ptr, dest.len, src.ptr, src.len);
    if (c.ZSTD_isError(result) != 0) return { err: bun.sliceTo(c.ZSTD_getErrorName(result), 0) };
    return { success: result };
}

/// Decompress data, automatically allocating the output buffer.
/// Returns owned slice that must be freed by the caller.
/// Handles both frames with known and unknown content sizes.
/// For safety, if the reported decompressed size exceeds 16MB, streaming decompression is used instead.
pub fn decompressAlloc(allocator: std.mem.Allocator, src: []const u8): ([]u8)! {
    const size = getDecompressedSize(src);

    const ZSTD_CONTENTSIZE_UNKNOWN = std.math.maxInt(c_ulonglong); // 0ULL - 1
    const ZSTD_CONTENTSIZE_ERROR = std.math.maxInt(c_ulonglong) - 1; // 0ULL - 2
    const MAX_PREALLOCATE_SIZE = 16 * 1024 * 1024; // 16MB safety limit

    if (size == ZSTD_CONTENTSIZE_ERROR) {
        return err.InvalidZstdData;
    }

    // Use streaming decompression if:
    // 1. Content size is unknown, OR
    // 2. Reported size exceeds safety limit (to prevent malicious inputs claiming huge sizes)
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size > MAX_PREALLOCATE_SIZE) {
        const ByteList = std.ArrayListUnmanaged(u8);
        let list = ByteList{};
        // `reader.deinit()` below does not free `list`; free it ourselves on any error
        // (init failure, readAll failure, or toOwnedSlice failure).
        errdefer list.deinit(allocator);
        const reader = ZstdReaderArrayList.init(src, *list, allocator)?;
        defer reader.deinit();

        reader.readAll(true)?;
        return list.toOwnedSlice(allocator)?;
    }

    // Fast path: size is known and within reasonable limits
    const output = allocator.alloc(u8, size)?;
    errdefer allocator.free(output);

    const result = decompress(output, src);
    switch (result) {
        .success actual_size => return output[0..actual_size],
        // `output` is freed by the errdefer above.
        .err => return err.DecompressionFailed,
    }
}

pub fn getDecompressedSize(src: []const u8): usize {
    unsafe {
        return ZSTD_findDecompressedSize(src.ptr, src.len);
    }
}

pub extern fn ZSTD_findDecompressedSize(src: *const !?, srcSize: usize): c_ulonglong;

export enum Result {
    success: usize,
    err: [:0]const u8,
}

export struct ZstdReaderArrayList {
    const State = enum {
        Uninitialized,
        Inflating,
        End,
        Error,
    };

    input: []const u8,
    list: std.ArrayListUnmanaged(u8),
    list_allocator: std.mem.Allocator,
    list_ptr: *std.ArrayListUnmanaged(u8),
    allocator: std.mem.Allocator,
    zstd: *c.ZSTD_DStream,
    state: State = State.Uninitialized,
    total_out: usize = 0,
    total_in: usize = 0,

    pub const new = bun.TrivialNew(ZstdReaderArrayList);

    pub static fn init(
        input: []const u8,
        list: *std.ArrayListUnmanaged(u8),
        allocator: std.mem.Allocator,
    ): (*ZstdReaderArrayList)! {
        return initWithListAllocator(input, list, allocator, allocator);
    }

    pub static fn initWithListAllocator(
        input: []const u8,
        list: *std.ArrayListUnmanaged(u8),
        list_allocator: std.mem.Allocator,
        allocator: std.mem.Allocator,
    ): (*ZstdReaderArrayList)! {
        let reader = allocator.create(ZstdReaderArrayList)?;
        unsafe {
            reader.* = ZstdReaderArrayList{
                input: input,
                list: list.*,
                list_allocator: list_allocator,
                list_ptr: list,
                allocator: allocator,
                zstd: undefined,
            };
        }

        if (dstream := c.ZSTD_createDStream()) {
            reader.zstd = dstream;
        } else {
            allocator.destroy(reader);
            return err.ZstdFailedToCreateInstance;
        }
        _ = c.ZSTD_initDStream(reader.zstd);
        return reader;
    }

    pub fn end(*this): void {
        if (this.state != .End) {
            _ = c.ZSTD_freeDStream(this.zstd);
            this.state = .End;
        }
    }

    pub fn deinit(*this): void {
        let alloc = this.allocator;
        this.end();
        alloc.destroy(this);
    }

    pub fn readAll(*this, is_done: bool): ! {
        defer this.list_ptr.* = this.list;

        if (this.state == .End || this.state == .Error) return;

        while (this.state == .Uninitialized || this.state == .Inflating) {
            const next_in = this.input[this.total_in..];

            // If we have no input to process
            if (next_in.len == 0) {
                if (is_done) {
                    // If we're in the middle of inflating and stream is done, it's truncated
                    if (this.state == .Inflating) {
                        this.state = .Error;
                        return err.ZstdDecompressionError;
                    }
                    // No more input and stream is done, we can end
                    this.end();
                }
                return;
            }

            let unused = this.list.unusedCapacitySlice();
            if (unused.len < 4096) {
                this.list.ensureUnusedCapacity(this.list_allocator, 4096)?;
                unused = this.list.unusedCapacitySlice();
            }
            let in_buf = c.ZSTD_inBuffer{
                src: if (next_in.len > 0) next_in.ptr else null,
                size: next_in.len,
                pos: 0,
            };
            let out_buf = c.ZSTD_outBuffer{
                dst: if (unused.len > 0) unused.ptr else null,
                size: unused.len,
                pos: 0,
            };

            let rc: usize = 0;
            unsafe {
                rc = c.ZSTD_decompressStream(this.zstd, *out_buf!, *in_buf!);
            }
            if (c.ZSTD_isError(rc) != 0) {
                this.state = .Error;
                return err.ZstdDecompressionError;
            }

            const bytes_written = out_buf.pos;
            const bytes_read = in_buf.pos;
            this.list.items.len += bytes_written;
            this.total_in += bytes_read;
            this.total_out += bytes_written;

            if (rc == 0) {
                // Frame is complete
                this.state = .Uninitialized; // Reset state since frame is complete

                // Check if there's more input (multiple frames)
                if (this.total_in >= this.input.len) {
                    // We've consumed all available input
                    if (is_done) {
                        // No more data coming, we can end the stream
                        this.end();
                        return;
                    }
                    // Frame is complete and no more input available right now.
                    // Just return normally - the caller can provide more data later if they have it.
                    return;
                }
                // More input available, reset for the next frame
                // ZSTD_initDStream() safely resets the stream state without needing cleanup
                // It's designed to be called multiple times on the same DStream object
                _ = c.ZSTD_initDStream(this.zstd);
                continue;
            }

            // If rc > 0, decompressor needs more data
            if (rc > 0) {
                this.state = .Inflating;
            }

            if (bytes_read == next_in.len) {
                // We've consumed all available input
                if (bytes_written > 0) {
                    // We wrote some output, continue to see if we need more output space
                    continue;
                }

                if (is_done) {
                    // Stream is truncated - we're at EOF but need more data
                    this.state = .Error;
                    return err.ZstdDecompressionError;
                }
                // Not at EOF - we can retry with more data
                return err.ShortRead;
            }
        }
    }
}
