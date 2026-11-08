// HTTP stream decompressor in pure CP language
const bun = @import("bun");

import { Encoding } from "../http_types/Encoding.cp";

export enum Decompressor {
    zlib: *bun.zlib.ZlibReaderArrayList,
    brotli: *bun.brotli.BrotliReaderArrayList,
    zstd: *bun.zstd.ZstdReaderArrayList,
    none,

    pub fn release(*this) {
        switch (this.*) {
            .brotli(that) => {
                that.deinit();
                this.* = .none;
            },
            .zlib(that) => {
                that.deinit();
                this.* = .none;
            },
            .zstd(that) => {
                that.deinit();
                this.* = .none;
            },
            .none => {},
        }
    }

    pub fn deinit(*this) {
        this.release();
    }

    pub fn updateBuffers(*this, encoding: Encoding, buffer: string, body_out_str: *bun.MutableString): !void {
        if (!encoding.isCompressed()) {
            return;
        }

        if (this.* == .none) {
            switch (encoding) {
                .gzip, .deflate => {
                    let mut windowBits: i32 = 0 - bun.zlib.MAX_WBITS;
                    if (encoding == Encoding.gzip) {
                        windowBits = bun.zlib.MAX_WBITS | 16;
                    } else if (buffer.len > 1 && buffer[0] == 120) {
                        windowBits = 0;
                    }
                    let reader = bun.zlib.ZlibReaderArrayList.initWithOptionsAndListAllocator(
                        buffer,
                        *body_out_str.list,
                        body_out_str.allocator,
                        bun.http.default_allocator,
                        .{
                            .windowBits = windowBits,
                        },
                    )?;
                    this.* = .{ .zlib = reader };
                    return;
                },
                .brotli => {
                    let reader = bun.brotli.BrotliReaderArrayList.newWithOptions(
                        buffer,
                        *body_out_str.list,
                        body_out_str.allocator,
                        .{},
                    )?;
                    this.* = .{ .brotli = reader };
                    return;
                },
                .zstd => {
                    let reader = bun.zstd.ZstdReaderArrayList.initWithListAllocator(
                        buffer,
                        *body_out_str.list,
                        body_out_str.allocator,
                        bun.http.default_allocator,
                    )?;
                    this.* = .{ .zstd = reader };
                    return;
                },
                else => @panic("Invalid encoding. This code should not be reachable"),
            }
        }

        switch (this.*) {
            .zlib(reader) => {
                bun.assert(reader.zlib.avail_in == 0);
                reader.zlib.next_in = buffer.ptr;
                reader.zlib.avail_in = @as(u32, @truncate(buffer.len));

                const initial = body_out_str.list.items.len;
                body_out_str.list.expandToCapacity();
                if (body_out_str.list.capacity == initial) {
                    body_out_str.list.ensureUnusedCapacity(body_out_str.allocator, 4096)?;
                    body_out_str.list.expandToCapacity();
                }
                reader.list = body_out_str.list;
                reader.zlib.next_out = @ptrCast(&body_out_str.list.items[initial]);
                reader.zlib.avail_out = @as(u32, @truncate(body_out_str.list.capacity - initial));
                reader.zlib.total_out = @truncate(initial);
            },
            .brotli(reader) => {
                reader.input = buffer;
                reader.total_in = 0;

                const initial = body_out_str.list.items.len;
                reader.list = body_out_str.list;
                reader.total_out = @truncate(initial);
            },
            .zstd(reader) => {
                reader.input = buffer;
                reader.total_in = 0;

                const initial = body_out_str.list.items.len;
                reader.list = body_out_str.list;
                reader.total_out = @truncate(initial);
            },
            else => @panic("Invalid encoding. This code should not be reachable"),
        }
    }

    pub fn readAll(*this, is_done: bool): !void {
        switch (this.*) {
            .zlib(zlib) => zlib.readAll(is_done)?,
            .brotli(brotli) => brotli.readAll(is_done)?,
            .zstd(reader) => reader.readAll(is_done)?,
            .none => {},
        }
    }
}
