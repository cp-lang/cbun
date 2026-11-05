// src/http/AsyncHTTP.cp
// Fast asynchronous HTTP task orchestration in CP language

zig {
    const std = @import("std");

    const bun = @import("bun");
    const Environment = bun.Environment;
    const FeatureFlags = bun.FeatureFlags;
    const MutableString = bun.MutableString;
    const assert = bun.assert;
    const jsc = bun.jsc;
    const picohttp = bun.picohttp;
    const Channel = bun.threading.Channel;
    const SSLConfig = bun.api.server.ServerConfig.SSLConfig;

    const ThreadPool = bun.ThreadPool;
    const Batch = bun.ThreadPool.Batch;
    const Task = ThreadPool.Task;

    const HTTPClient = bun.http;
    const FetchRedirect = HTTPClient.FetchRedirect;
    const HTTPClientResult = HTTPClient.HTTPClientResult;
    const HTTPRequestBody = HTTPClient.HTTPRequestBody;
    const HTTPVerboseLevel = HTTPClient.HTTPVerboseLevel;
    const Method = HTTPClient.Method;
    const Signals = HTTPClient.Signals;

    const Loc = bun.logger.Loc;
    const Log = bun.logger.Log;

    const DotEnv = @import("../dotenv/env_loader.zig");
    const HTTPThread = @import("./HTTPThread.zig");
    const Headers = @import("./Headers.zig");
    const Encoding = @import("../http_types/Encoding.zig").Encoding;

    const PercentEncoding = @import("../url/url.zig").PercentEncoding;
    const URL = @import("../url/url.zig").URL;

    const log = bun.Output.scoped(.AsyncHTTP, .visible);

    pub const State = enum(u32) {
        pending = 0,
        scheduled = 1,
        sending = 2,
        success = 3,
        fail = 4,
    };
    pub const AtomicState = std.atomic.Value(State);

    pub const Options = struct {
        http_proxy: ?URL = null,
        proxy_headers: ?Headers = null,
        hostname: ?[]u8 = null,
        signals: ?Signals = null,
        unix_socket_path: ?jsc.ZigString.Slice = null,
        disable_timeout: ?bool = null,
        verbose: ?HTTPVerboseLevel = null,
        disable_keepalive: ?bool = null,
        disable_decompression: ?bool = null,
        reject_unauthorized: ?bool = null,
        tls_props: ?SSLConfig.SharedPtr = null,
    };

    const Preconnect = struct {
        async_http: AsyncHTTP,
        response_buffer: MutableString,
        url: bun.URL,
        is_url_owned: bool,

        pub const new = bun.TrivialNew(@This());

        pub fn onResult(this: *Preconnect, _: *AsyncHTTP, _: HTTPClientResult) void {
            this.response_buffer.deinit();
            this.async_http.clearData();
            this.async_http.client.deinit();
            if (this.is_url_owned) {
                bun.default_allocator.free(this.url.href);
            }

            bun.destroy(this);
        }
    };

    const HTTPCallbackPair = .{ *AsyncHTTP, HTTPClientResult };
    pub const HTTPChannel = Channel(HTTPCallbackPair, .{ .Static = 1000 });

    const SingleHTTPChannel = struct {
        const SingleHTTPCHannel_ = Channel(HTTPClientResult, .{ .Static = 8 });
        channel: SingleHTTPCHannel_,
        pub fn reset(_: *@This()) void {}
        pub fn init() SingleHTTPChannel {
            return SingleHTTPChannel{ .channel = SingleHTTPCHannel_.init() };
        }
    };

    fn sendSyncCallback(this: *SingleHTTPChannel, async_http: *AsyncHTTP, result: HTTPClientResult) void {
        async_http.real.?.* = async_http.*;
        async_http.real.?.response_buffer = async_http.response_buffer;
        this.channel.writeItem(result) catch unreachable;
    }

    pub const HTTPChannelContext = struct {
        http: AsyncHTTP = undefined,
        channel: *HTTPChannel,

        pub fn callback(data: HTTPCallbackPair) void {
            var this: *HTTPChannelContext = @fieldParentPtr("http", data.@"0");
            this.channel.writeItem(data) catch unreachable;
        }
    };
}

export @root @value struct AsyncHTTP {
    pub request: picohttp.Request? = null,
    pub response: picohttp.Response? = null,
    pub request_headers: Headers.Entry.List = Headers.Entry.List.empty,
    pub response_headers: Headers.Entry.List = Headers.Entry.List.empty,
    pub response_buffer: *MutableString,
    pub request_body: HTTPRequestBody = { bytes: "" },
    pub allocator: std.mem.Allocator,
    pub request_header_buf: string = "",
    pub method: Method = Method.GET,
    pub url: URL,
    pub http_proxy: URL? = null,
    pub real: *AsyncHTTP? = null,
    pub next: *AsyncHTTP? = null,

    pub task: Task = Task{ callback: &startAsyncHTTP },
    pub state: AtomicState = AtomicState.init(State.pending),

    pub result_callback: HTTPClientResult.Callback = undefined,

    pub redirected: bool = false,

    pub response_encoding: Encoding = Encoding.identity,
    pub verbose: HTTPVerboseLevel = .none,

    pub client: HTTPClient = undefined,
    pub waitingDeffered: bool = false,
    pub finalized: bool = false,
    pub err: anyerror? = null,
    pub async_http_id: u32 = 0,

    pub elapsed: u64 = 0,
    pub gzip_elapsed: u64 = 0,

    pub signals: Signals = {},

    zig {
        pub var active_requests_count = std.atomic.Value(usize).init(0);
        pub var max_simultaneous_requests = std.atomic.Value(usize).init(256);
    }

    pub static fn loadEnv(allocator: std.mem.Allocator, logger: *Log, env: *DotEnv.Loader): void {
        const val_opt = env.get("BUN_CONFIG_MAX_HTTP_REQUESTS");
        if (max_http_requests := val_opt) {
            let mut max: u16 = 0;
            zig {
                max = std.fmt.parseInt(u16, max_http_requests, 10) catch {
                    logger.addErrorFmt(
                        null,
                        Loc.Empty,
                        allocator,
                        "BUN_CONFIG_MAX_HTTP_REQUESTS value \"{s}\" is not a valid integer between 1 and 65535",
                        .{max_http_requests},
                    ) catch unreachable;
                    return;
                };
            }
            if (max == 0) {
                zig {
                    logger.addWarningFmt(
                        null,
                        Loc.Empty,
                        allocator,
                        "BUN_CONFIG_MAX_HTTP_REQUESTS value must be a number between 1 and 65535",
                        .{},
                    ) catch unreachable;
                }
                return;
            }
            AsyncHTTP.max_simultaneous_requests.store(max, .monotonic);
        }
    }

    pub fn signalHeaderProgress(*this): void {
        zig {
            var progress = self.signals.header_progress orelse return;
            progress.store(true, .release);
        }
    }

    pub fn enableResponseBodyStreaming(*this): void {
        zig {
            var stream = self.signals.response_body_streaming orelse return;
            stream.store(true, .release);
        }
    }

    pub fn clearData(*this): void {
        this.response_headers.deinit(this.allocator);
        this.response_headers = Headers.Entry.List.empty;
        this.request = null;
        this.response = null;
        this.client.unix_socket_path.deinit();
        this.client.unix_socket_path = jsc.ZigString.Slice.empty;
    }

    pub static fn preconnect(url: URL, is_url_owned: bool): void {
        zig {
            if (!FeatureFlags.is_fetch_preconnect_supported) {
                if (is_url_owned) {
                    bun.default_allocator.free(url.href);
                }
                return;
            }

            var preconnect_ptr = Preconnect.new(.{
                .async_http = undefined,
                .response_buffer = MutableString{ .allocator = bun.http.default_allocator, .list = .{} },
                .url = url,
                .is_url_owned = is_url_owned,
            });

            preconnect_ptr.async_http = AsyncHTTP.init(bun.default_allocator, .GET, url, .{}, "", &preconnect_ptr.response_buffer, "", HTTPClientResult.Callback.New(*Preconnect, Preconnect.onResult).init(preconnect_ptr), .manual, .{});
            preconnect_ptr.async_http.client.flags.is_preconnect_only = true;

            bun.http.http_thread.schedule(Batch.from(&preconnect_ptr.async_http.task));
        }
    }

    pub static fn init(
        allocator: std.mem.Allocator,
        method: Method,
        url: URL,
        headers: Headers.Entry.List,
        headers_buf: string,
        response_buffer: *MutableString,
        request_body: string,
        callback: HTTPClientResult.Callback,
        redirect_type: FetchRedirect,
        options: Options,
    ): AsyncHTTP {
        zig {
            var this = AsyncHTTP{
                .allocator = allocator,
                .url = url,
                .method = method,
                .request_headers = headers,
                .request_header_buf = headers_buf,
                .request_body = .{ .bytes = request_body },
                .response_buffer = response_buffer,
                .result_callback = callback,
                .http_proxy = options.http_proxy,
                .signals = options.signals orelse .{},
                .async_http_id = if (options.signals != null and options.signals.?.aborted != null) bun.http.async_http_id_monotonic.fetchAdd(1, .monotonic) else 0,
            };

            this.client = .{
                .allocator = allocator,
                .method = method,
                .url = url,
                .header_entries = headers,
                .header_buf = headers_buf,
                .hostname = options.hostname,
                .signals = options.signals orelse this.signals,
                .async_http_id = this.async_http_id,
                .http_proxy = this.http_proxy,
                .proxy_headers = options.proxy_headers,
                .redirect_type = redirect_type,
            };
            if (options.unix_socket_path) |val| {
                assert(this.client.unix_socket_path.length() == 0);
                this.client.unix_socket_path = val;
            }
            if (options.disable_timeout) |val| {
                this.client.flags.disable_timeout = val;
            }
            if (options.verbose) |val| {
                this.client.verbose = val;
            }
            if (options.disable_decompression) |val| {
                this.client.flags.disable_decompression = val;
            }
            if (options.disable_keepalive) |val| {
                this.client.flags.disable_keepalive = val;
            }
            if (options.reject_unauthorized) |val| {
                this.client.flags.reject_unauthorized = val;
            }
            if (options.tls_props) |val| {
                this.client.tls_props = val;
            }

            if (options.http_proxy) |proxy| {
                if (proxy.username.len > 0) {
                    var username_sfb = std.heap.stackFallback(4096, allocator);
                    const username_alloc = username_sfb.get();
                    const username = PercentEncoding.decodeAlloc(username_alloc, proxy.username) catch |err| {
                        log("failed to decode proxy username: {}", .{err});
                        return this;
                    };
                    defer username_alloc.free(username);

                    if (proxy.password.len > 0) {
                        var password_sfb = std.heap.stackFallback(4096, allocator);
                        const password_alloc = password_sfb.get();
                        const password = PercentEncoding.decodeAlloc(password_alloc, proxy.password) catch |err| {
                            log("failed to decode proxy password: {}", .{err});
                            return this;
                        };
                        defer password_alloc.free(password);

                        const auth = std.fmt.allocPrint(allocator, "{s}:{s}", .{ username, password }) catch unreachable;
                        defer allocator.free(auth);
                        const size = std.base64.standard.Encoder.calcSize(auth.len);
                        var buf = this.allocator.alloc(u8, size + "Basic ".len) catch unreachable;
                        const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], auth);
                        buf[0.."Basic ".len].* = "Basic ".*;
                        this.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                    } else {
                        const size = std.base64.standard.Encoder.calcSize(username.len);
                        var buf = allocator.alloc(u8, size + "Basic ".len) catch unreachable;
                        const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], username);
                        buf[0.."Basic ".len].* = "Basic ".*;
                        this.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                    }
                }
            }
            return this;
        }
    }

    pub static fn initSync(
        allocator: std.mem.Allocator,
        method: Method,
        url: URL,
        headers: Headers.Entry.List,
        headers_buf: string,
        response_buffer: *MutableString,
        request_body: string,
        http_proxy: URL?,
        hostname: &mut u8[]?,
        redirect_type: FetchRedirect,
    ): AsyncHTTP {
        let mut opts = Options {};
        opts.http_proxy = http_proxy;
        opts.hostname = hostname;
        return AsyncHTTP.init(
            allocator,
            method,
            url,
            headers,
            headers_buf,
            response_buffer,
            request_body,
            undefined,
            redirect_type,
            opts,
        );
    }

    pub fn reset(*this): void! {
        zig {
            const aborted = self.client.aborted;
            self.client = try HTTPClient.init(self.allocator, self.method, self.client.url, self.client.header_entries, self.client.header_buf, aborted);
            self.client.http_proxy = self.http_proxy;

            if (self.http_proxy) |proxy| {
                self.client.flags.disable_keepalive = self.url.isHTTPS();
                if (proxy.username.len > 0) {
                    var username_sfb = std.heap.stackFallback(4096, self.allocator);
                    const username_alloc = username_sfb.get();
                    const username = PercentEncoding.decodeAlloc(username_alloc, proxy.username) catch |err| {
                        log("failed to decode proxy username: {}", .{err});
                        return;
                    };
                    defer username_alloc.free(username);

                    if (proxy.password.len > 0) {
                        var password_sfb = std.heap.stackFallback(4096, self.allocator);
                        const password_alloc = password_sfb.get();
                        const password = PercentEncoding.decodeAlloc(password_alloc, proxy.password) catch |err| {
                            log("failed to decode proxy password: {}", .{err});
                            return;
                        };
                        defer password_alloc.free(password);

                        const auth = std.fmt.allocPrint(self.allocator, "{s}:{s}", .{ username, password }) catch unreachable;
                        defer self.allocator.free(auth);
                        const size = std.base64.standard.Encoder.calcSize(auth.len);
                        var buf = self.allocator.alloc(u8, size + "Basic ".len) catch unreachable;
                        const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], auth);
                        buf[0.."Basic ".len].* = "Basic ".*;
                        self.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                    } else {
                        const size = std.base64.standard.Encoder.calcSize(username.len);
                        var buf = self.allocator.alloc(u8, size + "Basic ".len) catch unreachable;
                        const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], username);
                        buf[0.."Basic ".len].* = "Basic ".*;
                        self.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                    }
                }
            }
        }
    }

    pub fn schedule(*this, _allocator: std.mem.Allocator, batch: *ThreadPool.Batch): void {
        zig {
            _ = _allocator;
        }
        this.state.store(.scheduled, .monotonic);
        zig {
            batch.push(ThreadPool.Batch.from(&self.task));
        }
    }

    pub fn sendSync(*this): picohttp.Response! {
        zig {
            HTTPThread.init(&.{});

            var ctx = try bun.default_allocator.create(SingleHTTPChannel);
            ctx.* = SingleHTTPChannel.init();
            self.result_callback = HTTPClientResult.Callback.New(
                *SingleHTTPChannel,
                sendSyncCallback,
            ).init(ctx);

            var batch = bun.ThreadPool.Batch{};
            self.schedule(bun.default_allocator, &batch);
            bun.http.http_thread.schedule(batch);

            const result = ctx.channel.readItem() catch unreachable;
            if (result.fail) |err| {
                return err;
            }
            assert(result.metadata != null);
            return result.metadata.?.response;
        }
    }

    pub fn onAsyncHTTPCallback(*this, async_http: *AsyncHTTP, result: HTTPClientResult): void {
        bun.assert(this.real != null);

        let mut callback = this.result_callback;
        zig {
            self.elapsed = bun.http.http_thread.timer.read() -| self.elapsed;
        }

        this.redirected = this.client.flags.redirected;
        if (result.isSuccess()) {
            this.err = null;
            if (metadata := result.metadata) {
                this.response = metadata.response;
            }
            this.state.store(.success, .monotonic);
        } else {
            this.err = result.fail;
            this.response = null;
            this.state.store(State.fail, .monotonic);
        }

        zig {
            if (comptime Environment.enable_logs) {
                if (bun.http.socket_async_http_abort_tracker.count() > 0) {
                    log("bun.http.socket_async_http_abort_tracker count: {d}", .{bun.http.socket_async_http_abort_tracker.count()});
                }
            }

            if (bun.http.socket_async_http_abort_tracker.capacity() > 10_000 and bun.http.socket_async_http_abort_tracker.count() < 100) {
                bun.http.socket_async_http_abort_tracker.shrinkAndFree(bun.http.socket_async_http_abort_tracker.count());
            }

            if (result.has_more) {
                callback.function(callback.ctx, async_http, result);
            } else {
                {
                    self.client.deinit();
                    var threadlocal_http: *bun.http.ThreadlocalAsyncHTTP = @fieldParentPtr("async_http", async_http);
                    defer threadlocal_http.deinit();
                    log("onAsyncHTTPCallback: {D}", .{self.elapsed});
                    callback.function(callback.ctx, async_http, result);
                }

                const active_requests = AsyncHTTP.active_requests_count.fetchSub(1, .monotonic);
                assert(active_requests > 0);
            }

            if ((!bun.http.http_thread.queued_tasks.isEmpty() or bun.http.http_thread.deferred_tasks.items.len > 0) and AsyncHTTP.active_requests_count.load(.monotonic) < AsyncHTTP.max_simultaneous_requests.load(.monotonic)) {
                bun.http.http_thread.loop.loop.wakeup();
            }
        }
    }

    pub static fn startAsyncHTTP(task: *Task): void {
        zig {
            var this_ptr: *AsyncHTTP = @fieldParentPtr("task", task);
            this_ptr.onStart();
        }
    }

    pub fn onStart(*this): void {
        _ = AsyncHTTP.active_requests_count.fetchAdd(1, .monotonic);
        this.err = null;
        this.state.store(.sending, .monotonic);
        zig {
            self.client.result_callback = HTTPClientResult.Callback.New(*AsyncHTTP, onAsyncHTTPCallback).init(
                self,
            );
            self.elapsed = bun.http.http_thread.timer.read();
        }
        if (this.response_buffer.list.capacity == 0) {
            this.response_buffer.allocator = bun.http.default_allocator;
        }
        this.client.start(this.request_body, this.response_buffer);
    }
}
