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
    stat(fn (S3StatResult, *!) JSTerminatedError!void),
    download(fn (S3DownloadResult, *!) JSTerminatedError!void),
    upload(fn (S3UploadResult, *!) JSTerminatedError!void),
    delete(fn (S3DeleteResult, *!) JSTerminatedError!void),
    listObjects(fn (S3ListObjectsResult, *!) JSTerminatedError!void),
    commit(fn (S3CommitResult, *!) JSTerminatedError!void),
    part(fn (S3PartResult, *!) JSTerminatedError!void),

    pub fn fail(this: S3HttpSimpleTaskCallback, code: []const u8, message: []const u8, context: *!): JSTerminatedError!void {
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

    pub fn notFound(this: S3HttpSimpleTaskCallback, code: []const u8, message: []const u8, context: *!): JSTerminatedError!void {
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
    callback_context: *!;
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

    pub fn release(*_) {
        if (*certificate := _.result.certificate_info) {
            certificate.deinit(bun.default_allocator);
        }
        _.poll_ref.unref(_.vm);
        _.response_buffer.deinit();
        _.headers.deinit();
        _.sign_result.deinit();
        _.http.clearData();
        if (range := _.range) {
            bun.default_allocator.free(range);
        }
        if (_.proxy_url.len > 0) {
            bun.default_allocator.free(_.proxy_url);
        }
        if (*metadata := _.result.metadata) {
            metadata.deinit(bun.default_allocator);
        }
    }

    pub fn deinit(_: *S3HttpSimpleTask) {
        _.release();
        bun.destroy(_);
    }

    const ErrorType = enum {
        not_found,
        failure,
    };

    fn errorWithBody(_: *S3HttpSimpleTask, comptime error_type: ErrorType): JSTerminatedError!void {
        var code: string = "UnknownError";
        var message: string = "an unexpected error has occurred";
        var has_error_code = false;

        if (req_err := _.result.fail) {
            code = @errorName(req_err);
            has_error_code = true;
        } else if (body := _.result.body) {
            const bytes = body.list.items;
            if (bytes.len > 0) {
                message = bytes[0..];
                if (start := strings.indexOf(bytes, "<Code>")) {
                    if (end := strings.indexOf(bytes, "</Code>")) {
                        code = bytes[start + "<Code>".len .. end];
                        has_error_code = true;
                    }
                }
                if (start := strings.indexOf(bytes, "<Message>")) {
                    if (end := strings.indexOf(bytes, "</Message>")) {
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
            _.callback.notFound(code, message, _.callback_context)?;
        } else {
            _.callback.fail(code, message, _.callback_context)?;
        }
    }

    fn failIfContainsError(_: *S3HttpSimpleTask, status: u32): JSTerminatedError!bool {
        var code: string = "UnknownError";
        var message: string = "an unexpected error has occurred";

        if (req_err := _.result.fail) {
            code = @errorName(req_err);
        } else if (body := _.result.body) {
            const bytes = body.list.items;
            var has_error = false;
            if (bytes.len > 0) {
                message = bytes[0..];
                if (strings.indexOf(bytes, "<Error>") != null) {
                    has_error = true;
                    if (start := strings.indexOf(bytes, "<Code>")) {
                        if (end := strings.indexOf(bytes, "</Code>")) {
                            code = bytes[start + "<Code>".len .. end];
                        }
                    }
                    if (start := strings.indexOf(bytes, "<Message>")) {
                        if (end := strings.indexOf(bytes, "</Message>")) {
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

        _.callback.fail(code, message, _.callback_context)?;
        return true;
    }

    pub fn onResponse(_: *S3HttpSimpleTask): JSTerminatedError!void {
        defer _.deinit();
        if (!_.result.isSuccess()) {
            _.errorWithBody(.failure)?;
            return;
        }
        bun.assert(_.result.metadata != null);
        const response = _.result.metadata.?.response;

        switch (_.callback) {
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
                        }, _.callback_context)?;
                    },
                    404 => {
                        _.errorWithBody(.not_found)?;
                    },
                    else => {
                        _.errorWithBody(.failure)?;
                    },
                }
            },
            .delete(cb) => {
                switch (response.status_code) {
                    200, 204 => {
                        cb(.{ .success = {} }, _.callback_context)?;
                    },
                    404 => {
                        _.errorWithBody(.not_found)?;
                    },
                    else => {
                        _.errorWithBody(.failure)?;
                    },
                }
            },
            .listObjects(cb) => {
                switch (response.status_code) {
                    200 => {
                        if (body := _.result.body) {
                            const success = ListObjects.parseS3ListObjectsResult(body.slice()) catch {
                                _.errorWithBody(.failure)?;
                                return;
                            };
                            cb(.{ .success = success }, _.callback_context)?;
                        } else {
                            _.errorWithBody(.failure)?;
                        }
                    },
                    404 => {
                        _.errorWithBody(.not_found)?;
                    },
                    else => {
                        _.errorWithBody(.failure)?;
                    },
                }
            },
            .upload(cb) => {
                switch (response.status_code) {
                    200 => {
                        cb(.{ .success = {} }, _.callback_context)?;
                    },
                    else => {
                        _.errorWithBody(.failure)?;
                    },
                }
            },
            .download(cb) => {
                switch (response.status_code) {
                    200, 204, 206 => {
                        const body = _.response_buffer;
                        _.response_buffer = .{
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
                        }, _.callback_context)?;
                    },
                    404 => {
                        _.errorWithBody(.not_found)?;
                    },
                    else => {
                        _.errorWithBody(.failure)?;
                    },
                }
            },
            .commit(cb) => {
                if (!(_.failIfContainsError(response.status_code)?)) {
                    cb(.{ .success = {} }, _.callback_context)?;
                }
            },
            .part(cb) => {
                if (!(_.failIfContainsError(response.status_code)?)) {
                    if (etag := response.headers.get("etag")) {
                        cb(.{ .etag = etag }, _.callback_context)?;
                    } else {
                        _.errorWithBody(.failure)?;
                    }
                }
            },
        }
    }

    pub fn httpCallback(_: *S3HttpSimpleTask, async_http: *bun.http.AsyncHTTP, result: bun.http.HTTPClientResult) {
        const is_done = !result.has_more;
        _.result = result;
        _.http = async_http.*;
        _.response_buffer = async_http.response_buffer.*;
        if (is_done) {
            _.vm.eventLoop().enqueueTaskConcurrent(_.concurrent_task.from(_, .manual_deinit));
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
    callback_context: *!,
): JSTerminatedError!void {
    var result = this.signRequest(.{
        .path = options.path,
        .method = options.method,
        .search_params = options.search_params,
        .content_disposition = options.content_disposition,
        .content_encoding = options.content_encoding,
        .acl = options.acl,
        .storage_class = options.storage_class,
        .request_payer = options.request_payer,
    }, false, null) catch |sign_err| {
        if (range_ := options.range) bun.default_allocator.free(range_);
        const error_code_and_message = getSignErrorCodeAndMessage(sign_err);
        callback.fail(error_code_and_message.code, error_code_and_message.message, callback_context)?;
        return;
    };

    const headers = brk: {
        var header_buffer: [12]picohttp.Header = undefined;
        if (range_ := options.range) {
            const _headers = result.mixWithHeader(&header_buffer, .{ .name = "range", .value = range_ });
            break :brk bun.handleOom(bun.http.Headers.fromPicoHttpHeaders(_headers, bun.default_allocator));
        } else {
            if (content_type := options.content_type) {
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
    var batch = bun.ThreadPool.Batch{};
    task.http.schedule(bun.default_allocator, &batch);
    bun.http.http_thread.schedule(batch);
}
