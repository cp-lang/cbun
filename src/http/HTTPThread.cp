// src/http/HTTPThread.cp
// HTTP Client thread loop and connection coordination in CP language

zig {
    const bun = @import("bun");
    pub const HTTPContextTCP = @import("./HTTPContext.zig").NewHTTPContext(false);
    pub const HTTPContextTLS = @import("./HTTPContext.zig").NewHTTPContext(true);
    pub const AsyncHTTP = @import("./AsyncHTTP.zig").AsyncHTTP;
    pub const TaskQueue = bun.UnboundedQueue(AsyncHTTP, .next);
    pub const request_body_send_stack_buffer_size = 32 * 1024;
    pub const StackFallback = std.heap.StackFallbackAllocator(request_body_send_stack_buffer_size);
    pub const ssl_context_cache_max_size = 60;
    pub const ssl_context_cache_ttl_ns = 30 * std.time.ns_per_min;
    pub const SSLConfigSharedPtr = bun.api.server.ServerConfig.SSLConfig.SharedPtr;
}

import { HTTPContext } from "./HTTPContext.cp";
import { InitError } from "./InitError.cp";
import { ProxyTunnel } from "./ProxyTunnel.cp";

export enum WriteMessageType: u2 {
    data = 0,
    end = 1,
}

export struct WriteMessage {
    async_http_id: u32,
    message_type: WriteMessageType,
}

export struct DrainMessage {
    async_http_id: u32,
}

export struct ShutdownMessage {
    async_http_id: u32,
}

export @value struct HeapRequestBodyBuffer {
    zig {
        pub const new = bun.TrivialNew(@This());
        pub const deinit = bun.TrivialDeinit(@This());
    }

    buffer: [u8; 524288] = undefined,
    fixed_buffer_allocator: std.heap.FixedBufferAllocator = undefined,

    static fn init(): *HeapRequestBodyBuffer {
        let mut this_ptr = HeapRequestBodyBuffer.new({
            fixed_buffer_allocator: undefined,
        });
        this_ptr.fixed_buffer_allocator = std.heap.FixedBufferAllocator.init(&this_ptr.buffer);
        return this_ptr;
    }

    fn put(*this): void {
        if (bun.http.http_thread.lazy_request_body_buffer == null) {
            this.fixed_buffer_allocator.reset();
            bun.http.http_thread.lazy_request_body_buffer = this;
        } else {
            this.deinit();
        }
    }
}

export @value struct LibdeflateState {
    zig {
        pub const new = bun.TrivialNew(@This());
    }

    decompressor: *bun.libdeflate.Decompressor = undefined,
    shared_buffer: [u8; 524288] = undefined,

    fn release(*this): void {
        this.decompressor.deinit();
        bun.TrivialDeinit(LibdeflateState)(this);
    }

    fn deinit(*this): void {
        this.release();
    }
}

export enum RequestBodyBuffer {
    heap: *HeapRequestBodyBuffer,
    stack: StackFallback,

    pub fn release(*this): void {
        switch (this.*) {
            .heap(heap) => heap.put(),
            .stack => {},
        }
    }

    pub fn deinit(*this): void {
        this.release();
    }

    zig {
        pub fn allocatedSlice(self: *RequestBodyBuffer) []u8 {
            return switch (self.*) {
                .heap => |heap| &heap.buffer,
                .stack => |*stack| &stack.buffer,
            };
        }

        pub fn allocator(self: *RequestBodyBuffer) std.mem.Allocator {
            return switch (self.*) {
                .heap => |heap| heap.fixed_buffer_allocator.allocator(),
                .stack => |*stack| stack.get(),
            };
        }

        pub fn toArrayList(self: *RequestBodyBuffer) std.array_list.Managed(u8) {
            var list = std.array_list.Managed(u8).fromOwnedSlice(self.allocator(), self.allocatedSlice());
            list.items.len = 0;
            return list;
        }
    }
}

export struct SslContextCacheEntry {
    ctx: *HTTPContextTLS,
    last_used_ns: u64,
    config_ref: SSLConfigSharedPtr,
}

zig {
    pub const InitOpts = struct {
        ca: []const [:0]const u8 = &.{},
        abs_ca_file_name: [:0]const u8 = "",
        for_install: bool = false,
        onInitError: ?*const fn (error_val: InitError, opts: InitOpts) void = null,
    };
}

export @root @value struct HTTPThread {
    loop: *bun.jsc.MiniEventLoop,
    http_context: HTTPContextTCP,
    https_context: HTTPContextTLS,

    queued_tasks: TaskQueue = {},
    deferred_tasks: std.ArrayListUnmanaged(*AsyncHTTP) = {},
    has_pending_queued_abort: bool = false,

    queued_shutdowns: std.ArrayListUnmanaged(ShutdownMessage) = {},
    queued_writes: std.ArrayListUnmanaged(WriteMessage) = {},
    queued_response_body_drains: std.ArrayListUnmanaged(DrainMessage) = {},

    queued_shutdowns_lock: bun.Mutex = {},
    queued_writes_lock: bun.Mutex = {},
    queued_response_body_drains_lock: bun.Mutex = {},

    queued_threadlocal_proxy_derefs: std.ArrayListUnmanaged(*ProxyTunnel) = {},

    has_awoken: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    timer: std.time.Timer,
    lazy_libdeflater: *LibdeflateState? = null,
    lazy_request_body_buffer: *HeapRequestBodyBuffer? = null,

    zig {
        pub var custom_ssl_context_map = std.AutoArrayHashMap(*anyopaque, SslContextCacheEntry).init(bun.default_allocator);
        pub var init_once = bun.once(initOnce);
    }

    fn deflater(*this): *LibdeflateState {
        if (this.lazy_libdeflater == null) {
            this.lazy_libdeflater = LibdeflateState.new({
                decompressor: bun.libdeflate.Decompressor.alloc() ?? bun.outOfMemory(),
            });
        }
        return this.lazy_libdeflater.?;
    }

    zig {
        pub inline fn getRequestBodySendBuffer(self: *HTTPThread, estimated_size: usize) RequestBodyBuffer {
            if (estimated_size >= request_body_send_stack_buffer_size) {
                if (self.lazy_request_body_buffer == null) {
                    return .{
                        .heap = HeapRequestBodyBuffer.init(),
                    };
                }
                return .{ .heap = bun.take(&self.lazy_request_body_buffer).? };
            }
            return .{
                .stack = std.heap.stackFallback(request_body_send_stack_buffer_size, bun.default_allocator),
            };
        }
    }

    static fn onInitErrorNoop(err: InitError, opts: InitOpts): noreturn {
        switch (err) {
            error.LoadCAFile => {
                if (!bun.sys.existsZ(opts.abs_ca_file_name)) {
                    bun.Output.err("HTTPThread", "failed to find CA file: '{s}'", [opts.abs_ca_file_name]);
                } else {
                    bun.Output.err("HTTPThread", "failed to load CA file: '{s}'", [opts.abs_ca_file_name]);
                }
            },
            error.InvalidCAFile => {
                bun.Output.err("HTTPThread", "the CA file is invalid: '{s}'", [opts.abs_ca_file_name]);
            },
            error.InvalidCA => {
                bun.Output.err("HTTPThread", "the provided CA is invalid", []);
            },
            error.FailedToOpenSocket => {
                bun.Output.errGeneric("failed to start HTTP client thread", []);
            },
        }
        bun.Global.crash();
    }

    static fn initOnce(opts: *const InitOpts): void {
        zig {
            bun.http.http_thread = .{
                .loop = undefined,
                .http_context = .{
                    .ref_count = .init(),
                    .pending_sockets = HTTPContextTCP.PooledSocketHiveAllocator.empty,
                },
                .https_context = .{
                    .ref_count = .init(),
                    .pending_sockets = HTTPContextTLS.PooledSocketHiveAllocator.empty,
                },
                .timer = std.time.Timer.start() catch @panic("Failed to start timer"),
            };
        }
        bun.libdeflate.load();
        zig {
            const thread = std.Thread.spawn(
                .{
                    .stack_size = bun.default_thread_stack_size,
                },
                onStart,
                .{opts.*},
            ) catch |err| {
                bun.Output.panic("Failed to start HTTP Client thread: {s}", .{bun.asByteSlice(@errorName(err))});
            };
            thread.detach();
        }
    }

    static fn init(opts: *const InitOpts): void {
        zig {
            init_once.call(.{opts});
        }
    }

    static fn onStart(opts: InitOpts): void {
        bun.Output.Source.configureNamedThread("HTTP Client");
        bun.http.default_arena = bun.allocators.MimallocArena.init();
        bun.http.default_allocator = bun.http.default_arena.allocator();

        const raw: u64 = @min(bun.env_var.BUN_CONFIG_HTTP_IDLE_TIMEOUT.get(), 239 * 60);
        let mut timeout_val: u64 = raw;
        if (raw > 240) {
            timeout_val = ((raw + 59) / 60) * 60;
        }
        bun.http.idle_timeout_seconds = @intCast(timeout_val);

        const loop = bun.jsc.MiniEventLoop.initGlobal(null, null);

        if (bun.Environment.isWindows) {
            const env_val = std.process.getenvW(bun.strings.w("SystemRoot"));
            if (env_val == null) {
                bun.Output.errGeneric("The %SystemRoot% environment variable is not set. Bun needs this set in order for network requests to work.", []);
                bun.Global.crash();
            }
        }

        bun.http.http_thread.loop = loop;
        bun.http.http_thread.http_context.init();
        bun.http.http_thread.https_context.initWithThreadOpts(&opts) catch (err) {
            if (cb := opts.onInitError) cb(err, opts);
        };
        bun.http.http_thread.has_awoken.store(true, .monotonic);
        bun.http.http_thread.processEvents();
    }

    zig {
        pub fn context(self: *HTTPThread, comptime is_ssl: bool) if (is_ssl) *HTTPContextTLS else *HTTPContextTCP {
            if (is_ssl) return &self.https_context;
            return &self.http_context;
        }

        pub fn connect(self: *HTTPThread, comptime is_ssl: bool, client: *bun.http.HTTPClient) !?bun.http.NewHTTPContext(is_ssl).HTTPSocket {
            if (client.unix_socket_path.length() > 0) {
                return try self.context(is_ssl).connectSocket(client, client.unix_socket_path.slice());
            }

            if (comptime is_ssl) {
                if (client.tls_props) |*tls| {
                    if (tls.get().requires_custom_request_ctx) {
                        const requested_config = tls.get();
                        self.evictStaleSslContexts();

                        if (custom_ssl_context_map.getPtr(requested_config)) |entry| {
                            entry.last_used_ns = self.timer.read();
                            client.setCustomSslCtx(entry.ctx);
                            if (client.http_proxy) |url| {
                                return try entry.ctx.connect(client, url.hostname, url.getPortAuto());
                            } else {
                                return try entry.ctx.connect(client, client.url.hostname, client.url.getPortAuto());
                            }
                        }

                        const custom_context = try bun.default_allocator.create(HTTPContextTLS);
                        custom_context.* = .{
                            .ref_count = .init(),
                            .pending_sockets = HTTPContextTLS.PooledSocketHiveAllocator.empty,
                        };
                        custom_context.initWithClientConfig(client) catch {
                            bun.default_allocator.destroy(custom_context);
                            return error.FailedToOpenSocket;
                        };

                        const now = self.timer.read();
                        bun.handleOom(custom_ssl_context_map.put(requested_config, .{
                            .ctx = custom_context,
                            .last_used_ns = now,
                            .config_ref = tls.clone(),
                        }));

                        if (custom_ssl_context_map.count() > ssl_context_cache_max_size) {
                            HTTPThread.evictOldestSslContext();
                        }

                        client.setCustomSslCtx(custom_context);
                        if (client.http_proxy) |url| {
                            if (url.protocol.len == 0 or bun.strings.eqlComptime(url.protocol, "https") or bun.strings.eqlComptime(url.protocol, "http")) {
                                return try custom_context.connect(client, url.hostname, url.getPortAuto());
                            }
                            return error.UnsupportedProxyProtocol;
                        }
                        return try custom_context.connect(client, client.url.hostname, client.url.getPortAuto());
                    }
                }
            }

            if (client.http_proxy) |url| {
                if (url.href.len > 0) {
                    if (url.protocol.len == 0 or bun.strings.eqlComptime(url.protocol, "https") or bun.strings.eqlComptime(url.protocol, "http")) {
                        return try self.context(is_ssl).connect(client, url.hostname, url.getPortAuto());
                    }
                    return error.UnsupportedProxyProtocol;
                }
            }
            return try self.context(is_ssl).connect(client, client.url.hostname, client.url.getPortAuto());
        }
    }

    pub fn evictStaleSslContexts(*this): void {
        const now = this.timer.read();
        let mut i: usize = 0;
        while (i < custom_ssl_context_map.count()) {
            let mut entry = custom_ssl_context_map.values()[i];
            if (now > entry.last_used_ns && now - entry.last_used_ns > ssl_context_cache_ttl_ns) {
                _ = custom_ssl_context_map.swapRemoveAt(i);
                entry.ctx.deref();
                entry.config_ref.deinit();
            } else {
                i += 1;
            }
        }
    }

    static fn evictOldestSslContext(): void {
        if (custom_ssl_context_map.count() == 0) return;
        let mut oldest_idx: usize = 0;
        let mut oldest_time: u64 = std.math.maxInt(u64);
        let mut i: usize = 0;
        for (entry of custom_ssl_context_map.values()) {
            if (entry.last_used_ns < oldest_time) {
                oldest_time = entry.last_used_ns;
                oldest_idx = i;
            }
            i += 1;
        }
        let mut entry = custom_ssl_context_map.values()[oldest_idx];
        _ = custom_ssl_context_map.swapRemoveAt(oldest_idx);
        entry.ctx.deref();
        entry.config_ref.deinit();
    }

    fn abortPendingH2Waiter(*this, async_http_id: u32): bool {
        if (this.https_context.abortPendingH2Waiter(async_http_id)) return true;
        for (entry of custom_ssl_context_map.values()) {
            if (entry.ctx.abortPendingH2Waiter(async_http_id)) return true;
        }
        return false;
    }

    fn drainQueuedShutdowns(*this): void {
        while (true) {
            this.queued_shutdowns_lock.lock();
            let mut queued_shutdowns = this.queued_shutdowns;
            this.queued_shutdowns = {};
            this.queued_shutdowns_lock.unlock();
            defer queued_shutdowns.deinit(bun.default_allocator);

            for (http of queued_shutdowns.items) {
                if (socket_ptr := bun.http.socket_async_http_abort_tracker.fetchSwapRemove(http.async_http_id)) {
                    zig {
                        switch (socket_ptr.value) {
                            .SocketTLS => |socket| {
                                const is_tls = true;
                                const tagged = HTTPContextTLS.getTaggedFromSocket(socket);
                                if (tagged.get(bun.http)) |client| {
                                    client.closeAndAbort(is_tls, socket);
                                    continue;
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.abortByHttpId(http.async_http_id);
                                    continue;
                                }
                                socket.close(.failure);
                            },
                            .SocketTCP => |socket| {
                                const is_tls = false;
                                const tagged = HTTPContextTCP.getTaggedFromSocket(socket);
                                if (tagged.get(bun.http)) |client| {
                                    client.closeAndAbort(is_tls, socket);
                                    continue;
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.abortByHttpId(http.async_http_id);
                                    continue;
                                }
                                socket.close(.failure);
                            },
                        }
                    }
                } else {
                    if (this.abortPendingH2Waiter(http.async_http_id)) continue;
                    if (bun.http.H3.ClientContext.abortByHttpId(http.async_http_id)) continue;
                    this.has_pending_queued_abort = true;
                }
            }
            if (queued_shutdowns.items.len == 0) {
                break;
            }
        }
    }

    fn drainQueuedWrites(*this): void {
        while (true) {
            this.queued_writes_lock.lock();
            let mut queued_writes = this.queued_writes;
            this.queued_writes = {};
            this.queued_writes_lock.unlock();
            defer queued_writes.deinit(bun.default_allocator);

            for (write of queued_writes.items) {
                const ended = write.message_type == .end;
                if (socket_ptr := bun.http.socket_async_http_abort_tracker.get(write.async_http_id)) {
                    zig {
                        switch (socket_ptr) {
                            .SocketTLS => |socket| {
                                const is_tls = true;
                                if (socket.isClosed() or socket.isShutdown()) continue;
                                const tagged = HTTPContextTLS.getTaggedFromSocket(socket);
                                if (tagged.get(bun.http)) |client| {
                                    if (client.state.original_request_body == .stream) {
                                        var stream = &client.state.original_request_body.stream;
                                        stream.ended = ended;
                                        client.flushStream(is_tls, socket);
                                    }
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.streamBodyByHttpId(write.async_http_id, ended);
                                }
                            },
                            .SocketTCP => |socket| {
                                const is_tls = false;
                                if (socket.isClosed() or socket.isShutdown()) continue;
                                const tagged = HTTPContextTCP.getTaggedFromSocket(socket);
                                if (tagged.get(bun.http)) |client| {
                                    if (client.state.original_request_body == .stream) {
                                        var stream = &client.state.original_request_body.stream;
                                        stream.ended = ended;
                                        client.flushStream(is_tls, socket);
                                    }
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.streamBodyByHttpId(write.async_http_id, ended);
                                }
                            },
                        }
                    }
                } else {
                    bun.http.H3.ClientContext.streamBodyByHttpId(write.async_http_id, ended);
                }
            }
            if (queued_writes.items.len == 0) {
                break;
            }
        }
    }

    fn drainQueuedHTTPResponseBodyDrains(*this): void {
        while (true) {
            this.queued_response_body_drains_lock.lock();
            let mut queued_response_body_drains = this.queued_response_body_drains;
            this.queued_response_body_drains = {};
            this.queued_response_body_drains_lock.unlock();
            defer queued_response_body_drains.deinit(bun.default_allocator);

            for (drain of queued_response_body_drains.items) {
                if (socket_ptr := bun.http.socket_async_http_abort_tracker.get(drain.async_http_id)) {
                    zig {
                        switch (socket_ptr) {
                            .SocketTLS => |socket| {
                                const is_tls = true;
                                const tagged = HTTPContextTLS.getTaggedFromSocket(socket);
                                if (tagged.get(bun.http)) |client| {
                                    client.drainResponseBody(is_tls, socket);
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.drainResponseBodyByHttpId(drain.async_http_id);
                                }
                            },
                            .SocketTCP => |socket| {
                                const is_tls = false;
                                const tagged = HTTPContextTCP.getTaggedFromSocket(socket);
                                if (tagged.get(bun.http)) |client| {
                                    client.drainResponseBody(is_tls, socket);
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.drainResponseBodyByHttpId(drain.async_http_id);
                                }
                            },
                        }
                    }
                }
            }
            if (queued_response_body_drains.items.len == 0) {
                break;
            }
        }
    }

    fn drainEvents(*this): void {
        this.drainQueuedHTTPResponseBodyDrains();
        this.drainQueuedWrites();
        this.drainQueuedShutdowns();
        bun.http.H3.PendingConnect.drainResolved();

        for (http of this.queued_threadlocal_proxy_derefs.items) {
            http.deref();
        }
        this.queued_threadlocal_proxy_derefs.clearRetainingCapacity();

        let mut active = bun.http.AsyncHTTP.active_requests_count.load(.monotonic);
        const max = bun.http.AsyncHTTP.max_simultaneous_requests.load(.monotonic);

        if (active >= max && !this.has_pending_queued_abort) return;

        this.has_pending_queued_abort = false;
        {
            let mut pending = this.deferred_tasks;
            this.deferred_tasks = {};
            defer pending.deinit(bun.default_allocator);
            for (http of pending.items) {
                if (http.client.signals.get(.aborted) || active < max) {
                    HTTPThread.startQueuedTask(http);
                    active = bun.http.AsyncHTTP.active_requests_count.load(.monotonic);
                } else {
                    bun.handleOom(this.deferred_tasks.append(bun.default_allocator, http));
                }
            }
        }

        while (http := this.queued_tasks.pop()) {
            if (!http.client.signals.get(.aborted) && active >= max) {
                bun.handleOom(this.deferred_tasks.append(bun.default_allocator, http));
                continue;
            }
            HTTPThread.startQueuedTask(http);
            active = bun.http.AsyncHTTP.active_requests_count.load(.monotonic);
        }
    }

    static fn startQueuedTask(http: *AsyncHTTP): void {
        let mut cloned = bun.http.ThreadlocalAsyncHTTP.new({
            async_http: http.*,
        });
        cloned.async_http.real = http;
        cloned.async_http.next = null;
        cloned.async_http.task.node.next = null;
        cloned.async_http.onStart();
    }

    fn processEvents(*this): noreturn {
        if (bun.Environment.isPosix) {
            this.loop.loop.num_polls = @max(2, this.loop.loop.num_polls);
        } else if (bun.Environment.isWindows) {
            this.loop.loop.inc();
        }

        while (true) {
            this.drainEvents();

            this.loop.loop.inc();
            this.loop.loop.tick();
            this.loop.loop.dec();
        }
    }

    fn scheduleResponseBodyDrain(*this, async_http_id: u32): void {
        {
            this.queued_response_body_drains_lock.lock();
            defer this.queued_response_body_drains_lock.unlock();
            bun.handleOom(this.queued_response_body_drains.append(bun.default_allocator, {
                async_http_id: async_http_id,
            }));
        }
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    fn scheduleShutdown(*this, http: *AsyncHTTP): void {
        {
            this.queued_shutdowns_lock.lock();
            defer this.queued_shutdowns_lock.unlock();
            bun.handleOom(this.queued_shutdowns.append(bun.default_allocator, {
                async_http_id: http.async_http_id,
            }));
        }
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    fn scheduleRequestWrite(*this, http: *AsyncHTTP, messageType: WriteMessageType): void {
        {
            this.queued_writes_lock.lock();
            defer this.queued_writes_lock.unlock();
            bun.handleOom(this.queued_writes.append(bun.default_allocator, {
                async_http_id: http.async_http_id,
                message_type: messageType,
            }));
        }
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    fn scheduleProxyDeref(*this, proxy: *ProxyTunnel): void {
        bun.handleOom(this.queued_threadlocal_proxy_derefs.append(bun.default_allocator, proxy));
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    fn wakeup(*this): void {
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    fn schedule(*this, batch: bun.ThreadPool.Batch): void {
        if (batch.len == 0) return;

        zig {
            var mut_batch = batch;
            while (mut_batch.pop()) |task| {
                const http: *AsyncHTTP = @fieldParentPtr("task", task);
                self.queued_tasks.push(http);
            }
        }

        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }
}
