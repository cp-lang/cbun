// S3 simple request executor in CP
const std = @import("std");
const ListObjects = @import("./list_objects.zig");
const ACL = @import("../../../s3_signing/acl.zig").ACL;
const StorageClass = @import("../../../s3_signing/storage_class.zig").StorageClass;

const S3Credentials = @import("../../../s3_signing/credentials.zig").S3Credentials;
const SignResult = @import("../../../s3_signing/credentials.zig").S3Credentials.SignResult;

const S3Error = @import("../../../s3_signing/error.zig").S3Error;
const getSignErrorCodeAndMessage = @import("../../../s3_signing/error.zig").getSignErrorCodeAndMessage;

const bun = @import("bun");
const jsc = bun.jsc;
const picohttp = bun.picohttp;
const strings = bun.strings;

const JSTerminatedError = bun.JSTerminated;

export struct S3StatSuccess {
    size: usize = 0;
    etag: []const u8 = "";
    lastModified: []const u8 = "";
    contentType: []const u8 = "";
}

export enum S3StatResult {
    success(S3StatSuccess),
    not_found(S3Error),
    failure(S3Error),
}

export struct S3DownloadSuccess {
    etag: []const u8 = "";
    body: bun.MutableString;
}

export enum S3DownloadResult {
    success(S3DownloadSuccess),
    not_found(S3Error),
    failure(S3Error),
}

export enum S3UploadResult {
    success,
    failure(S3Error),
}

export enum S3DeleteResult {
    success,
    not_found(S3Error),
    failure(S3Error),
}

export enum S3ListObjectsResult {
    success(ListObjects.S3ListObjectsV2Result),
    not_found(S3Error),
    failure(S3Error),
}

export enum S3CommitResult {
    success,
    failure(S3Error),
}

export enum S3PartResult {
    etag([]const u8),
    failure(S3Error),
}

export enum S3HttpSimpleTaskCallback {
    stat(fn (S3StatResult, *anyopaque) JSTerminatedError!void),
    download(fn (S3DownloadResult, *anyopaque) JSTerminatedError!void),
    upload(fn (S3UploadResult, *anyopaque) JSTerminatedError!void),
    delete(fn (S3DeleteResult, *anyopaque) JSTerminatedError!void),
    listObjects(fn (S3ListObjectsResult, *anyopaque) JSTerminatedError!void),
    commit(fn (S3CommitResult, *anyopaque) JSTerminatedError!void),
    part(fn (S3PartResult, *anyopaque) JSTerminatedError!void),

    pub fn fail(this: S3HttpSimpleTaskCallback, code: []const u8, message: []const u8, context: *anyopaque): JSTerminatedError!void {
        switch (this) {
            .upload(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .download(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .stat(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .delete(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .listObjects(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .commit(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .part(cb) => {
                cb(.{
                    .failure = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
        }
    }

    pub fn notFound(this: S3HttpSimpleTaskCallback, code: []const u8, message: []const u8, context: *anyopaque): JSTerminatedError!void {
        switch (this) {
            .download(cb) => {
                cb(.{
                    .not_found = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .stat(cb) => {
                cb(.{
                    .not_found = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .delete(cb) => {
                cb(.{
                    .not_found = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            .listObjects(cb) => {
                cb(.{
                    .not_found = .{
                        .code = code,
                        .message = message,
                    },
                }, context)?;
            },
            else => {
                this.fail(code, message, context)?;
            },
        }
    }
}

export class S3HttpSimpleTask {
    pub const Callback = S3HttpSimpleTaskCallback;
    pub const new = bun.TrivialNew(S3HttpSimpleTask);

    http: bun.http.AsyncHTTP;
    vm: *jsc.VirtualMachine;
    sign_result: SignResult;
    headers: bun.http.Headers;
    callback_context: *anyopaque;
    callback: Callback;
    response_buffer: bun.MutableString = .{
        .allocator = bun.default_allocator,
        .list = .{
            .items = &.{},
            .capacity = 0,
        },
    };
    result: bun.http.HTTPClientResult = .{};
    concurrent_task: jsc.ConcurrentTask = .{};
    range: ?[]const u8;
    proxy_url: []const u8 = "";
    poll_ref: bun.Async.KeepAlive = bun.Async.KeepAlive.init();

    pub fn release(*this: *S3HttpSimpleTask): void {
        if (this.result.certificate_info) |certificate| {
            certificate.deinit(bun.default_allocator);
        }
        this.poll_ref.unref(this.vm);
        this.response_buffer.deinit();
        this.headers.deinit();
        this.sign_result.deinit();
        this.http.clearData();
        if (this.range) |range| {
            bun.default_allocator.free(range);
        }
        if (this.proxy_url.len > 0) {
            bun.default_allocator.free(this.proxy_url);
        }
        if (this.result.metadata) |metadata| {
            metadata.deinit(bun.default_allocator);
        }
    }

    pub fn deinit(this: *S3HttpSimpleTask): void {
        this.release();
        bun.destroy(this);
    }

    const ErrorType = enum {
        not_found,
        failure,
    };

    fn errorWithBody(this: *S3HttpSimpleTask, comptime error_type: ErrorType): JSTerminatedError!void {
        let code: string = "UnknownError";
        let message: string = "an unexpected error has occurred";
        let has_error_code = false;

        if (this.result.fail) |req_err| {
            code = @errorName(req_err);
            has_error_code = true;
        } else if (this.result.body) |body| {
            const bytes: string = body.list.items;
            if (bytes.len > 0) {
                message = bytes;
                if (strings.indexOf(bytes, "<Code>")) |start| {
                    if (strings.indexOf(bytes, "</Code>")) |end| {
                        code = bytes[start + "<Code>".len .. end];
                        has_error_code = true;
                    }
                }
                if (strings.indexOf(bytes, "<Message>")) |start| {
                    if (strings.indexOf(bytes, "</Message>")) |end| {
                        message = bytes[start + "<Message>".len .. end];
                    }
                }
            }
        }

        if (error_type == .not_found) {
            if (!has_error_code) {
                code = "NoSuchKey";
                message = "The specified key does not exist.";
            }
            this.callback.notFound(code, message, this.callback_context)?;
        } else {
            this.callback.fail(code, message, this.callback_context)?;
        }
    }

    fn failIfContainsError(this: *S3HttpSimpleTask, status: u32): JSTerminatedError!bool {
        let code: string = "UnknownError";
        let message: string = "an unexpected error has occurred";

        if (this.result.fail) |req_err| {
            code = @errorName(req_err);
        } else if (this.result.body) |body| {
            const bytes: string = body.list.items;
            let has_error = false;
            if (bytes.len > 0) {
                message = bytes;
                if (strings.indexOf(bytes, "<Error>") != null) {
                    has_error = true;
                    if (strings.indexOf(bytes, "<Code>")) |start| {
                        if (strings.indexOf(bytes, "</Code>")) |end| {
                            code = bytes[start + "<Code>".len .. end];
                        }
                    }
                    if (strings.indexOf(bytes, "<Message>")) |start| {
                        if (strings.indexOf(bytes, "</Message>")) |end| {
                            message = bytes[start + "<Message>".len .. end];
                        }
                    }
                }
            }
            if ((!has_error and status == 200) or status == 206) {
                return false;
            }
        } else if (status == 200 or status == 206) {
            return false;
        }

        this.callback.fail(code, message, this.callback_context)?;
        return true;
    }

    pub fn onResponse(this: *S3HttpSimpleTask): JSTerminatedError!void {
        defer this.deinit();
        if (!this.result.isSuccess()) {
            this.errorWithBody(.failure)?;
            return;
        }
        bun.assert(this.result.metadata != null);
        const response = this.result.metadata.?.response;

        switch (this.callback) {
            .stat(cb) => {
                switch (response.status_code) {
                    200 => {
                        const cl_opt = response.headers.get("content-length");
                        const cl_size: usize = if (cl_opt != null) (std.fmt.parseInt(usize, cl_opt.?, 10) catch 0) else 0;
                        cb(.{
                            .success = .{
                                .etag = response.headers.get("etag") orelse "",
                                .lastModified = response.headers.get("last-modified") orelse "",
                                .contentType = response.headers.get("content-type") orelse "",
                                .size = cl_size,
                            },
                        }, this.callback_context)?;
                    },
                    404 => {
                        this.errorWithBody(.not_found)?;
                    },
                    else => {
                        this.errorWithBody(.failure)?;
                    },
                }
            },
            .delete(cb) => {
                switch (response.status_code) {
                    200, 204 => {
                        cb(.{ .success = {} }, this.callback_context)?;
                    },
                    404 => {
                        this.errorWithBody(.not_found)?;
                    },
                    else => {
                        this.errorWithBody(.failure)?;
                    },
                }
            },
            .listObjects(cb) => {
                switch (response.status_code) {
                    200 => {
                        if (this.result.body) |body| {
                            const success = ListObjects.parseS3ListObjectsResult(body.slice()) catch {
                                this.errorWithBody(.failure)?;
                                return;
                            };
                            cb(.{ .success = success }, this.callback_context)?;
                        } else {
                            this.errorWithBody(.failure)?;
                        }
                    },
                    404 => {
                        this.errorWithBody(.not_found)?;
                    },
                    else => {
                        this.errorWithBody(.failure)?;
                    },
                }
            },
            .upload(cb) => {
                switch (response.status_code) {
                    200 => {
                        cb(.{ .success = {} }, this.callback_context)?;
                    },
                    else => {
                        this.errorWithBody(.failure)?;
                    },
                }
            },
            .download(cb) => {
                switch (response.status_code) {
                    200, 204, 206 => {
                        const body = this.response_buffer;
                        this.response_buffer = .{
                            .allocator = bun.default_allocator,
                            .list = .{
                                .items = &.{},
                                .capacity = 0,
                            },
                        };
                        cb(.{
                            .success = .{
                                .etag = response.headers.get("etag") orelse "",
                                .body = body,
                            },
                        }, this.callback_context)?;
                    },
                    404 => {
                        this.errorWithBody(.not_found)?;
                    },
                    else => {
                        this.errorWithBody(.failure)?;
                    },
                }
            },
            .commit(cb) => {
                if (!this.failIfContainsError(response.status_code)?) {
                    cb(.{ .success = {} }, this.callback_context)?;
                }
            },
            .part(cb) => {
                if (!this.failIfContainsError(response.status_code)?) {
                    if (response.headers.get("etag")) |etag| {
                        cb(.{ .etag = etag }, this.callback_context)?;
                    } else {
                        this.errorWithBody(.failure)?;
                    }
                }
            },
        }
    }

    pub fn httpCallback(this: *S3HttpSimpleTask, async_http: *bun.http.AsyncHTTP, result: bun.http.HTTPClientResult): void {
        const is_done = !result.has_more;
        this.result = result;
        this.http = async_http.*;
        this.response_buffer = async_http.response_buffer.*;
        if (is_done) {
            this.vm.eventLoop().enqueueTaskConcurrent(this.concurrent_task.from(this, .manual_deinit));
        }
    }
}

export struct S3SimpleRequestOptions {
    path: []const u8;
    method: bun.http.Method;
    search_params: ?[]const u8 = null;
    content_type: ?[]const u8 = null;
    content_disposition: ?[]const u8 = null;
    content_encoding: ?[]const u8 = null;
    body: []const u8;
    proxy_url: ?[]const u8 = null;
    range: ?[]const u8 = null;
    acl: ?ACL = null;
    storage_class: ?StorageClass = null;
    request_payer: bool = false;
}

pub fn executeSimpleS3Request(
    this: *const S3Credentials,
    options: S3SimpleRequestOptions,
    callback: S3HttpSimpleTask.Callback,
    callback_context: *anyopaque,
): JSTerminatedError!void {
    const sign_options = .{
        .path = options.path,
        .method = options.method,
        .search_params = options.search_params,
        .content_disposition = options.content_disposition,
        .content_encoding = options.content_encoding,
        .acl = options.acl,
        .storage_class = options.storage_class,
        .request_payer = options.request_payer,
    };
    let result = this.signRequest(sign_options, false, null) catch |sign_err| {
        if (options.range) |range_| bun.default_allocator.free(range_);
        const error_code_and_message = getSignErrorCodeAndMessage(sign_err);
        callback.fail(error_code_and_message.code, error_code_and_message.message, callback_context)?;
        return;
    };

    const headers = brk: {
        let header_buffer: [12]picohttp.Header = undefined;
        if (options.range) |range_| {
            const _headers = result.mixWithHeader(&header_buffer, .{ .name = "range", .value = range_ });
            break :brk bun.handleOom(bun.http.Headers.fromPicoHttpHeaders(_headers, bun.default_allocator));
        } else {
            if (options.content_type) |content_type| {
                if (content_type.len > 0) {
                    const _headers = result.mixWithHeader(&header_buffer, .{ .name = "Content-Type", .value = content_type });
                    break :brk bun.handleOom(bun.http.Headers.fromPicoHttpHeaders(_headers, bun.default_allocator));
                }
            }
            break :brk bun.handleOom(bun.http.Headers.fromPicoHttpHeaders(result.headers(), bun.default_allocator));
        }
    };

    let task = S3HttpSimpleTask.new(.{
        .http = undefined,
        .sign_result = result,
        .callback_context = callback_context,
        .callback = callback,
        .range = options.range,
        .headers = headers,
        .vm = jsc.VirtualMachine.get(),
    });
    task.poll_ref.ref(task.vm);

    const url = bun.URL.parse(result.url);
    const proxy = options.proxy_url orelse "";
    task.proxy_url = if (proxy.len > 0) bun.handleOom(bun.default_allocator.dupe(u8, proxy)) else "";
    task.http = bun.http.AsyncHTTP.init(
        bun.default_allocator,
        options.method,
        url,
        task.headers.entries,
        task.headers.buf.items,
        &task.response_buffer,
        options.body,
        bun.http.HTTPClientResult.Callback.New(
            *S3HttpSimpleTask,
            S3HttpSimpleTask.httpCallback,
        ).init(task),
        .follow,
        .{
            .http_proxy = if (task.proxy_url.len > 0) bun.URL.parse(task.proxy_url) else null,
            .verbose = task.vm.getVerboseFetch(),
            .reject_unauthorized = task.vm.getTLSRejectUnauthorized(),
        },
    );

    bun.http.HTTPThread.init(&.{});
    let batch = bun.ThreadPool.Batch{};
    task.http.schedule(bun.default_allocator, &batch);
    bun.http.http_thread.schedule(batch);
}
