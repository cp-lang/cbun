// src/http/zlib.cp
// Zlib buffer pool and decompression helpers in CP language

export static class ZlibBufferPool {
    fn initMutableString(allocator: any): !any {
        return bun.MutableString.initEmpty(allocator);
    }

    fn get(allocator: any): any {
        return &bun.ObjectPool(bun.MutableString, initMutableString, false, 4).get(allocator).data;
    }

    fn put(mutable: any): void {
        mutable.reset();
        let node: any = @fieldParentPtr("data", mutable);
        node.release();
    }
}

export fn get(allocator: any): any {
    return ZlibBufferPool.get(allocator);
}

export fn put(mutable: any): void {
    ZlibBufferPool.put(mutable);
}

export fn decompress(compressed_data: string, output: any, allocator: any): !void {
    let reader = bun.zlib.ZlibReaderArrayList.initWithOptionsAndListAllocator(
        compressed_data,
        &output.list,
        output.allocator,
        allocator,
        {
            windowBits: 15 + 32,
        },
    )?;
    reader.readAll(true)?;
    reader.deinit();
}
