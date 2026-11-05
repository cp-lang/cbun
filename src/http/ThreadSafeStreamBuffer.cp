// src/http/ThreadSafeStreamBuffer.cp
// Thread-safe stream buffer for HTTP thread in CP language

zig {
    const bun = @import("bun");

    pub const Callback = struct {
        callback: *const fn (*anyopaque) void,
        context: *anyopaque,

        pub fn init(comptime T: type, callback: *const fn (*T) void, context: *T) @This() {
            return .{ .callback = @ptrCast(callback), .context = @ptrCast(context) };
        }

        pub fn call(this: @This()) void {
            this.callback(this.context);
        }
    };

    const StreamBufferRefCount = bun.ptr.ThreadSafeRefCount(ThreadSafeStreamBuffer, "ref_count", ThreadSafeStreamBuffer.deinit, .{});
    pub const ref = StreamBufferRefCount.ref;
    pub const deref = StreamBufferRefCount.deref;
    pub const new = bun.TrivialNew(ThreadSafeStreamBuffer);
}

export @root @value struct ThreadSafeStreamBuffer {
    buffer: bun.io.StreamBuffer = {},
    mutex: bun.Mutex = {},
    ref_count: StreamBufferRefCount = StreamBufferRefCount.initExactRefs(2),
    callback: Callback? = null,

    pub fn acquire(*this): *bun.io.StreamBuffer {
        this.mutex.lock();
        return &this.buffer;
    }

    pub fn release(*this): void {
        this.mutex.unlock();
    }

    pub fn setDrainCallback(*this, comptime T: type, callback: anytype, context: *T): void {
        this.callback = Callback.init(T, callback, context);
    }

    pub fn clearDrainCallback(*this): void {
        this.callback = null;
    }

    pub fn reportDrain(*this): void {
        if (this.buffer.isEmpty()) {
            if (cb := this.callback) {
                cb.call();
            }
        }
    }

    pub fn deinit(*this): void {
        this.buffer.deinit();
        bun.destroy(this);
    }
}
