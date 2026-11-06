// src/http/AsyncHTTP.cp
// Fast asynchronous HTTP task orchestration in CP language

zig {
    const bun = @import("bun");
    pub const PercentEncoding = @import("../url/url.zig").PercentEncoding;
}

import { Headers } from "./Headers.cp";
import { HTTPThread } from "./HTTPThread.cp";
import { Encoding } from "../http_types/Encoding.cp";
import { HTTPClient, HTTPClientResult } from "./http.cp";
import { FetchRedirect } from "../http_types/FetchRedirect.cp";

type MutableString = bun.MutableString;
type ThreadPoolBatch = bun.ThreadPool.Batch;

export enum State: u32 {
    pending = 0,
    scheduled = 1,
    sending = 2,
    success = 3,
    fail = 4,
}

export struct Options {
    http_proxy: bun.URL? = null,
    proxy_headers: Headers? = null,
    hostname: string? = null,
    signals: HTTPClient.Signals? = null,
    unix_socket_path: bun.jsc.ZigString.Slice? = null,
    disable_timeout: bool? = null,
    verbose: HTTPClient.HTTPVerboseLevel? = null,
    disable_keepalive: bool? = null,
    disable_decompression: bool? = null,
    reject_unauthorized: bool? = null,
    tls_props: bun.api.server.ServerConfig.SSLConfig.SharedPtr? = null,
}

zig {
    pub const Channel8 = bun.threading.Channel(HTTPClientResult, .{ .Static = 8 });
    pub const Channel1000 = bun.threading.Channel(HTTPClientResult, .{ .Static = 1000 });
}

struct Preconnect {
    zig {
        pub const new = bun.TrivialNew(Preconnect);
    }

    async_http: AsyncHTTP,
    response_buffer: bun.MutableString,
    url: bun.URL,
    is_url_owned: bool,

    fn onResult(*this, dummy_async: *AsyncHTTP, dummy_res: HTTPClientResult): void {
        this.response_buffer.deinit();
        this.async_http.clearData();
        this.async_http.client.deinit();
        if (this.is_url_owned) {
            bun.default_allocator.free(this.url.href);
        }
        bun.destroy(this);
    }
}

struct SingleHTTPChannel {
    channel: Channel8,

    fn reset(*this): void {}

    static fn init(): SingleHTTPChannel {
        return SingleHTTPChannel{
            channel: Channel8.init(),
        };
    }
}

fn sendSyncCallback(channel_holder: *SingleHTTPChannel, async_http: *AsyncHTTP, result: HTTPClientResult): void {
    if (real := async_http.real) {
        real.* = async_http.*;
        real.response_buffer = async_http.response_buffer;
    }
    channel_holder.channel.writeItem(result) catch @panic("unreachable");
}

export struct HTTPChannelContext {
    http: AsyncHTTP = undefined,
    channel: *Channel1000,

    fn callback(*this, _async: *AsyncHTTP, result: HTTPClientResult): void {
        this.channel.writeItem(result) catch @panic("unreachable");
    }
}

export @root @value struct AsyncHTTP {
    request: bun.picohttp.Request? = null,
    response: bun.picohttp.Response? = null,
    request_headers: Headers.Entry.List = Headers.Entry.List.empty,
    response_headers: Headers.Entry.List = Headers.Entry.List.empty,
    response_buffer: *bun.MutableString,
    request_body: HTTPClient.HTTPRequestBody = { bytes: "" },
    allocator: std.mem.Allocator,
    request_header_buf: string = "",
    method: HTTPClient.Method = .GET,
    url: bun.URL,
    http_proxy: bun.URL? = null,
    real: *AsyncHTTP? = null,
    next: *AsyncHTTP? = null,

    task: bun.ThreadPool.Task = { callback: &startAsyncHTTP },
    state: std.atomic.Value(State) = std.atomic.Value(State).init(.pending),

    result_callback: HTTPClientResult.Callback = undefined,

    redirected: bool = false,

    response_encoding: Encoding = Encoding.identity,
    verbose: HTTPClient.HTTPVerboseLevel = .none,

    client: HTTPClient = undefined,
    waitingDeffered: bool = false,
    finalized: bool = false,
    err: anyerror? = null,
    async_http_id: u32 = 0,

    elapsed: u64 = 0,
    gzip_elapsed: u64 = 0,

    signals: HTTPClient.Signals = {},

    zig {
        pub var active_requests_count = std.atomic.Value(usize).init(0);
        pub var max_simultaneous_requests = std.atomic.Value(usize).init(256);
    }

    static fn loadEnv(allocator: std.mem.Allocator, logger: anytype, env: anytype): void {
        if (max_http_requests := env.get("BUN_CONFIG_MAX_HTTP_REQUESTS")) {
            const max = std.fmt.parseInt(u16, max_http_requests, 10) catch {
                logger.addErrorFmt(
                    null,
                    bun.logger.Loc.Empty,
                    allocator,
                    "BUN_CONFIG_MAX_HTTP_REQUESTS value \"{s}\" is not a valid integer between 1 and 65535",
                    [max_http_requests],
                ) catch @panic("unreachable");
                return;
            };
            if (max == 0) {
                logger.addWarningFmt(
                    null,
                    bun.logger.Loc.Empty,
                    allocator,
                    "BUN_CONFIG_MAX_HTTP_REQUESTS value must be a number between 1 and 65535",
                    [],
                ) catch @panic("unreachable");
                return;
            }
            AsyncHTTP.max_simultaneous_requests.store(max, .monotonic);
        }
    }

    fn signalHeaderProgress(*this): void {
        if (progress := this.signals.header_progress) {
            progress.store(true, .release);
        }
    }

    fn enableResponseBodyStreaming(*this): void {
        if (stream := this.signals.response_body_streaming) {
            stream.store(true, .release);
        }
    }

    fn clearData(*this): void {
        this.response_headers.deinit(this.allocator);
        this.response_headers = Headers.Entry.List.empty;
        this.request = null;
        this.response = null;
        this.client.unix_socket_path.deinit();
        this.client.unix_socket_path = bun.jsc.ZigString.Slice.empty;
    }

    static fn preconnect(url: bun.URL, is_url_owned: bool): void {
        if (!bun.FeatureFlags.is_fetch_preconnect_supported) {
            if (is_url_owned) {
                bun.default_allocator.free(url.href);
            }
            return;
        }

        let preconnect_ptr = Preconnect.new({
            async_http: undefined,
            response_buffer: MutableString{ allocator: bun.http.default_allocator, list: {} },
            url: url,
            is_url_owned: is_url_owned,
        });

        zig {
            preconnect_ptr.async_http = AsyncHTTP.init(
                bun.default_allocator,
                .GET,
                url,
                .{},
                "",
                &preconnect_ptr.response_buffer,
                "",
                HTTPClientResult.Callback.New(*Preconnect, Preconnect.onResult).init(preconnect_ptr),
                .manual,
                .{},
            );
        }
        preconnect_ptr.async_http.client.flags.is_preconnect_only = true;

        bun.http.http_thread.schedule(bun.ThreadPool.Batch.from(&preconnect_ptr.async_http.task));
    }

    static fn init(
        allocator: std.mem.Allocator,
        method: HTTPClient.Method,
        url: bun.URL,
        headers: Headers.Entry.List,
        headers_buf: string,
        response_buffer: *bun.MutableString,
        request_body: string,
        callback: HTTPClientResult.Callback,
        redirect_type: FetchRedirect,
        options: Options,
    ): AsyncHTTP {
        let async_http_id: u32 = 0;
        if (options.signals != null && options.signals?.aborted != null) {
            async_http_id = bun.http.async_http_id_monotonic.fetchAdd(1, .monotonic);
        }

        let mut async_http = AsyncHTTP{
            allocator: allocator,
            url: url,
            method: method,
            request_headers: headers,
            request_header_buf: headers_buf,
            request_body: { bytes: request_body },
            response_buffer: response_buffer,
            result_callback: callback,
            http_proxy: options.http_proxy,
            signals: options.signals ?? {},
            async_http_id: async_http_id,
        };

        async_http.client = {
            allocator: allocator,
            method: method,
            url: url,
            header_entries: headers,
            header_buf: headers_buf,
            hostname: options.hostname,
            signals: options.signals ?? async_http.signals,
            async_http_id: async_http.async_http_id,
            http_proxy: async_http.http_proxy,
            proxy_headers: options.proxy_headers,
            redirect_type: redirect_type,
        };
        if (val := options.unix_socket_path) {
            bun.assert(async_http.client.unix_socket_path.length() == 0);
            async_http.client.unix_socket_path = val;
        }
        if (val := options.disable_timeout) {
            async_http.client.flags.disable_timeout = val;
        }
        if (val := options.verbose) {
            async_http.client.verbose = val;
        }
        if (val := options.disable_decompression) {
            async_http.client.flags.disable_decompression = val;
        }
        if (val := options.disable_keepalive) {
            async_http.client.flags.disable_keepalive = val;
        }
        if (val := options.reject_unauthorized) {
            async_http.client.flags.reject_unauthorized = val;
        }
        if (val := options.tls_props) {
            async_http.client.tls_props = val;
        }

        if (proxy := options.http_proxy) {
            if (proxy.username.len > 0) {
                let username_sfb = std.heap.stackFallback(4096, allocator);
                const username_alloc = username_sfb.get();
                const username = PercentEncoding.decodeAlloc(username_alloc, proxy.username) catch (error_val) {
                    bun.Output.scoped(.AsyncHTTP, .visible)("failed to decode proxy username: {}", [error_val]);
                    return async_http;
                };
                defer username_alloc.free(username);

                if (proxy.password.len > 0) {
                    let password_sfb = std.heap.stackFallback(4096, allocator);
                    const password_alloc = password_sfb.get();
                    const password = PercentEncoding.decodeAlloc(password_alloc, proxy.password) catch (error_val) {
                        bun.Output.scoped(.AsyncHTTP, .visible)("failed to decode proxy password: {}", [error_val]);
                        return async_http;
                    };
                    defer password_alloc.free(password);

                    const auth = std.fmt.allocPrint(allocator, "{s}:{s}", [username, password]) catch @panic("unreachable");
                    defer allocator.free(auth);
                    const size = std.base64.standard.Encoder.calcSize(auth.len);
                    let buf = async_http.allocator.alloc(u8, size + "Basic ".len) catch @panic("unreachable");
                    const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], auth);
                    buf[0.."Basic ".len].* = "Basic ".*;
                    async_http.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                } else {
                    const size = std.base64.standard.Encoder.calcSize(username.len);
                    let buf = allocator.alloc(u8, size + "Basic ".len) catch @panic("unreachable");
                    const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], username);
                    buf[0.."Basic ".len].* = "Basic ".*;
                    async_http.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                }
            }
        }
        return async_http;
    }

    static fn initSync(
        allocator: std.mem.Allocator,
        method: HTTPClient.Method,
        url: bun.URL,
        headers: Headers.Entry.List,
        headers_buf: string,
        response_buffer: *bun.MutableString,
        request_body: string,
        http_proxy: bun.URL?,
        hostname: string?,
        redirect_type: FetchRedirect,
    ): AsyncHTTP {
        let opts = Options {};
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

    fn reset(*this): !void {
        const aborted = this.client.aborted;
        this.client = HTTPClient.init(this.allocator, this.method, this.client.url, this.client.header_entries, this.client.header_buf, aborted)?;
        this.client.http_proxy = this.http_proxy;

        if (proxy := this.http_proxy) {
            this.client.flags.disable_keepalive = this.url.isHTTPS();
            if (proxy.username.len > 0) {
                let username_sfb = std.heap.stackFallback(4096, this.allocator);
                const username_alloc = username_sfb.get();
                const username = PercentEncoding.decodeAlloc(username_alloc, proxy.username) catch (error_val) {
                    bun.Output.scoped(.AsyncHTTP, .visible)("failed to decode proxy username: {}", [error_val]);
                    return;
                };
                defer username_alloc.free(username);

                if (proxy.password.len > 0) {
                    let password_sfb = std.heap.stackFallback(4096, this.allocator);
                    const password_alloc = password_sfb.get();
                    const password = PercentEncoding.decodeAlloc(password_alloc, proxy.password) catch (error_val) {
                        bun.Output.scoped(.AsyncHTTP, .visible)("failed to decode proxy password: {}", [error_val]);
                        return;
                    };
                    defer password_alloc.free(password);

                    const auth = std.fmt.allocPrint(this.allocator, "{s}:{s}", [username, password]) catch @panic("unreachable");
                    defer this.allocator.free(auth);
                    const size = std.base64.standard.Encoder.calcSize(auth.len);
                    let buf = this.allocator.alloc(u8, size + "Basic ".len) catch @panic("unreachable");
                    const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], auth);
                    buf[0.."Basic ".len].* = "Basic ".*;
                    this.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                } else {
                    const size = std.base64.standard.Encoder.calcSize(username.len);
                    let buf = this.allocator.alloc(u8, size + "Basic ".len) catch @panic("unreachable");
                    const encoded = std.base64.url_safe.Encoder.encode(buf["Basic ".len..], username);
                    buf[0.."Basic ".len].* = "Basic ".*;
                    this.client.proxy_authorization = buf[0 .. "Basic ".len + encoded.len];
                }
            }
        }
    }

    fn schedule(*this, _allocator: std.mem.Allocator, batch: *bun.ThreadPool.Batch): void {
        _allocator;
        this.state.store(.scheduled, .monotonic);
        batch.push(bun.ThreadPool.Batch.from(&this.task));
    }

    fn sendSync(*this): !bun.picohttp.Response {
        HTTPThread.init(&{});

        let ctx = bun.default_allocator.create(SingleHTTPChannel)?;
        ctx.* = SingleHTTPChannel.init();
        zig {
            self.result_callback = HTTPClientResult.Callback.New(
                *SingleHTTPChannel,
                sendSyncCallback,
            ).init(ctx);
        }

        let batch = ThreadPoolBatch{};
        this.schedule(bun.default_allocator, &batch);
        bun.http.http_thread.schedule(batch);

        const result = ctx.channel.readItem() catch @panic("unreachable");
        if (err := result.fail) {
            return err;
        }
        bun.assert(result.metadata != null);
        zig {
            return result.metadata.?.response;
        }
    }

    fn onAsyncHTTPCallback(*this, async_http: *AsyncHTTP, result: HTTPClientResult): void {
        bun.assert(this.real != null);

        let callback = this.result_callback;
        let now = bun.http.http_thread.timer.read();
        if (now > this.elapsed) {
            this.elapsed = now - this.elapsed;
        } else {
            this.elapsed = 0;
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

        @if (bun.Environment.enable_logs) {
            if (bun.http.socket_async_http_abort_tracker.count() > 0) {
                bun.Output.scoped(.AsyncHTTP, .visible)("bun.http.socket_async_http_abort_tracker count: {d}", [bun.http.socket_async_http_abort_tracker.count()]);
            }
        }

        if (bun.http.socket_async_http_abort_tracker.capacity() > 10000 && bun.http.socket_async_http_abort_tracker.count() < 100) {
            bun.http.socket_async_http_abort_tracker.shrinkAndFree(bun.http.socket_async_http_abort_tracker.count());
        }

        if (result.has_more) {
            callback.function(callback.ctx, async_http, result);
        } else {
            {
                this.client.deinit();
                zig {
                    const threadlocal_http: *bun.http.ThreadlocalAsyncHTTP = @fieldParentPtr("async_http", async_http);
                    defer threadlocal_http.deinit();
                    bun.Output.scoped(.AsyncHTTP, .visible)("onAsyncHTTPCallback: {D}", .{self.elapsed});
                    callback.function(callback.ctx, async_http, result);
                }
            }

            const active_requests = AsyncHTTP.active_requests_count.fetchSub(1, .monotonic);
            bun.assert(active_requests > 0);
        }

        if ((!bun.http.http_thread.queued_tasks.isEmpty() || bun.http.http_thread.deferred_tasks.items.len > 0) && AsyncHTTP.active_requests_count.load(.monotonic) < AsyncHTTP.max_simultaneous_requests.load(.monotonic)) {
            bun.http.http_thread.loop.loop.wakeup();
        }
    }

    zig {
        pub fn startAsyncHTTP(task: *bun.ThreadPool.Task) void {
            const this_ptr: *AsyncHTTP = @fieldParentPtr("task", task);
            this_ptr.onStart();
        }
    }

    fn onStart(*this): void {
        AsyncHTTP.active_requests_count.fetchAdd(1, .monotonic);
        this.err = null;
        this.state.store(.sending, .monotonic);
        zig {
            self.client.result_callback = HTTPClient.HTTPClientResult.Callback.New(*AsyncHTTP, onAsyncHTTPCallback).init(self);
        }
        this.elapsed = bun.http.http_thread.timer.read();
        if (this.response_buffer.list.capacity == 0) {
            this.response_buffer.allocator = bun.http.default_allocator;
        }
        this.client.start(this.request_body, this.response_buffer);
    }
}
