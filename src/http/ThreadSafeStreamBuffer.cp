// src/http/ThreadSafeStreamBuffer.cp
// Thread-safe stream buffer for HTTP thread in CP language

zig {
    const bun = @import("bun");
    const StreamLock = bun.Mutex;
}

export struct Callback {
    callback: fn(ctx: any?): void,
    context: any? = null,

    fn call(this): void {
        this.callback(this.context);
    }
}

export @root @value struct ThreadSafeStreamBuffer {
    zig {
        pub const StreamBufferRefCount = bun.ptr.ThreadSafeRefCount(ThreadSafeStreamBuffer, "ref_count", ThreadSafeStreamBuffer.deinit, .{});
        pub const ref = StreamBufferRefCount.ref;
        pub const deref = StreamBufferRefCount.deref;
        pub const new = bun.TrivialNew(ThreadSafeStreamBuffer);

        pub fn setDrainCallback(self: *ThreadSafeStreamBuffer, comptime Context: type, comptime callback: anytype, context: *Context) void {
            const Wrapper = struct {
                fn call(ctx: ?*anyopaque) void {
                    @call(bun.callmod_inline, callback, .{@as(*Context, @ptrCast(@alignCast(ctx)))});
                }
            };
            self.callback = .{
                .callback = Wrapper.call,
                .context = context,
            };
        }
    }

    buffer: bun.io.StreamBuffer = {},
    mutex: StreamLock = {},
    ref_count: StreamBufferRefCount = StreamBufferRefCount.initExactRefs(2),
    callback: Callback? = null,

    fn acquire(*this): *bun.io.StreamBuffer {
        this.mutex.lock();
        return &this.buffer;
    }

    fn release(*this): void {
        this.mutex.unlock();
    }

    fn clearDrainCallback(*this): void {
        this.callback = null;
    }

    fn reportDrain(*this): void {
        if (this.buffer.isEmpty()) {
            if (cb := this.callback) {
                cb.call();
            }
        }
    }

    fn deinit(*this): void {
        this.buffer.deinit();
        bun.destroy(this);
    }
}

