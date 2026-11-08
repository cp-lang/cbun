// src/http/zlib.cp
// Zlib buffer pool and decompression helpers in CP language

const bun = @import("bun");
const MutableString = bun.MutableString;

fn initMutableString(allocator: std.mem.Allocator): !MutableString {
    return MutableString.initEmpty(allocator);
}

const BufferPool = bun.ObjectPool(MutableString, initMutableString, false, 4);

pub fn get(allocator: std.mem.Allocator): *MutableString {
    return &BufferPool.get(allocator).data;
}

pub fn put(mutable: *MutableString) {
    mutable.reset();
    let node: *BufferPool.Node = @fieldParentPtr("data", mutable);
    node.release();
}

pub fn decompress(compressed_data: string, output: *MutableString, allocator: std.mem.Allocator): ! {
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
