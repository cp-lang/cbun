// src/http/HTTPThread.cp
// HTTP Client thread loop and connection coordination in CP language

zig {
    const std = @import("std");
    const bun = @import("bun");
    const Environment = bun.Environment;
    const Global = bun.Global;
    const Output = bun.Output;
    const jsc = bun.jsc;
    const strings = bun.strings;
    const Arena = bun.allocators.MimallocArena;
    const Batch = bun.ThreadPool.Batch;
    const UnboundedQueue = bun.threading.UnboundedQueue;
    const SSLConfig = bun.api.server.ServerConfig.SSLConfig;

    const HTTPClient = bun.http;
    const AsyncHTTP = bun.http.AsyncHTTP;
    const InitError = HTTPClient.InitError;
    const NewHTTPContext = bun.http.NewHTTPContext;
    const ProxyTunnel = @import("./ProxyTunnel.zig");

    const log = Output.scoped(.HTTPThread, .visible);
    const threadlog = Output.scoped(.HTTPThread, .hidden);
    const stringZ = [:0]const u8;

    pub const Queue = UnboundedQueue(AsyncHTTP, .next);
    const request_body_send_stack_buffer_size = 32 * 1024;

    pub const RequestBodyBuffer = union(enum) {
        heap: *HeapRequestBodyBuffer,
        stack: std.heap.StackFallbackAllocator(request_body_send_stack_buffer_size),

        pub fn deinit(this: *@This()) void {
            switch (this.*) {
                .heap => |heap| heap.put(),
                .stack => {},
            }
        }

        pub fn allocatedSlice(this: *@This()) []u8 {
            return switch (this.*) {
                .heap => |heap| &heap.buffer,
                .stack => |*stack| &stack.buffer,
            };
        }

        pub fn allocator(this: *@This()) std.mem.Allocator {
            return switch (this.*) {
                .heap => |heap| heap.fixed_buffer_allocator.allocator(),
                .stack => |*stack| stack.get(),
            };
        }

        pub fn toArrayList(this: *@This()) std.array_list.Managed(u8) {
            var arraylist = std.array_list.Managed(u8).fromOwnedSlice(this.allocator(), this.allocatedSlice());
            arraylist.items.len = 0;
            return arraylist;
        }
    };

    pub const SslContextCacheEntry = struct {
        ctx: *NewHTTPContext(true),
        last_used_ns: u64,
        config_ref: SSLConfig.SharedPtr,
    };

    const ssl_context_cache_max_size = 60;
    const ssl_context_cache_ttl_ns = 30 * std.time.ns_per_min;
    var custom_ssl_context_map = std.AutoArrayHashMap(*SSLConfig, SslContextCacheEntry).init(bun.default_allocator);
    var init_once = bun.once(initOnce);
}

export struct WriteMessage {
    pub async_http_id: u32,
    pub message_type: Type,

    zig {
        pub const Type = enum(u2) {
            data = 0,
            end = 1,
        };
    }
}

export struct DrainMessage {
    pub async_http_id: u32,
}

export struct ShutdownMessage {
    pub async_http_id: u32,
}

export @value struct HeapRequestBodyBuffer {
    pub buffer: [u8; 524288] = undefined,
    pub fixed_buffer_allocator: std.heap.FixedBufferAllocator = undefined,

    zig {
        pub const new = bun.TrivialNew(@This());
        pub const deinit = bun.TrivialDeinit(@This());
    }

    pub static fn init(): *HeapRequestBodyBuffer {
        let mut this_ptr = HeapRequestBodyBuffer.new({
            fixed_buffer_allocator: undefined,
        });
        this_ptr.fixed_buffer_allocator = std.heap.FixedBufferAllocator.init(&this_ptr.buffer);
        return this_ptr;
    }

    pub fn put(*this): void {
        if (bun.http.http_thread.lazy_request_body_buffer == null) {
            this.fixed_buffer_allocator.reset();
            bun.http.http_thread.lazy_request_body_buffer = this;
        } else {
            this.deinit();
        }
    }
}

export @value struct LibdeflateState {
    pub decompressor: *bun.libdeflate.Decompressor = undefined,
    pub shared_buffer: [u8; 524288] = undefined,

    zig {
        pub const new = bun.TrivialNew(@This());
    }

    pub fn deinit(*this): void {
        this.decompressor.deinit();
        bun.TrivialDeinit(LibdeflateState)(this);
    }
}

export @root @value struct HTTPThread {
    pub loop: *jsc.MiniEventLoop,
    pub http_context: NewHTTPContext(false),
    pub https_context: NewHTTPContext(true),

    pub queued_tasks: Queue = {},
    pub deferred_tasks: std.ArrayListUnmanaged(*AsyncHTTP) = {},
    pub has_pending_queued_abort: bool = false,

    pub queued_shutdowns: std.ArrayListUnmanaged(ShutdownMessage) = {},
    pub queued_writes: std.ArrayListUnmanaged(WriteMessage) = {},
    pub queued_response_body_drains: std.ArrayListUnmanaged(DrainMessage) = {},

    pub queued_shutdowns_lock: bun.Mutex = {},
    pub queued_writes_lock: bun.Mutex = {},
    pub queued_response_body_drains_lock: bun.Mutex = {},

    pub queued_threadlocal_proxy_derefs: std.ArrayListUnmanaged(*ProxyTunnel) = {},

    pub has_awoken: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    pub timer: std.time.Timer,
    pub lazy_libdeflater: *LibdeflateState? = null,
    pub lazy_request_body_buffer: *HeapRequestBodyBuffer? = null,

    pub fn deflater(*this): *LibdeflateState {
        if (this.lazy_libdeflater == null) {
            this.lazy_libdeflater = LibdeflateState.new({
                decompressor: bun.libdeflate.Decompressor.alloc() ?? bun.outOfMemory(),
            });
        }
        return this.lazy_libdeflater.?;
    }

    pub fn getRequestBodySendBuffer(*this, estimated_size: usize): RequestBodyBuffer {
        if (estimated_size >= request_body_send_stack_buffer_size) {
            if (this.lazy_request_body_buffer == null) {
                log("Allocating HeapRequestBodyBuffer due to {d} bytes request body", [estimated_size]);
                return {
                    heap: HeapRequestBodyBuffer.init(),
                };
            }
            return { heap: bun.take(&this.lazy_request_body_buffer).? };
        }
        return {
            stack: std.heap.stackFallback(request_body_send_stack_buffer_size, bun.default_allocator),
        };
    }

    pub static fn onInitErrorNoop(err: InitError, opts: InitOpts): noreturn {
        switch (err) {
            error.LoadCAFile => {
                if (!bun.sys.existsZ(opts.abs_ca_file_name)) {
                    Output.err("HTTPThread", "failed to find CA file: '{s}'", [opts.abs_ca_file_name]);
                } else {
                    Output.err("HTTPThread", "failed to load CA file: '{s}'", [opts.abs_ca_file_name]);
                }
            },
            error.InvalidCAFile => {
                Output.err("HTTPThread", "the CA file is invalid: '{s}'", [opts.abs_ca_file_name]);
            },
            error.InvalidCA => {
                Output.err("HTTPThread", "the provided CA is invalid", []);
            },
            error.FailedToOpenSocket => {
                Output.errGeneric("failed to start HTTP client thread", []);
            },
        }
        Global.crash();
    }

    zig {
        pub const InitOpts = struct {
            ca: []stringZ = &.{},
            abs_ca_file_name: stringZ = &.{},
            for_install: bool = false,
            onInitError: *const fn (err: InitError, opts: InitOpts) noreturn = &onInitErrorNoop,
        };

        pub fn initOnce(opts: *const InitOpts) void {
            bun.http.http_thread = .{
                .loop = undefined,
                .http_context = .{
                    .ref_count = .init(),
                    .pending_sockets = NewHTTPContext(false).PooledSocketHiveAllocator.empty,
                },
                .https_context = .{
                    .ref_count = .init(),
                    .pending_sockets = NewHTTPContext(true).PooledSocketHiveAllocator.empty,
                },
                .timer = std.time.Timer.start() catch unreachable,
            };
            bun.libdeflate.load();
            const thread = std.Thread.spawn(
                .{
                    .stack_size = bun.default_thread_stack_size,
                },
                onStart,
                .{opts.*},
            ) catch |err| Output.panic("Failed to start HTTP Client thread: {s}", .{@errorName(err)});
            thread.detach();
        }

        pub fn init(opts: *const InitOpts) void {
            init_once.call(.{opts});
        }

        pub fn onStart(opts: InitOpts) void {
            Output.Source.configureNamedThread("HTTP Client");
            bun.http.default_arena = Arena.init();
            bun.http.default_allocator = bun.http.default_arena.allocator();

            const raw: u64 = @min(bun.env_var.BUN_CONFIG_HTTP_IDLE_TIMEOUT.get(), 239 * 60);
            bun.http.idle_timeout_seconds = @intCast(if (raw > 240) ((raw + 59) / 60) * 60 else raw);

            const loop = bun.jsc.MiniEventLoop.initGlobal(null, null);

            if (Environment.isWindows) {
                _ = std.process.getenvW(comptime bun.strings.w("SystemRoot")) orelse {
                    bun.Output.errGeneric("The %SystemRoot% environment variable is not set. Bun needs this set in order for network requests to work.", .{});
                    Global.crash();
                };
            }

            bun.http.http_thread.loop = loop;
            bun.http.http_thread.http_context.init();
            bun.http.http_thread.https_context.initWithThreadOpts(&opts) catch |err| opts.onInitError(err, opts);
            bun.http.http_thread.has_awoken.store(true, .monotonic);
            bun.http.http_thread.processEvents();
        }

        pub fn context(this: *@This(), comptime is_ssl: bool) *NewHTTPContext(is_ssl) {
            return if (is_ssl) &this.https_context else &this.http_context;
        }

        pub fn connect(this: *@This(), client: *HTTPClient, comptime is_ssl: bool) !?NewHTTPContext(is_ssl).HTTPSocket {
            if (client.unix_socket_path.length() > 0) {
                return try this.context(is_ssl).connectSocket(client, client.unix_socket_path.slice());
            }

            if (comptime is_ssl) custom_ctx: {
                if (client.tls_props) |tls| {
                    if (!tls.get().requires_custom_request_ctx) break :custom_ctx;
                    const requested_config = tls.get();

                    evictStaleSslContexts(this);

                    if (custom_ssl_context_map.getPtr(requested_config)) |entry| {
                        entry.last_used_ns = this.timer.read();
                        client.setCustomSslCtx(entry.ctx);
                        if (client.http_proxy) |url| {
                            return try entry.ctx.connect(client, url.hostname, url.getPortAuto());
                        } else {
                            return try entry.ctx.connect(client, client.url.hostname, client.url.getPortAuto());
                        }
                    }

                    var custom_context = try bun.default_allocator.create(NewHTTPContext(is_ssl));
                    custom_context.* = .{
                        .ref_count = .init(),
                        .pending_sockets = NewHTTPContext(is_ssl).PooledSocketHiveAllocator.empty,
                    };
                    custom_context.initWithClientConfig(client) catch |err| {
                        bun.default_allocator.destroy(custom_context);
                        return switch (err) {
                            error.FailedToOpenSocket => |e| e,
                            error.InvalidCA => error.FailedToOpenSocket,
                            error.InvalidCAFile => error.FailedToOpenSocket,
                            error.LoadCAFile => error.FailedToOpenSocket,
                        };
                    };

                    const now = this.timer.read();
                    bun.handleOom(custom_ssl_context_map.put(requested_config, .{
                        .ctx = custom_context,
                        .last_used_ns = now,
                        .config_ref = tls.clone(),
                    }));

                    if (custom_ssl_context_map.count() > ssl_context_cache_max_size) {
                        evictOldestSslContext();
                    }

                    client.setCustomSslCtx(custom_context);
                    if (client.http_proxy) |url| {
                        if (url.protocol.len == 0 or strings.eqlComptime(url.protocol, "https") or strings.eqlComptime(url.protocol, "http")) {
                            return try custom_context.connect(client, url.hostname, url.getPortAuto());
                        }
                        return error.UnsupportedProxyProtocol;
                    }
                    return try custom_context.connect(client, client.url.hostname, client.url.getPortAuto());
                }
            }

            if (client.http_proxy) |url| {
                if (url.href.len > 0) {
                    if (url.protocol.len == 0 or strings.eqlComptime(url.protocol, "https") or strings.eqlComptime(url.protocol, "http")) {
                        return try this.context(is_ssl).connect(client, url.hostname, url.getPortAuto());
                    }
                    return error.UnsupportedProxyProtocol;
                }
            }
            return try this.context(is_ssl).connect(client, client.url.hostname, client.url.getPortAuto());
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

    pub static fn evictOldestSslContext(): void {
        if (custom_ssl_context_map.count() == 0) return;
        zig {
            var oldest_idx: usize = 0;
            var oldest_time: u64 = std.math.maxInt(u64);
            for (custom_ssl_context_map.values(), 0..) |entry, i| {
                if (entry.last_used_ns < oldest_time) {
                    oldest_time = entry.last_used_ns;
                    oldest_idx = i;
                }
            }
            var entry = custom_ssl_context_map.values()[oldest_idx];
            _ = custom_ssl_context_map.swapRemoveAt(oldest_idx);
            entry.ctx.deref();
            entry.config_ref.deinit();
        }
    }

    pub fn abortPendingH2Waiter(*this, async_http_id: u32): bool {
        if (this.https_context.abortPendingH2Waiter(async_http_id)) return true;
        for (entry of custom_ssl_context_map.values()) {
            if (entry.ctx.abortPendingH2Waiter(async_http_id)) return true;
        }
        return false;
    }

    pub fn drainQueuedShutdowns(*this): void {
        while (true) {
            this.queued_shutdowns_lock.lock();
            let mut queued_shutdowns = this.queued_shutdowns;
            this.queued_shutdowns = {};
            this.queued_shutdowns_lock.unlock();
            defer queued_shutdowns.deinit(bun.default_allocator);

            for (http of queued_shutdowns.items) {
                zig {
                    if (bun.http.socket_async_http_abort_tracker.fetchSwapRemove(http.async_http_id)) |socket_ptr| {
                        switch (socket_ptr.value) {
                            inline .SocketTLS, .SocketTCP => |socket, tag| {
                                const is_tls = tag == .SocketTLS;
                                const HTTPContext = NewHTTPContext(comptime is_tls);
                                const tagged = HTTPContext.getTaggedFromSocket(socket);
                                if (tagged.get(HTTPClient)) |client| {
                                    client.closeAndAbort(comptime is_tls, socket);
                                    continue;
                                }
                                if (tagged.get(bun.http.H2.ClientSession)) |session| {
                                    session.abortByHttpId(http.async_http_id);
                                    continue;
                                }
                                socket.close(.failure);
                            },
                        }
                    } else {
                        if (self.abortPendingH2Waiter(http.async_http_id)) continue;
                        if (bun.http.H3.ClientContext.abortByHttpId(http.async_http_id)) continue;
                        self.has_pending_queued_abort = true;
                    }
                }
            }
            if (queued_shutdowns.items.len == 0) {
                break;
            }
            threadlog("drained {d} queued shutdowns", [queued_shutdowns.items.len]);
        }
    }

    pub fn drainQueuedWrites(*this): void {
        while (true) {
            this.queued_writes_lock.lock();
            let mut queued_writes = this.queued_writes;
            this.queued_writes = {};
            this.queued_writes_lock.unlock();
            defer queued_writes.deinit(bun.default_allocator);

            for (write of queued_writes.items) {
                const ended = write.message_type == .end;
                zig {
                    if (bun.http.socket_async_http_abort_tracker.get(write.async_http_id)) |socket_ptr| {
                        switch (socket_ptr) {
                            inline .SocketTLS, .SocketTCP => |socket, tag| {
                                const is_tls = tag == .SocketTLS;
                                if (socket.isClosed() or socket.isShutdown()) {
                                    continue;
                                }
                                const tagged = NewHTTPContext(comptime is_tls).getTaggedFromSocket(socket);
                                if (tagged.get(HTTPClient)) |client| {
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
                    } else {
                        bun.http.H3.ClientContext.streamBodyByHttpId(write.async_http_id, ended);
                    }
                }
            }
            if (queued_writes.items.len == 0) {
                break;
            }
            threadlog("drained {d} queued writes", [queued_writes.items.len]);
        }
    }

    pub fn drainQueuedHTTPResponseBodyDrains(*this): void {
        while (true) {
            this.queued_response_body_drains_lock.lock();
            let mut queued_response_body_drains = this.queued_response_body_drains;
            this.queued_response_body_drains = {};
            this.queued_response_body_drains_lock.unlock();
            defer queued_response_body_drains.deinit(bun.default_allocator);

            for (drain of queued_response_body_drains.items) {
                zig {
                    if (bun.http.socket_async_http_abort_tracker.get(drain.async_http_id)) |socket_ptr| {
                        switch (socket_ptr) {
                            inline .SocketTLS, .SocketTCP => |socket, tag| {
                                const is_tls = tag == .SocketTLS;
                                const HTTPContext = NewHTTPContext(comptime is_tls);
                                const tagged = HTTPContext.getTaggedFromSocket(socket);
                                if (tagged.get(HTTPClient)) |client| {
                                    client.drainResponseBody(comptime is_tls, socket);
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
            threadlog("drained {d} queued drains", [queued_response_body_drains.items.len]);
        }
    }

    pub fn drainEvents(*this): void {
        this.drainQueuedHTTPResponseBodyDrains();
        this.drainQueuedWrites();
        this.drainQueuedShutdowns();
        bun.http.H3.PendingConnect.drainResolved();

        for (http of this.queued_threadlocal_proxy_derefs.items) {
            http.deref();
        }
        this.queued_threadlocal_proxy_derefs.clearRetainingCapacity();

        let mut count: usize = 0;
        let mut active = AsyncHTTP.active_requests_count.load(.monotonic);
        const max = AsyncHTTP.max_simultaneous_requests.load(.monotonic);
        defer {
            if (Environment.allow_assert) {
                if (count > 0) {
                    log("Processed {d} tasks\n", [count]);
                }
            }
        }

        if (active >= max && !this.has_pending_queued_abort) return;

        this.has_pending_queued_abort = false;
        {
            let mut pending = this.deferred_tasks;
            this.deferred_tasks = {};
            defer pending.deinit(bun.default_allocator);
            for (http of pending.items) {
                if (http.client.signals.get(.aborted) || active < max) {
                    HTTPThread.startQueuedTask(http);
                    if (Environment.allow_assert) count += 1;
                    active = AsyncHTTP.active_requests_count.load(.monotonic);
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
            if (Environment.allow_assert) count += 1;
            active = AsyncHTTP.active_requests_count.load(.monotonic);
        }
    }

    pub static fn startQueuedTask(http: *AsyncHTTP): void {
        let mut cloned = bun.http.ThreadlocalAsyncHTTP.new({
            async_http: http.*,
        });
        cloned.async_http.real = http;
        cloned.async_http.next = null;
        cloned.async_http.task.node.next = null;
        cloned.async_http.onStart();
    }

    pub fn processEvents(*this): noreturn {
        if (Environment.isPosix) {
            this.loop.loop.num_polls = @max(2, this.loop.loop.num_polls);
        } else if (Environment.isWindows) {
            this.loop.loop.inc();
        } else {
            @compileError("TODO:");
        }

        while (true) {
            this.drainEvents();
            zig {
                if (comptime Environment.isDebug and bun.asan.enabled) {
                    for (bun.http.socket_async_http_abort_tracker.keys(), bun.http.socket_async_http_abort_tracker.values()) |http_id, socket| {
                        if (socket.socket().get()) |usocket| {
                            _ = http_id;
                            bun.asan.assertUnpoisoned(usocket);
                        }
                    }
                }
            }

            let mut start_time: i128 = 0;
            if (Environment.isDebug) {
                start_time = std.time.nanoTimestamp();
            }
            Output.flush();

            this.loop.loop.inc();
            this.loop.loop.tick();
            this.loop.loop.dec();

            zig {
                if (comptime Environment.isDebug and bun.asan.enabled) {
                    for (bun.http.socket_async_http_abort_tracker.keys(), bun.http.socket_async_http_abort_tracker.values()) |http_id, socket| {
                        if (socket.socket().get()) |usocket| {
                            _ = http_id;
                            bun.asan.assertUnpoisoned(usocket);
                        }
                    }
                }
            }

            if (Environment.isDebug) {
                const end = std.time.nanoTimestamp();
                threadlog("Waited {D}\n", [@as(i64, @truncate(end - start_time))]);
                Output.flush();
            }
        }
    }

    pub fn scheduleResponseBodyDrain(*this, async_http_id: u32): void {
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

    pub fn scheduleShutdown(*this, http: *AsyncHTTP): void {
        threadlog("scheduleShutdown {d}", [http.async_http_id]);
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

    pub fn scheduleRequestWrite(*this, http: *AsyncHTTP, messageType: WriteMessage.Type): void {
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

    pub fn scheduleProxyDeref(*this, proxy: *ProxyTunnel): void {
        bun.handleOom(this.queued_threadlocal_proxy_derefs.append(bun.default_allocator, proxy));
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    pub fn wakeup(*this): void {
        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }

    pub fn schedule(*this, batch: Batch): void {
        if (batch.len == 0) return;

        {
            let mut batch_ = batch;
            while (task := batch_.pop()) {
                zig {
                    const http: *AsyncHTTP = @fieldParentPtr("task", task);
                    self.queued_tasks.push(http);
                }
            }
        }

        if (this.has_awoken.load(.monotonic)) {
            this.loop.loop.wakeup();
        }
    }
}
