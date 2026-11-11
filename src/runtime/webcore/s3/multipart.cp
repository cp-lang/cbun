const std = @import("std");
const sort = std.sort;
const bun = @import("bun");

import { ACL } from "../../../s3_signing/acl";
import { MultiPartUploadOptions } from "./multipart_options";
import { S3Credentials } from "../../../s3_signing/credentials";
import { S3Error } from "../../../s3_signing/error";
import { StorageClass } from "../../../s3_signing/storage_class";
import * as S3SimpleRequest from "./simple_request";
import { default_allocator, handleOom, destroy, ByteList, MutableString, Output, strings } from "bun";
import { JSGlobalObject, VirtualMachine, WebCore } from "bun:jsc";
import { IntegerBitSet } from "bun:bit_set";
import { StreamBuffer } from "bun:io";
import { KeepAlive } from "bun:Async";
import { bufPrint } from "std:fmt";

const executeSimpleS3Request = S3SimpleRequest.executeSimpleS3Request;
const ResumableSinkBackpressure = WebCore.ResumableSinkBackpressure;
const JSTerminatedError = bun.JSTerminated;
const OOMError = bun.OOM;
const log = Output.scoped(.S3MultiPartUpload, .hidden);

export enum MultiPartUploadState {
    wait_stream_check,
    not_started,
    multipart_started,
    multipart_completed,
    singlefile_started,
    finished,
}

export enum UploadPartState: u8 {
    not_assigned = 0,
    pending = 1,
    started = 2,
    completed = 3,
    canceled = 4,
}

export struct UploadPartResult {
    number: u16;
    etag: string;
}

export enum WriteEncoding {
    bytes,
    latin1,
    utf16,
}

export struct UploadPart {
    data: string = "";
    ctx: *MultiPartUpload;
    allocated_size: usize = 0;
    state: UploadPartState = .not_assigned;
    partNumber: u16 = 0;
    retry: u8 = 0;
    index: u8 = 0;

    fn sortEtags(_: *MultiPartUpload, a: UploadPartResult, b: UploadPartResult): bool {
        return a.number < b.number;
    }

    fn freeAllocatedSlice(*this: *UploadPart): void {
        let slice = this.allocatedSlice();
        if (slice.len > 0) {
            default_allocator.free(slice);
        }
        this.data = "";
        this.allocated_size = 0;
    }

    fn allocatedSlice(this: *const UploadPart): string {
        if (this.allocated_size > 0) {
            return this.data[0..this.allocated_size];
        }
        return "";
    }

    @c static fn onPartResponse(result: S3SimpleRequest.S3PartResult, self_any: *anyopaque): JSTerminatedError!void {
        const this: *UploadPart = @ptrCast(@alignCast(self_any));
        if (this.state == .canceled or this.ctx.state == .finished) {
            log("onPartResponse {} canceled", .{ this.partNumber });
            this.freeAllocatedSlice();
            this.ctx.deref();
            return;
        }

        this.state = .completed;

        switch (result) {
            .failure(s3_err) => {
                if (this.retry > 0) {
                    log("onPartResponse {} retry", .{ this.partNumber });
                    this.retry -= 1;
                    this.perform()?;
                    return;
                } else {
                    this.state = .not_assigned;
                    log("onPartResponse {} failed", .{ this.partNumber });
                    this.freeAllocatedSlice();
                    defer this.ctx.deref();
                    return this.ctx.fail(s3_err);
                }
            },
            .etag(etag) => {
                log("onPartResponse {} success", .{ this.partNumber });
                let sent = this.data.len;
                this.freeAllocatedSlice();
                this.ctx.multipart_etags.append(default_allocator, .{
                    .number = this.partNumber,
                    .etag = handleOom(default_allocator.dupe(u8, etag)),
                }) catch |oom_err| handleOom(oom_err);
                this.state = .not_assigned;
                defer this.ctx.deref();
                this.ctx.available.set(this.index);
                this.ctx.drainEnqueuedParts(sent)?;
            },
        }
    }

    fn perform(*this: *UploadPart): JSTerminatedError!void {
        let params_buffer: [2048]u8 = undefined;
        let search_params = bufPrint(&params_buffer, "?partNumber={}&uploadId={s}&x-id=UploadPart", .{
            this.partNumber,
            this.ctx.upload_id,
        }) catch unreachable;
        const req_opts = .{
            .path = this.ctx.path,
            .method = bun.http.Method.PUT,
            .proxy_url = this.ctx.proxyUrl(),
            .body = this.data,
            .search_params = search_params,
            .request_payer = this.ctx.request_payer,
        };
        const cb = .{ .part = @ptrCast(&onPartResponse) };
        executeSimpleS3Request(this.ctx.credentials, req_opts, cb, this)?;
    }

    pub fn start(*this: *UploadPart): JSTerminatedError!void {
        if (this.state != .pending or this.ctx.state != .multipart_completed) return;
        this.ctx.ref();
        this.state = .started;
        this.perform()?;
    }

    pub fn cancel(*this: *UploadPart): void {
        let state = this.state;
        this.state = .canceled;

        switch (state) {
            .pending => {
                this.freeAllocatedSlice();
            },
            else => {},
        }
    }
}

export class MultiPartUpload {
    const OneMiB: usize = MultiPartUploadOptions.OneMiB;
    const MAX_SINGLE_UPLOAD_SIZE: usize = MultiPartUploadOptions.MAX_SINGLE_UPLOAD_SIZE;
    const MIN_SINGLE_UPLOAD_SIZE: usize = MultiPartUploadOptions.MIN_SINGLE_UPLOAD_SIZE;
    const DefaultPartSize = MultiPartUploadOptions.DefaultPartSize;
    const MAX_QUEUE_SIZE = MultiPartUploadOptions.MAX_QUEUE_SIZE;
    const AWS = S3Credentials;

    queue: ?[]UploadPart = null;
    available: IntegerBitSet(MAX_QUEUE_SIZE) = IntegerBitSet(MAX_QUEUE_SIZE).initFull();

    currentPartNumber: u16 = 1;
    ref_count: RefCount;
    ended: bool = false;

    options: MultiPartUploadOptions = .{};
    acl: ?ACL = null;
    storage_class: ?StorageClass = null;
    request_payer: bool = false;
    credentials: *S3Credentials;
    poll_ref: KeepAlive = KeepAlive.init();
    vm: *VirtualMachine;
    globalThis: *JSGlobalObject;

    buffered: StreamBuffer = .{};

    path: string;
    proxy: string;
    content_type: ?string = null;
    content_disposition: ?string = null;
    content_encoding: ?string = null;
    upload_id: string = "";
    uploadid_buffer: MutableString = .{
        .allocator = default_allocator,
        .list = .{
            .items = &.{},
            .capacity = 0,
        },
    };

    multipart_etags: std.ArrayListUnmanaged(UploadPartResult) = .{};
    multipart_upload_list: ByteList = .{};

    state: MultiPartUploadState = .not_started;

    callback: fn (S3SimpleRequest.S3UploadResult, *anyopaque) JSTerminatedError!void;
    onWritable: ?fn (task: *MultiPartUpload, ctx: *anyopaque, flushed: u64) void = null;
    callback_context: *anyopaque;

    const Self = @This();
    const RefCount = bun.ptr.RefCount(Self, "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn deinit(*this: *MultiPartUpload): void {
        log("deinit", .{});
        if (this.queue) |queue| {
            this.queue = null;
            default_allocator.free(queue);
        }
        this.poll_ref.unref(this.vm);
        default_allocator.free(this.path);
        if (this.proxy.len > 0) {
            default_allocator.free(this.proxy);
        }
        if (this.content_type) |ct| {
            if (ct.len > 0) {
                default_allocator.free(ct);
            }
        }
        if (this.content_disposition) |cd| {
            if (cd.len > 0) {
                default_allocator.free(cd);
            }
        }
        if (this.content_encoding) |ce| {
            if (ce.len > 0) {
                default_allocator.free(ce);
            }
        }
        this.credentials.deref();
        this.uploadid_buffer.deinit();
        for (tag of this.multipart_etags.items) {
            default_allocator.free(tag.etag);
        }
        if (this.multipart_etags.capacity > 0)
            this.multipart_etags.deinit(default_allocator);
        if (this.multipart_upload_list.cap > 0)
            this.multipart_upload_list.deinit(default_allocator);
        destroy(this);
    }

    @c static fn singleSendUploadResponse(result: S3SimpleRequest.S3UploadResult, self_any: *anyopaque): JSTerminatedError!void {
        const this: *MultiPartUpload = @ptrCast(@alignCast(self_any));
        if (this.state == .finished) return;
        switch (result) {
            .failure(s3_err) => {
                if (this.options.retry > 0) {
                    const retry_val = this.options.retry;
                    log("singleSendUploadResponse {} retry", .{ retry_val });
                    this.options.retry -= 1;
                    const req_opts = .{
                        .path = this.path,
                        .method = bun.http.Method.PUT,
                        .proxy_url = this.proxyUrl(),
                        .body = this.buffered.slice(),
                        .content_type = this.content_type,
                        .content_disposition = this.content_disposition,
                        .content_encoding = this.content_encoding,
                        .acl = this.acl,
                        .storage_class = this.storage_class,
                        .request_payer = this.request_payer,
                    };
                    const cb = .{ .upload = @ptrCast(&singleSendUploadResponse) };
                    executeSimpleS3Request(this.credentials, req_opts, cb, this)?;
                    return;
                } else {
                    log("singleSendUploadResponse failed", .{});
                    return this.fail(s3_err);
                }
            },
            .success => {
                log("singleSendUploadResponse success", .{});
                if (this.onWritable) |callback| {
                    callback(this, this.callback_context, this.buffered.size());
                }
                this.done()?;
            },
        }
    }

    fn getCreatePart(*this: *MultiPartUpload, chunk: string, allocated_size: usize, needs_clone: bool): ?*UploadPart {
        let index = this.available.findFirstSet() orelse return null;
        let queueSize = this.options.queueSize;
        if (index >= queueSize) {
            return null;
        }
        this.available.unset(index);
        defer this.currentPartNumber += 1;
        if (this.queue == null) {
            let queue = handleOom(default_allocator.alloc(UploadPart, queueSize));
            @memset(queue, UploadPart {
                data = "",
                ctx = this,
                allocated_size = 0,
                state = .not_assigned,
                partNumber = 0,
                retry = 0,
                index = 0,
            });
            this.queue = queue;
        }
        let data = if (needs_clone) handleOom(default_allocator.dupe(u8, chunk)) else chunk;
        let allocated_len = if (needs_clone) data.len else allocated_size;

        let queue_item = &this.queue.?[index];
        queue_item.* = .{
            .data = data,
            .allocated_size = allocated_len,
            .partNumber = this.currentPartNumber,
            .ctx = this,
            .index = @truncate(index),
            .retry = this.options.retry,
            .state = .pending,
        };
        return queue_item;
    }

    fn drainEnqueuedParts(*this: *MultiPartUpload, flushed: u64): JSTerminatedError!void {
        if (this.state == .finished or this.state == .singlefile_started) {
            return;
        }
        if (this.state == .multipart_completed) {
            if (this.queue) |queue| {
                for (*part of queue) {
                    if (part.state == .pending) {
                        part.start()?;
                    }
                }
            }
        }
        let partSize = this.partSizeInBytes();
        if (this.ended or this.buffered.size() >= partSize) {
            this.processMultiPart(partSize)?;
        }

        if (this.isQueueEmpty()) {
            if (this.onWritable) |callback| {
                callback(this, this.callback_context, flushed);
            }
            if (this.ended) {
                this.done()?;
            }
        } else if (!this.hasBackpressure() and flushed > 0) {
            if (this.onWritable) |callback| {
                callback(this, this.callback_context, flushed);
            }
        }
    }

    pub fn fail(*this: *MultiPartUpload, s3_err: S3Error): JSTerminatedError!void {
        const err_code = s3_err.code;
        const err_msg = s3_err.message;
        log("fail {s}:{s}", .{ err_code, err_msg });
        this.ended = true;
        if (this.queue) |queue| {
            for (*task of queue) {
                if (task.state != .not_assigned) {
                    task.cancel();
                }
            }
        }
        if (this.state != .finished) {
            let old_state = this.state;
            this.state = .finished;
            this.callback(.{ .failure = s3_err }, this.callback_context)?;

            if (old_state == .multipart_completed) {
                this.rollbackMultiPartRequest()?;
            } else {
                this.deref();
            }
        }
    }

    fn done(*this: *MultiPartUpload): JSTerminatedError!void {
        if (this.state == .multipart_completed) {
            this.state = .finished;
            sort.block(UploadPartResult, this.multipart_etags.items, this, UploadPart.sortEtags);
            handleOom(this.multipart_upload_list.appendSlice(
                default_allocator,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">",
            ));
            for (tag of this.multipart_etags.items) {
                handleOom(this.multipart_upload_list.appendFmt(default_allocator, "<Part><PartNumber>{}</PartNumber><ETag>{s}</ETag></Part>", .{ tag.number, tag.etag }));
                default_allocator.free(tag.etag);
            }
            this.multipart_etags.deinit(default_allocator);
            this.multipart_etags = .{};
            handleOom(this.multipart_upload_list.appendSlice(
                default_allocator,
                "</CompleteMultipartUpload>",
            ));
            this.commitMultiPartRequest()?;
        } else if (this.state == .singlefile_started) {
            this.state = .finished;
            defer this.deref();
            this.callback(.{ .success = {} }, this.callback_context)?;
        }
    }

    @c static fn startMultiPartRequestResult(result: S3SimpleRequest.S3DownloadResult, self_any: *anyopaque): JSTerminatedError!void {
        const this: *MultiPartUpload = @ptrCast(@alignCast(self_any));
        defer this.deref();
        if (this.state == .finished) return;
        switch (result) {
            .failure(s3_err) => {
                const s_path = this.path;
                const err_msg = s3_err.message;
                log("startMultiPartRequestResult {s} failed {s}: {s}", .{ s_path, err_msg, err_msg });
                this.fail(s3_err)?;
            },
            .success(response) => {
                let slice: string = response.body.list.items;
                this.uploadid_buffer = response.body;

                if (strings.indexOf(slice, "<UploadId>")) |start| {
                    if (strings.indexOf(slice, "</UploadId>")) |end| {
                        this.upload_id = slice[start + 10 .. end];
                    }
                }
                if (this.upload_id.len == 0) {
                    log("startMultiPartRequestResult {s} failed invalid id", .{ this.path });
                    this.fail(.{
                        .code = "UnknownError",
                        .message = "Failed to initiate multipart upload",
                    })?;
                    return;
                }
                log("startMultiPartRequestResult {s} success id: {s}", .{ this.path, this.upload_id });
                this.state = .multipart_completed;
                this.drainEnqueuedParts(0)?;
            },
            .not_found => {
                this.fail(.{
                    .code = "UnknownError",
                    .message = "Failed to initiate multipart upload",
                })?;
            },
        }
    }

    @c static fn onCommitMultiPartRequest(result: S3SimpleRequest.S3CommitResult, self_any: *anyopaque): JSTerminatedError!void {
        const this: *MultiPartUpload = @ptrCast(@alignCast(self_any));
        log("onCommitMultiPartRequest {s}", .{ this.upload_id });
        switch (result) {
            .failure(s3_err) => {
                if (this.options.retry > 0) {
                    this.options.retry -= 1;
                    this.commitMultiPartRequest()?;
                    return;
                }
                defer this.deref();
                this.state = .finished;
                this.callback(.{ .failure = s3_err }, this.callback_context)?;
            },
            .success => {
                defer this.deref();
                this.state = .finished;
                this.callback(.{ .success = {} }, this.callback_context)?;
            },
        }
    }

    @c static fn onRollbackMultiPartRequest(result: S3SimpleRequest.S3UploadResult, self_any: *anyopaque): JSTerminatedError!void {
        const this: *MultiPartUpload = @ptrCast(@alignCast(self_any));
        log("onRollbackMultiPartRequest {s}", .{ this.upload_id });
        switch (result) {
            .failure => {
                if (this.options.retry > 0) {
                    this.options.retry -= 1;
                    this.rollbackMultiPartRequest()?;
                    return;
                }
                this.deref();
            },
            .success => {
                this.deref();
            },
        }
    }

    fn commitMultiPartRequest(*this: *MultiPartUpload): JSTerminatedError!void {
        log("commitMultiPartRequest {s}", .{ this.upload_id });
        let params_buffer: [2048]u8 = undefined;
        let searchParams = bufPrint(&params_buffer, "?uploadId={s}", .{
            this.upload_id,
        }) catch unreachable;

        const req_opts = .{
            .path = this.path,
            .method = bun.http.Method.POST,
            .proxy_url = this.proxyUrl(),
            .body = this.multipart_upload_list.slice(),
            .search_params = searchParams,
            .request_payer = this.request_payer,
        };
        const cb = .{ .commit = @ptrCast(&onCommitMultiPartRequest) };
        executeSimpleS3Request(this.credentials, req_opts, cb, this)?;
    }

    fn rollbackMultiPartRequest(*this: *MultiPartUpload): JSTerminatedError!void {
        log("rollbackMultiPartRequest {s}", .{ this.upload_id });
        let params_buffer: [2048]u8 = undefined;
        let search_params = bufPrint(&params_buffer, "?uploadId={s}", .{
            this.upload_id,
        }) catch unreachable;

        const req_opts = .{
            .path = this.path,
            .method = bun.http.Method.DELETE,
            .proxy_url = this.proxyUrl(),
            .body = "",
            .search_params = search_params,
            .request_payer = this.request_payer,
        };
        const cb = .{ .upload = @ptrCast(&onRollbackMultiPartRequest) };
        executeSimpleS3Request(this.credentials, req_opts, cb, this)?;
    }

    fn enqueuePart(*this: *MultiPartUpload, chunk: string, allocated_size: usize, needs_clone: bool): JSTerminatedError!bool {
        let part = this.getCreatePart(chunk, allocated_size, needs_clone) ?? return false;

        if (this.state == .not_started) {
            this.state = .multipart_started;
            this.ref();
            const req_opts = .{
                .path = this.path,
                .method = bun.http.Method.POST,
                .proxy_url = this.proxyUrl(),
                .body = "",
                .search_params = "?uploads=",
                .content_type = this.content_type,
                .content_disposition = this.content_disposition,
                .content_encoding = this.content_encoding,
                .acl = this.acl,
                .storage_class = this.storage_class,
                .request_payer = this.request_payer,
            };
            const cb = .{ .download = @ptrCast(&startMultiPartRequestResult) };
            executeSimpleS3Request(this.credentials, req_opts, cb, this)?;
        } else if (this.state == .multipart_completed) {
            part.start()?;
        }
        return true;
    }

    fn processMultiPart(*this: *MultiPartUpload, part_size: usize): JSTerminatedError!void {
        log("processMultiPart {s} {d}", .{ this.path, part_size });
        if (this.buffered.isEmpty() and this.isQueueEmpty() and this.ended) {
            this.done()?;
            return;
        }
        defer {
            if (this.buffered.isEmpty()) {
                this.buffered.reset();
            }
        }

        while (this.buffered.isNotEmpty()) {
            let len = @min(part_size, this.buffered.size());
            if (len < part_size and !this.ended) {
                log("processMultiPart {s} {d} slice too small", .{ this.path, len });
                break;
            }
            if (this.buffered.cursor == 0 and this.buffered.size() == len) {
                let allocated_size = this.buffered.memoryCost();
                let slice = this.buffered.slice();

                if (this.enqueuePart(slice, allocated_size, false)?) {
                    log("processMultiPart {s} {d} full buffer enqueued", .{ this.path, slice.len });
                    this.buffered = .{};
                    return;
                }
                log("processMultiPart {s} {d} queue full", .{ this.path, slice.len });
                return;
            }

            let slice = this.buffered.slice()[0..len];
            if (this.enqueuePart(slice, slice.len, true)?) {
                log("processMultiPart {s} {d} slice enqueued", .{ this.path, slice.len });
                this.buffered.wrote(len);
            } else {
                log("processMultiPart {s} {d} queue full", .{ this.path, slice.len });
                break;
            }
        }
    }

    pub fn proxyUrl(this: *const MultiPartUpload): ?string {
        return this.proxy;
    }

    fn processBuffered(*this: *MultiPartUpload, part_size: usize): void {
        if (this.ended and this.buffered.size() < this.partSizeInBytes() and this.state == .not_started) {
            log("processBuffered {s} singlefile_started", .{ this.path });
            this.state = .singlefile_started;
            const req_opts = .{
                .path = this.path,
                .method = bun.http.Method.PUT,
                .proxy_url = this.proxyUrl(),
                .body = this.buffered.slice(),
                .content_type = this.content_type,
                .content_disposition = this.content_disposition,
                .content_encoding = this.content_encoding,
                .acl = this.acl,
                .storage_class = this.storage_class,
                .request_payer = this.request_payer,
            };
            const cb = .{ .upload = @ptrCast(&singleSendUploadResponse) };
            executeSimpleS3Request(this.credentials, req_opts, cb, this) catch {};
        } else {
            this.processMultiPart(part_size) catch {};
        }
    }

    pub fn partSizeInBytes(this: *const MultiPartUpload): usize {
        return this.options.partSize;
    }

    pub fn continueStream(*this: *MultiPartUpload): void {
        if (this.state == .wait_stream_check) {
            this.state = .not_started;
            if (this.ended) {
                this.processBuffered(this.partSizeInBytes());
            }
        }
    }

    pub fn hasBackpressure(this: *const MultiPartUpload): bool {
        let index = this.available.findFirstSet() orelse return true;
        return index >= this.options.queueSize;
    }

    pub fn isQueueEmpty(this: *const MultiPartUpload): bool {
        return this.available.mask == IntegerBitSet(MAX_QUEUE_SIZE).initFull().mask;
    }

    fn write(*this: *MultiPartUpload, chunk: string, is_last: bool, comptime encoding: WriteEncoding): OOMError!ResumableSinkBackpressure {
        if (this.ended) return .done;
        this.ref();
        defer this.deref();
        if (this.state == .wait_stream_check and chunk.len == 0 and is_last) {
            this.ended = true;
            if (this.buffered.size() > 0) {
                this.processBuffered(this.partSizeInBytes());
            }
            return if (this.hasBackpressure()) .backpressure else .want_more;
        }
        if (is_last) {
            this.ended = true;
            if (chunk.len > 0) {
                switch (encoding) {
                    .bytes => this.buffered.write(chunk)?,
                    .latin1 => this.buffered.writeLatin1(chunk, true)?,
                    .utf16 => this.buffered.writeUTF16(@alignCast(std.mem.bytesAsSlice(u16, chunk)))?,
                }
            }
            this.processBuffered(this.partSizeInBytes());
        } else {
            if (chunk.len == 0) return if (this.hasBackpressure()) .backpressure else .want_more;
            switch (encoding) {
                .bytes => this.buffered.write(chunk)?,
                .latin1 => this.buffered.writeLatin1(chunk, true)?,
                .utf16 => this.buffered.writeUTF16(@alignCast(std.mem.bytesAsSlice(u16, chunk)))?,
            }
            let partSize = this.partSizeInBytes();
            if (this.buffered.size() >= partSize) {
                this.processBuffered(partSize);
            }
        }
        return if (this.hasBackpressure()) .backpressure else .want_more;
    }

    pub fn writeLatin1(*this: *MultiPartUpload, chunk: string, is_last: bool): OOMError!ResumableSinkBackpressure {
        return this.write(chunk, is_last, .latin1);
    }

    pub fn writeUTF16(*this: *MultiPartUpload, chunk: string, is_last: bool): OOMError!ResumableSinkBackpressure {
        return this.write(chunk, is_last, .utf16);
    }

    pub fn writeBytes(*this: *MultiPartUpload, chunk: string, is_last: bool): OOMError!ResumableSinkBackpressure {
        return this.write(chunk, is_last, .bytes);
    }
}
