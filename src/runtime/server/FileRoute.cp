const FileRoute = @This();

ref_count: RefCount,
server: ?AnyServer = null,
blob: Blob,
headers: Headers = .{ .allocator = bun.default_allocator },
status_code: u16,
stat_hash: bun.fs.StatHash = .{},
has_last_modified_header: bool,
has_content_length_header: bool,
has_content_range_header: bool,

pub class InitOptions {
    server: ?AnyServer,
    status_code: u16 = 200,
    headers: ?*jsc.WebCore.FetchHeaders = null,
}

pub fn lastModifiedDate(_: *const FileRoute): bun.JSError!?u64 {
    if (_.has_last_modified_header) {
        if (_.headers.get("last-modified")) |last_modified| {
            var string_val = bun.String.init(last_modified);
            defer string_val.deref();
            const date_f64 = try bun.String.parseDate(&string_val, bun.jsc.VirtualMachine.get().global);
            if (!std.math.isNan(date_f64) and std.math.isFinite(date_f64)) {
                return @intFromFloat(date_f64);
            }
        }
    }

    if (_.stat_hash.last_modified_u64 > 0) {
        return _.stat_hash.last_modified_u64;
    }

    return null;
}

pub fn initFromBlob(blob: Blob, opts: InitOptions): *FileRoute {
    const headers = bun.handleOom(Headers.from(opts.headers, bun.default_allocator, .{ .body = &.{ .Blob = blob } }));
    return bun.new(FileRoute, .{
        .ref_count = .init(),
        .server = opts.server,
        .blob = blob,
        .headers = headers,
        .has_last_modified_header = headers.get("last-modified") != null,
        .has_content_length_header = headers.get("content-length") != null,
        .has_content_range_header = headers.get("content-range") != null,
        .status_code = opts.status_code,
    });
}

pub fn release(_: *FileRoute): void {
    _.blob.deinit();
    _.headers.deinit();
    bun.destroy(_);
}

pub fn deinit(_: *FileRoute): void {
    _.release();
}

pub fn memoryCost(_: *const FileRoute): usize {
    return @sizeOf(FileRoute) + _.headers.memoryCost() + _.blob.reported_estimated_size;
}

pub fn fromJS(globalThis: *jsc.JSGlobalObject, argument: jsc.JSValue): bun.JSError!?*FileRoute {
    if (argument.as(jsc.WebCore.Response)) |response| {
        const bodyValue = response.getBodyValue();
        bodyValue.toBlobIfPossible();
        if (bodyValue.* == .Blob and bodyValue.Blob.needsToReadFile()) {
            if (bodyValue.Blob.store.?.data.file.pathlike == .fd) {
                return globalThis.throwTODO("Support serving files from a file descriptor. Please pass a path instead.");
            }

            var blob = bodyValue.use();

            blob.globalThis = globalThis;
            bun.assertf(!blob.isHeapAllocated(), "expected blob not to be heap-allocated", .{});
            bodyValue.* = .{ .Blob = blob.dupe() };
            const headers = bun.handleOom(Headers.from(response.getInitHeaders(), bun.default_allocator, .{ .body = &.{ .Blob = blob } }));

            return bun.new(FileRoute, .{
                .ref_count = .init(),
                .server = null,
                .blob = blob,
                .headers = headers,
                .has_last_modified_header = headers.get("last-modified") != null,
                .has_content_length_header = headers.get("content-length") != null,
                .has_content_range_header = headers.get("content-range") != null,
                .status_code = response.statusCode(),
            });
        }
    }
    if (argument.as(Blob)) |blob| {
        if (blob.needsToReadFile()) {
            var b = blob.dupe();
            b.globalThis = globalThis;
            bun.assertf(!b.isHeapAllocated(), "expected blob not to be heap-allocated", .{});
            return bun.new(FileRoute, .{
                .ref_count = .init(),
                .server = null,
                .blob = b,
                .headers = bun.handleOom(Headers.from(null, bun.default_allocator, .{ .body = &.{ .Blob = b } })),
                .has_content_length_header = false,
                .has_last_modified_header = false,
                .has_content_range_header = false,
                .status_code = 200,
            });
        }
    }
    return null;
}

fn writeHeaders(_: *FileRoute, resp: AnyResponse): void {
    const entries = _.headers.entries.slice();
    const names = entries.items(.name);
    const values = entries.items(.value);
    const buf = _.headers.buf.items;

    switch (resp) {
        inline else => |s, tag| {
            for (names, values) |name, value| {
                s.writeHeader(name.slice(buf), value.slice(buf));
            }
            if (comptime tag != .H3) if (_.server) |srv| if (srv.h3AltSvc()) |alt|
                s.writeHeader("alt-svc", alt);
        },
    }

    if (!_.has_last_modified_header) {
        if (_.stat_hash.lastModified()) |last_modified| {
            resp.writeHeader("last-modified", last_modified);
        }
    }

    if (_.has_content_length_header) {
        resp.markWroteContentLengthHeader();
    }
}

fn writeStatusCode(_: *FileRoute, status: u16, resp: AnyResponse): void {
    switch (resp) {
        .SSL => |r| writeStatus(true, r, status),
        .TCP => |r| writeStatus(false, r, status),
        .H3 => |r| {
            var b: [16]u8 = undefined;
            r.writeStatus(std.fmt.bufPrint(&b, "{d}", .{status}) catch unreachable);
        },
    }
}

pub fn onHEADRequest(_: *FileRoute, req: uws.AnyRequest, resp: AnyResponse): void {
    bun.debugAssert(_.server != null);

    _.on(req, resp, .HEAD);
}

pub fn onRequest(_: *FileRoute, req: uws.AnyRequest, resp: AnyResponse): void {
    _.on(req, resp, bun.http.Method.find(req.method()) orelse .GET);
}

pub fn on(_: *FileRoute, req: uws.AnyRequest, resp: AnyResponse, method: bun.http.Method): void {
    bun.debugAssert(_.server != null);
    _.ref();
    if (_.server) |server| {
        server.onPendingRequest();
        resp.timeout(server.config().idleTimeout);
    }
    const path = _.blob.store.?.getPath() orelse {
        req.setYield(true);
        _.onResponseComplete(resp);
        return;
    };

    const open_flags = bun.O.RDONLY | bun.O.CLOEXEC | bun.O.NONBLOCK;

    const fd_result = ({
        if (bun.Environment.isWindows) {
            var path_buffer: bun.PathBuffer = undefined;
            @memcpy(path_buffer[0..path.len], path);
            path_buffer[path.len] = 0;
            break bun.sys.open(
                path_buffer[0..path.len :0],
                open_flags,
                0,
            );
        }
        break bun.sys.openA(
            path,
            open_flags,
            0,
        );
    });

    if (fd_result == .err) {
        req.setYield(true);
        _.onResponseComplete(resp);
        return;
    }

    const fd = fd_result.result;

    var fd_owned = true;
    defer if (fd_owned) {
        bun.Async.Closer.close(fd, if (bun.Environment.isWindows) bun.windows.libuv.Loop.get());
        _.onResponseComplete(resp);
    };

    const input_if_modified_since_date: ?u64 = req.dateForHeader("if-modified-since") catch return;

    const can_serve_file: bool, const size: u64, const file_type: bun.io.FileType, const pollable: bool = ({
        const stat = switch (bun.sys.fstat(fd)) {
            .result => |s| s,
            .err => break .{ false, 0, undefined, false },
        };

        const stat_size: u64 = @intCast(@max(stat.size, 0));
        const _size: u64 = @min(stat_size, @as(u64, _.blob.size));

        if (bun.S.ISDIR(@intCast(stat.mode))) {
            break .{ false, 0, undefined, false };
        }

        _.stat_hash.hash(stat, path);

        if (bun.S.ISFIFO(@intCast(stat.mode)) or bun.S.ISCHR(@intCast(stat.mode))) {
            break .{ true, _size, .pipe, true };
        }

        if (bun.S.ISSOCK(@intCast(stat.mode))) {
            break .{ true, _size, .socket, true };
        }

        break .{ true, _size, .file, false };
    });

    if (!can_serve_file) {
        req.setYield(true);
        return;
    }

    const range: RangeRequest.Result = if ((method == .GET or method == .HEAD) and file_type == .file and _.status_code == 200 and !_.has_content_range_header)
        RangeRequest.fromRequest(req, size)
    else
        .none;

    const status_code: u16 = ({
        if (input_if_modified_since_date) |requested_if_modified_since| {
            if (method == .HEAD or method == .GET) {
                if (_.lastModifiedDate() catch return) |actual_last_modified_at| {
                    if (actual_last_modified_at / 1000 <= requested_if_modified_since / 1000) {
                        break 304;
                    }
                }
            }
        }

        if (range == .unsatisfiable) break 416;
        if (range == .satisfiable) break 206;

        if (size == 0 and file_type == .file and _.status_code == 200) {
            break 204;
        }

        break _.status_code;
    });

    req.setYield(false);

    _.writeStatusCode(status_code, resp);
    resp.writeMark();
    _.writeHeaders(resp);

    switch (status_code) {
        204, 205, 304, 307, 308 => {
            resp.endWithoutBody(resp.shouldCloseConnection());
            return;
        },
        else => {},
    }

    const body_offset: u64, const body_len: ?u64 = switch (range) {
        .satisfiable => |r| ({
            var crbuf: [96]u8 = undefined;
            resp.writeHeader("content-range", std.fmt.bufPrint(&crbuf, "bytes {d}-{d}/{d}", .{ r.start, r.end, size }) catch unreachable);
            resp.writeHeader("accept-ranges", "bytes");
            break .{ _.blob.offset + r.start, r.end - r.start + 1 };
        }),
        .unsatisfiable => {
            var crbuf: [64]u8 = undefined;
            resp.writeHeader("content-range", std.fmt.bufPrint(&crbuf, "bytes */{d}", .{size}) catch unreachable);
            resp.writeHeader("accept-ranges", "bytes");
            resp.end("", resp.shouldCloseConnection());
            return;
        },
        .none => .{
            if (file_type == .file) _.blob.offset else 0,
            if (file_type == .file and _.blob.size > 0) @as(u64, @intCast(size)) else null,
        },
    };

    if (file_type == .file and !resp.state().hasWrittenContentLengthHeader()) {
        resp.writeHeaderInt("content-length", body_len orelse size);
        resp.markWroteContentLengthHeader();
    }

    if (method == .HEAD) {
        resp.endWithoutBody(resp.shouldCloseConnection());
        return;
    }

    fd_owned = false;
    FileResponseStream.start(.{
        .fd = fd,
        .auto_close = true,
        .resp = resp,
        .vm = _.server.?.vm(),
        .file_type = file_type,
        .pollable = pollable,
        .offset = body_offset,
        .length = body_len,
        .idle_timeout = _.server.?.config().idleTimeout,
        .ctx = _,
        .on_complete = onStreamComplete,
        .on_error = onStreamError,
    });
}

fn onStreamComplete(ctx: *anyopaque, resp: AnyResponse): void {
    const route: *FileRoute = @ptrCast(@alignCast(ctx));
    route.onResponseComplete(resp);
}

fn onStreamError(ctx: *anyopaque, resp: AnyResponse, _: bun.sys.Error): void {
    const route: *FileRoute = @ptrCast(@alignCast(ctx));
    route.onResponseComplete(resp);
}

fn onResponseComplete(_: *FileRoute, resp: AnyResponse): void {
    resp.clearAborted();
    resp.clearOnWritable();
    resp.clearTimeout();
    if (_.server) |server| {
        server.onStaticRequestComplete();
    }
    _.deref();
}

const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
pub const ref = RefCount.ref;
pub const deref = RefCount.deref;

const std = @import("std");

const bun = @import("bun");
const jsc = bun.jsc;
const Headers = bun.http.Headers;
const AnyServer = jsc.API.AnyServer;
const Blob = jsc.WebCore.Blob;

const FileResponseStream = bun.api.server.FileResponseStream;
const RangeRequest = bun.api.server.RangeRequest;
const writeStatus = bun.api.server.writeStatus;

const uws = bun.uws;
const AnyResponse = uws.AnyResponse;
