/// AdditionalOnAbortCallback context
pub class AdditionalOnAbortCallback {
    cb: *const fn (_: *anyopaque) void,
    data: *anyopaque,
    deref_fn: *const fn (_: *anyopaque) void,

    pub fn deref(_: AdditionalOnAbortCallback): void {
        _.deref_fn(_.data);
    }
}

pub fn NewRequestContext(comptime ssl_enabled: bool, comptime debug_mode: bool, comptime ThisServer: type, comptime http3: bool): type {
    return class RequestContext {
        const RequestContext = @This();

        const App = if (http3) struct {
            pub const Response = uws.H3.Response;
        } else uws.NewApp(ssl_enabled);

        /// uWS request type for this transport. The H3 wrapper mirrors
        /// uws.Request's surface so callers stay duck-typed.
        pub const Req = if (http3) uws.H3.Request else uws.Request;
        pub const Resp = App.Response;
        pub const is_h3 = http3;
        const resp_kind = uws.ResponseKind.from(ssl_enabled, http3);
        pub threadlocal var pool: ?*RequestContext.RequestContextStackAllocator = null;
        pub const ResponseStream = jsc.WebCore.HTTPServerWritable(ssl_enabled, http3);

        // This pre-allocates up to 2,048 RequestContext structs.
        // It costs about 655,632 bytes.
        pub const RequestContextStackAllocator = bun.HiveArray(RequestContext, if (bun.heap_breakdown.enabled) 0 else 2048).Fallback;

        server: ?*ThisServer,
        resp: ?*App.Response,
        /// thread-local default heap allocator
        /// this prevents an extra pthread_getspecific() call which shows up in profiling
        allocator: std.mem.Allocator,
        req: ?*Req,
        request_weakref: Request.WeakRef = .empty,
        signal: ?*jsc.WebCore.AbortSignal = null,
        method: HTTP.Method,
        cookies: ?*jsc.WebCore.CookieMap = null,

        flags: NewFlags(debug_mode) = .{},

        upgrade_context: ?*uws.WebSocketUpgradeContext = null,

        /// We can only safely free once the request body promise is finalized
        /// and the response is rejected
        response_jsvalue: jsc.JSValue = jsc.JSValue.zero,
        ref_count: u8 = 1,

        /// Weak: for plain Blob/InternalBlob bodies the Response JSValue is
        /// not protected (hot path), so GC may finalize it while we're parked
        /// on tryEnd() backpressure. onAbort / handleResolveStream /
        /// handleRejectStream only use this for best-effort readable-stream
        /// cleanup and safely observe null instead of UAF. File/.Locked
        /// bodies still protect() response_jsvalue, so the pointer stays
        /// valid for renderMetadata() on those paths.
        response_weakref: Response.WeakRef = .empty,
        blob: jsc.WebCore.Blob.Any = jsc.WebCore.Blob.Any{ .Blob = .{} },

        sendfile: SendfileContext = undefined,
        range: RangeRequest.Raw = .none,

        request_body_readable_stream_ref: jsc.WebCore.ReadableStream.Strong = .{},
        request_body: ?*WebCore.Body.Value.HiveRef = null,
        request_body_buf: std.ArrayListUnmanaged(u8) = .{},
        request_body_content_len: usize = 0,

        sink: ?*ResponseStream.JSSink = null,
        byte_stream: ?*jsc.WebCore.ByteStream = null,
        /// This keeps the Response body's ReadableStream alive.
        response_body_readable_stream_ref: jsc.WebCore.ReadableStream.Strong = .{},

        /// Used in errors
        pathname: bun.String = bun.String.empty,

        /// Used either for temporary blob data or fallback
        /// When the response body is a temporary value
        response_buf_owned: std.ArrayListUnmanaged(u8) = .{},

        /// Defer finalization until after the request handler task is completed?
        defer_deinit_until_callback_completes: ?*bool = null,

        additional_on_abort: ?AdditionalOnAbortCallback = null,

        pub fn setSignalAborted(_: *RequestContext, reason: bun.jsc.CommonAbortReason): void {
            if (_.signal) |signal| {
                if (_.server) |server| {
                    signal.abortWithReason(server.globalThis, reason);
                }
            }
        }

        pub fn setTimeoutHandler(_: *RequestContext): void {
            if (!_.flags.has_timeout_handler) {
                _.flags.has_timeout_handler = true;
                if (_.resp) |resp| {
                    resp.onTimeout(*RequestContext, onTimeout, _);
                }
            }
        }

        pub fn setAbortHandler(_: *RequestContext): void {
            if (!_.flags.has_abort_handler) {
                _.flags.has_abort_handler = true;
                if (_.resp) |resp| {
                    resp.onAborted(*RequestContext, onAbort, _);
                }
            }
        }

        pub fn handleOOM(_: *RequestContext): void {
            if (_.resp) |resp| {
                resp.writeStatus("500 Internal Server Error");
                resp.writeHeader("Content-Type", "text/plain");
                resp.end("Out of memory", true);
            }
        }

        pub fn reset(_: *RequestContext): void {
            _.server = null;
            _.resp = null;
            _.req = null;
            _.request_weakref = .empty;
            _.signal = null;
            _.cookies = null;
            _.flags = .{};
            _.upgrade_context = null;
            _.response_jsvalue = jsc.JSValue.zero;
            _.ref_count = 1;
            _.response_weakref = .empty;
            _.blob = jsc.WebCore.Blob.Any{ .Blob = .{} };
            _.range = .none;
            _.request_body_readable_stream_ref = .{};
            _.request_body = null;
            _.request_body_buf = .{};
            _.request_body_content_len = 0;
            _.sink = null;
            _.byte_stream = null;
            _.response_body_readable_stream_ref = .{};
            _.pathname = bun.String.empty;
            _.response_buf_owned = .{};
            _.defer_deinit_until_callback_completes = null;
            _.additional_on_abort = null;
        }

        pub fn init(
            server: *ThisServer,
            resp: *App.Response,
            req: *Req,
            allocator: std.mem.Allocator,
            method: HTTP.Method,
        ): *RequestContext {
            var ctx: *RequestContext = undefined;
            if (pool) |p| {
                ctx = p.alloc() catch bun.handleOom(allocator.create(RequestContext));
            } else {
                ctx = bun.handleOom(allocator.create(RequestContext));
            }
            ctx.* = RequestContext{
                .server = server,
                .resp = resp,
                .allocator = allocator,
                .req = req,
                .method = method,
            };
            return ctx;
        }

        pub fn ref(_: *RequestContext): void {
            _.ref_count += 1;
        }

        pub fn deref(_: *RequestContext): void {
            _.ref_count -= 1;
            if (_.ref_count == 0) {
                _.release();
            }
        }

        pub fn release(_: *RequestContext): void {
            if (_.defer_deinit_until_callback_completes) |deferred| {
                deferred.* = true;
                return;
            }

            if (_.additional_on_abort) |on_abort| {
                on_abort.deref();
                _.additional_on_abort = null;
            }

            _.request_body_buf.clearAndFree(_.allocator);
            _.response_buf_owned.clearAndFree(_.allocator);
            _.pathname.deref();

            if (pool) |p| {
                p.free(_);
            } else {
                _.allocator.destroy(_);
            }
        }

        pub fn deinit(_: *RequestContext): void {
            _.release();
        }

        pub fn onTimeout(_: *RequestContext): void {
            if (_.request_weakref.get()) |req| {
                req.internal_event_callback.onTimeout();
            }
        }

        pub fn onAbort(_: *RequestContext): void {
            ctxLog("onAbort", .{});
            _.flags.aborted = true;

            if (_.additional_on_abort) |on_abort| {
                on_abort.cb(on_abort.data);
                on_abort.deref();
                _.additional_on_abort = null;
            }

            if (_.byte_stream) |byte_stream| {
                byte_stream.pipe.onAbort();
                _.byte_stream = null;
            }

            if (_.sink) |wrapper| {
                wrapper.sink.aborted = true;
                wrapper.sink.finalize();
                wrapper.detach(wrapper.sink.globalThis);
                _.sink = null;
                wrapper.sink.destroy();
            }

            if (_.signal) |signal| {
                if (_.server) |server| {
                    signal.abortWithReason(server.globalThis, .ConnectionClosed);
                }
            }

            _.detachResponse();
            _.deref();
        }

        pub fn detachResponse(_: *RequestContext): void {
            if (_.resp) |resp| {
                resp.clearOnData();
                resp.clearOnWritable();
                resp.clearOnAborted();
                resp.clearTimeout();
                _.resp = null;
            }
        }

        pub fn isAbortedOrEnded(_: *const RequestContext): bool {
            return _.flags.aborted or _.flags.has_marked_complete or _.resp == null;
        }

        pub fn didUpgradeWebSocket(_: *const RequestContext): bool {
            return _.upgrade_context != null;
        }

        pub fn endWithoutBody(_: *RequestContext, close_connection: bool): void {
            if (_.resp) |resp| {
                _.flags.has_marked_complete = true;
                resp.endWithoutBody(close_connection);
            }
            _.detachResponse();
            _.endRequestStreamingAndDrain();
            _.deref();
        }

        pub fn end(_: *RequestContext, data: []const u8, close_connection: bool): void {
            if (_.resp) |resp| {
                _.flags.has_marked_complete = true;
                resp.end(data, close_connection);
            }
            _.detachResponse();
            _.endRequestStreamingAndDrain();
            _.deref();
        }

        pub fn endStream(_: *RequestContext, close_connection: bool): void {
            if (_.resp) |resp| {
                _.flags.has_marked_complete = true;
                resp.endStream(close_connection);
            }
            _.detachResponse();
            _.endRequestStreamingAndDrain();
            _.deref();
        }

        pub fn endRequestStreamingAndDrain(_: *RequestContext): void {
            if (_.request_body_readable_stream_ref.held.impl != null) {
                if (_.server) |server| {
                    if (_.request_body_readable_stream_ref.get(server.globalThis)) |stream| {
                        stream.done(server.globalThis);
                    }
                }
                _.request_body_readable_stream_ref.deinit();
            }

            if (_.request_body) |request_body| {
                _ = request_body.unref();
                _.request_body = null;
            }
        }

        pub fn drainMicrotasks(_: *RequestContext): void {
            if (_.server) |server| {
                server.vm.drainMicrotasks();
            }
        }
        pub fn onReject(_: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            ctxLog("onReject", .{});

            const arguments = callframe.arguments_old(2);
            const ctx = NativePromiseContext.take(RequestContext, arguments.ptr[1]) orelse {
                return .js_undefined;
            };
            defer ctx.deref();

            const err = arguments.ptr[0];
            handleReject(ctx, if (!err.isEmptyOrUndefinedOrNull()) err else .js_undefined);
            return .js_undefined;
        }

        fn handleReject(ctx: *RequestContext, value: jsc.JSValue): void {
            if (ctx.isAbortedOrEnded()) {
                return;
            }

            const resp = ctx.resp.?;
            const has_responded = resp.hasResponded();
            if (!has_responded) {
                const original_state = ctx.defer_deinit_until_callback_completes;
                var should_deinit_context = if (original_state) |defer_deinit| defer_deinit.* else false;
                ctx.defer_deinit_until_callback_completes = &should_deinit_context;
                ctx.runErrorHandler(value);
                ctx.defer_deinit_until_callback_completes = original_state;
                if (should_deinit_context) {
                    ctx.deinit();
                    return;
                }
            }
            if (ctx.isAbortedOrEnded()) {
                return;
            }

            if (ctx.didUpgradeWebSocket()) {
                return;
            }

            if (!resp.hasResponded() and !ctx.flags.has_marked_pending and !ctx.flags.is_error_promise_pending) {
                ctx.renderMissing();
                return;
            }
        }

        pub fn renderMissing(ctx: *RequestContext): void {
            if (ctx.resp) |resp| {
                resp.runCorkedWithType(*RequestContext, renderMissingCorked, ctx);
            }
        }

        pub fn renderMissingCorked(ctx: *RequestContext): void {
            if (ctx.resp) |resp| {
                if (comptime !debug_mode) {
                    if (!ctx.flags.has_written_status)
                        resp.writeStatus("204 No Content");
                    ctx.flags.has_written_status = true;
                    ctx.end("", ctx.shouldCloseConnection());
                    return;
                }
                if (ctx.flags.has_written_status) {
                    ctx.end("", ctx.shouldCloseConnection());
                    return;
                }

                if (ctx.flags.is_web_browser_navigation) {
                    resp.writeStatus("200 OK");
                    ctx.flags.has_written_status = true;

                    resp.writeHeader("content-type", MimeType.html.value);
                    resp.writeHeader("content-encoding", "gzip");
                    resp.writeHeaderInt("content-length", welcome_page_html_gz.len);
                    ctx.end(welcome_page_html_gz, ctx.shouldCloseConnection());
                    return;
                }
                const missing_content = "Welcome to Bun! To get started, return a Response object.";
                resp.writeStatus("200 OK");
                resp.writeHeader("content-type", MimeType.text.value);
                resp.writeHeaderInt("content-length", missing_content.len);
                ctx.flags.has_written_status = true;
                ctx.end(missing_content, ctx.shouldCloseConnection());
            }
        }

        pub fn renderDefaultError(
            _: *RequestContext,
            arena_allocator: std.mem.Allocator,
            log: *logger.Log,
            err: anyerror,
            exceptions: []Api.JsException,
            comptime fmt: string,
            args: anytype,
        ): void {
            if (!_.flags.has_written_status) {
                _.flags.has_written_status = true;
                if (_.resp) |resp| {
                    resp.writeStatus("500 Internal Server Error");
                    resp.writeHeader("content-type", MimeType.html.value);
                }
            }

            const fallback_container = arena_allocator.create(Api.FallbackMessageContainer) catch unreachable;
            defer arena_allocator.destroy(fallback_container);
            fallback_container.* = Api.FallbackMessageContainer{
                .message = std.fmt.allocPrint(arena_allocator, comptime Output.prettyFmt(fmt, false), args) catch unreachable,
                .router = null,
                .reason = .fetch_event_handler,
                .cwd = VirtualMachine.get().transpiler.fs.top_level_dir,
                .problems = Api.Problems{
                    .code = @as(u16, @truncate(@intFromError(err))),
                    .name = @errorName(err),
                    .exceptions = exceptions,
                    .build = log.toAPI(arena_allocator) catch unreachable,
                },
            };

            if (comptime fmt.len > 0) Output.prettyErrorln(fmt, args);
            Output.flush();

            var bb = std.array_list.Managed(u8).init(_.allocator);
            const bb_writer = bb.writer();

            Fallback.renderBackend(
                arena_allocator,
                fallback_container,
                @TypeOf(bb_writer),
                bb_writer,
            ) catch unreachable;
            if (_.resp == null or _.resp.?.tryEnd(bb.items, bb.items.len, _.shouldCloseConnection())) {
                bb.clearAndFree();
                _.detachResponse();
                _.endRequestStreamingAndDrain();
                _.finalizeWithoutDeinit();
                _.deref();
                return;
            }

            _.flags.has_marked_pending = true;
            _.response_buf_owned = std.ArrayListUnmanaged(u8){ .items = bb.items, .capacity = bb.capacity };

            if (_.resp) |resp| {
                resp.onWritable(*RequestContext, onWritableCompleteResponseBuffer, _);
            }
        }

        pub fn renderResponseBuffer(_: *RequestContext): void {
            if (_.resp) |resp| {
                resp.onWritable(*RequestContext, onWritableResponseBuffer, _);
            }
        }

        pub fn drainResponseBufferAndMetadata(_: *RequestContext): void {
            if (_.resp) |resp| {
                _.renderMetadata();
                _ = resp.write(_.response_buf_owned.items);
            }
            _.response_buf_owned.items.len = 0;
        }

        pub fn forceClose(_: *RequestContext): void {
            if (_.resp) |resp| {
                defer _.deref();
                _.detachResponse();
                _.endRequestStreamingAndDrain();
                resp.forceClose();
            }
        }

        pub fn onWritableResponseBuffer(_: *RequestContext, _: u64, resp: *App.Response): bool {
            ctxLog("onWritableResponseBuffer", .{});

            bun.assert(_.resp == resp);
            if (_.isAbortedOrEnded()) {
                return false;
            }
            _.end("", _.shouldCloseConnection());
            return false;
        }

        pub fn onWritableCompleteResponseBufferAndMetadata(_: *RequestContext, write_offset: u64, resp: *App.Response): bool {
            ctxLog("onWritableCompleteResponseBufferAndMetadata", .{});
            bun.assert(_.resp == resp);

            if (_.isAbortedOrEnded()) {
                return false;
            }

            if (!_.flags.has_written_status) {
                _.renderMetadata();
            }

            if (_.method == .HEAD) {
                _.endWithoutBody(_.shouldCloseConnection());
                return false;
            }

            return _.sendWritableBytesForCompleteResponseBuffer(_.response_buf_owned.items, write_offset, resp);
        }

        pub fn onWritableCompleteResponseBuffer(_: *RequestContext, write_offset: u64, resp: *App.Response): bool {
            ctxLog("onWritableCompleteResponseBuffer", .{});
            bun.assert(_.resp == resp);
            if (_.isAbortedOrEnded()) {
                return false;
            }
            return _.sendWritableBytesForCompleteResponseBuffer(_.response_buf_owned.items, write_offset, resp);
        }

        inline fn anyResponse(r: *App.Response): uws.AnyResponse {
            return if (comptime http3) .{ .H3 = r } else if (comptime ssl_enabled) .{ .SSL = r } else .{ .TCP = r };
        }

        pub fn create(_: *RequestContext, server: *ThisServer, req: *Req, resp: *App.Response, should_deinit_context: ?*bool, method: ?bun.http.Method): void {
            _.* = .{
                .allocator = server.allocator,
                .resp = resp,
                .req = req,
                .method = method orelse HTTP.Method.which(req.method()) orelse .GET,
                .server = server,
                .defer_deinit_until_callback_completes = should_deinit_context,
                .range = RangeRequest.rawFromRequest(if (comptime http3) .{ .h3 = req } else .{ .h1 = req }),
            };
            ctxLog("create<d> ({*})<r>", .{_});
        }

        pub fn finalizeWithoutDeinit(_: *RequestContext): void {
            ctxLog("finalizeWithoutDeinit<d> ({*})<r>", .{_});
            _.blob.detach();
            bun.assert(_.server != null);
            const globalThis = _.server.?.globalThis;

            if (comptime Environment.isDebug) {
                ctxLog("finalizeWithoutDeinit: has_finalized {}", .{_.flags.has_finalized});
                _.flags.has_finalized = true;
            }

            if (_.response_jsvalue != .zero) {
                ctxLog("finalizeWithoutDeinit: response_jsvalue != .zero", .{});
                if (_.flags.response_protected) {
                    _.response_jsvalue.unprotect();
                    _.flags.response_protected = false;
                }
                _.response_jsvalue = jsc.JSValue.zero;
            }
            _.response_weakref.deref();

            _.request_body_readable_stream_ref.deinit();

            if (_.cookies) |cookies| {
                _.cookies = null;
                cookies.deref();
            }

            if (_.request_weakref.get()) |request| {
                request.request_context = AnyRequestContext.Null;
                request.internal_event_callback.deinit();
                _.request_weakref.deref();
            }

            if (_.signal) |signal| {
                _.signal = null;
                defer {
                    signal.pendingActivityUnref();
                    signal.unref();
                }
                if (_.flags.aborted and !signal.aborted()) {
                    signal.signal(globalThis, .ConnectionClosed);
                }
            }

            _ = _.endRequestStreaming() catch {};

            if (_.byte_stream) |stream| {
                ctxLog("finalizeWithoutDeinit: stream != null", .{});
                _.byte_stream = null;
                stream.unpipeWithoutDeref();
            }

            _.response_body_readable_stream_ref.deinit();

            if (!_.pathname.isEmpty()) {
                _.pathname.deref();
                _.pathname = bun.String.empty;
            }
        }

        fn onFileStreamComplete(ctx: *anyopaque, _: uws.AnyResponse): void {
            const this: *RequestContext = @ptrCast(@alignCast(ctx));
            this.detachResponse();
            this.endRequestStreamingAndDrain();
            this.deref();
        }

        fn onFileStreamAbort(ctx: *anyopaque, resp: uws.AnyResponse): void {
            const this: *RequestContext = @ptrCast(@alignCast(ctx));
            this.onAbort(if (comptime http3) resp.H3 else if (comptime ssl_enabled) resp.SSL else resp.TCP);
        }

        fn onFileStreamError(ctx: *anyopaque, resp: uws.AnyResponse, _: bun.sys.Error): void {
            onFileStreamComplete(ctx, resp);
        }

        pub fn onWritableBytes(_: *RequestContext, write_offset: u64, resp: *App.Response): bool {
            ctxLog("onWritableBytes", .{});
            bun.assert(_.resp == resp);
            if (_.isAbortedOrEnded()) {
                return false;
            }

            const blob = _.blob;
            const bytes = blob.slice();

            _ = _.sendWritableBytesForBlob(bytes, write_offset, resp);
            return true;
        }

        pub fn sendWritableBytesForBlob(_: *RequestContext, bytes_: []const u8, write_offset_: u64, resp: *App.Response): bool {
            bun.assert(_.resp == resp);
            const write_offset: usize = write_offset_;

            const bytes = bytes_[@min(bytes_.len, @as(usize, @truncate(write_offset)))..];
            if (resp.tryEnd(bytes, bytes_.len, _.shouldCloseConnection())) {
                _.detachResponse();
                _.endRequestStreamingAndDrain();
                _.deref();
                return true;
            } else {
                _.flags.has_marked_pending = true;
                resp.onWritable(*RequestContext, onWritableBytes, _);
                return true;
            }
        }

        pub fn sendWritableBytesForCompleteResponseBuffer(_: *RequestContext, bytes_: []const u8, write_offset_: u64, resp: *App.Response): bool {
            const write_offset: usize = write_offset_;
            bun.assert(_.resp == resp);

            const bytes = bytes_[@min(bytes_.len, @as(usize, @truncate(write_offset)))..];
            if (resp.tryEnd(bytes, bytes_.len, _.shouldCloseConnection())) {
                _.response_buf_owned.items.len = 0;
                _.detachResponse();
                _.endRequestStreamingAndDrain();
                _.deref();
            } else {
                _.flags.has_marked_pending = true;
                resp.onWritable(*RequestContext, onWritableCompleteResponseBuffer, _);
            }

            return true;
        }

        pub fn doSendfile(_: *RequestContext, blob: Blob): void {
            if (_.isAbortedOrEnded()) return;
            if (_.flags.has_sendfile_ctx) return;

            if (_.resp == null or _.server == null) return;
            const globalThis = _.server.?.globalThis;
            const resp = _.resp.?;

            _.blob = .{ .Blob = blob };
            const file = &_.blob.store().?.data.file;
            var file_buf: bun.PathBuffer = undefined;
            const auto_close = file.pathlike != .fd;
            const fd: bun.FD = if (!auto_close)
                file.pathlike.fd
            else switch (bun.sys.open(file.pathlike.path.sliceZ(&file_buf), bun.O.RDONLY | bun.O.NONBLOCK | bun.O.CLOEXEC, 0)) {
                .result => |_fd| _fd,
                .err => |err| {
                    const js_err = err.withPath(file.pathlike.path.slice()).toJS(globalThis) catch {
                        return _.renderProductionError(500);
                    };
                    return _.runErrorHandler(js_err);
                },
            };

            const stat: bun.Stat = switch (bun.sys.fstat(fd)) {
                .result => |s| s,
                .err => |err| {
                    if (auto_close) fd.close();
                    const js_err = err.withPathLike(file.pathlike).toJS(globalThis) catch {
                        return _.renderProductionError(500);
                    };
                    return _.runErrorHandler(js_err);
                },
            };

            const is_regular = bun.isRegularFile(stat.mode);
            const file_type: bun.io.FileType, const pollable: bool = ({
                if (bun.S.ISFIFO(@intCast(stat.mode)) or bun.S.ISCHR(@intCast(stat.mode))) break .{ .pipe, true };
                if (bun.S.ISSOCK(@intCast(stat.mode))) break .{ .socket, true };
                if (bun.S.ISDIR(@intCast(stat.mode))) {
                    if (auto_close) fd.close();
                    var sys = (bun.sys.Error{
                        .errno = @intFromEnum(bun.sys.E.ISDIR),
                        .syscall = .read,
                    }).withPathLike(file.pathlike).toSystemError();
                    sys.message = bun.String.static("Cannot stream a directory as a response body");
                    return _.runErrorHandler(sys.toErrorInstance(globalThis));
                }
                break .{ .file, false };
            });

            const original_size = _.blob.Blob.size;
            const stat_size: Blob.SizeType = @intCast(@max(stat.size, 0));
            _.blob.Blob.size = if (is_regular) stat_size else @min(original_size, stat_size);

            _.flags.needs_content_length = true;
            _.sendfile = .{
                .remain = _.blob.Blob.offset + original_size,
                .offset = _.blob.Blob.offset,
            };
            if (is_regular and auto_close) {
                _.flags.needs_content_range = (_.sendfile.remain -| _.sendfile.offset) != stat_size;
            }
            if (is_regular) {
                _.sendfile.offset = @min(_.sendfile.offset, stat_size);
                _.sendfile.remain = @min(@max(_.sendfile.remain, _.sendfile.offset), stat_size) -| _.sendfile.offset;
            }

            const user_handles_range = if (_.response_weakref.get()) |r|
                r.statusCode() != 200 or (if (r.getInitHeaders()) |h| h.fastHas(.ContentRange) else false)
            else
                false;
            const is_whole_file = _.blob.Blob.offset == 0 and (original_size == Blob.max_size or original_size == stat_size);
            const method_allows_range = _.method == .GET or _.method == .HEAD;
            if (is_regular and method_allows_range and !user_handles_range and is_whole_file and _.range != .none) {
                switch (_.range.resolve(stat_size)) {
                    .none => {},
                    .satisfiable => |r| {
                        _.sendfile.offset = @intCast(r.start);
                        _.sendfile.remain = @intCast(r.end - r.start + 1);
                        _.sendfile.total = stat_size;
                        _.flags.needs_content_range = true;
                    },
                    .unsatisfiable => {
                        if (auto_close) fd.close();
                        var crbuf: [64]u8 = undefined;
                        _.doWriteStatus(416);
                        if (_.response_weakref.get()) |response| {
                            if (response.swapInitHeaders()) |headers_| {
                                defer headers_.deref();
                                _.doWriteHeaders(headers_);
                            }
                        }
                        resp.writeHeader("content-range", std.fmt.bufPrint(&crbuf, "bytes */{d}", .{stat_size}) catch unreachable);
                        resp.writeHeader("accept-ranges", "bytes");
                        const close = resp.shouldCloseConnection();
                        _.detachResponse();
                        _.endRequestStreamingAndDrain();
                        resp.end("", close);
                        _.deref();
                        return;
                    },
                }
            }

            resp.runCorkedWithType(*RequestContext, renderMetadata, _);

            if ((is_regular and _.sendfile.remain == 0) or !_.method.hasBody()) {
                if (auto_close) fd.close();
                const close = resp.shouldCloseConnection();
                _.detachResponse();
                _.endRequestStreamingAndDrain();
                resp.end("", close);
                _.deref();
                return;
            }

            _.flags.has_sendfile_ctx = true;
            _.flags.has_abort_handler = true;
            _.flags.has_marked_pending = true;

            FileResponseStream.start(.{
                .fd = fd,
                .auto_close = auto_close,
                .resp = anyResponse(resp),
                .vm = _.server.?.vm,
                .file_type = file_type,
                .pollable = pollable,
                .offset = _.sendfile.offset,
                .length = if (is_regular) @as(u64, _.sendfile.remain) else null,
                .idle_timeout = _.server.?.config.idleTimeout,
                .ctx = _,
                .on_complete = onFileStreamComplete,
                .on_abort = onFileStreamAbort,
                .on_error = onFileStreamError,
            });
        }

        pub fn doRenderWithBodyLocked(this: *anyopaque, value: *jsc.WebCore.Body.Value): void {
            doRenderWithBody(bun.cast(*RequestContext, this), value, null);
        }

        fn renderWithBlobFromBodyValue(_: *RequestContext): void {
            if (_.isAbortedOrEnded()) {
                return;
            }

            if (_.blob.needsToReadFile()) {
                if (!_.flags.has_sendfile_ctx)
                    _.doSendfile(_.blob.Blob);
                return;
            }

            _.doRenderBlob();
        }

        const StreamPair = struct { this: *RequestContext, stream: jsc.WebCore.ReadableStream };

        fn handleFirstStreamWrite(this: *@This()): void {
            if (!this.flags.has_written_status) {
                this.renderMetadata();
            }
        }

        fn doRenderStream(pair: *StreamPair): void {
            ctxLog("doRenderStream", .{});
            var this = pair.this;
            var stream = pair.stream;
            bun.assert(this.server != null);
            const globalThis = this.server.?.globalThis;

            if (this.isAbortedOrEnded()) {
                stream.cancel(globalThis);
                this.response_body_readable_stream_ref.deinit();
                return;
            }
            const resp = this.resp.?;

            stream.value.ensureStillAlive();

            var response_stream = this.allocator.create(ResponseStream.JSSink) catch unreachable;
            response_stream.* = ResponseStream.JSSink{
                .sink = .{
                    .res = resp,
                    .allocator = this.allocator,
                    .buffer = bun.ByteList{},
                    .onFirstWrite = @ptrCast(&handleFirstStreamWrite),
                    .ctx = this,
                    .globalThis = globalThis,
                },
            };
            var signal = &response_stream.sink.signal;
            this.sink = response_stream;

            signal.* = ResponseStream.JSSink.SinkSignal.init(JSValue.zero);

            signal.clear();
            bun.assert(signal.isDead());
            if (!this.flags.has_written_status) {
                this.renderMetadata();
            }

            const assignment_result: JSValue = ResponseStream.JSSink.assignToStream(
                globalThis,
                stream.value,
                response_stream,
                @as(**anyopaque, @ptrCast(&signal.ptr)),
            );

            assignment_result.ensureStillAlive();

            bun.assert(!signal.isDead());

            this.flags.aborted = this.flags.aborted or response_stream.sink.aborted;

            if (assignment_result.toError()) |err_value| {
                streamLog("returned an error", .{});
                response_stream.detach(globalThis);
                this.sink = null;
                response_stream.sink.destroy();
                return this.handleReject(err_value);
            }

            if (resp.hasResponded()) {
                streamLog("done", .{});
                response_stream.detach(globalThis);
                this.sink = null;
                response_stream.sink.destroy();
                stream.done(globalThis);
                this.response_body_readable_stream_ref.deinit();
                this.endStream(this.shouldCloseConnection());
                return;
            }

            var effective_result = assignment_result;
            if (effective_result.isEmptyOrUndefinedOrNull()) {
                if (response_stream.sink.pending_flush) |flush| {
                    effective_result = flush.toJS();
                }
            }

            if (!effective_result.isEmptyOrUndefinedOrNull()) {
                effective_result.ensureStillAlive();
                if (effective_result.asAnyPromise()) |promise| {
                    streamLog("returned a promise", .{});
                    this.drainMicrotasks();

                    switch (promise.status()) {
                        .pending => {
                            streamLog("promise still Pending", .{});
                            if (!this.flags.has_written_status) {
                                response_stream.sink.onFirstWrite = null;
                                response_stream.sink.ctx = null;
                                this.renderMetadata();
                            }

                            const bodyValue = this.response_weakref.get().?.getBodyValue();
                            bodyValue.* = .{
                                .Locked = .{
                                    .readable = jsc.WebCore.ReadableStream.Strong.init(stream, globalThis),
                                    .global = globalThis,
                                },
                            };
                            this.ref();
                            const cell = NativePromiseContext.create(globalThis, this);
                            effective_result.thenWithValue(
                                globalThis,
                                cell,
                                onResolveStream,
                                onRejectStream,
                            ) catch {};
                        },
                        .fulfilled => {
                            streamLog("promise Fulfilled", .{});
                            var response_body_readable_stream_ref = this.response_body_readable_stream_ref;
                            this.response_body_readable_stream_ref = .{};
                            defer {
                                stream.done(globalThis);
                                response_body_readable_stream_ref.deinit();
                            }

                            this.handleResolveStream();
                        },
                        .rejected => {
                            streamLog("promise Rejected", .{});
                            var response_body_readable_stream_ref = this.response_body_readable_stream_ref;
                            this.response_body_readable_stream_ref = .{};
                            defer {
                                stream.cancel(globalThis);
                                response_body_readable_stream_ref.deinit();
                            }
                            this.handleRejectStream(globalThis, promise.result(globalThis.vm()));
                        },
                    }
                    return;
                } else {
                    streamLog("returned an error", .{});
                    response_stream.detach(globalThis);
                    this.sink = null;
                    response_stream.sink.destroy();
                    return this.handleReject(effective_result);
                }
            }

            if (this.isAbortedOrEnded()) {
                response_stream.detach(globalThis);
                stream.cancel(globalThis);
                defer this.response_body_readable_stream_ref.deinit();

                response_stream.sink.markDone();
                response_stream.sink.onFirstWrite = null;

                response_stream.sink.finalize();
                this.sink = null;
                response_stream.sink.destroy();
                return;
            }
            var response_body_readable_stream_ref = this.response_body_readable_stream_ref;
            this.response_body_readable_stream_ref = .{};
            defer response_body_readable_stream_ref.deinit();

            const is_in_progress = response_stream.sink.has_backpressure or !(response_stream.sink.wrote == 0 and
                response_stream.sink.buffer.len == 0);

            if (!stream.isLocked(globalThis) and !is_in_progress) {
                if (jsc.WebCore.ReadableStream.fromJS(stream.value, globalThis) catch null) |comparator| {
                    if (std.meta.activeTag(comparator.ptr) == std.meta.activeTag(stream.ptr)) {
                        streamLog("is not locked", .{});
                        response_stream.sink.onFirstWrite = null;
                        response_stream.sink.ctx = null;
                        response_stream.detach(globalThis);
                        response_stream.sink.markDone();
                        response_stream.sink.finalize();
                        this.sink = null;
                        response_stream.sink.destroy();
                        this.renderMissing();
                        return;
                    }
                }
            }

            streamLog("is in progress, but did not return a Promise. Finalizing request context", .{});
            response_stream.sink.onFirstWrite = null;
            response_stream.sink.ctx = null;
            response_stream.detach(globalThis);
            stream.cancel(globalThis);
            response_stream.sink.markDone();
            response_stream.sink.finalize();
            this.sink = null;
            response_stream.sink.destroy();
            this.renderMissing();
        }

        const streamLog = Output.scoped(.ReadableStream, .visible);

        fn toAsyncWithoutAbortHandler(ctx: *RequestContext, req: *Req, request_object: *Request): void {
            bun.assert(ctx.server != null);

            if (comptime !http3) request_object.request_context.setRequest(req);

            request_object.ensureURL() catch {
                request_object.url = bun.String.empty;
            };

            if (!request_object.hasFetchHeaders()) {
                if (comptime !http3) request_object.setFetchHeaders(.createFromUWS(req));
            }

            request_object.request_context.detachRequest();
        }

        pub fn toAsync(
            ctx: *RequestContext,
            req: *Req,
            request_object: *Request,
        ): void {
            ctxLog("toAsync", .{});
            ctx.toAsyncWithoutAbortHandler(req, request_object);
            if (comptime debug_mode) {
                ctx.pathname = request_object.url.clone();
            }
            ctx.setAbortHandler();
        }

        fn endRequestStreaming(_: *RequestContext): bun.JSTerminated!bool {
            bun.assert(_.server != null);

            _.request_body_buf.clearAndFree(bun.default_allocator);

            if (_.request_body) |body| {
                if (body.value == .Locked) {
                    try body.value.toErrorInstance(.{ .AbortReason = .ConnectionClosed }, _.server.?.globalThis);
                    return true;
                }
            }
            return false;
        }

        const HeaderResponseSizePair = struct { this: *RequestContext, size: usize };
        pub fn doRenderHeadResponseAfterS3SizeResolved(pair: *HeaderResponseSizePair): void {
            var this = pair.this;
            this.renderMetadata();

            if (this.resp) |resp| {
                resp.writeHeaderInt("content-length", pair.size);
            }
            this.endWithoutBody(this.shouldCloseConnection());
            this.deref();
        }

        pub fn onS3SizeResolved(result: S3.S3StatResult, this: *RequestContext): void {
            defer {
                this.deref();
            }
            if (this.resp) |resp| {
                var pair = HeaderResponseSizePair{ .this = this, .size = switch (result) {
                    .failure, .not_found => 0,
                    .success => |stat| stat.size,
                } };
                resp.runCorkedWithType(*HeaderResponseSizePair, doRenderHeadResponseAfterS3SizeResolved, &pair);
            }
        }

        const HeaderResponsePair = struct { this: *RequestContext, response: *jsc.WebCore.Response };

        fn doRenderHeadResponse(pair: *HeaderResponsePair): void {
            var this = pair.this;
            var response = pair.response;
            if (this.resp == null) {
                return;
            }
            this.flags.needs_content_length = false;

            const resp = this.resp.?;
            this.setResponse(response);
            const server = this.server orelse {
                this.renderMetadata();
                resp.writeHeaderInt("content-length", 0);
                this.endWithoutBody(this.shouldCloseConnection());
                return;
            };
            const globalThis = server.globalThis;
            if (response.getFetchHeaders()) |headers| {
                if (comptime !http3) if (headers.fastGet(.TransferEncoding)) |transfer_encoding| {
                    const transfer_encoding_str = bun.handleOom(transfer_encoding.toSliceClone(server.allocator));
                    defer transfer_encoding_str.deinit();
                    this.renderMetadata();
                    resp.writeHeader("transfer-encoding", transfer_encoding_str.slice());
                    this.endWithoutBody(this.shouldCloseConnection());
                    return;
                };
                if (headers.fastGet(.ContentLength)) |content_length| {
                    const content_length_str = content_length.toSlice(server.allocator);
                    const len = std.fmt.parseInt(usize, content_length_str.slice(), 10) catch 0;
                    content_length_str.deinit();

                    this.renderMetadata();
                    resp.writeHeaderInt("content-length", len);
                    this.endWithoutBody(this.shouldCloseConnection());
                    return;
                }
            }
            const bodyValue = response.getBodyValue();
            bodyValue.toBlobIfPossible();
            switch (bodyValue.*) {
                .InternalBlob, .WTFStringImpl => {
                    var blob = bodyValue.useAsAnyBlobAllowNonUTF8String();
                    defer blob.detach();
                    const size = blob.size();
                    this.renderMetadata();

                    if (size == Blob.max_size) {
                        resp.writeHeaderInt("content-length", 0);
                    } else {
                        resp.writeHeaderInt("content-length", size);
                    }
                    this.endWithoutBody(this.shouldCloseConnection());
                },
                .Blob => |*blob| {
                    if (blob.isS3()) {
                        this.ref();
                        const credentials = blob.store.?.data.s3.getCredentials();
                        const path = blob.store.?.data.s3.path();
                        const env = globalThis.bunVM().transpiler.env;

                        S3.stat(credentials, path, @ptrCast(&onS3SizeResolved), this, if (env.getHttpProxy(true, null, null)) |proxy| proxy.href else null, blob.store.?.data.s3.request_payer) catch {};
                        return;
                    }
                    this.renderMetadata();

                    blob.resolveSize();
                    if (blob.size == Blob.max_size) {
                        resp.writeHeaderInt("content-length", 0);
                    } else {
                        resp.writeHeaderInt("content-length", blob.size);
                    }
                    this.endWithoutBody(this.shouldCloseConnection());
                },
                .Locked => {
                    this.renderMetadata();
                    if (comptime !http3) resp.writeHeader("transfer-encoding", "chunked");
                    this.endWithoutBody(this.shouldCloseConnection());
                },
                .Used, .Null, .Empty, .Error => {
                    this.renderMetadata();
                    resp.writeHeaderInt("content-length", 0);
                    this.endWithoutBody(this.shouldCloseConnection());
                },
            }
        }
        pub fn onResponse(
            ctx: *RequestContext,
            this: *ThisServer,
            request_value: JSValue,
            response_value: JSValue,
        ): void {
            request_value.ensureStillAlive();
            response_value.ensureStillAlive();
            ctx.drainMicrotasks();

            if (ctx.isAbortedOrEnded()) {
                return;
            }
            if (ctx.didUpgradeWebSocket()) {
                return;
            }

            if (response_value.isEmptyOrUndefinedOrNull()) {
                ctx.renderMissingInvalidResponse(response_value);
                return;
            }

            if (response_value.toError()) |err_value| {
                ctx.runErrorHandler(err_value);
                return;
            }

            if (response_value.as(jsc.WebCore.Response)) |response| {
                ctx.response_jsvalue = response_value;
                ctx.response_jsvalue.ensureStillAlive();
                ctx.flags.response_protected = false;
                if (ctx.method == .HEAD) {
                    if (ctx.resp) |resp| {
                        var pair = HeaderResponsePair{ .this = ctx, .response = response };
                        resp.runCorkedWithType(*HeaderResponsePair, doRenderHeadResponse, &pair);
                    }
                    return;
                } else {
                    const bodyValue = response.getBodyValue();
                    bodyValue.toBlobIfPossible();

                    switch (bodyValue.*) {
                        .Blob => |*blob| {
                            if (blob.needsToReadFile()) {
                                response_value.protect();
                                ctx.flags.response_protected = true;
                            }
                        },
                        .Locked => {
                            response_value.protect();
                            ctx.flags.response_protected = true;
                        },
                        else => {},
                    }
                    ctx.render(response);
                }
                return;
            }

            var vm = this.vm;

            if (response_value.asAnyPromise()) |promise| {
                switch (promise.unwrap(vm.global.vm(), .mark_handled)) {
                    .pending => {
                        ctx.ref();
                        const cell = NativePromiseContext.create(this.globalThis, ctx);
                        response_value.thenWithValue(this.globalThis, cell, RequestContext.onResolve, RequestContext.onReject) catch {};
                        return;
                    },
                    .fulfilled => |fulfilled_value| {
                        if (ctx.didUpgradeWebSocket()) {
                            return;
                        }

                        if (fulfilled_value.isEmptyOrUndefinedOrNull()) {
                            ctx.renderMissingInvalidResponse(fulfilled_value);
                            return;
                        }
                        var response = fulfilled_value.as(jsc.WebCore.Response) orelse {
                            ctx.renderMissingInvalidResponse(fulfilled_value);
                            return;
                        };

                        ctx.response_jsvalue = fulfilled_value;
                        ctx.response_jsvalue.ensureStillAlive();
                        ctx.flags.response_protected = false;
                        if (ctx.method == .HEAD) {
                            if (ctx.resp) |resp| {
                                var pair = HeaderResponsePair{ .this = ctx, .response = response };
                                resp.runCorkedWithType(*HeaderResponsePair, doRenderHeadResponse, &pair);
                            }
                            return;
                        }
                        const bodyValue = response.getBodyValue();
                        bodyValue.toBlobIfPossible();
                        switch (bodyValue.*) {
                            .Blob => |*blob| {
                                if (blob.needsToReadFile()) {
                                    fulfilled_value.protect();
                                    ctx.flags.response_protected = true;
                                }
                            },
                            .Locked => {
                                fulfilled_value.protect();
                                ctx.flags.response_protected = true;
                            },
                            else => {},
                        }
                        ctx.render(response);
                        return;
                    },
                    .rejected => |err| {
                        ctx.handleReject(err);
                        return;
                    },
                }
            }
        }

        pub fn renderMissingInvalidResponse(ctx: *RequestContext, value: JSValue): void {
            if (ctx.server) |server| {
                const globalThis = server.globalThis;
                const err = (jsc.SystemError{
                    .code = bun.String.static("ERR_INVALID_RETURN_VALUE"),
                    .message = bun.String.static("Expected a Response object, but got something else"),
                }).toErrorInstance(globalThis);
                ctx.runErrorHandler(err);
                return;
            }
            ctx.renderMissing();
        }

        pub fn onResolve(_: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            ctxLog("onResolve", .{});
            const arguments = callframe.arguments_old(2);
            const ctx = NativePromiseContext.take(RequestContext, arguments.ptr[1]) orelse return .js_undefined;
            defer ctx.deref();

            const value = arguments.ptr[0];
            ctx.onResolved(value);
            return .js_undefined;
        }

        pub fn onResolved(ctx: *RequestContext, value: JSValue): void {
            if (ctx.isAbortedOrEnded()) {
                return;
            }
            if (ctx.server) |server| {
                ctx.onResponse(server, .js_undefined, value);
            }
        }

        pub fn handleResolveStream(req: *RequestContext): void {
            streamLog("handleResolveStream", .{});

            var wrote_anything = false;
            if (req.sink) |wrapper| {
                req.flags.aborted = req.flags.aborted or wrapper.sink.aborted;
                wrote_anything = wrapper.sink.wrote > 0;

                wrapper.sink.finalize();
                wrapper.detach(wrapper.sink.globalThis);
                req.sink = null;
                wrapper.sink.destroy();
            }

            if (req.response_weakref.get()) |resp| {
                bun.assert(req.server != null);
                const globalThis = req.server.?.globalThis;
                const bodyValue = resp.getBodyValue();
                if (resp.getBodyReadableStream(globalThis)) |stream| {
                    stream.value.ensureStillAlive();
                    resp.detachReadableStream(globalThis);

                    stream.done(globalThis);
                }

                bodyValue.* = .Used;
            }

            if (req.isAbortedOrEnded()) {
                return;
            }

            streamLog("onResolve({})", .{wrote_anything});
            if (!req.flags.has_written_status) {
                req.renderMetadata();
            }
            req.endStream(req.shouldCloseConnection());
        }

        pub fn onResolveStream(_: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            streamLog("onResolveStream", .{});
            const args = callframe.arguments_old(2);
            const req = NativePromiseContext.take(RequestContext, args.ptr[args.len - 1]) orelse return .js_undefined;
            defer req.deref();
            req.handleResolveStream();
            return .js_undefined;
        }

        pub fn onRejectStream(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
            streamLog("onRejectStream", .{});
            const args = callframe.arguments_old(2);
            const req = NativePromiseContext.take(RequestContext, args.ptr[args.len - 1]) orelse return .js_undefined;
            const err = args.ptr[0];
            defer req.deref();

            req.handleRejectStream(globalThis, err);
            return .js_undefined;
        }

        pub fn handleRejectStream(req: *@This(), globalThis: *jsc.JSGlobalObject, err: JSValue): void {
            streamLog("handleRejectStream", .{});

            if (req.sink) |wrapper| {
                if (wrapper.sink.pending_flush) |prom| {
                    prom.toJS().unprotect();
                    wrapper.sink.pending_flush = null;
                }
                wrapper.sink.done = true;
                req.flags.aborted = req.flags.aborted or wrapper.sink.aborted;
                wrapper.sink.finalize();
                wrapper.detach(wrapper.sink.globalThis);
                req.sink = null;
                wrapper.sink.destroy();
            }

            if (req.response_weakref.get()) |resp| {
                const bodyValue = resp.getBodyValue();

                if (resp.getBodyReadableStream(globalThis)) |stream| {
                    stream.value.ensureStillAlive();
                    resp.detachReadableStream(globalThis);
                    stream.done(globalThis);
                }

                if (bodyValue.* == .Locked)
                    bodyValue.* = .{ .Used = {} };
            }

            if (req.isAbortedOrEnded()) {
                return;
            }

            streamLog("onReject()", .{});

            if (!req.flags.has_written_status) {
                req.renderMetadata();
            }

            if (comptime debug_mode) {
                if (req.server) |server| {
                    if (!err.isEmptyOrUndefinedOrNull()) {
                        var exception_list: std.array_list.Managed(Api.JsException) = std.array_list.Managed(Api.JsException).init(req.allocator);
                        defer exception_list.deinit();
                        server.vm.runErrorHandler(err, &exception_list);

                        if (server.dev_server) |dev_server| {
                            _ = dev_server;
                            if (!req.flags.has_written_status) {
                                req.flags.has_written_status = true;
                                if (req.resp) |resp| {
                                    resp.writeStatus("500 Internal Server Error");
                                    resp.writeHeader("content-type", MimeType.html.value);
                                }
                            }

                            const allocator = req.allocator;
                            const fallback_container = bun.handleOom(allocator.create(Api.FallbackMessageContainer));
                            defer allocator.destroy(fallback_container);

                            fallback_container.* = Api.FallbackMessageContainer{
                                .message = "Stream error during server-side rendering",
                                .router = null,
                                .reason = .fetch_event_handler,
                                .cwd = server.vm.transpiler.fs.top_level_dir,
                                .problems = Api.Problems{
                                    .code = 500,
                                    .name = "StreamError",
                                    .exceptions = exception_list.items,
                                    .build = .{
                                        .msgs = &.{},
                                    },
                                },
                            };

                            var bb = std.array_list.Managed(u8).init(allocator);
                            defer bb.clearAndFree();
                            const bb_writer = bb.writer();

                            Fallback.renderBackend(
                                allocator,
                                fallback_container,
                                @TypeOf(bb_writer),
                                bb_writer,
                            ) catch unreachable;

                            if (req.resp) |resp| {
                                _ = resp.write(bb.items);
                            }

                            req.endStream(req.shouldCloseConnection());
                            return;
                        }
                    }
                }
            }
            req.endStream(req.shouldCloseConnection());
        }

        pub fn doRenderWithBody(_: *RequestContext, value: *jsc.WebCore.Body.Value, owned_readable: ?jsc.WebCore.ReadableStream): void {
            _.drainMicrotasks();

            value.toBlobIfPossible();
            const globalThis = _.server.?.globalThis;
            switch (value.*) {
                .Error => |*err_ref| {
                    _ = value.use();
                    if (_.isAbortedOrEnded()) {
                        return;
                    }
                    _.runErrorHandler(err_ref.toJS(globalThis));
                    return;
                },
                .WTFStringImpl,
                .InternalBlob,
                .Blob,
                => {
                    _.blob = value.useAsAnyBlobAllowNonUTF8String();
                    _.renderWithBlobFromBodyValue();
                    return;
                },
                .Locked => |*lock| {
                    if (_.isAbortedOrEnded()) {
                        return;
                    }
                    const readable_stream = ({
                        if (lock.readable.get(globalThis)) |stream| {
                            _.response_body_readable_stream_ref = lock.readable;
                            break stream;
                        }
                        if (owned_readable) |stream| {
                            _.response_body_readable_stream_ref = .init(stream, globalThis);
                            break stream;
                        }
                        break null;
                    });
                    if (readable_stream) |stream| {
                        value.* = .{ .Used = {} };

                        if (stream.isLocked(globalThis)) {
                            streamLog("was locked but it shouldn't be", .{});
                            var err = jsc.SystemError{
                                .code = bun.String.static(@tagName(jsc.Node.ErrorCode.ERR_STREAM_CANNOT_PIPE)),
                                .message = bun.String.static("Stream already used, please create a new one"),
                            };
                            stream.value.unprotect();
                            const js_err = err.toErrorInstance(globalThis);
                            _.runErrorHandler(js_err);
                            return;
                        }

                        switch (stream.ptr) {
                            .Invalid => {
                                _.response_body_readable_stream_ref.deinit();
                                _.doRenderBlob();
                                return;
                            },
                            .Blob,
                            .File,
                            .JavaScript,
                            .Direct,
                            => {
                                if (_.resp) |resp| {
                                    var pair = StreamPair{ .stream = stream, .this = _ };
                                    resp.runCorkedWithType(*StreamPair, doRenderStream, &pair);
                                }
                                return;
                            },

                            .Bytes => |byte_stream| {
                                bun.assert(byte_stream.pipe.ctx == null);
                                bun.assert(_.byte_stream == null);
                                if (_.resp == null) {
                                    stream.done(globalThis);
                                    _.response_body_readable_stream_ref.deinit();
                                    return;
                                }
                                const resp = _.resp.?;
                                if (byte_stream.has_received_last_chunk) {
                                    var byte_list = byte_stream.drain();
                                    _.blob = .fromArrayList(byte_list.moveToListManaged(bun.default_allocator));
                                    _.response_body_readable_stream_ref.deinit();
                                    _.doRenderBlob();
                                    return;
                                }
                                _.ref();
                                byte_stream.pipe = jsc.WebCore.Pipe.Wrap(@This(), onPipe).init(_);
                                _.response_body_readable_stream_ref.deinit();
                                _.response_body_readable_stream_ref = jsc.WebCore.ReadableStream.Strong.init(stream, globalThis);

                                _.byte_stream = byte_stream;
                                var response_buf = byte_stream.drain();
                                _.response_buf_owned = response_buf.moveToList();

                                _.blob.detach();

                                if (_.response_buf_owned.items.len > 0) {
                                    resp.runCorkedWithType(*RequestContext, drainResponseBufferAndMetadata, _);
                                } else {
                                    resp.runCorkedWithType(*RequestContext, renderMetadata, _);
                                }
                                return;
                            },
                        }
                    }

                    if (lock.onReceiveValue != null or lock.task != null) {
                        const readable = value.toReadableStream(globalThis) catch return;
                        readable.ensureStillAlive();
                        _.doRenderWithBody(value, null);
                        return;
                    }

                    lock.onReceiveValue = doRenderWithBodyLocked;
                    lock.task = _;

                    return;
                },
                else => {},
            }

            _.doRenderBlob();
        }

        pub fn onPipe(_: *RequestContext, stream: jsc.WebCore.streams.Result, allocator: std.mem.Allocator): void {
            var stream_ = stream;
            const stream_needs_deinit = stream == .owned or stream == .owned_and_done;
            const is_done = stream.isDone();
            defer {
                if (is_done) _.deref();
                if (stream_needs_deinit) {
                    switch (stream_) {
                        .owned_and_done => |*owned| owned.deinit(allocator),
                        .owned => |*owned| owned.deinit(allocator),
                        else => unreachable,
                    }
                }
            }

            if (_.isAbortedOrEnded()) {
                return;
            }
            const resp = _.resp.?;

            const chunk = stream.slice();
            if (resp.write(chunk) == .want_more) {
                if (is_done) {
                    _.endStream(_.shouldCloseConnection());
                }
            } else {
                if (is_done) {
                    _.flags.has_marked_pending = true;
                    resp.onWritable(*RequestContext, onWritableResponseBuffer, _);
                }
            }
        }

        pub fn doRenderBlob(_: *RequestContext): void {
            if (_.flags.has_abort_handler and _.blob.fastSize() < 16384 - 1024) {
                if (_.resp) |resp| {
                    resp.runCorkedWithType(*RequestContext, doRenderBlobCorked, _);
                }
            } else {
                _.doRenderBlobCorked();
            }
        }

        pub fn doRenderBlobCorked(_: *RequestContext): void {
            _.renderMetadata();
            _.renderBytes();
        }

        pub fn doRender(_: *RequestContext): void {
            ctxLog("doRender", .{});

            if (_.isAbortedOrEnded()) {
                return;
            }
            var response = _.response_weakref.get().?;
            _.doRenderWithBody(response.getBodyValue(), response.getBodyReadableStream(_.server.?.globalThis));
        }

        pub fn renderProductionError(_: *RequestContext, status: u16): void {
            if (_.resp) |resp| {
                switch (status) {
                    404 => {
                        if (!_.flags.has_written_status) {
                            resp.writeStatus("404 Not Found");
                            _.flags.has_written_status = true;
                        }
                        _.endWithoutBody(_.shouldCloseConnection());
                    },
                    else => {
                        if (!_.flags.has_written_status) {
                            resp.writeStatus("500 Internal Server Error");
                            resp.writeHeader("content-type", "text/plain");
                            _.flags.has_written_status = true;
                        }

                        _.end("Something went wrong!", _.shouldCloseConnection());
                    },
                }
            }
        }

        pub fn runErrorHandler(
            _: *RequestContext,
            value: jsc.JSValue,
        ): void {
            runErrorHandlerWithStatusCode(_, value, 500);
        }

        const PathnameFormatter = struct {
            ctx: *RequestContext,

            pub fn format(formatter: @This(), writer: *std.Io.Writer): !void {
                var this = formatter.ctx;

                if (!this.pathname.isEmpty()) {
                    try this.pathname.format(writer);
                    return;
                }

                if (!this.flags.has_abort_handler) {
                    if (this.req) |req| {
                        try writer.writeAll(req.url());
                        return;
                    }
                }

                try writer.writeAll("/");
            }
        };

        fn ensurePathname(_: *RequestContext): PathnameFormatter {
            return .{ .ctx = _ };
        }

        pub inline fn shouldCloseConnection(_: *const RequestContext): bool {
            if (_.resp) |resp| {
                return resp.shouldCloseConnection();
            }
            return false;
        }

        fn finishRunningErrorHandler(_: *RequestContext, value: jsc.JSValue, status: u16): void {
            if (_.server == null) return _.renderProductionError(status);
            var vm: *jsc.VirtualMachine = _.server.?.vm;
            const globalThis = _.server.?.globalThis;
            if (comptime debug_mode) {
                var arena = std.heap.ArenaAllocator.init(_.allocator);
                defer arena.deinit();
                const allocator = arena.allocator();
                var exception_list: std.array_list.Managed(Api.JsException) = std.array_list.Managed(Api.JsException).init(allocator);
                defer exception_list.deinit();
                const prev_exception_list = vm.onUnhandledRejectionExceptionList;
                vm.onUnhandledRejectionExceptionList = &exception_list;
                vm.onUnhandledRejection(vm, globalThis, value);
                vm.onUnhandledRejectionExceptionList = prev_exception_list;

                _.renderDefaultError(
                    allocator,
                    vm.log,
                    error.ExceptionOcurred,
                    exception_list.items,
                    "<r><red>{s}<r> - <b>{f}<r> failed",
                    .{ @as(string, @tagName(_.method)), _.ensurePathname() },
                );
            } else {
                if (status != 404) {
                    vm.onUnhandledRejection(vm, globalThis, value);
                }
                _.renderProductionError(status);
            }

            vm.log.reset();
        }

        pub fn runErrorHandlerWithStatusCodeDontCheckResponded(
            _: *RequestContext,
            value: jsc.JSValue,
            status: u16,
        ): void {
            jsc.markBinding(@src());
            if (_.server) |server| {
                if (server.config.onError != .zero and !_.flags.has_called_error_handler) {
                    _.flags.has_called_error_handler = true;
                    const result = server.config.onError.call(
                        server.globalThis,
                        server.js_value.tryGet() orelse .js_undefined,
                        &.{value},
                    ) catch |err| server.globalThis.takeException(err);
                    defer result.ensureStillAlive();
                    if (!result.isEmptyOrUndefinedOrNull()) {
                        if (result.toError()) |err| {
                            _.finishRunningErrorHandler(err, status);
                            return;
                        } else if (result.asAnyPromise()) |promise| {
                            _.processOnErrorPromise(result, promise, value, status);
                            return;
                        } else if (result.as(Response)) |response| {
                            _.render(response);
                            return;
                        }
                    }
                }
            }

            _.finishRunningErrorHandler(value, status);
        }

        fn processOnErrorPromise(
            ctx: *RequestContext,
            promise_js: jsc.JSValue,
            promise: jsc.AnyPromise,
            value: jsc.JSValue,
            status: u16,
        ): void {
            bun.assert(ctx.server != null);
            var vm = ctx.server.?.vm;

            switch (promise.unwrap(vm.global.vm(), .mark_handled)) {
                .pending => {
                    ctx.flags.is_error_promise_pending = true;
                    ctx.ref();
                    const cell = NativePromiseContext.create(ctx.server.?.globalThis, ctx);
                    promise_js.thenWithValue(
                        ctx.server.?.globalThis,
                        cell,
                        RequestContext.onResolve,
                        RequestContext.onReject,
                    ) catch {};
                },
                .fulfilled => |fulfilled_value| {
                    if (ctx.didUpgradeWebSocket()) {
                        return;
                    }

                    var response = fulfilled_value.as(jsc.WebCore.Response) orelse {
                        ctx.finishRunningErrorHandler(value, status);
                        return;
                    };

                    ctx.response_jsvalue = fulfilled_value;
                    ctx.response_jsvalue.ensureStillAlive();
                    ctx.flags.response_protected = false;

                    const bodyValue = response.getBodyValue();
                    bodyValue.toBlobIfPossible();
                    switch (bodyValue.*) {
                        .Blob => |*blob| {
                            if (blob.needsToReadFile()) {
                                fulfilled_value.protect();
                                ctx.flags.response_protected = true;
                            }
                        },
                        .Locked => {
                            fulfilled_value.protect();
                            ctx.flags.response_protected = true;
                        },
                        else => {},
                    }
                    ctx.render(response);
                    return;
                },
                .rejected => |err| {
                    ctx.finishRunningErrorHandler(err, status);
                    return;
                },
            }
        }

        pub fn runErrorHandlerWithStatusCode(
            _: *RequestContext,
            value: jsc.JSValue,
            status: u16,
        ): void {
            jsc.markBinding(@src());
            if (_.resp == null or _.resp.?.hasResponded()) return;

            runErrorHandlerWithStatusCodeDontCheckResponded(_, value, status);
        }

        pub fn renderMetadata(_: *RequestContext): void {
            if (_.resp == null) return;
            const resp = _.resp.?;

            var response: *jsc.WebCore.Response = _.response_weakref.get().?;
            var status = response.statusCode();
            var needs_content_range = _.flags.needs_content_range and (_.sendfile.total > 0 or _.sendfile.remain < _.blob.size());

            const size = if (needs_content_range)
                _.sendfile.remain
            else
                _.blob.size();

            status = if (status == 200 and size == 0 and !_.blob.isDetached())
                204
            else
                status;

            const content_type, const needs_content_type, const content_type_needs_free = getContentType(
                response.getInitHeaders(),
                &_.blob,
                _.allocator,
            );
            defer if (content_type_needs_free) content_type.deinit(_.allocator);
            var has_content_disposition = false;
            var has_content_range = false;
            if (response.swapInitHeaders()) |headers_| {
                defer headers_.deref();
                has_content_disposition = headers_.fastHas(.ContentDisposition);
                has_content_range = headers_.fastHas(.ContentRange);
                needs_content_range = needs_content_range and (_.sendfile.total > 0 or has_content_range);
                if (needs_content_range) {
                    status = 206;
                }

                _.doWriteStatus(status);
                _.doWriteHeaders(headers_);
            } else if (needs_content_range) {
                status = 206;
                _.doWriteStatus(status);
            } else {
                _.doWriteStatus(status);
            }

            if (_.cookies) |cookies| {
                _.cookies = null;
                defer cookies.deref();
                cookies.write(_.server.?.globalThis, resp_kind, @ptrCast(_.resp.?)) catch return;
            }

            if (needs_content_type and
                (!_.blob.isDetached() or content_type.value.ptr != MimeType.other.value.ptr))
            {
                resp.writeHeader("content-type", content_type.value);
            }

            if (comptime !http3 and @hasDecl(ThisServer, "h3AltSvc")) {
                if (_.server.?.h3AltSvc()) |alt| resp.writeHeader("alt-svc", alt);
            }

            if (!has_content_disposition and content_type.category.autosetFilename()) {
                if (_.blob.getFileName()) |filename| {
                    const basename = std.fs.path.basename(filename);
                    if (basename.len > 0) {
                        var filename_buf: [1024]u8 = undefined;

                        resp.writeHeader(
                            "content-disposition",
                            std.fmt.bufPrint(&filename_buf, "filename=\"{s}\"", .{basename[0..@min(basename.len, 1024 - 32)]}) catch "",
                        );
                    }
                }
            }

            if (_.flags.needs_content_length) {
                resp.writeHeaderInt("content-length", size);
                resp.markWroteContentLengthHeader();
                _.flags.needs_content_length = false;
            }

            if (needs_content_range and !has_content_range) {
                var content_range_buf: [1024]u8 = undefined;

                resp.writeHeader(
                    "content-range",
                    if (_.sendfile.total > 0)
                        std.fmt.bufPrint(&content_range_buf, "bytes {d}-{d}/{d}", .{
                            _.sendfile.offset,
                            _.sendfile.offset + (_.sendfile.remain -| 1),
                            _.sendfile.total,
                        }) catch "bytes */*"
                    else
                        std.fmt.bufPrint(&content_range_buf, "bytes {d}-{d}/*", .{
                            _.sendfile.offset,
                            _.sendfile.offset + (_.sendfile.remain -| 1),
                        }) catch "bytes */*",
                );
                if (_.sendfile.total > 0) resp.writeHeader("accept-ranges", "bytes");
                _.flags.needs_content_range = false;
            }
        }

        fn doWriteStatus(_: *RequestContext, status: u16): void {
            bun.assert(!_.flags.has_written_status);
            _.flags.has_written_status = true;

            const resp = _.resp orelse return;
            if (HTTPStatusText.get(status)) |text| {
                resp.writeStatus(text);
            } else {
                var buf: [48]u8 = undefined;
                resp.writeStatus(std.fmt.bufPrint(&buf, "{d} HM", .{status}) catch unreachable);
            }
        }

        fn doWriteHeaders(_: *RequestContext, headers: *WebCore.FetchHeaders): void {
            ctxLog("writeHeaders", .{});
            headers.fastRemove(.ContentLength);
            headers.fastRemove(.TransferEncoding);
            if (comptime http3) {
                headers.fastRemove(.Connection);
                headers.fastRemove(.KeepAlive);
                headers.fastRemove(.ProxyConnection);
                headers.fastRemove(.Upgrade);
            }
            if (_.resp) |resp| headers.toUWSResponse(resp_kind, resp);
        }

        pub fn renderBytes(_: *RequestContext): void {
            const blob = _.blob;
            const bytes = blob.slice();
            if (_.resp) |resp| {
                if (!resp.tryEnd(
                    bytes,
                    bytes.len,
                    _.shouldCloseConnection(),
                )) {
                    _.flags.has_marked_pending = true;
                    resp.onWritable(*RequestContext, onWritableBytes, _);
                    return;
                }
            }
            _.detachResponse();
            _.endRequestStreamingAndDrain();
            _.deref();
        }

        fn setResponse(_: *RequestContext, response: *jsc.WebCore.Response): void {
            if (_.response_weakref.raw_ptr == response) return;
            _.response_weakref.deref();
            _.response_weakref = .initRef(response);
        }

        pub fn render(_: *RequestContext, response: *jsc.WebCore.Response): void {
            ctxLog("render", .{});
            _.setResponse(response);

            _.doRender();
        }

        pub fn onBufferedBodyChunk(_: *RequestContext, resp: *App.Response, chunk: []const u8, last: bool): void {
            ctxLog("onBufferedBodyChunk {} {}", .{ chunk.len, last });

            bun.assert(_.resp == resp);

            _.flags.is_waiting_for_request_body = last == false;
            if (_.isAbortedOrEnded() or _.flags.has_marked_complete) return;
            if (!last and chunk.len == 0) {
                return;
            }
            const vm = _.server.?.vm;
            const globalThis = _.server.?.globalThis;

            if (_.request_body_readable_stream_ref.get(globalThis)) |readable| {
                bun.assert(_.request_body_buf.items.len == 0);
                vm.eventLoop().enter();
                defer vm.eventLoop().exit();

                if (!last) {
                    readable.ptr.Bytes.onData(
                        .{
                            .temporary = bun.ByteList.fromBorrowedSliceDangerous(chunk),
                        },
                        bun.default_allocator,
                    ) catch {};
                } else {
                    var strong = _.request_body_readable_stream_ref;
                    _.request_body_readable_stream_ref = .{};
                    defer strong.deinit();

                    if (_.request_body) |request_body| {
                        _ = request_body.unref();
                        _.request_body = null;
                    }

                    readable.value.ensureStillAlive();
                    readable.ptr.Bytes.onData(
                        .{
                            .temporary_and_done = bun.ByteList.fromBorrowedSliceDangerous(chunk),
                        },
                        bun.default_allocator,
                    ) catch {};
                }

                return;
            }

            if (_.request_body != null) {
                var body = _.request_body.?;

                if (_.request_body_buf.items.len +| chunk.len > _.server.?.config.max_request_body_size) {
                    _.request_body_buf.clearAndFree(_.allocator);
                    resp.clearOnData();
                    _.flags.is_waiting_for_request_body = false;

                    var loop = vm.eventLoop();
                    loop.enter();
                    defer loop.exit();
                    body.value.toErrorInstance(.{
                        .Message = bun.String.static("Request body exceeded maxRequestBodySize"),
                    }, globalThis) catch {};

                    if (_.resp != null and !resp.hasResponded()) {
                        _.flags.has_written_status = true;
                        resp.writeStatus("413 Payload Too Large");
                    }
                    _.endWithoutBody(comptime !http3);
                    return;
                }

                if (last) {
                    var bytes = &_.request_body_buf;

                    var old = body.value;

                    const total = bytes.items.len + chunk.len;
                    ({
                        bytes.ensureTotalCapacityPrecise(_.allocator, total) catch |err| {
                            _.request_body_buf.clearAndFree(_.allocator);
                            body.value.toError(err, globalThis) catch {};
                            break;
                        };

                        const prev_len = bytes.items.len;
                        bytes.items.len = total;
                        var slice = bytes.items[prev_len..];
                        @memcpy(slice[0..chunk.len], chunk);
                        body.value = .{
                            .InternalBlob = .{
                                .bytes = bytes.toManaged(_.allocator),
                            },
                        };
                    });
                    _.request_body_buf = .{};

                    if (old == .Locked) {
                        var loop = vm.eventLoop();
                        loop.enter();
                        defer loop.exit();

                        old.resolve(&body.value, globalThis, null) catch {};
                    }
                    return;
                }

                if (_.request_body_buf.capacity == 0) {
                    _.request_body_buf.ensureTotalCapacityPrecise(_.allocator, @min(_.request_body_content_len, max_request_body_preallocate_length)) catch @panic("Out of memory while allocating request body buffer");
                }
                _.request_body_buf.appendSlice(_.allocator, chunk) catch @panic("Out of memory while allocating request body");
            }
        }

        pub fn onStartStreamingRequestBody(_: *RequestContext): jsc.WebCore.DrainResult {
            ctxLog("onStartStreamingRequestBody", .{});
            if (_.isAbortedOrEnded()) {
                return jsc.WebCore.DrainResult{
                    .aborted = {},
                };
            }

            if (_.request_body_buf.items.len > 0) {
                var emptied = _.request_body_buf;
                _.request_body_buf = .{};
                return .{
                    .owned = .{
                        .list = emptied.toManaged(_.allocator),
                        .size_hint = if (emptied.capacity < max_request_body_preallocate_length)
                            emptied.capacity
                        else
                            0,
                    },
                };
            }

            return .{
                .estimated_size = _.request_body_content_len,
            };
        }
        const max_request_body_preallocate_length = 1024 * 256;

        pub fn onStartBuffering(_: *RequestContext): void {
            if (_.server) |server| {
                ctxLog("onStartBuffering", .{});
                if (!http3 and _.flags.is_transfer_encoding == false and _.request_body_content_len == 0) {
                    if (_.request_body != null) {
                        var body = _.request_body.?;
                        var old = body.value;
                        old.Locked.onReceiveValue = null;
                        var new_body: WebCore.Body.Value = .{ .Null = {} };
                        old.resolve(&new_body, server.globalThis, null) catch {};
                        body.value = new_body;
                    }
                }
            }
        }

        pub fn onRequestBodyReadableStreamAvailable(ptr: *anyopaque, globalThis: *jsc.JSGlobalObject, readable: jsc.WebCore.ReadableStream): void {
            var this = bun.cast(*RequestContext, ptr);
            bun.debugAssert(this.request_body_readable_stream_ref.held.impl == null);
            this.request_body_readable_stream_ref = jsc.WebCore.ReadableStream.Strong.init(readable, globalThis);
        }

        pub fn onStartBufferingCallback(this: *anyopaque): void {
            onStartBuffering(bun.cast(*RequestContext, this));
        }

        pub fn onStartStreamingRequestBodyCallback(this: *anyopaque): jsc.WebCore.DrainResult {
            return onStartStreamingRequestBody(bun.cast(*RequestContext, this));
        }

        pub fn getRemoteSocketInfo(_: *RequestContext): ?uws.SocketAddress {
            return (_.resp orelse return null).getRemoteSocketInfo();
        }

        pub fn setTimeout(_: *RequestContext, seconds: c_uint): bool {
            if (_.resp) |resp| {
                resp.timeout(@min(seconds, 255));
                if (seconds > 0) {
                    if (_.request_weakref.get()) |req| {
                        if (req.internal_event_callback.hasCallback()) {
                            _.setTimeoutHandler();
                        }
                    }
                } else {
                    resp.clearTimeout();
                }
                return true;
            }
            return false;
        }

        comptime {
            const export_prefix = "Bun__HTTPRequestContext" ++ (if (debug_mode) "Debug" else "") ++ (if (http3) "H3" else if (ThisServer.ssl_enabled) "TLS" else "");
            if (bun.Environment.export_cpp_apis) {
                @export(&jsc.toJSHostFn(onResolve), .{ .name = export_prefix ++ "__onResolve" });
                @export(&jsc.toJSHostFn(onReject), .{ .name = export_prefix ++ "__onReject" });
                @export(&jsc.toJSHostFn(onResolveStream), .{ .name = export_prefix ++ "__onResolveStream" });
                @export(&jsc.toJSHostFn(onRejectStream), .{ .name = export_prefix ++ "__onRejectStream" });
            }
        }
    };
}

pub class SendfileContext {
    remain: Blob.SizeType = 0,
    offset: Blob.SizeType = 0,
    total: Blob.SizeType = 0,
}

pub fn NewFlags(comptime debug_mode: bool): type {
    return packed struct(u16) {
        has_marked_complete: bool = false,
        has_marked_pending: bool = false,
        has_abort_handler: bool = false,
        has_timeout_handler: bool = false,
        has_sendfile_ctx: bool = false,
        has_called_error_handler: bool = false,
        needs_content_length: bool = false,
        needs_content_range: bool = false,
        is_transfer_encoding: bool = false,

        is_waiting_for_request_body: bool = false,
        is_web_browser_navigation: if (debug_mode) bool else void = if (debug_mode) false,
        has_written_status: bool = false,
        response_protected: bool = false,
        aborted: bool = false,
        has_finalized: bun.DebugOnly(bool) = if (Environment.isDebug) false,

        is_error_promise_pending: bool = false,

        _padding: PaddingInt = 0,

        const PaddingInt = ({
            var size: usize = 2;
            if (Environment.isDebug) {
                size -= 1;
            }

            if (debug_mode) {
                size -= 1;
            }

            break std.meta.Int(.unsigned, size);
        });
    };
}

fn getContentType(headers: ?*WebCore.FetchHeaders, blob: *const WebCore.Blob.Any, allocator: std.mem.Allocator): struct { MimeType, bool, bool } {
    var needs_content_type = true;
    var content_type_needs_free = false;

    const content_type: MimeType = ({
        if (headers) |headers_| {
            if (headers_.fastGet(.ContentType)) |content| {
                needs_content_type = false;

                var content_slice = content.toSlice(allocator);
                defer content_slice.deinit();

                const content_type_allocator = if (content_slice.allocator.isNull()) null else allocator;
                break MimeType.init(content_slice.slice(), content_type_allocator, &content_type_needs_free);
            }
        }

        break if (blob.contentType().len > 0)
            MimeType.byName(blob.contentType())
        else if (MimeType.sniff(blob.slice())) |content|
            content
        else if (blob.wasString())
            MimeType.text
        else
            MimeType.other;
    });

    return .{ content_type, needs_content_type, content_type_needs_free };
}

pub const welcome_page_html_gz = @embedFile("../api/welcome-page.html.gz");

const ctxLog = Output.scoped(.RequestContext, .visible);
const string = []const u8;

const std = @import("std");
const Fallback = @import("../../js_parser/runtime.zig").Fallback;

const bun = @import("bun");
const Environment = bun.Environment;
const JSError = bun.JSError;
const Output = bun.Output;
const S3 = bun.S3;
const String = bun.String;
const assert = bun.assert;
const logger = bun.logger;
const uws = bun.uws;
const Api = bun.schema.api;

const FileResponseStream = bun.api.server.FileResponseStream;
const HTTPStatusText = bun.api.server.HTTPStatusText;
const RangeRequest = bun.api.server.RangeRequest;

const HTTP = bun.http;
const MimeType = bun.http.MimeType;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;

const AnyRequestContext = jsc.API.AnyRequestContext;
const NativePromiseContext = jsc.API.NativePromiseContext;

const WebCore = jsc.WebCore;
const Blob = jsc.WebCore.Blob;
const Body = jsc.WebCore.Body;
const FetchHeaders = jsc.WebCore.FetchHeaders;
const Request = jsc.WebCore.Request;
const Response = jsc.WebCore.Response;
