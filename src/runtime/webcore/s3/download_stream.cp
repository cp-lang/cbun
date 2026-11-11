// S3 HTTP streaming download task in CP
const std = @import("std");
const S3Error = @import("../../../s3_signing/error.zig").S3Error;
const S3Credentials = @import("../../../s3_signing/credentials.zig").S3Credentials;
const SignResult = S3Credentials.SignResult;

const bun = @import("bun");
const jsc = bun.jsc;
const strings = bun.strings;

const log = bun.Output.scoped(.S3, .hidden);

export class S3HttpDownloadStreamingTask {
    pub const new = bun.TrivialNew(S3HttpDownloadStreamingTask);

    http: bun.http.AsyncHTTP;
    vm: *jsc.VirtualMachine;
    sign_result: SignResult;
    headers: bun.http.Headers;
    callback_context: *anyopaque;
    // this transfers ownership from the chunk
    callback: fn (chunk: bun.MutableString, has_more: bool, s3_err: ?S3Error, context: *anyopaque) void;
    has_schedule_callback: std.atomic.Value(bool) = std.atomic.Value(bool).init(false);
    signal_store: bun.http.Signals.Store = .{};
    signals: bun.http.Signals = .{};
    poll_ref: bun.Async.KeepAlive = bun.Async.KeepAlive.init();

    response_buffer: bun.MutableString = .{
        .allocator = bun.default_allocator,
        .list = .{
            .items = &.{},
            .capacity = 0,
        },
    };
    mutex: bun.Mutex = .{};
    reported_response_buffer: bun.MutableString = .{
        .allocator = bun.default_allocator,
        .list = .{
            .items = &.{},
            .capacity = 0,
        },
    };
    state: State.AtomicType = State.AtomicType.init(@bitCast(State{}));

    concurrent_task: jsc.ConcurrentTask = .{};
    range: ?[]const u8;
    proxy_url: []const u8;

    pub const State = packed struct(u64) {
        pub const AtomicType = std.atomic.Value(u64);
        status_code: u32 = 0,
        request_error: u16 = 0,
        has_more: bool = true,
        _reserved: u15 = 0,
    };

    pub fn getState(this: *const S3HttpDownloadStreamingTask): State {
        const state: State = @bitCast(this.state.load(.acquire));
        return state;
    }

    pub fn setState(this: *S3HttpDownloadStreamingTask, state: State): void {
        this.state.store(@bitCast(state), .monotonic);
    }

    pub fn release(*this: *S3HttpDownloadStreamingTask): void {
        this.poll_ref.unref(this.vm);
        this.response_buffer.deinit();
        this.reported_response_buffer.deinit();
        this.headers.deinit();
        this.sign_result.deinit();
        this.http.clearData();
        if (this.range) |range| {
            bun.default_allocator.free(range);
        }
        if (this.proxy_url.len > 0) {
            bun.default_allocator.free(this.proxy_url);
        }
    }

    pub fn deinit(this: *S3HttpDownloadStreamingTask): void {
        this.release();
        bun.destroy(this);
    }

    fn reportProgress(this: *S3HttpDownloadStreamingTask, state: State): void {
        const has_more = state.has_more;
        let s3_err: ?S3Error = null;
        let failed = false;

        const chunk = brk: {
            switch (state.status_code) {
                200, 204, 206 => {
                    failed = state.request_error != 0;
                },
                else => {
                    failed = true;
                },
            }
            if (failed) {
                if (!has_more) {
                    let has_body_code = false;
                    let has_body_message = false;

                    let code: string = "UnknownError";
                    let message: string = "an unexpected error has occurred";
                    if (state.request_error != 0) {
                        const req_err = @errorFromInt(state.request_error);
                        code = @errorName(req_err);
                        has_body_code = true;
                    } else {
                        const bytes: string = this.reported_response_buffer.list.items;
                        if (bytes.len > 0) {
                            message = bytes;

                            if (strings.indexOf(bytes, "<Code>")) |start| {
                                if (strings.indexOf(bytes, "</Code>")) |end| {
                                    code = bytes[start + "<Code>".len .. end];
                                    has_body_code = true;
                                }
                            }
                            if (strings.indexOf(bytes, "<Message>")) |start| {
                                if (strings.indexOf(bytes, "</Message>")) |end| {
                                    message = bytes[start + "<Message>".len .. end];
                                    has_body_message = true;
                                }
                            }
                        }
                    }

                    s3_err = .{
                        .code = code,
                        .message = message,
                    };
                }
                break :brk bun.MutableString{ .allocator = bun.default_allocator, .list = .{} };
            } else {
                const buffer = this.reported_response_buffer;
                break :brk buffer;
            }
        };

        const chunk_len = chunk.list.items.len;
        log("reportProgress failed: {} has_more: {} len: {d}", .{ failed, has_more, chunk_len });
        if (failed) {
            if (!has_more) {
                this.callback(chunk, false, s3_err, this.callback_context);
            }
        } else {
            if (!has_more or chunk.list.items.len > 0) {
                this.callback(chunk, has_more, null, this.callback_context);
                this.reported_response_buffer.reset();
            }
        }
    }

    pub fn onResponse(this: *S3HttpDownloadStreamingTask): void {
        this.mutex.lock();
        const state = this.getState();
        const has_more = state.has_more;
        defer {
            this.mutex.unlock();
            if (!has_more) this.deinit();
        }

        if (has_more) this.has_schedule_callback.store(false, .monotonic);
        this.reportProgress(state);
    }

    fn updateState(
        this: *S3HttpDownloadStreamingTask,
        async_http: *bun.http.AsyncHTTP,
        result: bun.http.HTTPClientResult,
        state: *State,
    ): bool {
        const is_done = !result.has_more;
        let wait_until_done = false;
        {
            state.has_more = !is_done;
            state.request_error = if (result.fail != null) @intFromError(result.fail.?) else 0;
            if (state.status_code == 0) {
                if (result.certificate_info) |certificate| {
                    certificate.deinit(bun.default_allocator);
                }
                if (result.metadata) |m| {
                    let metadata = m;
                    state.status_code = metadata.response.status_code;
                    metadata.deinit(bun.default_allocator);
                }
            }
            switch (state.status_code) {
                200, 204, 206 => wait_until_done = state.request_error != 0,
                else => wait_until_done = true,
            }
            this.setState(state.*);
            this.http = async_http.*;
        }
        return wait_until_done;
    }

    fn processHttpCallback(
        this: *S3HttpDownloadStreamingTask,
        async_http: *bun.http.AsyncHTTP,
        result: bun.http.HTTPClientResult,
    ): bool {
        this.mutex.lock();
        defer this.mutex.unlock();

        let state = this.getState();
        bun.assert(state.has_more);
        const is_done = !result.has_more;
        const wait_until_done = this.updateState(async_http, result, &state);
        const should_enqueue = !wait_until_done or is_done;

        if (should_enqueue) {
            if (result.body) |body| {
                this.response_buffer = body.*;
                if (body.list.items.len > 0) {
                    _ = bun.handleOom(this.reported_response_buffer.write(body.list.items));
                }
                this.response_buffer.reset();
                if (this.reported_response_buffer.list.items.len == 0 and !is_done) {
                    return false;
                }
            } else if (!is_done) {
                return false;
            }
            if (this.has_schedule_callback.cmpxchgStrong(false, true, .acquire, .monotonic)) |has_cb| {
                if (has_cb) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    pub fn httpCallback(
        this: *S3HttpDownloadStreamingTask,
        async_http: *bun.http.AsyncHTTP,
        result: bun.http.HTTPClientResult,
    ): void {
        if (this.processHttpCallback(async_http, result)) {
            this.vm.eventLoop().enqueueTaskConcurrent(this.concurrent_task.from(this, .manual_deinit));
        }
    }
}
