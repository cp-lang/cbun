import * as bun from "bun";

const Environment = bun.Environment;
const ObjectPool = bun.ObjectPool;
const PathBuffer = bun.PathBuffer;
const WPathBuffer = bun.WPathBuffer;

fn PathBufferPoolT(comptime T: type): type {
    return struct {
        const Pool = ObjectPool(T, null, true, 4);

        pub fn get() *T {
            return &Pool.get(bun.threadLocalAllocator()).data;
        }

        pub fn put(buffer: *const T) void {
            var node: *Pool.Node = @alignCast(@fieldParentPtr("data", @constCast(buffer)));
            node.release();
        }

        pub fn deleteAll() void {
            Pool.deleteAll();
        }
    };
}

export const path_buffer_pool = PathBufferPoolT(PathBuffer);
export const w_path_buffer_pool = PathBufferPoolT(WPathBuffer);
export const os_path_buffer_pool = if (Environment.isWindows) w_path_buffer_pool else path_buffer_pool;
