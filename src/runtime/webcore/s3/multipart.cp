const std = @import("std");
const sort = std.sort;
const bun = @import("bun");

import { ACL } from "../../../s3_signing/acl";
import { MultiPartUploadOptions } from "./multipart_options";
import { S3Credentials } from "../../../s3_signing/credentials";
import { S3Error } from "../../../s3_signing/error";
import { StorageClass } from "../../../s3_signing/storage_class";
import * as S3SimpleRequest from "./simple_request";
import { default_allocator, handleOom, destroy, ByteList, MutableString, Output, JSTerminated, JSError, OOM, strings } from "bun";
import { JSGlobalObject, VirtualMachine, WebCore } from "bun:jsc";
import { IntegerBitSet } from "bun:bit_set";
import { StreamBuffer } from "bun:io";
import { KeepAlive } from "bun:Async";
import { ArrayListUnmanaged } from "std:array_list";
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

    fn freeAllocatedSlice(*_) {
        let slice = _.allocatedSlice();
        if (slice.len > 0) {
            default_allocator.free(slice);
        }
        _.data = "";
        _.allocated_size = 0;
    }

    fn allocatedSlice(*_): string {
        if (_.allocated_size > 0) {
            return _.data[0.._.allocated_size];
        }
        return "";
    }

    pub fn onPartResponse(result: S3SimpleRequest.S3PartResult, self: *UploadPart): JSTerminatedError!void {
        if (self.state == .canceled or self.ctx.state == .finished) {
            log("onPartResponse {} canceled", [self.partNumber]);
            self.freeAllocatedSlice();
            self.ctx.deref();
            return;
        }

        self.state = .completed;

        switch (result) {
            .failure s3_err => {
                if (self.retry > 0) {
                    log("onPartResponse {} retry", [self.partNumber]);
                    self.retry -= 1;
                    self.perform()?;
                    return;
                } else {
                    self.state = .not_assigned;
                    log("onPartResponse {} failed", [self.partNumber]);
                    self.freeAllocatedSlice();
                    defer self.ctx.deref();
                    return self.ctx.fail(s3_err);
                }
            },
            .etag etag => {
                log("onPartResponse {} success", [self.partNumber]);
                let sent = self.data.len;
                self.freeAllocatedSlice();
                self.ctx.multipart_etags.append(default_allocator, .{
                    number: self.partNumber,
                    etag: handleOom(default_allocator.dupe(u8, etag)),
                }) catch (oom_err) handleOom(oom_err);
                self.state = .not_assigned;
                defer self.ctx.deref();
                self.ctx.available.set(self.index);
                self.ctx.drainEnqueuedParts(sent)?;
            },
        }
    }

    fn perform(*_): JSTerminatedError!void {
        let params_buffer: [2048]u8 = undefined;
        let search_params = bufPrint(&params_buffer, "?partNumber={}&uploadId={s}&x-id=UploadPart", [
            _.partNumber,
            _.ctx.upload_id,
        ]) catch unreachable;
        executeSimpleS3Request(_.ctx.credentials, .{
            path: _.ctx.path,
            method: .PUT,
            proxy_url: _.ctx.proxyUrl(),
            body: _.data,
            search_params: search_params,
            request_payer: _.ctx.request_payer,
        }, .{ part: @ptrCast(&onPartResponse) }, _)?;
    }

    pub fn start(*_): JSTerminatedError!void {
        if (_.state != .pending or _.ctx.state != .multipart_completed) return;
        _.ctx.ref();
        _.state = .started;
        _.perform()?;
    }

    pub fn cancel(*_) {
        let state = _.state;
        _.state = .canceled;

        switch (state) {
            .pending => {
                _.freeAllocatedSlice();
            },
            else => {},
        }
    }
}

export struct MultiPartUpload {
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
    uploadid_buffer: MutableString = .{ allocator: default_allocator, list: .{} };

    multipart_etags: ArrayListUnmanaged(UploadPartResult) = .{};
    multipart_upload_list: ByteList = .{};

    state: MultiPartUploadState = .not_started;

    callback: fn (S3SimpleRequest.S3UploadResult, *!) JSTerminatedError!void;
    onWritable: ?fn (task: *MultiPartUpload, ctx: *!, flushed: u64) void = null;
    callback_context: *!;

    const Self = @This();
    const RefCount = bun.ptr.RefCount(Self, "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn deinit(*_) {
        log("deinit", .{});
        if (queue := _.queue) {
            _.queue = null;
            default_allocator.free(queue);
        }
        _.poll_ref.unref(_.vm);
        default_allocator.free(_.path);
        if (_.proxy.len > 0) {
            default_allocator.free(_.proxy);
        }
        if (ct := _.content_type) {
            if (ct.len > 0) {
                default_allocator.free(ct);
            }
        }
        if (cd := _.content_disposition) {
            if (cd.len > 0) {
                default_allocator.free(cd);
            }
        }
        if (ce := _.content_encoding) {
            if (ce.len > 0) {
                default_allocator.free(ce);
            }
        }
        _.credentials.deref();
        _.uploadid_buffer.deinit();
        for (tag of _.multipart_etags.items) {
            default_allocator.free(tag.etag);
        }
        if (_.multipart_etags.capacity > 0)
            _.multipart_etags.deinit(default_allocator);
        if (_.multipart_upload_list.cap > 0)
            _.multipart_upload_list.deinit(default_allocator);
        destroy(_);
    }

    pub fn singleSendUploadResponse(result: S3SimpleRequest.S3UploadResult, self: *MultiPartUpload): JSError!void {
        if (self.state == .finished) return;
        switch (result) {
            .failure s3_err => {
                if (self.options.retry > 0) {
                    const retry_val = self.options.retry;
                    log("singleSendUploadResponse {} retry", .{retry_val});
                    self.options.retry -= 1;
                    executeSimpleS3Request(self.credentials, .{
                        path: self.path,
                        method: .PUT,
                        proxy_url: self.proxyUrl(),
                        body: self.buffered.slice(),
                        content_type: self.content_type,
                        content_disposition: self.content_disposition,
                        content_encoding: self.content_encoding,
                        acl: self.acl,
                        storage_class: self.storage_class,
                        request_payer: self.request_payer,
                    }, .{ upload: @ptrCast(&singleSendUploadResponse) }, self)?;
                    return;
                } else {
                    log("singleSendUploadResponse failed", .{});
                    return self.fail(s3_err);
                }
            },
            .success => {
                log("singleSendUploadResponse success", .{});
                if (callback := self.onWritable) {
                    callback(self, self.callback_context, self.buffered.size());
                }
                self.done()?;
            },
        }
    }

    fn getCreatePart(*_, chunk: string, allocated_size: usize, needs_clone: bool): ?*UploadPart {
        let index = _.available.findFirstSet() ?? return null;
        let queueSize = _.options.queueSize;
        if (index >= queueSize) {
            return null;
        }
        _.available.unset(index);
        defer _.currentPartNumber += 1;
        if (_.queue == null) {
            let queue = handleOom(default_allocator.alloc(UploadPart, queueSize));
            @memset(queue, UploadPart {
                data: "",
                ctx: _,
                allocated_size: 0,
                state: .not_assigned,
                partNumber: 0,
                retry: 0,
                index: 0,
            });
            _.queue = queue;
        }
        let data = if (needs_clone) handleOom(default_allocator.dupe(u8, chunk)) else chunk;
        let allocated_len = if (needs_clone) data.len else allocated_size;

        let queue_item = &_.queue.?[index];
        queue_item.* = .{
            data: data,
            allocated_size: allocated_len,
            partNumber: _.currentPartNumber,
            ctx: _,
            index: @truncate(index),
            retry: _.options.retry,
            state: .pending,
        };
        return queue_item;
    }

    fn drainEnqueuedParts(*_, flushed: u64): JSTerminatedError!void {
        if (_.state == .finished or _.state == .singlefile_started) {
            return;
        }
        if (_.state == .multipart_completed) {
            if (queue := _.queue) {
                for (*part of queue) {
                    if (part.state == .pending) {
                        part.start()?;
                    }
                }
            }
        }
        let partSize = _.partSizeInBytes();
        if (_.ended or _.buffered.size() >= partSize) {
            _.processMultiPart(partSize)?;
        }

        if (_.isQueueEmpty()) {
            if (callback := _.onWritable) {
                callback(_, _.callback_context, flushed);
            }
            if (_.ended) {
                _.done()?;
            }
        } else if (!_.hasBackpressure() and flushed > 0) {
            if (callback := _.onWritable) {
                callback(_, _.callback_context, flushed);
            }
        }
    }

    pub fn fail(*_, s3_err: S3Error): JSTerminatedError!void {
        const err_code = s3_err.code;
        const err_msg = s3_err.message;
        log("fail {s}:{s}", .{ err_code, err_msg });
        _.ended = true;
        if (queue := _.queue) {
            for (*task of queue) {
                if (task.state != .not_assigned) {
                    task.cancel();
                }
            }
        }
        if (_.state != .finished) {
            let old_state = _.state;
            _.state = .finished;
            _.callback(.{ failure: s3_err }, _.callback_context)?;

            if (old_state == .multipart_completed) {
                _.rollbackMultiPartRequest()?;
            } else {
                _.deref();
            }
        }
    }

    fn done(*_): JSTerminatedError!void {
        if (_.state == .multipart_completed) {
            _.state = .finished;
            sort.block(UploadPartResult, _.multipart_etags.items, _, UploadPart.sortEtags);
            handleOom(_.multipart_upload_list.appendSlice(
                default_allocator,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">",
            ));
            for (tag of _.multipart_etags.items) {
                handleOom(_.multipart_upload_list.appendFmt(default_allocator, "<Part><PartNumber>{}</PartNumber><ETag>{s}</ETag></Part>", [tag.number, tag.etag]));
                default_allocator.free(tag.etag);
            }
            _.multipart_etags.deinit(default_allocator);
            _.multipart_etags = .{};
            handleOom(_.multipart_upload_list.appendSlice(
                default_allocator,
                "</CompleteMultipartUpload>",
            ));
            _.commitMultiPartRequest()?;
        } else if (_.state == .singlefile_started) {
            _.state = .finished;
            defer _.deref();
            _.callback(.{ success: {} }, _.callback_context)?;
        }
    }

    pub fn startMultiPartRequestResult(result: S3SimpleRequest.S3DownloadResult, self: *MultiPartUpload): JSError!void {
        defer self.deref();
        if (self.state == .finished) return;
        switch (result) {
            .failure s3_err => {
                const s_path = self.path;
                const err_msg = s3_err.message;
                log("startMultiPartRequestResult {s} failed {s}: {s}", .{ s_path, err_msg, err_msg });
                self.fail(s3_err)?;
            },
            .success response => {
                let slice = response.body.list.items;
                self.uploadid_buffer = result.success.body;

                if (start := strings.indexOf(slice, "<UploadId>")) {
                    if (end := strings.indexOf(slice, "</UploadId>")) {
                        self.upload_id = slice[start + 10 .. end];
                    }
                }
                if (self.upload_id.len == 0) {
                    log("startMultiPartRequestResult {s} failed invalid id", [self.path]);
                    self.fail(.{
                        code: "UnknownError",
                        message: "Failed to initiate multipart upload",
                    })?;
                    return;
                }
                log("startMultiPartRequestResult {s} success id: {s}", [self.path, self.upload_id]);
                self.state = .multipart_completed;
                self.drainEnqueuedParts(0)?;
            },
            .not_found => self.fail(.{
                code: "UnknownError",
                message: "Failed to initiate multipart upload",
            })?,
        }
    }

    pub fn onCommitMultiPartRequest(result: S3SimpleRequest.S3CommitResult, self: *MultiPartUpload): JSTerminatedError!void {
        log("onCommitMultiPartRequest {s}", [self.upload_id]);
        switch (result) {
            .failure s3_err => {
                if (self.options.retry > 0) {
                    self.options.retry -= 1;
                    self.commitMultiPartRequest()?;
                    return;
                }
                defer self.deref();
                self.state = .finished;
                self.callback(.{ failure: s3_err }, self.callback_context)?;
            },
            .success => {
                defer self.deref();
                self.state = .finished;
                self.callback(.{ success: {} }, self.callback_context)?;
            },
        }
    }

    pub fn onRollbackMultiPartRequest(result: S3SimpleRequest.S3UploadResult, self: *MultiPartUpload): JSTerminatedError!void {
        log("onRollbackMultiPartRequest {s}", [self.upload_id]);
        switch (result) {
            .failure => {
                if (self.options.retry > 0) {
                    self.options.retry -= 1;
                    self.rollbackMultiPartRequest()?;
                    return;
                }
                self.deref();
            },
            .success => {
                self.deref();
            },
        }
    }

    fn commitMultiPartRequest(*_): JSTerminatedError!void {
        log("commitMultiPartRequest {s}", [_.upload_id]);
        let params_buffer: [2048]u8 = undefined;
        let searchParams = bufPrint(&params_buffer, "?uploadId={s}", [
            _.upload_id,
        ]) catch unreachable;

        executeSimpleS3Request(_.credentials, .{
            path: _.path,
            method: .POST,
            proxy_url: _.proxyUrl(),
            body: _.multipart_upload_list.slice(),
            search_params: searchParams,
            request_payer: _.request_payer,
        }, .{ commit: @ptrCast(&onCommitMultiPartRequest) }, _)?;
    }

    fn rollbackMultiPartRequest(*_): JSTerminatedError!void {
        log("rollbackMultiPartRequest {s}", [_.upload_id]);
        let params_buffer: [2048]u8 = undefined;
        let search_params = bufPrint(&params_buffer, "?uploadId={s}", [
            _.upload_id,
        ]) catch unreachable;

        executeSimpleS3Request(_.credentials, .{
            path: _.path,
            method: .DELETE,
            proxy_url: _.proxyUrl(),
            body: "",
            search_params: search_params,
            request_payer: _.request_payer,
        }, .{ upload: @ptrCast(&onRollbackMultiPartRequest) }, _)?;
    }

    fn enqueuePart(*_, chunk: string, allocated_size: usize, needs_clone: bool): JSTerminatedError!bool {
        let part = _.getCreatePart(chunk, allocated_size, needs_clone) ?? return false;

        if (_.state == .not_started) {
            _.state = .multipart_started;
            _.ref();
            executeSimpleS3Request(_.credentials, .{
                path: _.path,
                method: .POST,
                proxy_url: _.proxyUrl(),
                body: "",
                search_params: "?uploads=",
                content_type: _.content_type,
                content_disposition: _.content_disposition,
                content_encoding: _.content_encoding,
                acl: _.acl,
                storage_class: _.storage_class,
                request_payer: _.request_payer,
            }, .{ download: @ptrCast(&startMultiPartRequestResult) }, _)?;
        } else if (_.state == .multipart_completed) {
            part.start()?;
        }
        return true;
    }

    fn processMultiPart(*_, part_size: usize): JSTerminatedError!void {
        log("processMultiPart {s} {d}", [_.path, part_size]);
        if (_.buffered.isEmpty() and _.isQueueEmpty() and _.ended) {
            _.done()?;
            return;
        }
        defer {
            if (_.buffered.isEmpty()) {
                _.buffered.reset();
            }
        }

        while (_.buffered.isNotEmpty()) {
            let len = @min(part_size, _.buffered.size());
            if (len < part_size and !_.ended) {
                log("processMultiPart {s} {d} slice too small", [_.path, len]);
                break;
            }
            if (_.buffered.cursor == 0 and _.buffered.size() == len) {
                let allocated_size = _.buffered.memoryCost();
                let slice = _.buffered.slice();

                if (_.enqueuePart(slice, allocated_size, false)?) {
                    log("processMultiPart {s} {d} full buffer enqueued", [_.path, slice.len]);
                    _.buffered = .{};
                    return;
                }
                log("processMultiPart {s} {d} queue full", [_.path, slice.len]);
                return;
            }

            let slice = _.buffered.slice()[0..len];
            if (_.enqueuePart(slice, slice.len, true)?) {
                log("processMultiPart {s} {d} slice enqueued", [_.path, slice.len]);
                _.buffered.wrote(len);
            } else {
                log("processMultiPart {s} {d} queue full", [_.path, slice.len]);
                break;
            }
        }
    }

    pub fn proxyUrl(*_): ?string {
        return _.proxy;
    }

    fn processBuffered(*_, part_size: usize) {
        if (_.ended and _.buffered.size() < _.partSizeInBytes() and _.state == .not_started) {
            log("processBuffered {s} singlefile_started", [_.path]);
            _.state = .singlefile_started;
            executeSimpleS3Request(_.credentials, .{
                path: _.path,
                method: .PUT,
                proxy_url: _.proxyUrl(),
                body: _.buffered.slice(),
                content_type: _.content_type,
                content_disposition: _.content_disposition,
                content_encoding: _.content_encoding,
                acl: _.acl,
                storage_class: _.storage_class,
                request_payer: _.request_payer,
            }, .{ upload: @ptrCast(&singleSendUploadResponse) }, _) catch {};
        } else {
            _.processMultiPart(part_size) catch {};
        }
    }

    pub fn partSizeInBytes(*_): usize {
        return _.options.partSize;
    }

    pub fn continueStream(*_) {
        if (_.state == .wait_stream_check) {
            _.state = .not_started;
            if (_.ended) {
                _.processBuffered(_.partSizeInBytes());
            }
        }
    }

    pub fn hasBackpressure(*_): bool {
        let index = _.available.findFirstSet() ?? return true;
        return index >= _.options.queueSize;
    }

    pub fn isQueueEmpty(*_): bool {
        return _.available.mask == IntegerBitSet(MAX_QUEUE_SIZE).initFull().mask;
    }

    fn write(*_, chunk: string, is_last: bool, comptime encoding: WriteEncoding): OOMError!ResumableSinkBackpressure {
        if (_.ended) return .done;
        _.ref();
        defer _.deref();
        if (_.state == .wait_stream_check and chunk.len == 0 and is_last) {
            _.ended = true;
            if (_.buffered.size() > 0) {
                _.processBuffered(_.partSizeInBytes());
            }
            return if (_.hasBackpressure()) .backpressure else .want_more;
        }
        if (is_last) {
            _.ended = true;
            if (chunk.len > 0) {
                switch (encoding) {
                    .bytes => _.buffered.write(chunk)?,
                    .latin1 => _.buffered.writeLatin1(chunk, true)?,
                    .utf16 => _.buffered.writeUTF16(@alignCast(std.mem.bytesAsSlice(u16, chunk)))?,
                }
            }
            _.processBuffered(_.partSizeInBytes());
        } else {
            if (chunk.len == 0) return if (_.hasBackpressure()) .backpressure else .want_more;
            switch (encoding) {
                .bytes => _.buffered.write(chunk)?,
                .latin1 => _.buffered.writeLatin1(chunk, true)?,
                .utf16 => _.buffered.writeUTF16(@alignCast(std.mem.bytesAsSlice(u16, chunk)))?,
            }
            let partSize = _.partSizeInBytes();
            if (_.buffered.size() >= partSize) {
                _.processBuffered(partSize);
            }
        }
        return if (_.hasBackpressure()) .backpressure else .want_more;
    }

    pub fn writeLatin1(*_, chunk: string, is_last: bool): OOMError!ResumableSinkBackpressure {
        return _.write(chunk, is_last, .latin1);
    }

    pub fn writeUTF16(*_, chunk: string, is_last: bool): OOMError!ResumableSinkBackpressure {
        return _.write(chunk, is_last, .utf16);
    }

    pub fn writeBytes(*_, chunk: string, is_last: bool): OOMError!ResumableSinkBackpressure {
        return _.write(chunk, is_last, .bytes);
    }
}
