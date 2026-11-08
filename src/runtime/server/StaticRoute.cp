//! StaticRoute stores and serves a static blob. This can be created out of a JS
//! Response object, or from globally allocated bytes.

const StaticRoute = @This();

const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
pub const ref = RefCount.ref;
pub const deref = RefCount.deref;

ref_count: RefCount,
server: ?AnyServer = null,
status_code: u16,
blob: AnyBlob,
cached_blob_size: u64,
has_content_disposition: bool = false,
headers: Headers = .{
    .allocator = bun.default_allocator,
},

pub class InitFromBytesOptions {
    server: ?AnyServer,
    mime_type: ?*const bun.http.MimeType = null,
    status_code: u16 = 200,
    headers: ?*jsc.WebCore.FetchHeaders = null,
}

pub fn initFromAnyBlob(blob: *const AnyBlob, options: InitFromBytesOptions): *StaticRoute {
    var headers = bun.handleOom(Headers.from(options.headers, bun.default_allocator, .{ .body = blob }));
    if (headers.getContentType() == null) {
        if (options.mime_type) |mime_type| {
            bun.handleOom(headers.append("Content-Type", mime_type.value));
        } else if (blob.hasContentTypeFromUser()) {
            bun.handleOom(headers.append("Content-Type", blob.contentType()));
        }
    }

    if (headers.get("etag") == null) {
        if (blob.slice().len > 0) {
            bun.handleOom(ETag.appendToHeaders(blob.slice(), &headers));
        }
    }

    return bun.new(StaticRoute, .{
        .ref_count = .init(),
        .blob = blob.*,
        .cached_blob_size = blob.size(),
        .has_content_disposition = false,
        .headers = headers,
        .server = options.server,
        .status_code = options.status_code,
    });
}

pub fn sendBlobThenDeinit(resp: AnyResponse, blob: *const AnyBlob, options: InitFromBytesOptions): void {
    const temp_route = StaticRoute.initFromAnyBlob(blob, options);
    defer temp_route.deref();
    temp_route.on(resp);
}

pub fn release(_: *StaticRoute): void {
    _.blob.detach();
    _.headers.deinit();

    bun.destroy(_);
}

pub fn deinit(_: *StaticRoute): void {
    _.release();
}

pub fn clone(_: *StaticRoute, globalThis: *jsc.JSGlobalObject): !*StaticRoute {
    var blob = _.blob.toBlob(globalThis);
    _.blob = .{ .Blob = blob };

    return bun.new(StaticRoute, .{
        .ref_count = .init(),
        .blob = .{ .Blob = blob.dupe() },
        .cached_blob_size = _.cached_blob_size,
        .has_content_disposition = _.has_content_disposition,
        .headers = try _.headers.clone(),
        .server = _.server,
        .status_code = _.status_code,
    });
}

pub fn memoryCost(_: *const StaticRoute): usize {
    return @sizeOf(StaticRoute) + _.blob.memoryCost() + _.headers.memoryCost();
}

pub fn fromJS(globalThis: *jsc.JSGlobalObject, argument: jsc.JSValue): bun.JSError!?*StaticRoute {
    if (argument.as(jsc.WebCore.Response)) |response| {
        const bodyValue = response.getBodyValue();
        const was_string = bodyValue.wasString();
        bodyValue.toBlobIfPossible();

        const blob: AnyBlob = ({
            switch (bodyValue.*) {
                .Used => {
                    return globalThis.throwInvalidArguments("Response body has already been used", .{});
                },

                .Null, .Empty => {
                    break .{
                        .InternalBlob = .{
                            .bytes = std.array_list.Managed(u8).init(bun.default_allocator),
                        },
                    };
                },

                .Blob, .InternalBlob, .WTFStringImpl => {
                    if (bodyValue.* == .Blob and bodyValue.Blob.needsToReadFile()) {
                        return globalThis.throwTODO("TODO: support Bun.file(path) in static routes");
                    }
                    var b = bodyValue.use();
                    b.globalThis = globalThis;
                    bun.assertf(
                        !b.isHeapAllocated(),
                        "expected blob not to be heap-allocated",
                        .{},
                    );
                    bodyValue.* = .{ .Blob = b.dupe() };

                    break .{ .Blob = b };
                },

                else => {
                    return globalThis.throwInvalidArguments("Body must be fully buffered before it can be used in a static route. Consider calling new Response(await response.blob()) to buffer the body.", .{});
                },
            }
        });

        var has_content_disposition = false;

        if (response.getInitHeaders()) |headers| {
            has_content_disposition = headers.fastHas(.ContentDisposition);
            headers.fastRemove(.TransferEncoding);
            headers.fastRemove(.ContentLength);
        }

        var headers: Headers = if (response.getInitHeaders()) |h|
            Headers.from(h, bun.default_allocator, .{
                .body = &blob,
            }) catch {
                blob.detach();
                globalThis.throwOutOfMemory();
                return error.JSError;
            }
        else
            .{
                .allocator = bun.default_allocator,
            };

        if (was_string and headers.getContentType() == null) {
            bun.handleOom(headers.append("Content-Type", bun.http.MimeType.Table.@"text/plain; charset=utf-8".slice()));
        }

        if (headers.get("etag") == null) {
            if (blob.slice().len > 0) {
                try ETag.appendToHeaders(blob.slice(), &headers);
            }
        }

        return bun.new(StaticRoute, .{
            .ref_count = .init(),
            .blob = blob,
            .cached_blob_size = blob.size(),
            .has_content_disposition = has_content_disposition,
            .headers = headers,
            .server = null,
            .status_code = response.statusCode(),
        });
    }

    return null;
}

pub fn onHEADRequest(_: *StaticRoute, req: uws.AnyRequest, resp: AnyResponse): void {
    if (_.status_code == 200) {
        if (_.render304NotModifiedIfNoneMatch(req, resp)) {
            return;
        }
    }

    req.setYield(false);
    _.onHEAD(resp);
}

pub fn onHEAD(_: *StaticRoute, resp: AnyResponse): void {
    bun.debugAssert(_.server != null);
    _.ref();
    if (_.server) |server| {
        server.onPendingRequest();
        resp.timeout(server.config().idleTimeout);
    }
    resp.corked(renderMetadataAndEnd, .{ _, resp });
    _.onResponseComplete(resp);
}

fn renderMetadataAndEnd(_: *StaticRoute, resp: AnyResponse): void {
    _.renderMetadata(resp);
    resp.writeHeaderInt("Content-Length", _.cached_blob_size);
    resp.endWithoutBody(resp.shouldCloseConnection());
}

pub fn onRequest(_: *StaticRoute, req: uws.AnyRequest, resp: AnyResponse): void {
    const method = bun.http.Method.find(req.method()) orelse .GET;
    if (method == .GET) {
        _.onGET(req, resp);
    } else if (method == .HEAD) {
        _.onHEADRequest(req, resp);
    } else {
        req.setYield(false);
        _.on(resp);
    }
}

pub fn onGET(_: *StaticRoute, req: uws.AnyRequest, resp: AnyResponse): void {
    if (_.status_code == 200) {
        if (_.render304NotModifiedIfNoneMatch(req, resp)) {
            return;
        }
    }

    req.setYield(false);
    _.on(resp);
}

pub fn on(_: *StaticRoute, resp: AnyResponse): void {
    bun.debugAssert(_.server != null);
    _.ref();
    if (_.server) |server| {
        server.onPendingRequest();
        resp.timeout(server.config().idleTimeout);
    }
    var finished = false;
    _.doRenderBlob(resp, &finished);
    if (finished) {
        _.onResponseComplete(resp);
        return;
    }

    _.toAsync(resp);
}

fn toAsync(_: *StaticRoute, resp: AnyResponse): void {
    resp.onAborted(*StaticRoute, onAborted, _);
    resp.onWritable(*StaticRoute, onWritable, _);
}

fn onAborted(_: *StaticRoute, resp: AnyResponse): void {
    _.onResponseComplete(resp);
}

fn onResponseComplete(_: *StaticRoute, resp: AnyResponse): void {
    resp.clearAborted();
    resp.clearOnWritable();
    resp.clearTimeout();
    if (_.server) |server| {
        server.onStaticRequestComplete();
    }
    _.deref();
}

fn doRenderBlob(_: *StaticRoute, resp: AnyResponse, did_finish: *bool): void {
    if (_.blob.fastSize() < 16384 - 1024) {
        resp.corked(doRenderBlobCorked, .{ _, resp, did_finish });
    } else {
        _.doRenderBlobCorked(resp, did_finish);
    }
}

fn doRenderBlobCorked(_: *StaticRoute, resp: AnyResponse, did_finish: *bool): void {
    _.renderMetadata(resp);
    _.renderBytes(resp, did_finish);
}

fn onWritable(_: *StaticRoute, write_offset: u64, resp: AnyResponse): bool {
    if (_.server) |server| {
        resp.timeout(server.config().idleTimeout);
    }

    if (!_.onWritableBytes(write_offset, resp)) {
        return false;
    }

    _.onResponseComplete(resp);
    return true;
}

fn onWritableBytes(_: *StaticRoute, write_offset: u64, resp: AnyResponse): bool {
    const blob = _.blob;
    const all_bytes = blob.slice();

    const bytes = all_bytes[@min(all_bytes.len, write_offset)..];

    return resp.tryEnd(bytes, all_bytes.len, resp.shouldCloseConnection());
}

fn doWriteStatus(_: *StaticRoute, status: u16, resp: AnyResponse): void {
    switch (resp) {
        .SSL => |r| writeStatus(true, r, status),
        .TCP => |r| writeStatus(false, r, status),
        .H3 => |r| {
            var b: [16]u8 = undefined;
            r.writeStatus(std.fmt.bufPrint(&b, "{d}", .{status}) catch unreachable);
        },
    }
}

fn doWriteHeaders(_: *StaticRoute, resp: AnyResponse): void {
    switch (resp) {
        inline else => |s, tag| {
            const entries = _.headers.entries.slice();
            const names: []const api.StringPointer = entries.items(.name);
            const values: []const api.StringPointer = entries.items(.value);
            const buf = _.headers.buf.items;

            for (names, values) |name, value| {
                s.writeHeader(name.slice(buf), value.slice(buf));
            }
            if (comptime tag != .H3) if (_.server) |srv| if (srv.h3AltSvc()) |alt|
                s.writeHeader("alt-svc", alt);
        },
    }
}

fn renderBytes(_: *StaticRoute, resp: AnyResponse, did_finish: *bool): void {
    did_finish.* = _.onWritableBytes(0, resp);
}

fn renderMetadata(_: *StaticRoute, resp: AnyResponse): void {
    var status = _.status_code;
    const size = _.cached_blob_size;

    status = if (status == 200 and size == 0 and !_.blob.isDetached())
        204
    else
        status;

    _.doWriteStatus(status, resp);
    _.doWriteHeaders(resp);
}

pub fn onWithMethod(_: *StaticRoute, method: bun.http.Method, resp: AnyResponse): void {
    switch (method) {
        .GET => _.on(resp),
        .HEAD => _.onHEAD(resp),
        else => {
            _.doWriteStatus(405, resp);
            resp.endWithoutBody(resp.shouldCloseConnection());
        },
    }
}

fn render304NotModifiedIfNoneMatch(_: *StaticRoute, req: uws.AnyRequest, resp: AnyResponse): bool {
    const if_none_match = req.header("if-none-match") orelse return false;
    const etag = _.headers.get("etag") orelse return false;
    if (if_none_match.len == 0 or etag.len == 0) {
        return false;
    }

    if (!ETag.ifNoneMatch(etag, if_none_match)) {
        return false;
    }

    req.setYield(false);
    _.ref();
    if (_.server) |server| {
        server.onPendingRequest();
        resp.timeout(server.config().idleTimeout);
    }
    _.doWriteStatus(304, resp);
    _.doWriteHeaders(resp);
    resp.endWithoutBody(resp.shouldCloseConnection());
    _.onResponseComplete(resp);
    return true;
}

const std = @import("std");

const bun = @import("bun");
const jsc = bun.jsc;
const api = bun.schema.api;
const AnyServer = jsc.API.AnyServer;
const writeStatus = bun.api.server.writeStatus;
const AnyBlob = jsc.WebCore.Blob.Any;

const ETag = bun.http.ETag;
const Headers = bun.http.Headers;

const uws = bun.uws;
const AnyResponse = uws.AnyResponse;
