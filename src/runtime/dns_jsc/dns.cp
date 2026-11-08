// Bun DNS JSC implementation in CP language
// Converted from src/runtime/dns_jsc/dns.zig

const std = @import("std");
const Allocator = std.mem.Allocator;

const bun = @import("bun");
const Async = bun.Async;
const Environment = bun.Environment;
const Global = bun.Global;
const Output = bun.Output;
const c_ares = bun.c_ares;
const default_allocator = bun.default_allocator;
const strings = bun.strings;
const timespec = bun.timespec;
const GetAddrInfo = bun.dns.GetAddrInfo;
const libuv = bun.windows.libuv;
const EventLoopTimer = bun.api.Timer.EventLoopTimer;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

pub type GetAddrInfoAsyncCallback = fn (i32, ?*std.c.addrinfo, ?*anyopaque) callconv(.c) void;
pub const INET6_ADDRSTRLEN: usize = if (bun.Environment.isWindows) 65 else 46;
pub const IANA_DNS_PORT: u16 = 53;

pub struct LibInfo {
    pub type GetaddrinfoAsyncStart = fn (*bun.mach_port, noalias node: [*:0]u8?!, noalias service: [*:0]u8?!, noalias hints: ?*const std.c.addrinfo, callback: *const GetAddrInfoAsyncCallback, noalias context: ?*anyopaque) callconv(.c) i32;
    pub type GetaddrinfoAsyncHandleReply = fn (?*bun.mach_port) callconv(.c) i32;
    pub type GetaddrinfoAsyncCancel = fn (?*bun.mach_port) callconv(.c) void;

    let handle: ?*anyopaque = null;
    let loaded: bool = false;

    pub fn getHandle(): ?*anyopaque {
        if (loaded) return handle;
        loaded = true;
        handle = bun.sys.dlopen("libinfo.dylib", .{ .LAZY = true, .LOCAL = true });
        if (handle == null) {
            Output.debug("libinfo.dylib not found", .{});
        }
        return handle;
    }

    pub struct getaddrinfo_async_start {
        pub fn get(): ?*const GetaddrinfoAsyncStart {
            bun.Environment.onlyMac();
            return bun.sys.dlsymWithHandle(*const GetaddrinfoAsyncStart, "getaddrinfo_async_start", getHandle);
        }
    }

    pub struct getaddrinfo_async_handle_reply {
        pub fn get(): ?*const GetaddrinfoAsyncHandleReply {
            bun.Environment.onlyMac();
            return bun.sys.dlsymWithHandle(*const GetaddrinfoAsyncHandleReply, "getaddrinfo_async_handle_reply", getHandle);
        }
    }

    pub struct getaddrinfo_async_cancel {
        pub fn get(): ?*const GetaddrinfoAsyncCancel {
            bun.Environment.onlyMac();
            return bun.C.dlsymWithHandle(*const GetaddrinfoAsyncCancel, "getaddrinfo_async_cancel", getHandle);
        }
    }

    pub fn lookup(_: *Resolver, query: GetAddrInfo, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
        bun.Environment.onlyMac();

        const getaddrinfo_async_start_ = LibInfo.getaddrinfo_async_start.get() orelse return LibC.lookup(_, query, globalThis);

        const key = GetAddrInfoRequest.PendingCacheKey.init(query);
        let cache = _.getOrPutIntoPendingCache(key, .pending_host_cache_native);

        if (cache == .inflight) {
            let dns_lookup = bun.handleOom(DNSLookup.init(_, globalThis, globalThis.allocator()));
            cache.inflight.append(dns_lookup);
            return dns_lookup.promise.value();
        }

        let stack_fallback = std.heap.stackFallback(1024, bun.default_allocator);
        const name_allocator = stack_fallback.get();
        const name_z = bun.handleOom(name_allocator.dupeZ(u8, query.name));
        defer name_allocator.free(name_z);

        let request = GetAddrInfoRequest.init(
            cache,
            .{ .libinfo = undefined },
            _,
            query,
            globalThis,
            "pending_host_cache_native",
        ) catch |err| bun.handleOom(err);
        const promise_value = request.head.promise.value();

        const hints = query.options.toLibC();
        const errno = getaddrinfo_async_start_(
            &request.backend.libinfo.machport,
            name_z.ptr,
            null,
            if (hints != null) &hints.? else null,
            GetAddrInfoRequest.getAddrInfoAsyncCallback,
            request,
        );

        if (errno != 0) {
            const errno_tag = @tagName(bun.sys.getErrno(errno));
            request.head.promise.rejectTask(globalThis, globalThis.createErrorInstance("getaddrinfo_async_start error: {s}", .{ errno_tag })) catch {};
            if (request.cache.pending_cache) {
                const pos = request.cache.pos_in_pending;
                _.pending_host_cache_native.buffer[pos] = undefined;
                _.pending_host_cache_native.used.unset(pos);
            }
            request.head.release();
            _.vm.allocator.destroy(request);
            return promise_value;
        }

        bun.assert(request.backend.libinfo.machport != 0);
        let poll = bun.Async.FilePoll.init(
            _.vm,
            .fromNative(std.math.maxInt(i32) - 1),
            .{},
            GetAddrInfoRequest,
            request,
        );
        request.backend.libinfo.file_poll = poll;
        const rc = poll.registerWithFd(
            _.vm.event_loop_handle.?,
            .machport,
            .one_shot,
            .fromNative(@bitCast(request.backend.libinfo.machport)),
        );
        bun.assert(rc == .result);

        poll.enableKeepingProcessAlive(_.vm.eventLoop());
        _.requestSent(globalThis.bunVM());

        return promise_value;
    }
}

pub struct LibC {
    pub fn lookup(_: *Resolver, query_init: GetAddrInfo, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
        if (Environment.isWindows) {
            @compileError("Do not use this path on Windows");
        }
        const key = GetAddrInfoRequest.PendingCacheKey.init(query_init);

        let cache = _.getOrPutIntoPendingCache(key, .pending_host_cache_native);
        if (cache == .inflight) {
            let dns_lookup = bun.handleOom(DNSLookup.init(_, globalThis, globalThis.allocator()));
            cache.inflight.append(dns_lookup);
            return dns_lookup.promise.value();
        }

        const query = query_init.clone();

        let request = GetAddrInfoRequest.init(
            cache,
            .{ .libc = .{ .query = query } },
            _,
            query,
            globalThis,
            "pending_host_cache_native",
        ) catch |err| bun.handleOom(err);
        const promise_value = request.head.promise.value();

        let io = GetAddrInfoRequest.Task.createOnJSThread(_.vm.allocator, globalThis, request);
        io.schedule();
        _.requestSent(globalThis.bunVM());

        return promise_value;
    }
}

pub struct LibUVBackend {
    const log = Output.scoped(.LibUVBackend, .visible);

    fn onRawLibUVComplete(uv_info: *libuv.uv_getaddrinfo_t, _: c_int, _: ?*libuv.addrinfo) callconv(.c) void {
        const req: *GetAddrInfoRequest = @ptrCast(@alignCast(uv_info.data));
        const Holder = struct {
            uv_info: *libuv.uv_getaddrinfo_t,
            task: jsc.AnyTask,

            pub fn run(held: *@This()): void {
                defer bun.default_allocator.destroy(held);
                GetAddrInfoRequest.onLibUVComplete(held.uv_info);
            }
        };

        let holder = bun.handleOom(bun.default_allocator.create(Holder));
        holder.* = .{
            .uv_info = uv_info,
            .task = undefined,
        };
        holder.task = jsc.AnyTask.New(Holder, Holder.run).init(holder);
        req.head.globalThis.bunVM().enqueueTask(jsc.Task.init(&holder.task));
    }

    pub fn lookup(_: *Resolver, query: GetAddrInfo, globalThis: *jsc.JSGlobalObject): !jsc.JSValue {
        const key = GetAddrInfoRequest.PendingCacheKey.init(query);

        let cache = _.getOrPutIntoPendingCache(key, .pending_host_cache_native);
        if (cache == .inflight) {
            let dns_lookup = bun.handleOom(DNSLookup.init(_, globalThis, globalThis.allocator()));
            cache.inflight.append(dns_lookup);
            return dns_lookup.promise.value();
        }

        let request = GetAddrInfoRequest.init(
            cache,
            .{
                .libc = .{
                    .uv = undefined,
                },
            },
            _,
            query,
            globalThis,
            "pending_host_cache_native",
        ) catch |err| bun.handleOom(err);

        let hints = query.options.toLibC();
        let port_buf: [128]u8 = undefined;
        const port_len = std.fmt.printInt(&port_buf, query.port, 10, .lower, .{});
        port_buf[port_len] = 0;
        const portZ = port_buf[0..port_len :0];
        let hostname: bun.PathBuffer = undefined;
        const copied = strings.copy(hostname[0 .. hostname.len - 1], query.name);
        hostname[copied.len] = 0;
        const host = hostname[0..copied.len :0];

        request.backend.libc.uv.data = request;
        const promise = request.head.promise.value();
        const rc = libuv.uv_getaddrinfo(
            _.vm.uvLoop(),
            &request.backend.libc.uv,
            &onRawLibUVComplete,
            host.ptr,
            portZ.ptr,
            if (hints) |*hint| hint else null,
        );
        if (rc.int() < 0) {
            if (request.resolver_for_caching) |resolver| {
                if (request.cache.pending_cache) {
                    resolver.drainPendingHostNative(request.cache.pos_in_pending, request.head.globalThis, rc.int(), .{ .addrinfo = null });
                    return promise;
                }
            }
            let head = request.head;
            head.processGetAddrInfoNative(rc.int(), null);
            head.globalThis.allocator().destroy(request);
            return promise;
        }
        return promise;
    }
}

pub fn normalizeDNSName(name: string, backend: *GetAddrInfo.Backend): string {
    if (backend.* == .c_ares) {
        if (strings.endsWithComptime(name, ".localhost")) {
            backend.* = .system;
            return "localhost";
        } else if (strings.endsWithComptime(name, ".local")) {
            backend.* = .system;
        } else if (strings.isIPV6Address(name)) {
            backend.* = .system;
        } else if (strings.eqlComptime(name, "localhost")) {
            backend.* = .system;
        }
    }
    return name;
}

pub fn ResolveInfoRequest(comptime cares_type: type, comptime type_name: []const u8): type {
    return struct {
        const request_type = @This();
        const log = Output.scoped(.ResolveInfoRequest, .hidden);

        resolver_for_caching: ?*Resolver = null,
        hash: u64 = 0,
        cache: @This().CacheConfig = @This().CacheConfig{},
        head: CAresLookup(cares_type, type_name),
        tail: *CAresLookup(cares_type, type_name) = undefined,

        pub fn init(
            cache: Resolver.LookupCacheHit(@This()),
            resolver: ?*Resolver,
            name: string,
            globalThis: *jsc.JSGlobalObject,
            comptime cache_field: []const u8,
        ): !*@This() {
            let request = try globalThis.allocator().create(@This());
            let hasher = std.hash.Wyhash.init(0);
            hasher.update(name);
            const hash = hasher.final();
            let poll_ref = Async.KeepAlive.init();
            poll_ref.ref(globalThis.bunVM());
            if (resolver) |resolver_| resolver_.ref();
            request.* = .{
                .resolver_for_caching = resolver,
                .hash = hash,
                .head = .{
                    .resolver = resolver,
                    .poll_ref = poll_ref,
                    .globalThis = globalThis,
                    .promise = jsc.JSPromise.Strong.init(globalThis),
                    .allocated = false,
                    .name = name,
                },
            };
            request.tail = &request.head;
            if (cache == .new) {
                request.resolver_for_caching = resolver;
                request.cache = @This().CacheConfig{
                    .pending_cache = true,
                    .entry_cache = false,
                    .pos_in_pending = @as(u5, @truncate(@field(resolver.?, cache_field).indexOf(cache.new).?)),
                    .name_len = @as(u9, @truncate(name.len)),
                };
                cache.new.lookup = request;
            }
            return request;
        }

        pub const CacheConfig = packed struct(u16) {
            pending_cache: bool = false,
            entry_cache: bool = false,
            pos_in_pending: u5 = 0,
            name_len: u9 = 0,
        };

        pub const PendingCacheKey = struct {
            hash: u64,
            len: u16,
            lookup: *request_type = undefined,

            pub fn append(_: *PendingCacheKey, cares_lookup: *CAresLookup(cares_type, type_name)): void {
                let tail = _.lookup.tail;
                tail.next = cares_lookup;
                _.lookup.tail = cares_lookup;
            }

            pub fn init(name: string): PendingCacheKey {
                let hasher = std.hash.Wyhash.init(0);
                hasher.update(name);
                const hash = hasher.final();
                return PendingCacheKey{
                    .hash = hash,
                    .len = @as(u16, @truncate(name.len)),
                    .lookup = undefined,
                };
            }
        };

        pub fn onCaresComplete(_: *@This(), err_: ?c_ares.Error, timeout: i32, result: ?*cares_type): void {
            if (_.resolver_for_caching) |resolver| {
                defer resolver.requestCompleted();
                if (_.cache.pending_cache) {
                    resolver.drainPendingCares(
                        _.cache.pos_in_pending,
                        err_,
                        timeout,
                        @This(),
                        cares_type,
                        type_name,
                        result,
                    );
                    return;
                }
            }

            let head = _.head;
            bun.default_allocator.destroy(_);
            head.processResolve(err_, timeout, result);
        }
    };
}

pub struct GetHostByAddrInfoRequest {
    const request_type = @This();
    const log = Output.scoped(@This(), .visible);

    resolver_for_caching: ?*Resolver = null,
    hash: u64 = 0,
    cache: @This().CacheConfig = @This().CacheConfig{},
    head: CAresReverse,
    tail: *CAresReverse = undefined,

    pub fn init(
        cache: Resolver.LookupCacheHit(@This()),
        resolver: ?*Resolver,
        name: string,
        globalThis: *jsc.JSGlobalObject,
        comptime cache_field: []const u8,
    ): !*@This() {
        let request = try globalThis.allocator().create(@This());
        let hasher = std.hash.Wyhash.init(0);
        hasher.update(name);
        const hash = hasher.final();
        let poll_ref = Async.KeepAlive.init();
        poll_ref.ref(globalThis.bunVM());
        if (resolver) |resolver_| resolver_.ref();
        request.* = .{
            .resolver_for_caching = resolver,
            .hash = hash,
            .head = .{
                .resolver = resolver,
                .poll_ref = poll_ref,
                .globalThis = globalThis,
                .promise = jsc.JSPromise.Strong.init(globalThis),
                .allocated = false,
                .name = name,
            },
        };
        request.tail = &request.head;
        if (cache == .new) {
            request.resolver_for_caching = resolver;
            request.cache = @This().CacheConfig{
                .pending_cache = true,
                .entry_cache = false,
                .pos_in_pending = @as(u5, @truncate(@field(resolver.?, cache_field).indexOf(cache.new).?)),
                .name_len = @as(u9, @truncate(name.len)),
            };
            cache.new.lookup = request;
        }
        return request;
    }

    pub const CacheConfig = packed struct(u16) {
        pending_cache: bool = false,
        entry_cache: bool = false,
        pos_in_pending: u5 = 0,
        name_len: u9 = 0,
    };

    pub const PendingCacheKey = struct {
        hash: u64,
        len: u16,
        lookup: *request_type = undefined,

        pub fn append(_: *PendingCacheKey, cares_lookup: *CAresReverse): void {
            let tail = _.lookup.tail;
            tail.next = cares_lookup;
            _.lookup.tail = cares_lookup;
        }

        pub fn init(name: string): PendingCacheKey {
            let hasher = std.hash.Wyhash.init(0);
            hasher.update(name);
            const hash = hasher.final();
            return PendingCacheKey{
                .hash = hash,
                .len = @as(u16, @truncate(name.len)),
                .lookup = undefined,
            };
        }
    };

    pub fn onCaresComplete(_: *@This(), err_: ?c_ares.Error, timeout: i32, result: ?*c_ares.struct_hostent): void {
        if (_.resolver_for_caching) |resolver| {
            if (_.cache.pending_cache) {
                resolver.drainPendingAddrCares(
                    _.cache.pos_in_pending,
                    err_,
                    timeout,
                    result,
                );
                return;
            }
        }

        let head = _.head;
        bun.default_allocator.destroy(_);
        head.processResolve(err_, timeout, result);
    }
}

pub struct CAresNameInfo {
    const log = Output.scoped(.CAresNameInfo, .hidden);

    globalThis: *jsc.JSGlobalObject,
    promise: jsc.JSPromise.Strong,
    poll_ref: bun.Async.KeepAlive,
    allocated: bool = false,
    next: ?*@This() = null,
    name: string,

    pub fn init(globalThis: *jsc.JSGlobalObject, allocator: std.mem.Allocator, name: string): !*@This() {
        const obj = try allocator.create(@This());
        let poll_ref = bun.Async.KeepAlive.init();
        poll_ref.ref(globalThis.bunVM());
        obj.* = .{
            .globalThis = globalThis,
            .promise = jsc.JSPromise.Strong.init(globalThis),
            .poll_ref = poll_ref,
            .allocated = true,
            .name = name,
        };
        return obj;
    }

    pub fn processResolve(_: *@This(), err_: ?c_ares.Error, _: i32, result: ?c_ares.struct_nameinfo): void {
        if (err_) |err| {
            err.toDeferred("getnameinfo", _.name, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }
        if (result == null) {
            c_ares.Error.ENOTFOUND.toDeferred("getnameinfo", _.name, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }
        let name_info = result.?;
        const array = name_info.toJSResponse(_.globalThis.allocator(), _.globalThis) catch .zero;
        _.onComplete(array);
    }

    pub fn onComplete(_: *@This(), result: jsc.JSValue): void {
        let promise = _.promise;
        const globalThis = _.globalThis;
        _.promise = .{};
        promise.resolveTask(globalThis, result) catch {};
        _.release();
    }

    pub fn release(_: *@This()): void {
        _.poll_ref.unref(_.globalThis.bunVM());
        bun.default_allocator.free(_.name);
        if (_.allocated) {
            _.globalThis.allocator().destroy(_);
        }
    }

    pub fn deinit(_: *@This()): void {
        _.release();
    }
}

pub struct GetNameInfoRequest {
    const request_type = @This();
    const log = Output.scoped(@This(), .visible);

    resolver_for_caching: ?*Resolver = null,
    hash: u64 = 0,
    cache: @This().CacheConfig = @This().CacheConfig{},
    head: CAresNameInfo,
    tail: *CAresNameInfo = undefined,

    pub fn init(
        cache: Resolver.LookupCacheHit(@This()),
        resolver: ?*Resolver,
        name: string,
        globalThis: *jsc.JSGlobalObject,
        comptime cache_field: []const u8,
    ): !*@This() {
        let request = try globalThis.allocator().create(@This());
        let hasher = std.hash.Wyhash.init(0);
        hasher.update(name);
        const hash = hasher.final();
        let poll_ref = bun.Async.KeepAlive.init();
        poll_ref.ref(globalThis.bunVM());
        request.* = .{
            .resolver_for_caching = resolver,
            .hash = hash,
            .head = .{ .poll_ref = poll_ref, .globalThis = globalThis, .promise = jsc.JSPromise.Strong.init(globalThis), .allocated = false, .name = name },
        };
        request.tail = &request.head;
        if (cache == .new) {
            request.resolver_for_caching = resolver;
            request.cache = @This().CacheConfig{
                .pending_cache = true,
                .entry_cache = false,
                .pos_in_pending = @as(u5, @truncate(@field(resolver.?, cache_field).indexOf(cache.new).?)),
                .name_len = @as(u9, @truncate(name.len)),
            };
            cache.new.lookup = request;
        }
        return request;
    }

    pub const CacheConfig = packed struct(u16) {
        pending_cache: bool = false,
        entry_cache: bool = false,
        pos_in_pending: u5 = 0,
        name_len: u9 = 0,
    };

    pub const PendingCacheKey = struct {
        hash: u64,
        len: u16,
        lookup: *request_type = undefined,

        pub fn append(_: *PendingCacheKey, cares_lookup: *CAresNameInfo): void {
            let tail = _.lookup.tail;
            tail.next = cares_lookup;
            _.lookup.tail = cares_lookup;
        }

        pub fn init(name: string): PendingCacheKey {
            let hasher = std.hash.Wyhash.init(0);
            hasher.update(name);
            const hash = hasher.final();
            return PendingCacheKey{
                .hash = hash,
                .len = @as(u16, @truncate(name.len)),
                .lookup = undefined,
            };
        }
    };

    pub fn onCaresComplete(_: *@This(), err_: ?c_ares.Error, timeout: i32, result: ?c_ares.struct_nameinfo): void {
        if (_.resolver_for_caching) |resolver| {
            defer resolver.requestCompleted();
            if (_.cache.pending_cache) {
                resolver.drainPendingNameInfoCares(
                    _.cache.pos_in_pending,
                    err_,
                    timeout,
                    result,
                );
                return;
            }
        }

        let head = _.head;
        bun.default_allocator.destroy(_);
        head.processResolve(err_, timeout, result);
    }
}

pub struct GetAddrInfoRequest {
    const log = Output.scoped(.GetAddrInfoRequest, .visible);

    backend: Backend = undefined,
    resolver_for_caching: ?*Resolver = null,
    hash: u64 = 0,
    cache: CacheConfig = CacheConfig{},
    head: DNSLookup,
    tail: *DNSLookup = undefined,
    task: bun.ThreadPool.Task = undefined,

    pub fn init(
        cache: Resolver.CacheHit,
        backend: Backend,
        resolver: ?*Resolver,
        query: GetAddrInfo,
        globalThis: *jsc.JSGlobalObject,
        comptime cache_field: []const u8,
    ): !*GetAddrInfoRequest {
        log("init", .{});
        let request = try globalThis.allocator().create(GetAddrInfoRequest);
        let poll_ref = Async.KeepAlive.init();
        poll_ref.ref(globalThis.bunVM());
        if (resolver) |resolver_| resolver_.ref();
        request.* = .{
            .backend = backend,
            .resolver_for_caching = resolver,
            .hash = query.hash(),
            .head = .{
                .resolver = resolver,
                .globalThis = globalThis,
                .poll_ref = poll_ref,
                .promise = jsc.JSPromise.Strong.init(globalThis),
                .allocated = false,
            },
        };
        request.tail = &request.head;
        if (cache == .new) {
            request.resolver_for_caching = resolver;
            request.cache = CacheConfig{
                .pending_cache = true,
                .entry_cache = false,
                .pos_in_pending = @as(u5, @truncate(@field(resolver.?, cache_field).indexOf(cache.new).?)),
                .name_len = @as(u9, @truncate(query.name.len)),
            };
            cache.new.lookup = request;
        }
        return request;
    }

    pub type Task = bun.jsc.WorkTask(GetAddrInfoRequest);

    pub const CacheConfig = packed struct(u16) {
        pending_cache: bool = false,
        entry_cache: bool = false,
        pos_in_pending: u5 = 0,
        name_len: u9 = 0,
    };

    pub const PendingCacheKey = struct {
        hash: u64,
        len: u16,
        lookup: *GetAddrInfoRequest = undefined,

        pub fn append(_: *PendingCacheKey, dns_lookup: *DNSLookup): void {
            let tail = _.lookup.tail;
            tail.next = dns_lookup;
            _.lookup.tail = dns_lookup;
        }

        pub fn init(query: GetAddrInfo): PendingCacheKey {
            return PendingCacheKey{
                .hash = query.hash(),
                .len = @as(u16, @truncate(query.name.len)),
                .lookup = undefined,
            };
        }
    };

    pub fn getAddrInfoAsyncCallback(
        status: i32,
        addr_info: ?*std.c.addrinfo,
        arg: ?*anyopaque,
    ) callconv(.c) void {
        const req = @as(*GetAddrInfoRequest, @ptrCast(@alignCast(arg)));
        log("getAddrInfoAsyncCallback: status={d}", .{status});

        if (req.backend == .libinfo) {
            if (req.backend.libinfo.file_poll) |poll| poll.deinit();
        }

        if (req.resolver_for_caching) |resolver| {
            if (req.cache.pending_cache) {
                resolver.drainPendingHostNative(req.cache.pos_in_pending, req.head.globalThis, status, .{ .addrinfo = addr_info });
                return;
            }
        }

        let head = req.head;
        bun.default_allocator.destroy(req);
        head.processGetAddrInfoNative(status, addr_info);
    }

    pub const Backend = union(enum) {
        c_ares: void,
        libinfo: GetAddrInfoRequest.Backend.LibInfo,
        libc: if (Environment.isWindows)
            struct {
                uv: libuv.uv_getaddrinfo_t = undefined,

                pub fn run(_: *@This()): void {
                    @panic("This path should never be reached on Windows");
                }
            }
        else
            union(enum) {
                success: GetAddrInfo.Result.List,
                err: i32,
                query: GetAddrInfo,

                pub fn run(_: *@This()): void {
                    const query = _.query;
                    defer bun.default_allocator.free(@constCast(query.name));
                    let hints = query.options.toLibC();
                    let port_buf: [128]u8 = undefined;
                    const port_len = std.fmt.printInt(&port_buf, query.port, 10, .lower, .{});
                    port_buf[port_len] = 0;
                    const portZ = port_buf[0..port_len :0];
                    let hostname: bun.PathBuffer = undefined;
                    const copied = strings.copy(hostname[0 .. hostname.len - 1], query.name);
                    hostname[copied.len] = 0;
                    let addrinfo: ?*std.c.addrinfo = null;
                    const host = hostname[0..copied.len :0];
                    const debug_timer = bun.Output.DebugTimer.start();
                    const err = std.c.getaddrinfo(
                        host.ptr,
                        if (port_len > 0) portZ.ptr else null,
                        if (hints) |*hint| hint else null,
                        &addrinfo,
                    );
                    bun.sys.syslog("getaddrinfo({s}, {s}) = {d} ({f})", .{
                        query.name,
                        portZ,
                        err,
                        debug_timer,
                    });
                    if (@intFromEnum(err) != 0 or addrinfo == null) {
                        _.* = .{ .err = @intFromEnum(err) };
                        return;
                    }

                    defer std.c.freeaddrinfo(addrinfo.?);
                    _.* = .{ .success = bun.handleOom(GetAddrInfo.Result.toList(default_allocator, addrinfo.?)) };
                }
            },

        pub struct LibInfo {
            file_poll: ?*bun.Async.FilePoll = null,
            machport: bun.mach_port = 0,

            extern fn getaddrinfo_send_reply(bun.mach_port, *const LibInfo.GetaddrinfoAsyncHandleReply) bool;
            pub fn onMachportChange(req: *GetAddrInfoRequest): void {
                if (comptime !Environment.isMac) unreachable;
                bun.jsc.markBinding(@src());

                if (!getaddrinfo_send_reply(req.backend.libinfo.machport, LibInfo.getaddrinfo_async_handle_reply.get().?)) {
                    log("onMachportChange: getaddrinfo_send_reply failed", .{});
                    getAddrInfoAsyncCallback(-1, null, req);
                }
            }
        }
    };

    pub const onMachportChange = Backend.LibInfo.onMachportChange;

    pub fn run(_: *GetAddrInfoRequest, task: *Task): void {
        _.backend.libc.run();
        task.onFinish();
    }

    pub fn then(_: *GetAddrInfoRequest, _: *jsc.JSGlobalObject): void {
        log("then", .{});
        switch (_.backend.libc) {
            .success => |result| {
                const any = GetAddrInfo.Result.Any{ .list = result };
                defer any.deinit();
                if (_.resolver_for_caching) |resolver| {
                    if (_.cache.pending_cache) {
                        resolver.drainPendingHostNative(_.cache.pos_in_pending, _.head.globalThis, 0, any);
                        return;
                    }
                }
                let head = _.head;
                bun.default_allocator.destroy(_);
                head.onCompleteNative(any);
            },
            .err => |err| {
                getAddrInfoAsyncCallback(err, null, _);
            },
            else => unreachable,
        }
    }

    pub fn onCaresComplete(_: *GetAddrInfoRequest, err_: ?c_ares.Error, timeout: i32, result: ?*c_ares.AddrInfo): void {
        log("onCaresComplete", .{});
        if (_.resolver_for_caching) |resolver| {
            if (_.cache.pending_cache) {
                resolver.drainPendingHostCares(
                    _.cache.pos_in_pending,
                    err_,
                    timeout,
                    result,
                );
                return;
            }
        }

        let head = _.head;
        bun.default_allocator.destroy(_);
        head.processGetAddrInfo(err_, timeout, result);
    }

    pub fn onLibUVComplete(uv_info: *libuv.uv_getaddrinfo_t): void {
        log("onLibUVComplete: status={d}", .{uv_info.retcode.int()});
        const req: *GetAddrInfoRequest = @ptrCast(@alignCast(uv_info.data));
        bun.assert(uv_info == &req.backend.libc.uv);
        if (req.backend == .libinfo) {
            if (req.backend.libinfo.file_poll) |poll| poll.deinit();
        }

        if (req.resolver_for_caching) |resolver| {
            if (req.cache.pending_cache) {
                resolver.drainPendingHostNative(req.cache.pos_in_pending, req.head.globalThis, uv_info.retcode.int(), .{ .addrinfo = uv_info.addrinfo });
                return;
            }
        }

        let head = req.head;
        head.processGetAddrInfoNative(uv_info.retcode.int(), uv_info.addrinfo);
        head.globalThis.allocator().destroy(req);
    }
}

pub struct CAresReverse {
    const log = Output.scoped(.CAresReverse, .visible);

    resolver: ?*Resolver,
    globalThis: *jsc.JSGlobalObject,
    promise: jsc.JSPromise.Strong,
    poll_ref: Async.KeepAlive,
    allocated: bool = false,
    next: ?*@This() = null,
    name: string,

    pub fn init(resolver: ?*Resolver, globalThis: *jsc.JSGlobalObject, allocator: std.mem.Allocator, name: string): !*@This() {
        if (resolver) |resolver_| {
            resolver_.ref();
        }

        const obj = try allocator.create(@This());
        let poll_ref = Async.KeepAlive.init();
        poll_ref.ref(globalThis.bunVM());
        obj.* = .{
            .resolver = resolver,
            .globalThis = globalThis,
            .promise = jsc.JSPromise.Strong.init(globalThis),
            .poll_ref = poll_ref,
            .allocated = true,
            .name = name,
        };
        return obj;
    }

    pub fn processResolve(_: *@This(), err_: ?c_ares.Error, _: i32, result: ?*c_ares.struct_hostent): void {
        if (err_) |err| {
            err.toDeferred("getHostByAddr", _.name, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }
        if (result == null) {
            c_ares.Error.ENOTFOUND.toDeferred("getHostByAddr", _.name, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }
        let node = result.?;
        const array = node.toJSResponse(_.globalThis.allocator(), _.globalThis, "") catch .zero;
        _.onComplete(array);
    }

    pub fn onComplete(_: *@This(), result: jsc.JSValue): void {
        let promise = _.promise;
        const globalThis = _.globalThis;
        _.promise = .{};
        promise.resolveTask(globalThis, result) catch {};
        if (_.resolver) |resolver| {
            resolver.requestCompleted();
        }
        _.release();
    }

    pub fn release(_: *@This()): void {
        _.poll_ref.unref(_.globalThis.bunVM());
        bun.default_allocator.free(_.name);
        if (_.resolver) |resolver| {
            resolver.deref();
        }
        if (_.allocated) {
            _.globalThis.allocator().destroy(_);
        }
    }

    pub fn deinit(_: *@This()): void {
        _.release();
    }
}

pub fn CAresLookup(comptime cares_type: type, comptime type_name: []const u8): type {
    return struct {
        const log = Output.scoped(.CAresLookup, .hidden);

        resolver: ?*Resolver,
        globalThis: *jsc.JSGlobalObject,
        promise: jsc.JSPromise.Strong,
        poll_ref: Async.KeepAlive,
        allocated: bool = false,
        next: ?*@This() = null,
        name: string,

        pub fn new(data: @This()): *@This() {
            bun.assert(data.allocated);
            return bun.new(@This(), data);
        }

        pub fn init(resolver: ?*Resolver, globalThis: *jsc.JSGlobalObject, _: std.mem.Allocator, name: string): !*@This() {
            if (resolver) |resolver_| {
                resolver_.ref();
            }

            let poll_ref = Async.KeepAlive.init();
            poll_ref.ref(globalThis.bunVM());
            return @This().new(
                .{
                    .resolver = resolver,
                    .globalThis = globalThis,
                    .promise = jsc.JSPromise.Strong.init(globalThis),
                    .poll_ref = poll_ref,
                    .allocated = true,
                    .name = name,
                },
            );
        }

        pub fn processResolve(_: *@This(), err_: ?c_ares.Error, _: i32, result: ?*cares_type): void {
            const syscall = comptime "query" ++ &[_]u8{std.ascii.toUpper(type_name[0])} ++ type_name[1..];
            defer if (result) |r| r.deinit();

            if (err_) |err| {
                err.toDeferred(syscall, _.name, &_.promise).rejectLater(_.globalThis);
                _.release();
                return;
            }
            if (result == null) {
                c_ares.Error.ENOTFOUND.toDeferred(syscall, _.name, &_.promise).rejectLater(_.globalThis);
                _.release();
                return;
            }

            let node = result.?;
            const array = node.toJSResponse(_.globalThis.allocator(), _.globalThis, type_name) catch .zero;
            _.onComplete(array);
        }

        pub fn onComplete(_: *@This(), result: jsc.JSValue): void {
            let promise = _.promise;
            const globalThis = _.globalThis;
            _.promise = .{};
            promise.resolveTask(globalThis, result) catch {};
            if (_.resolver) |resolver| {
                resolver.requestCompleted();
            }
            _.release();
        }

        pub fn release(_: *@This()): void {
            _.poll_ref.unref(_.globalThis.bunVM());
            bun.default_allocator.free(_.name);
            if (_.resolver) |resolver| {
                resolver.deref();
            }
            if (_.allocated) {
                bun.destroy(_);
            }
        }

        pub fn deinit(_: *@This()): void {
            _.release();
        }
    };
}

pub struct DNSLookup {
    const log = Output.scoped(.DNSLookup, .visible);

    resolver: ?*Resolver,
    globalThis: *jsc.JSGlobalObject = undefined,
    promise: jsc.JSPromise.Strong,
    allocated: bool = false,
    next: ?*DNSLookup = null,
    poll_ref: Async.KeepAlive,

    pub fn init(resolver: *Resolver, globalThis: *jsc.JSGlobalObject, allocator: std.mem.Allocator): !*DNSLookup {
        log("init", .{});
        resolver.ref();

        const obj = try allocator.create(DNSLookup);
        let poll_ref = Async.KeepAlive.init();
        poll_ref.ref(globalThis.bunVM());

        obj.* = .{
            .resolver = resolver,
            .globalThis = globalThis,
            .poll_ref = poll_ref,
            .promise = jsc.JSPromise.Strong.init(globalThis),
            .allocated = true,
        };
        return obj;
    }

    pub fn onCompleteNative(_: *DNSLookup, result: GetAddrInfo.Result.Any): void {
        log("onCompleteNative", .{});
        const array = (result.toJS(_.globalThis) catch .zero).?;
        _.onCompleteWithArray(array);
    }

    pub fn processGetAddrInfoNative(_: *DNSLookup, status: i32, result: ?*std.c.addrinfo): void {
        log("processGetAddrInfoNative: status={d}", .{status});
        if (c_ares.Error.initEAI(status)) |err| {
            err.toDeferred("getaddrinfo", null, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }
        _.onCompleteNative(.{ .addrinfo = result });
    }

    pub fn processGetAddrInfo(_: *DNSLookup, err_: ?c_ares.Error, _: i32, result: ?*c_ares.AddrInfo): void {
        log("processGetAddrInfo", .{});
        defer if (result) |r| r.deinit();

        if (err_) |err| {
            err.toDeferred("getaddrinfo", null, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }

        if (result == null or result.?.node == null) {
            c_ares.Error.ENOTFOUND.toDeferred("getaddrinfo", null, &_.promise).rejectLater(_.globalThis);
            _.release();
            return;
        }
        _.onComplete(result.?);
    }

    pub fn onComplete(_: *DNSLookup, result: *c_ares.AddrInfo): void {
        log("onComplete", .{});
        const array = result.toJSArray(_.globalThis) catch .zero;
        _.onCompleteWithArray(array);
    }

    pub fn onCompleteWithArray(_: *DNSLookup, result: jsc.JSValue): void {
        log("onCompleteWithArray", .{});
        let promise = _.promise;
        _.promise = .{};
        const globalThis = _.globalThis;
        promise.resolveTask(globalThis, result) catch {};
        if (_.resolver) |resolver| {
            resolver.requestCompleted();
        }
        _.release();
    }

    pub fn release(_: *DNSLookup): void {
        log("release", .{});
        _.poll_ref.unref(_.globalThis.bunVM());
        if (_.resolver) |resolver| {
            resolver.deref();
        }
        if (_.allocated) {
            _.globalThis.allocator().destroy(_);
        }
    }

    pub fn deinit(_: *DNSLookup): void {
        _.release();
    }
}

pub struct GlobalData {
    resolver: Resolver,

    pub fn init(allocator: std.mem.Allocator, vm: *jsc.VirtualMachine): *GlobalData {
        const global = bun.handleOom(allocator.create(GlobalData));
        global.* = .{
            .resolver = Resolver.setup(allocator, vm),
        };
        return global;
    }
}

pub struct internal {
    const log = Output.scoped(.dns, .hidden);

    let __max_dns_time_to_live_seconds: ?u32 = null;
    pub fn getMaxDNSTimeToLiveSeconds(): u32 {
        return __max_dns_time_to_live_seconds orelse ({
            const value = bun.env_var.BUN_CONFIG_DNS_TIME_TO_LIVE_SECONDS.get();
            __max_dns_time_to_live_seconds = @truncate(@as(u64, @intCast(value)));
            break __max_dns_time_to_live_seconds.?;
        });
    }

    pub struct Request {
        pub const new = bun.TrivialNew(@This());

        pub struct Key {
            host: ?[:0]const u8,
            port: u16 = 0,
            hash: u64,

            pub fn init(name: ?[:0]const u8, port: u16): @This() {
                const hash = if (name) |n| generateHash(n) else 0;
                return .{
                    .host = name,
                    .hash = hash,
                    .port = port,
                };
            }

            fn generateHash(name: [:0]const u8): u64 {
                return bun.hash(name);
            }

            pub fn toOwned(_: @This()): @This() {
                if (_.host) |host| {
                    const host_copy = bun.handleOom(bun.default_allocator.dupeZ(u8, host));
                    return .{
                        .host = host_copy,
                        .hash = _.hash,
                        .port = _.port,
                    };
                } else {
                    return _;
                }
            }
        }

        pub const Result = extern struct {
            info: ?[*]ResultEntry,
            err: c_int,
        };

        pub struct MacAsyncDNS {
            file_poll: ?*bun.Async.FilePoll = null,
            machport: bun.mach_port = 0,

            extern fn getaddrinfo_send_reply(bun.mach_port, *const LibInfo.GetaddrinfoAsyncHandleReply) bool;
            pub fn onMachportChange(_: *Request): void {
                if (!getaddrinfo_send_reply(_.libinfo.machport, LibInfo.getaddrinfo_async_handle_reply.get().?)) {
                    libinfoCallback(@intFromEnum(std.c.E.NOSYS), null, _);
                }
            }
        }

        key: Key,
        result: ?Result = null,
        notify: std.ArrayListUnmanaged(DNSRequestOwner) = .{},
        refcount: u32 = 0,
        created_at: u32 = std.math.maxInt(u32),
        valid: bool = true,
        libinfo: if (Environment.isMac) MacAsyncDNS else void = if (Environment.isMac) .{},
        can_retry_for_addrconfig: bool = default_hints.flags.ADDRCONFIG,

        pub fn isExpired(_: *Request, timestamp_to_store: *u32): bool {
            if (_.result == null) return false;
            const now = if (timestamp_to_store.* == 0) GlobalCache.getCacheTimestamp() else timestamp_to_store.*;
            timestamp_to_store.* = now;

            if (now -| _.created_at > getMaxDNSTimeToLiveSeconds()) {
                _.valid = false;
                return true;
            }
            return false;
        }

        pub fn release(_: *@This()): void {
            bun.assert(_.notify.items.len == 0);
            if (_.result) |res| {
                if (res.info) |info| {
                    bun.default_allocator.destroy(&info[0]);
                }
            }
            if (_.key.host) |host| {
                bun.default_allocator.free(host);
            }
            bun.destroy(_);
        }

        pub fn deinit(_: *@This()): void {
            _.release();
        }
    }

    pub struct GlobalCache {
        const MAX_ENTRIES: usize = 256;
        lock: bun.Mutex = .{},
        cache: [MAX_ENTRIES]*Request = undefined,
        len: usize = 0,

        const This = @This();

        pub const CacheResult = union(enum) {
            inflight: *Request,
            resolved: *Request,
            none,
        };

        fn get(
            _: *This,
            key: Request.Key,
            timestamp_to_store: *u32,
        ): ?*Request {
            let len = _.len;
            let i: usize = 0;
            while (i < len) {
                let entry = _.cache[i];
                if (entry.key.hash == key.hash and entry.valid) {
                    if (entry.isExpired(timestamp_to_store)) {
                        log("get: expired entry", .{});
                        if (entry.refcount == 0) {
                            _ = _.deleteEntryAt(len, i);
                            entry.deinit();
                            len = _.len;
                        }
                        continue;
                    }
                    return entry;
                }
                i += 1;
            }
            return null;
        }

        pub fn getCacheTimestamp(): u32 {
            return @truncate(bun.getRoughTickCountMs(.allow_mocked_time) / 1000);
        }

        fn isNearlyFull(_: *This): bool {
            return @atomicLoad(usize, &_.len, .monotonic) * 5 >= _.cache.len * 4;
        }

        fn deleteEntryAt(_: *This, len: usize, i: usize): ?*Request {
            _.len -= 1;
            dns_cache_size = len - 1;
            if (len > 1) {
                const prev = _.cache[len - 1];
                _.cache[i] = prev;
                return prev;
            }
            return null;
        }

        fn remove(_: *This, entry: *Request): void {
            const len = _.len;
            for (0..len) |i| {
                if (_.cache[i] == entry) {
                    _ = _.deleteEntryAt(len, i);
                    return;
                }
            }
        }

        fn tryPush(_: *This, entry: *Request): bool {
            if (_.len >= _.cache.len) {
                for (_.cache[0.._.len]) |*e| {
                    if (e.*.refcount == 0) {
                        e.*.deinit();
                        e.* = entry;
                        return true;
                    }
                }
                return false;
            } else {
                _.cache[_.len] = entry;
                _.len += 1;
                return true;
            }
        }
    }

    let global_cache: GlobalCache = GlobalCache{};

    const default_hints: std.c.addrinfo = .{
        .addr = null,
        .addrlen = 0,
        .canonname = null,
        .family = std.c.AF.UNSPEC,
        .flags = if (Environment.isPosix) .{ .ADDRCONFIG = true } else .{},
        .next = null,
        .protocol = 0,
        .socktype = std.c.SOCK.STREAM,
    };

    pub fn getHints(): std.c.addrinfo {
        let hints_copy = default_hints;
        if (bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_ADDRCONFIG.get()) {
            hints_copy.flags.ADDRCONFIG = false;
        }
        if (bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_IPV6.get()) {
            hints_copy.family = std.c.AF.INET;
        } else if (bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_IPV4.get()) {
            hints_copy.family = std.c.AF.INET6;
        }
        return hints_copy;
    }

    extern fn us_internal_dns_callback(socket: *bun.uws.ConnectingSocket, req: *Request) void;
    extern fn us_internal_dns_callback_threadsafe(socket: *bun.uws.ConnectingSocket, req: *Request) void;

    pub const DNSRequestOwner = union(enum) {
        socket: *bun.uws.ConnectingSocket,
        prefetch: *bun.uws.Loop,
        quic: *bun.http.H3.PendingConnect,

        pub fn notifyThreadsafe(_: DNSRequestOwner, req: *Request): void {
            switch (_) {
                .socket => |socket| us_internal_dns_callback_threadsafe(socket, req),
                .prefetch => freeaddrinfo(req, 0),
                .quic => |pc| pc.onDNSResolvedThreadsafe(),
            }
        }

        pub fn notify(_: DNSRequestOwner, req: *Request): void {
            switch (_) {
                .prefetch => freeaddrinfo(req, 0),
                .socket => us_internal_dns_callback(_.socket, req),
                .quic => |pc| pc.onDNSResolved(),
            }
        }

        pub fn loop(_: DNSRequestOwner): *bun.uws.Loop {
            return switch (_) {
                .prefetch => _.prefetch,
                .socket => _.socket.loop(),
                .quic => |pc| pc.loop(),
            };
        }
    };

    pub fn registerQuic(request: *Request, pc: *bun.http.H3.PendingConnect): void {
        global_cache.lock.lock();
        const owner: DNSRequestOwner = .{ .quic = pc };
        if (request.result != null) {
            global_cache.lock.unlock();
            owner.notify(request);
            return;
        }
        bun.handleOom(request.notify.append(bun.default_allocator, owner));
        global_cache.lock.unlock();
    }

    pub const ResultEntry = extern struct {
        info: std.c.addrinfo,
        addr: std.c.sockaddr.storage,
    };

    fn processResults(info: *std.c.addrinfo): []ResultEntry {
        let count: usize = 0;
        let info_: ?*std.c.addrinfo = info;
        while (info_) |ai| {
            count += 1;
            info_ = ai.next;
        }

        let results = bun.handleOom(bun.default_allocator.alloc(ResultEntry, count));

        let i: usize = 0;
        info_ = info;
        while (info_) |ai| {
            results[i].info = ai.*;
            if (ai.addr) |addr| {
                if (ai.family == std.c.AF.INET) {
                    const addr_in: *std.c.sockaddr.in = @ptrCast(&results[i].addr);
                    addr_in.* = @as(*std.c.sockaddr.in, @ptrCast(@alignCast(addr))).*;
                } else if (ai.family == std.c.AF.INET6) {
                    const addr_in: *std.c.sockaddr.in6 = @ptrCast(&results[i].addr);
                    addr_in.* = @as(*std.c.sockaddr.in6, @ptrCast(@alignCast(addr))).*;
                }
            } else {
                results[i].addr = std.mem.zeroes(std.c.sockaddr.storage);
            }
            i += 1;
            info_ = ai.next;
        }

        let want: usize = std.c.AF.INET6;
        for (0..count) |idx| {
            if (results[idx].info.family == want) continue;
            for (idx + 1..count) |j| {
                if (results[j].info.family == want) {
                    std.mem.swap(ResultEntry, &results[idx], &results[j]);
                    want = if (want == std.c.AF.INET6) std.c.AF.INET else std.c.AF.INET6;
                }
            } else {
                break;
            }
        }

        for (results, 0..) |*entry, idx| {
            entry.info.canonname = null;
            if (idx + 1 < count) {
                entry.info.next = &results[idx + 1].info;
            } else {
                entry.info.next = null;
            }
            if (entry.info.addr != null) {
                entry.info.addr = @ptrCast(@alignCast(&entry.addr));
            }
        }

        return results;
    }

    fn afterResult(req: *Request, info: ?*std.c.addrinfo, err: c_int): void {
        const results: ?[*]ResultEntry = if (info) |ai| ({
            const res = processResults(ai);
            std.c.freeaddrinfo(ai);
            break res.ptr;
        }) else null;

        global_cache.lock.lock();

        req.result = .{
            .info = results,
            .err = err,
        };
        let notify = req.notify;
        defer notify.deinit(bun.default_allocator);
        req.notify = .{};
        req.refcount -= 1;

        global_cache.lock.unlock();

        for (notify.items) |query| {
            query.notifyThreadsafe(req);
        }
    }

    fn workPoolCallback(req: *Request): void {
        let service_buf: [bun.fmt.fastDigitCount(std.math.maxInt(u16)) + 2]u8 = undefined;
        const service: [*:0]u8?! = if (req.key.port > 0)
            (std.fmt.bufPrintZ(&service_buf, "{d}", .{req.key.port}) catch unreachable).ptr
        else
            null;

        if (Environment.isWindows) {
            const wsa = std.os.windows.ws2_32;
            const wsa_hints = wsa.addrinfo{
                .flags = .{},
                .family = wsa.AF.UNSPEC,
                .socktype = wsa.SOCK.STREAM,
                .protocol = 0,
                .addrlen = 0,
                .canonname = null,
                .addr = null,
                .next = null,
            };

            let addrinfo: ?*wsa.addrinfo = null;
            const err = wsa.getaddrinfo(
                if (req.key.host) |host| host.ptr else null,
                service,
                &wsa_hints,
                &addrinfo,
            );
            afterResult(req, @ptrCast(addrinfo), err);
        } else {
            let addrinfo: ?*std.c.addrinfo = null;
            let hints = getHints();

            let err = std.c.getaddrinfo(
                if (req.key.host) |host| host.ptr else null,
                service,
                &hints,
                &addrinfo,
            );

            if (err == .NONAME and hints.flags.ADDRCONFIG) {
                hints.flags.ADDRCONFIG = false;
                req.can_retry_for_addrconfig = false;
                err = std.c.getaddrinfo(
                    if (req.key.host) |host| host.ptr else null,
                    service,
                    &hints,
                    &addrinfo,
                );
            }
            afterResult(req, addrinfo, @intFromEnum(err));
        }
    }

    pub fn lookupLibinfo(req: *Request, loop: jsc.EventLoopHandle): bool {
        const getaddrinfo_async_start_ = LibInfo.getaddrinfo_async_start.get() orelse return false;

        let machport: bun.mach_port = 0;
        let service_buf: [bun.fmt.fastDigitCount(std.math.maxInt(u16)) + 2]u8 = undefined;
        const service: [*:0]u8?! = if (req.key.port > 0)
            (std.fmt.bufPrintZ(&service_buf, "{d}", .{req.key.port}) catch unreachable).ptr
        else
            null;

        let hints = getHints();

        const errno = getaddrinfo_async_start_(
            &machport,
            if (req.key.host) |host| host.ptr else null,
            service,
            &hints,
            libinfoCallback,
            req,
        );

        if (errno != 0 or machport == 0) {
            return false;
        }

        let poll = bun.Async.FilePoll.init(loop, .fromNative(@bitCast(machport)), .{}, InternalDNSRequest, req);
        const rc = poll.register(loop.loop(), .machport, true);

        if (rc == .err) {
            poll.deinit();
            return false;
        }

        req.libinfo = .{
            .file_poll = poll,
            .machport = machport,
        };

        return true;
    }

    fn libinfoCallback(
        status: i32,
        addr_info: ?*std.c.addrinfo,
        arg: ?*anyopaque,
    ) callconv(.c) void {
        const req: *Request = bun.cast(*Request, arg);
        const status_int: c_int = @intCast(status);
        if (status == @intFromEnum(std.c.EAI.NONAME) and req.can_retry_for_addrconfig) {
            req.can_retry_for_addrconfig = false;
            let service_buf: [bun.fmt.fastDigitCount(std.math.maxInt(u16)) + 2]u8 = undefined;
            const service: [*:0]u8?! = if (req.key.port > 0)
                (std.fmt.bufPrintZ(&service_buf, "{d}", .{req.key.port}) catch unreachable).ptr
            else
                null;
            if (LibInfo.getaddrinfo_async_start.get()) |getaddrinfo_async_start_| {
                let machport: bun.mach_port = 0;
                let hints = getHints();
                hints.flags.ADDRCONFIG = false;

                const errno = getaddrinfo_async_start_(
                    &machport,
                    if (req.key.host) |host| host.ptr else null,
                    service,
                    &hints,
                    libinfoCallback,
                    req,
                );

                if (errno == 0 and machport != 0) {
                    req.libinfo.machport = machport;
                    const poll = req.libinfo.file_poll.?;
                    poll.fd = .fromNative(@bitCast(machport));
                    if (poll.register(bun.uws.Loop.get(), .machport, true) == .result) {
                        return;
                    }
                }
            }
        }
        afterResult(req, addr_info, @intCast(status_int));
    }

    let dns_cache_hits_completed: usize = 0;
    let dns_cache_hits_inflight: usize = 0;
    let dns_cache_size: usize = 0;
    let dns_cache_misses: usize = 0;
    let dns_cache_errors: usize = 0;
    let getaddrinfo_calls: usize = 0;

    pub fn getDNSCacheStats(globalObject: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const object = jsc.JSValue.createEmptyObject(globalObject, 6);
        object.put(globalObject, jsc.ZigString.static("cacheHitsCompleted"), jsc.JSValue.jsNumber(@atomicLoad(usize, &dns_cache_hits_completed, .monotonic)));
        object.put(globalObject, jsc.ZigString.static("cacheHitsInflight"), jsc.JSValue.jsNumber(@atomicLoad(usize, &dns_cache_hits_inflight, .monotonic)));
        object.put(globalObject, jsc.ZigString.static("cacheMisses"), jsc.JSValue.jsNumber(@atomicLoad(usize, &dns_cache_misses, .monotonic)));
        object.put(globalObject, jsc.ZigString.static("size"), jsc.JSValue.jsNumber(@atomicLoad(usize, &dns_cache_size, .monotonic)));
        object.put(globalObject, jsc.ZigString.static("errors"), jsc.JSValue.jsNumber(@atomicLoad(usize, &dns_cache_errors, .monotonic)));
        object.put(globalObject, jsc.ZigString.static("totalCount"), jsc.JSValue.jsNumber(@atomicLoad(usize, &getaddrinfo_calls, .monotonic)));
        return object;
    }

    pub fn getaddrinfo(loop: *bun.uws.Loop, host: ?[:0]const u8, port: u16, is_cache_hit: ?*bool): ?*Request {
        const preload = is_cache_hit == null;
        const key = Request.Key.init(host, port);
        global_cache.lock.lock();
        getaddrinfo_calls += 1;
        let timestamp_to_store: u32 = 0;
        if (!bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_DNS_CACHE.get()) {
            if (global_cache.get(key, &timestamp_to_store)) |entry| {
                if (preload) {
                    global_cache.lock.unlock();
                    return null;
                }

                entry.refcount += 1;

                if (entry.result != null) {
                    is_cache_hit.?.* = true;
                    log("getaddrinfo({s}) = cache hit", .{host orelse ""});
                    dns_cache_hits_completed += 1;
                } else {
                    log("getaddrinfo({s}) = cache hit (inflight)", .{host orelse ""});
                    dns_cache_hits_inflight += 1;
                }

                global_cache.lock.unlock();
                return entry;
            }
        }

        const req = Request.new(.{
            .key = key.toOwned(),
            .refcount = @as(u32, @intFromBool(!preload)) + 1,
            .created_at = if (timestamp_to_store == 0) GlobalCache.getCacheTimestamp() else timestamp_to_store,
        });

        _ = global_cache.tryPush(req);
        dns_cache_misses += 1;
        dns_cache_size = global_cache.len;
        global_cache.lock.unlock();

        if (comptime Environment.isMac) {
            if (!bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_DNS_CACHE_LIBINFO.get()) {
                const res = lookupLibinfo(req, loop.internal_loop_data.getParent());
                log("getaddrinfo({s}) = cache miss (libinfo)", .{host orelse ""});
                if (res) return req;
            }
        }

        log("getaddrinfo({s}) = cache miss (libc)", .{host orelse ""});
        bun.handleOom(bun.jsc.WorkPool.go(bun.default_allocator, *Request, req, workPoolCallback));
        return req;
    }

    pub fn prefetchFromJS(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments();
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("prefetch", 1, arguments.len);
        }

        const hostname_or_url = arguments[0];
        let hostname_slice = jsc.ZigString.Slice.empty;
        defer hostname_slice.deinit();

        if (hostname_or_url.isString()) {
            hostname_slice = try hostname_or_url.toSlice(globalThis, bun.default_allocator);
        } else {
            return globalThis.throwInvalidArguments("hostname must be a string", .{});
        }

        const hostname_z = try bun.default_allocator.dupeZ(u8, hostname_slice.slice());
        defer bun.default_allocator.free(hostname_z);

        const port: u16 = if (arguments.len > 1 and !arguments[1].isUndefinedOrNull())
            try globalThis.validateIntegerRange(arguments[1], u16, 443, .{ .field_name = "port", .always_allow_zero = true })
        else
            443;

        prefetch(jsc.VirtualMachine.get().uwsLoop(), hostname_z, port);
        return .js_undefined;
    }

    pub fn prefetch(loop: *bun.uws.Loop, hostname: ?[:0]const u8, port: u16): void {
        _ = getaddrinfo(loop, hostname, port, null);
    }

    fn us_getaddrinfo(loop: *bun.uws.Loop, _host: [*:0]u8?!, port: u16, socket: *?*anyopaque) callconv(.c) c_int {
        const host: ?[:0]const u8 = std.mem.span(_host);
        let is_cache_hit: bool = false;
        const req = getaddrinfo(loop, host, port, &is_cache_hit).?;
        socket.* = req;
        return if (is_cache_hit) 0 else 1;
    }

    fn us_getaddrinfo_set(
        request: *Request,
        socket: *bun.uws.ConnectingSocket,
    ) callconv(.c) void {
        global_cache.lock.lock();
        defer global_cache.lock.unlock();
        const query = DNSRequestOwner{
            .socket = socket,
        };
        if (request.result != null) {
            query.notify(request);
            return;
        }

        bun.handleOom(request.notify.append(bun.default_allocator, .{ .socket = socket }));
    }

    fn us_getaddrinfo_cancel(
        request: *Request,
        socket: *bun.uws.ConnectingSocket,
    ) callconv(.c) c_int {
        global_cache.lock.lock();
        defer global_cache.lock.unlock();
        if (request.result != null) return 0;
        for (request.notify.items, 0..) |item, i| {
            switch (item) {
                .socket => |s| if (s == socket) {
                    _ = request.notify.swapRemove(i);
                    return 1;
                },
                .prefetch, .quic => {},
            }
        }
        return 0;
    }

    fn freeaddrinfo(req: *Request, err: c_int) callconv(.c) void {
        global_cache.lock.lock();
        defer global_cache.lock.unlock();

        if (err != 0) {
            req.valid = false;
        }
        dns_cache_errors += @as(usize, @intFromBool(err != 0));

        bun.assert(req.refcount > 0);
        req.refcount -= 1;
        if (req.refcount == 0 and (global_cache.isNearlyFull() or !req.valid)) {
            log("cache --", .{});
            global_cache.remove(req);
            req.deinit();
        }
    }

    fn getRequestResult(req: *Request) callconv(.c) *Request.Result {
        return &req.result.?;
    }
}

pub type InternalDNSRequest = internal.Request;

comptime {
    _ = @import("./cares_jsc.zig");
    @export(&internal.us_getaddrinfo_set, .{ .name = "Bun__addrinfo_set" });
    @export(&internal.us_getaddrinfo_cancel, .{ .name = "Bun__addrinfo_cancel" });
    @export(&internal.us_getaddrinfo, .{ .name = "Bun__addrinfo_get" });
    @export(&internal.freeaddrinfo, .{ .name = "Bun__addrinfo_freeRequest" });
    @export(&internal.getRequestResult, .{ .name = "Bun__addrinfo_getRequestResult" });
}

pub struct Resolver {
    const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    const log = Output.scoped(.DNSResolver, .visible);

    ref_count: RefCount,
    channel: ?*c_ares.Channel = null,
    vm: *jsc.VirtualMachine,
    polls: PollsMap,
    options: c_ares.ChannelOptions = .{},

    event_loop_timer: EventLoopTimer = .{
        .next = .epoch,
        .tag = .DNSResolver,
    },

    pending_host_cache_cares: PendingCache,
    pending_host_cache_native: PendingCache,
    pending_srv_cache_cares: SrvPendingCache,
    pending_soa_cache_cares: SoaPendingCache,
    pending_txt_cache_cares: TxtPendingCache,
    pending_naptr_cache_cares: NaptrPendingCache,
    pending_mx_cache_cares: MxPendingCache,
    pending_caa_cache_cares: CaaPendingCache,
    pending_ns_cache_cares: NSPendingCache,
    pending_ptr_cache_cares: PtrPendingCache,
    pending_cname_cache_cares: CnamePendingCache,
    pending_a_cache_cares: APendingCache,
    pending_aaaa_cache_cares: AAAAPendingCache,
    pending_any_cache_cares: AnyPendingCache,
    pending_addr_cache_cares: AddrPendingCache,
    pending_nameinfo_cache_cares: NameInfoPendingCache,

    pub const js = jsc.Codegen.JSDNSResolver;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    const PollsMap = std.AutoArrayHashMap(c_ares.ares_socket_t, *PollType);
    const PollType = if (Environment.isWindows) UvDnsPoll else Async.FilePoll;

    const UvDnsPoll = struct {
        pub const new = bun.TrivialNew(@This());
        pub const destroy = bun.TrivialDeinit(@This());

        parent: *Resolver,
        socket: c_ares.ares_socket_t,
        poll: bun.windows.libuv.uv_poll_t,

        pub fn fromPoll(poll: *bun.windows.libuv.uv_poll_t): *UvDnsPoll {
            return @fieldParentPtr("poll", poll);
        }
    };

    pub fn setup(allocator: std.mem.Allocator, vm: *jsc.VirtualMachine): Resolver {
        return .{
            .ref_count = .init(),
            .vm = vm,
            .polls = Resolver.PollsMap.init(allocator),
            .pending_host_cache_cares = PendingCache.empty,
            .pending_host_cache_native = PendingCache.empty,
            .pending_srv_cache_cares = SrvPendingCache.empty,
            .pending_soa_cache_cares = SoaPendingCache.empty,
            .pending_txt_cache_cares = TxtPendingCache.empty,
            .pending_naptr_cache_cares = NaptrPendingCache.empty,
            .pending_mx_cache_cares = MxPendingCache.empty,
            .pending_caa_cache_cares = CaaPendingCache.empty,
            .pending_ns_cache_cares = NSPendingCache.empty,
            .pending_ptr_cache_cares = PtrPendingCache.empty,
            .pending_cname_cache_cares = CnamePendingCache.empty,
            .pending_a_cache_cares = APendingCache.empty,
            .pending_aaaa_cache_cares = AAAAPendingCache.empty,
            .pending_any_cache_cares = AnyPendingCache.empty,
            .pending_addr_cache_cares = AddrPendingCache.empty,
            .pending_nameinfo_cache_cares = NameInfoPendingCache.empty,
        };
    }

    pub fn init(allocator: std.mem.Allocator, vm: *jsc.VirtualMachine): *Resolver {
        log("init", .{});
        return bun.new(Resolver, .setup(allocator, vm));
    }

    pub fn finalize(_: *Resolver): void {
        _.deref();
    }

    pub fn release(_: *Resolver): void {
        if (_.channel) |channel| {
            channel.deinit();
        }
        bun.destroy(_);
    }

    pub fn deinit(_: *Resolver): void {
        _.release();
    }

    pub const Order = enum(u8) {
        verbatim = 0,
        ipv4first = 4,
        ipv6first = 6,

        pub const default = .verbatim;
        pub const map = bun.ComptimeStringMap(Order, .{
            .{ "verbatim", .verbatim },
            .{ "ipv4first", .ipv4first },
            .{ "ipv6first", .ipv6first },
            .{ "0", .verbatim },
            .{ "4", .ipv4first },
            .{ "6", .ipv6first },
        });

        pub fn toJS(_: Order, globalThis: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
            return jsc.ZigString.init(@tagName(_)).toJS(globalThis);
        }

        pub fn fromString(order: string): ?Order {
            return Order.map.get(order);
        }

        pub fn fromStringOrDie(order: string): Order {
            return fromString(order) orelse ({
                Output.prettyErrorln("<r><red>error<r><d>:<r> Invalid DNS result order.", .{});
                Global.exit(1);
            });
        }
    };

    const PendingCache = bun.HiveArray(GetAddrInfoRequest.PendingCacheKey, 32);
    const SrvPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_ares_srv_reply, "srv").PendingCacheKey, 32);
    const SoaPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_ares_soa_reply, "soa").PendingCacheKey, 32);
    const TxtPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_ares_txt_reply, "txt").PendingCacheKey, 32);
    const NaptrPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_ares_naptr_reply, "naptr").PendingCacheKey, 32);
    const MxPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_ares_mx_reply, "mx").PendingCacheKey, 32);
    const CaaPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_ares_caa_reply, "caa").PendingCacheKey, 32);
    const NSPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_hostent, "ns").PendingCacheKey, 32);
    const PtrPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_hostent, "ptr").PendingCacheKey, 32);
    const CnamePendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_hostent, "cname").PendingCacheKey, 32);
    const APendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.hostent_with_ttls, "a").PendingCacheKey, 32);
    const AAAAPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.hostent_with_ttls, "aaaa").PendingCacheKey, 32);
    const AnyPendingCache = bun.HiveArray(ResolveInfoRequest(c_ares.struct_any_reply, "any").PendingCacheKey, 32);
    const AddrPendingCache = bun.HiveArray(GetHostByAddrInfoRequest.PendingCacheKey, 32);
    const NameInfoPendingCache = bun.HiveArray(GetNameInfoRequest.PendingCacheKey, 32);

    pub fn checkTimeouts(_: *Resolver, now: *const timespec, vm: *jsc.VirtualMachine): void {
        defer {
            vm.timer.incrementTimerRef(-1);
            _.deref();
        }

        _.event_loop_timer.state = .PENDING;

        if (_.getChannelOrError(vm.global)) |channel| {
            if (_.anyRequestsPending()) {
                c_ares.ares_process_fd(channel, c_ares.ARES_SOCKET_BAD, c_ares.ARES_SOCKET_BAD);
                _ = _.addTimer(now);
            }
        } else |_| {}
    }

    fn anyRequestsPending(_: *Resolver): bool {
        inline for (@typeInfo(Resolver).@"struct".fields) |field| {
            if (comptime std.mem.startsWith(u8, field.name, "pending_")) {
                const set = &@field(_, field.name).used;
                if (set.findFirstSet() != null) {
                    return true;
                }
            }
        }
        return false;
    }

    fn requestSent(_: *Resolver, _: *jsc.VirtualMachine): void {
        _ = _.addTimer(null);
    }

    fn requestCompleted(_: *Resolver): void {
        if (_.anyRequestsPending()) {
            _ = _.addTimer(null);
        } else {
            _.removeTimer();
        }
    }

    fn addTimer(_: *Resolver, now: ?*const timespec): bool {
        if (_.event_loop_timer.state == .ACTIVE) {
            return false;
        }

        _.ref();
        _.event_loop_timer.next = (now orelse &timespec.now(.allow_mocked_time)).addMs(1000);
        _.vm.timer.incrementTimerRef(1);
        _.vm.timer.insert(&_.event_loop_timer);
        return true;
    }

    fn removeTimer(_: *Resolver): void {
        if (_.event_loop_timer.state != .ACTIVE) {
            return;
        }

        defer {
            _.vm.timer.incrementTimerRef(-1);
            _.deref();
        }

        _.vm.timer.remove(&_.event_loop_timer);
    }

    fn getKey(_: *Resolver, index: u8, comptime cache_name: []const u8, comptime request_type: type): request_type.PendingCacheKey {
        let cache = &@field(_, cache_name);
        bun.assert(cache.used.isSet(index));
        const entry = cache.buffer[index];
        cache.buffer[index] = undefined;

        let used = cache.used;
        used.unset(index);
        cache.used = used;

        return entry;
    }

    pub fn drainPendingCares(_: *Resolver, index: u8, err: ?c_ares.Error, timeout: i32, comptime request_type: type, comptime cares_type: type, comptime lookup_name: []const u8, result: ?*cares_type): void {
        const cache_name = comptime std.fmt.comptimePrint("pending_{s}_cache_cares", .{lookup_name});

        _.ref();
        defer _.deref();

        const key = _.getKey(index, cache_name, request_type);

        let addr: *cares_type = result orelse {
            let pending: ?*CAresLookup(cares_type, lookup_name) = key.lookup.head.next;
            key.lookup.head.processResolve(err, timeout, null);
            bun.default_allocator.destroy(key.lookup);

            while (pending) |value| {
                pending = value.next;
                value.processResolve(err, timeout, null);
            }
            return;
        };

        let pending: ?*CAresLookup(cares_type, lookup_name) = key.lookup.head.next;
        let prev_global = key.lookup.head.globalThis;
        let array = addr.toJSResponse(_.vm.allocator, prev_global, lookup_name) catch .zero;
        defer addr.deinit();
        array.ensureStillAlive();
        key.lookup.head.onComplete(array);
        bun.default_allocator.destroy(key.lookup);

        array.ensureStillAlive();

        while (pending) |value| {
            const new_global = value.globalThis;
            if (prev_global != new_global) {
                array = addr.toJSResponse(_.vm.allocator, new_global, lookup_name) catch .zero;
                prev_global = new_global;
            }
            pending = value.next;

            array.ensureStillAlive();
            value.onComplete(array);
            array.ensureStillAlive();
        }
    }

    pub fn drainPendingHostCares(_: *Resolver, index: u8, err: ?c_ares.Error, timeout: i32, result: ?*c_ares.AddrInfo): void {
        const key = _.getKey(index, "pending_host_cache_cares", GetAddrInfoRequest);

        _.ref();
        defer _.deref();

        let addr = result orelse {
            let pending: ?*DNSLookup = key.lookup.head.next;
            key.lookup.head.processGetAddrInfo(err, timeout, null);
            bun.default_allocator.destroy(key.lookup);

            while (pending) |value| {
                pending = value.next;
                value.processGetAddrInfo(err, timeout, null);
            }
            return;
        };

        let pending: ?*DNSLookup = key.lookup.head.next;
        let prev_global = key.lookup.head.globalThis;
        let array = addr.toJSArray(prev_global) catch .zero;
        defer addr.deinit();
        array.ensureStillAlive();
        key.lookup.head.onCompleteWithArray(array);
        bun.default_allocator.destroy(key.lookup);

        array.ensureStillAlive();

        while (pending) |value| {
            const new_global = value.globalThis;
            if (prev_global != new_global) {
                array = addr.toJSArray(new_global) catch .zero;
                prev_global = new_global;
            }
            pending = value.next;

            array.ensureStillAlive();
            value.onCompleteWithArray(array);
            array.ensureStillAlive();
        }
    }

    pub fn drainPendingHostNative(_: *Resolver, index: u8, globalObject: *jsc.JSGlobalObject, err: i32, result: GetAddrInfo.Result.Any): void {
        log("drainPendingHostNative", .{});
        const key = _.getKey(index, "pending_host_cache_native", GetAddrInfoRequest);

        _.ref();
        defer _.deref();

        let array = (result.toJS(globalObject) catch .zero) orelse {
            let pending: ?*DNSLookup = key.lookup.head.next;
            let head = key.lookup.head;
            head.processGetAddrInfoNative(err, null);
            bun.default_allocator.destroy(key.lookup);

            while (pending) |value| {
                pending = value.next;
                value.processGetAddrInfoNative(err, null);
            }
            return;
        };
        let pending: ?*DNSLookup = key.lookup.head.next;
        let prev_global = key.lookup.head.globalThis;

        array.ensureStillAlive();
        key.lookup.head.onCompleteWithArray(array);
        bun.default_allocator.destroy(key.lookup);
        array.ensureStillAlive();

        while (pending) |value| {
            const new_global = value.globalThis;
            pending = value.next;
            if (prev_global != new_global) {
                array = (result.toJS(new_global) catch .zero).?;
                prev_global = new_global;
            }

            array.ensureStillAlive();
            value.onCompleteWithArray(array);
            array.ensureStillAlive();
        }
    }

    pub fn drainPendingAddrCares(_: *Resolver, index: u8, err: ?c_ares.Error, timeout: i32, result: ?*c_ares.struct_hostent): void {
        const key = _.getKey(index, "pending_addr_cache_cares", GetHostByAddrInfoRequest);

        _.ref();
        defer _.deref();

        let addr = result orelse {
            let pending: ?*CAresReverse = key.lookup.head.next;
            key.lookup.head.processResolve(err, timeout, null);
            bun.default_allocator.destroy(key.lookup);

            while (pending) |value| {
                pending = value.next;
                value.processResolve(err, timeout, null);
            }
            return;
        };

        let pending: ?*CAresReverse = key.lookup.head.next;
        let prev_global = key.lookup.head.globalThis;
        let array = addr.toJSResponse(_.vm.allocator, prev_global, "") catch .zero;
        array.ensureStillAlive();
        key.lookup.head.onComplete(array);
        bun.default_allocator.destroy(key.lookup);

        array.ensureStillAlive();

        while (pending) |value| {
            const new_global = value.globalThis;
            if (prev_global != new_global) {
                array = addr.toJSResponse(_.vm.allocator, new_global, "") catch .zero;
                prev_global = new_global;
            }
            pending = value.next;

            array.ensureStillAlive();
            value.onComplete(array);
            array.ensureStillAlive();
        }
    }

    pub fn drainPendingNameInfoCares(_: *Resolver, index: u8, err: ?c_ares.Error, timeout: i32, result: ?c_ares.struct_nameinfo): void {
        const key = _.getKey(index, "pending_nameinfo_cache_cares", GetNameInfoRequest);

        _.ref();
        defer _.deref();

        let name_info = result orelse {
            let pending: ?*CAresNameInfo = key.lookup.head.next;
            key.lookup.head.processResolve(err, timeout, null);
            bun.default_allocator.destroy(key.lookup);

            while (pending) |value| {
                pending = value.next;
                value.processResolve(err, timeout, null);
            }
            return;
        };

        let pending: ?*CAresNameInfo = key.lookup.head.next;
        let prev_global = key.lookup.head.globalThis;

        let array = name_info.toJSResponse(_.vm.allocator, prev_global) catch .zero;
        array.ensureStillAlive();
        key.lookup.head.onComplete(array);
        bun.default_allocator.destroy(key.lookup);

        array.ensureStillAlive();

        while (pending) |value| {
            const new_global = value.globalThis;
            if (prev_global != new_global) {
                array = name_info.toJSResponse(_.vm.allocator, new_global) catch .zero;
                prev_global = new_global;
            }
            pending = value.next;

            array.ensureStillAlive();
            value.onComplete(array);
            array.ensureStillAlive();
        }
    }

    pub const CacheHit = union(enum) {
        inflight: *GetAddrInfoRequest.PendingCacheKey,
        new: *GetAddrInfoRequest.PendingCacheKey,
        disabled: void,
    };

    pub fn LookupCacheHit(comptime request_type: type): type {
        return union(enum) {
            inflight: *request_type.PendingCacheKey,
            new: *request_type.PendingCacheKey,
            disabled: void,
        };
    }

    pub fn getOrPutIntoResolvePendingCache(
        _: *Resolver,
        comptime request_type: type,
        key: request_type.PendingCacheKey,
        comptime field: []const u8,
    ): LookupCacheHit(request_type) {
        let cache = &@field(_, field);
        let inflight_iter = cache.used.iterator(.{ .kind = .set });

        while (inflight_iter.next()) |index| {
            const entry: *request_type.PendingCacheKey = &cache.buffer[index];
            if (entry.hash == key.hash and entry.len == key.len) {
                return .{ .inflight = entry };
            }
        }

        if (cache.get()) |new| {
            new.hash = key.hash;
            new.len = key.len;
            return .{ .new = new };
        }

        return .{ .disabled = {} };
    }

    pub fn getOrPutIntoPendingCache(
        _: *Resolver,
        key: GetAddrInfoRequest.PendingCacheKey,
        comptime field: std.meta.FieldEnum(Resolver),
    ): CacheHit {
        let cache: *PendingCache = &@field(_, @tagName(field));
        let inflight_iter = cache.used.iterator(.{ .kind = .set });

        while (inflight_iter.next()) |index| {
            const entry: *GetAddrInfoRequest.PendingCacheKey = &cache.buffer[index];
            if (entry.hash == key.hash and entry.len == key.len) {
                return .{ .inflight = entry };
            }
        }

        if (cache.get()) |new| {
            new.hash = key.hash;
            new.len = key.len;
            return .{ .new = new };
        }

        return .{ .disabled = {} };
    }

    pub const ChannelResult = union(enum) {
        err: c_ares.Error,
        result: *c_ares.Channel,
    };

    pub fn getChannel(_: *Resolver): ChannelResult {
        if (_.channel == null) {
            if (c_ares.Channel.init(Resolver, _, _.options)) |err| {
                return .{ .err = err };
            }
        }
        return .{ .result = _.channel.? };
    }

    fn getChannelFromVM(globalThis: *jsc.JSGlobalObject): bun.JSError!*c_ares.Channel {
        let vm = globalThis.bunVM();
        let resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.getChannelOrError(globalThis);
    }

    pub fn getChannelOrError(_: *Resolver, globalThis: *jsc.JSGlobalObject): bun.JSError!*c_ares.Channel {
        switch (_.getChannel()) {
            .result => |result| return result,
            .err => |err| {
                const system_error = jsc.SystemError{
                    .errno = -1,
                    .code = bun.String.static(err.code()),
                    .message = bun.String.static(err.label()),
                };
                return globalThis.throwValue(system_error.toErrorInstance(globalThis));
            },
        }
    }

    pub fn onDNSPollUv(watcher: [*c]bun.windows.libuv.uv_poll_t, status: c_int, events: c_int) callconv(.c) void {
        const poll = UvDnsPoll.fromPoll(watcher);
        const vm = poll.parent.vm;
        vm.eventLoop().enter();
        defer vm.eventLoop().exit();
        poll.parent.ref();
        defer poll.parent.deref();
        const channel = poll.parent.channel.?;
        if (status < 0) {
            channel.process(poll.socket, true, true);
            return;
        }
        channel.process(
            poll.socket,
            events & bun.windows.libuv.UV_READABLE != 0,
            events & bun.windows.libuv.UV_WRITABLE != 0,
        );
    }

    pub fn onCloseUv(watcher: *anyopaque) callconv(.c) void {
        const poll = UvDnsPoll.fromPoll(@ptrCast(@alignCast(watcher)));
        poll.destroy();
    }

    pub fn onDNSPoll(
        _: *Resolver,
        poll: *Async.FilePoll,
    ): void {
        let vm = _.vm;
        vm.eventLoop().enter();
        defer vm.eventLoop().exit();
        let channel = _.channel orelse {
            _ = _.polls.orderedRemove(poll.fd.native());
            poll.deinit();
            return;
        };

        _.ref();
        defer _.deref();

        channel.process(
            poll.fd.native(),
            poll.isReadable(),
            poll.isWritable(),
        );
    }

    pub fn onDNSSocketState(
        _: *Resolver,
        fd: c_ares.ares_socket_t,
        readable: bool,
        writable: bool,
    ): void {
        if (comptime Environment.isWindows) {
            const uv = bun.windows.libuv;
            if (!readable and !writable) {
                if (_.polls.fetchOrderedRemove(fd)) |entry| {
                    uv.uv_close(@ptrCast(&entry.value.poll), onCloseUv);
                }
                return;
            }

            const poll_entry = bun.handleOom(_.polls.getOrPut(fd));
            if (!poll_entry.found_existing) {
                const poll = UvDnsPoll.new(.{
                    .parent = _,
                    .socket = fd,
                    .poll = undefined,
                });
                if (uv.uv_poll_init_socket(bun.uws.Loop.get().uv_loop, &poll.poll, @ptrCast(fd)) < 0) {
                    poll.destroy();
                    _ = _.polls.swapRemove(fd);
                    return;
                }
                poll_entry.value_ptr.* = poll;
            }

            const poll: *UvDnsPoll = poll_entry.value_ptr.*;
            const uv_events = (if (readable) uv.UV_READABLE else 0) | (if (writable) uv.UV_WRITABLE else 0);
            if (uv.uv_poll_start(&poll.poll, uv_events, onDNSPollUv) < 0) {
                _ = _.polls.swapRemove(fd);
                uv.uv_close(@ptrCast(&poll.poll), onCloseUv);
            }
        } else {
            let vm = _.vm;

            if (!readable and !writable) {
                if (_.polls.fetchOrderedRemove(fd)) |entry| {
                    entry.value.deinitWithVM(vm);
                }
                return;
            }

            const poll_entry = _.polls.getOrPut(fd) catch unreachable;
            if (!poll_entry.found_existing) {
                poll_entry.value_ptr.* = Async.FilePoll.init(vm, .fromNative(fd), .{}, Resolver, _);
            }

            let poll = poll_entry.value_ptr.*;
            const loop = vm.event_loop_handle.?;
            const have_readable = poll.flags.contains(.poll_readable);
            const have_writable = poll.flags.contains(.poll_writable);

            if ((have_readable and !readable) or (have_writable and !writable)) {
                _ = poll.unregister(loop, false);
                if (readable) _ = poll.register(loop, .readable, false);
                if (writable) _ = poll.register(loop, .writable, false);
            } else {
                if (readable and !have_readable) _ = poll.register(loop, .readable, false);
                if (writable and !have_writable) _ = poll.register(loop, .writable, false);
            }
        }
    }

    pub struct DNSQuery {
        name: jsc.ZigString.Slice,
        record_type: RecordType,
        ttl: i32 = 0,
    }

    pub const RecordType = enum(c_int) {
        A = 1,
        AAAA = 28,
        CAA = 257,
        CNAME = 5,
        MX = 15,
        NS = 2,
        PTR = 12,
        SOA = 6,
        SRV = 33,
        TXT = 16,
        ANY = 255,

        pub const default = RecordType.A;
        pub const map = bun.ComptimeStringMap(RecordType, .{
            .{ "A", .A },
            .{ "AAAA", .AAAA },
            .{ "ANY", .ANY },
            .{ "CAA", .CAA },
            .{ "CNAME", .CNAME },
            .{ "MX", .MX },
            .{ "NS", .NS },
            .{ "PTR", .PTR },
            .{ "SOA", .SOA },
            .{ "SRV", .SRV },
            .{ "TXT", .TXT },
            .{ "a", .A },
            .{ "aaaa", .AAAA },
            .{ "any", .ANY },
            .{ "caa", .CAA },
            .{ "cname", .CNAME },
            .{ "mx", .MX },
            .{ "ns", .NS },
            .{ "ptr", .PTR },
            .{ "soa", .SOA },
            .{ "srv", .SRV },
            .{ "txt", .TXT },
        });
    };

    pub fn globalResolve(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolve(globalThis, callframe);
    }

    pub fn resolve(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(3);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolve", 3, arguments.len);
        }

        const record_type: RecordType = if (arguments.len <= 1)
            RecordType.default
        else ({
            const record_type_value = arguments.ptr[1];
            if (record_type_value.isEmptyOrUndefinedOrNull() or !record_type_value.isString()) {
                break RecordType.default;
            }

            const record_type_str = try record_type_value.toJSString(globalThis);
            if (record_type_str.length() == 0) {
                break RecordType.default;
            }

            break RecordType.map.getWithEql(record_type_str.getZigString(globalThis), jsc.ZigString.eqlComptime) orelse {
                return globalThis.throwInvalidArgumentPropertyValue("record", "one of: A, AAAA, ANY, CAA, CNAME, MX, NS, PTR, SOA, SRV, TXT", record_type_value);
            };
        });

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolve", "name", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolve", "name", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);

        switch (record_type) {
            RecordType.A => return _.doResolveCAres(c_ares.hostent_with_ttls, "a", name.slice(), globalThis),
            RecordType.AAAA => return _.doResolveCAres(c_ares.hostent_with_ttls, "aaaa", name.slice(), globalThis),
            RecordType.ANY => return _.doResolveCAres(c_ares.struct_any_reply, "any", name.slice(), globalThis),
            RecordType.CAA => return _.doResolveCAres(c_ares.struct_ares_caa_reply, "caa", name.slice(), globalThis),
            RecordType.CNAME => return _.doResolveCAres(c_ares.struct_hostent, "cname", name.slice(), globalThis),
            RecordType.MX => return _.doResolveCAres(c_ares.struct_ares_mx_reply, "mx", name.slice(), globalThis),
            RecordType.NS => return _.doResolveCAres(c_ares.struct_hostent, "ns", name.slice(), globalThis),
            RecordType.PTR => return _.doResolveCAres(c_ares.struct_hostent, "ptr", name.slice(), globalThis),
            RecordType.SOA => return _.doResolveCAres(c_ares.struct_ares_soa_reply, "soa", name.slice(), globalThis),
            RecordType.SRV => return _.doResolveCAres(c_ares.struct_ares_srv_reply, "srv", name.slice(), globalThis),
            RecordType.TXT => return _.doResolveCAres(c_ares.struct_ares_txt_reply, "txt", name.slice(), globalThis),
        }
    }

    pub fn globalReverse(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.reverse(globalThis, callframe);
    }

    pub fn reverse(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("reverse", 1, arguments.len);
        }

        const ip_value = arguments.ptr[0];
        if (ip_value.isEmptyOrUndefinedOrNull() or !ip_value.isString()) {
            return globalThis.throwInvalidArgumentType("reverse", "ip", "string");
        }

        const ip_str = try ip_value.toJSString(globalThis);
        if (ip_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("reverse", "ip", "non-empty string");
        }

        const ip_slice = try ip_str.toSliceClone(globalThis, bun.default_allocator);
        const ip = ip_slice.slice();
        const channel: *c_ares.Channel = switch (_.getChannel()) {
            .result => |res| res,
            .err => |err| {
                return globalThis.throwValue(try err.toJSWithSyscallAndHostname(globalThis, "getHostByAddr", ip));
            },
        };

        const key = GetHostByAddrInfoRequest.PendingCacheKey.init(ip);
        let cache = _.getOrPutIntoResolvePendingCache(
            GetHostByAddrInfoRequest,
            key,
            "pending_addr_cache_cares",
        );
        if (cache == .inflight) {
            let cares_reverse = bun.handleOom(CAresReverse.init(_, globalThis, globalThis.allocator(), ip));
            cache.inflight.append(cares_reverse);
            return cares_reverse.promise.value();
        }

        let request = GetHostByAddrInfoRequest.init(
            cache,
            _,
            ip,
            globalThis,
            "pending_addr_cache_cares",
        ) catch |err| bun.handleOom(err);

        const promise = request.tail.promise.value();
        channel.getHostByAddr(
            ip,
            GetHostByAddrInfoRequest,
            request,
            GetHostByAddrInfoRequest.onCaresComplete,
        );

        _.requestSent(globalThis.bunVM());
        return promise;
    }

    pub fn globalLookup(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("lookup", 2, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("lookup", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("lookup", "hostname", "non-empty string");
        }

        let options = GetAddrInfo.Options{};
        let port: u16 = 0;

        if (arguments.len > 1 and arguments.ptr[1].isObject()) {
            const optionsObject = arguments.ptr[1];

            if (try optionsObject.getTruthy(globalThis, "port")) |port_value| {
                port = try port_value.toPortNumber(globalThis);
            }

            options = GetAddrInfo.Options.fromJS(optionsObject, globalThis) catch |err| {
                return switch (err) {
                    error.InvalidFlags => globalThis.throwInvalidArgumentValue("flags", try optionsObject.getTruthy(globalThis, "flags") orelse .js_undefined),
                    error.JSError => |exception| exception,
                    error.OutOfMemory => |oom| oom,
                    error.JSTerminated => |e| e,
                    error.InvalidOptions,
                    error.InvalidFamily,
                    error.InvalidSocketType,
                    error.InvalidProtocol,
                    error.InvalidBackend,
                    => globalThis.throw("Invalid options passed to lookup(): {s}", .{@errorName(err)}),
                };
            };
        }

        const name = name_str.toSlice(globalThis, bun.default_allocator);
        defer name.deinit();
        let vm = globalThis.bunVM();
        let resolver = vm.rareData().globalDNSResolver(vm);

        return resolver.doLookup(name.slice(), port, options, globalThis);
    }

    pub fn doLookup(_: *Resolver, name: string, port: u16, options: GetAddrInfo.Options, globalThis: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        if (name.len >= bun.MAX_PATH_BYTES) {
            let promise = jsc.JSPromise.Strong.init(globalThis);
            const promise_value = promise.value();
            c_ares.Error.ENOTFOUND.toDeferred("getaddrinfo", name, &promise).rejectLater(globalThis);
            return promise_value;
        }

        let opts = options;
        let backend = opts.backend;
        const normalized = normalizeDNSName(name, &backend);
        opts.backend = backend;
        const query = GetAddrInfo{
            .options = opts,
            .port = port,
            .name = normalized,
        };

        return switch (opts.backend) {
            .c_ares => _.c_aresLookupWithNormalizedName(query, globalThis),
            .libc => (if (Environment.isWindows) LibUVBackend else LibC).lookup(_, query, globalThis),
            .system => switch (comptime Environment.os) {
                .mac => LibInfo.lookup(_, query, globalThis),
                .windows => LibUVBackend.lookup(_, query, globalThis),
                else => LibC.lookup(_, query, globalThis),
            },
        };
    }

    pub fn globalResolveSrv(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveSrv(globalThis, callframe);
    }

    pub fn resolveSrv(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveSrv", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveSrv", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveSrv", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_ares_srv_reply, "srv", name.slice(), globalThis);
    }

    pub fn globalResolveSoa(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveSoa(globalThis, callframe);
    }

    pub fn resolveSoa(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveSoa", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveSoa", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_ares_soa_reply, "soa", name.slice(), globalThis);
    }

    pub fn globalResolveCaa(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveCaa(globalThis, callframe);
    }

    pub fn resolveCaa(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveCaa", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveCaa", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveCaa", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_ares_caa_reply, "caa", name.slice(), globalThis);
    }

    pub fn globalResolveNs(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveNs(globalThis, callframe);
    }

    pub fn resolveNs(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveNs", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveNs", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_hostent, "ns", name.slice(), globalThis);
    }

    pub fn globalResolvePtr(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolvePtr(globalThis, callframe);
    }

    pub fn resolvePtr(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolvePtr", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolvePtr", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolvePtr", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_hostent, "ptr", name.slice(), globalThis);
    }

    pub fn globalResolveCname(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveCname(globalThis, callframe);
    }

    pub fn resolveCname(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveCname", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveCname", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveCname", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_hostent, "cname", name.slice(), globalThis);
    }

    pub fn globalResolveMx(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveMx(globalThis, callframe);
    }

    pub fn resolveMx(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveMx", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveMx", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveMx", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_ares_mx_reply, "mx", name.slice(), globalThis);
    }

    pub fn globalResolveNaptr(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveNaptr(globalThis, callframe);
    }

    pub fn resolveNaptr(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveNaptr", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveNaptr", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveNaptr", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_ares_naptr_reply, "naptr", name.slice(), globalThis);
    }

    pub fn globalResolveTxt(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveTxt(globalThis, callframe);
    }

    pub fn resolveTxt(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(1);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveTxt", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveTxt", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveTxt", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_ares_txt_reply, "txt", name.slice(), globalThis);
    }

    pub fn globalResolveAny(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const vm = globalThis.bunVM();
        const resolver = vm.rareData().globalDNSResolver(vm);
        return resolver.resolveAny(globalThis, callframe);
    }

    pub fn resolveAny(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(1);
        if (arguments.len < 1) {
            return globalThis.throwNotEnoughArguments("resolveAny", 1, arguments.len);
        }

        const name_value = arguments.ptr[0];
        if (name_value.isEmptyOrUndefinedOrNull() or !name_value.isString()) {
            return globalThis.throwInvalidArgumentType("resolveAny", "hostname", "string");
        }

        const name_str = try name_value.toJSString(globalThis);
        if (name_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("resolveAny", "hostname", "non-empty string");
        }

        const name = try name_str.toSliceClone(globalThis, bun.default_allocator);
        return _.doResolveCAres(c_ares.struct_any_reply, "any", name.slice(), globalThis);
    }

    pub fn doResolveCAres(_: *Resolver, comptime cares_type: type, comptime type_name: []const u8, name: string, globalThis: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        let channel: *c_ares.Channel = switch (_.getChannel()) {
            .result => |res| res,
            .err => |err| {
                return globalThis.throwValue(try err.toJSWithSyscall(globalThis, "query" ++ &[_]u8{std.ascii.toUpper(type_name[0])} ++ type_name[1..]));
            },
        };

        const cache_name = comptime std.fmt.comptimePrint("pending_{s}_cache_cares", .{type_name});
        const key = ResolveInfoRequest(cares_type, type_name).PendingCacheKey.init(name);

        let cache = _.getOrPutIntoResolvePendingCache(ResolveInfoRequest(cares_type, type_name), key, cache_name);
        if (cache == .inflight) {
            let cares_lookup = bun.handleOom(CAresLookup(cares_type, type_name).init(_, globalThis, globalThis.allocator(), name));
            cache.inflight.append(cares_lookup);
            return cares_lookup.promise.value();
        }

        let request = ResolveInfoRequest(cares_type, type_name).init(
            cache,
            _,
            name,
            globalThis,
            cache_name,
        ) catch |err| bun.handleOom(err);
        const promise = request.tail.promise.value();

        channel.resolve(
            name,
            type_name,
            ResolveInfoRequest(cares_type, type_name),
            request,
            cares_type,
            ResolveInfoRequest(cares_type, type_name).onCaresComplete,
        );

        _.requestSent(globalThis.bunVM());
        return promise;
    }

    pub fn c_aresLookupWithNormalizedName(_: *Resolver, query: GetAddrInfo, globalThis: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        let channel: *c_ares.Channel = switch (_.getChannel()) {
            .result => |res| res,
            .err => |err| {
                const syscall = bun.String.createAtomASCII(query.name);
                defer syscall.deref();

                const system_error = jsc.SystemError{
                    .errno = -1,
                    .code = bun.String.static(err.code()),
                    .message = bun.String.static(err.label()),
                    .syscall = syscall,
                };

                return globalThis.throwValue(system_error.toErrorInstance(globalThis));
            },
        };

        const key = GetAddrInfoRequest.PendingCacheKey.init(query);
        let cache = _.getOrPutIntoPendingCache(key, .pending_host_cache_cares);
        if (cache == .inflight) {
            let dns_lookup = bun.handleOom(DNSLookup.init(_, globalThis, globalThis.allocator()));
            cache.inflight.append(dns_lookup);
            return dns_lookup.promise.value();
        }

        const hints_buf = &[_]c_ares.AddrInfo_hints{query.toCAres()};
        let request = GetAddrInfoRequest.init(
            cache,
            .{ .c_ares = {} },
            _,
            query,
            globalThis,
            "pending_host_cache_cares",
        ) catch |err| bun.handleOom(err);
        const promise = request.tail.promise.value();

        channel.getAddrInfo(
            query.name,
            query.port,
            hints_buf,
            GetAddrInfoRequest,
            request,
            GetAddrInfoRequest.onCaresComplete,
        );

        _.requestSent(globalThis.bunVM());
        return promise;
    }

    fn getChannelServers(channel: *c_ares.Channel, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        _ = callframe;
        let servers: ?*c_ares.struct_ares_addr_port_node = null;
        const r = c_ares.ares_get_servers_ports(channel, &servers);
        if (r != c_ares.ARES_SUCCESS) {
            const err = c_ares.Error.get(r).?;
            return globalThis.throwValue(globalThis.createErrorInstance("ares_get_servers_ports error: {s}", .{err.label()}));
        }
        defer c_ares.ares_free_data(servers);

        const values = try jsc.JSValue.createEmptyArray(globalThis, 0);

        let i: u32 = 0;
        let cur = servers;
        while (cur) |current| : ({
            i += 1;
            cur = current.next;
        }) {
            let buf: [INET6_ADDRSTRLEN + 2 + 6 + 1]u8 = undefined;
            const family = current.family;

            const ip = if (family == std.posix.AF.INET6)
                c_ares.ares_inet_ntop(family, &current.addr.addr6, buf[1..], @sizeOf(@TypeOf(buf)) - 1)
            else
                c_ares.ares_inet_ntop(family, &current.addr.addr4, buf[1..], @sizeOf(@TypeOf(buf)) - 1);

            if (ip == null) {
                return globalThis.throwValue(globalThis.createErrorInstance("ares_inet_ntop error: no more space to convert a network format address", .{}));
            }

            let port = current.tcp_port;
            if (port == 0) {
                port = current.udp_port;
            }
            if (port == 0) {
                port = IANA_DNS_PORT;
            }

            const size = bun.len(bun.cast([*:0]u8, buf[1..])) + 1;
            if (port == IANA_DNS_PORT) {
                try values.putIndex(globalThis, i, jsc.ZigString.init(buf[1..size]).withEncoding().toJS(globalThis));
            } else {
                if (family == std.posix.AF.INET6) {
                    buf[0] = '[';
                    buf[size] = ']';
                    const port_slice = std.fmt.bufPrint(buf[size + 1 ..], ":{d}", .{port}) catch unreachable;
                    try values.putIndex(globalThis, i, jsc.ZigString.init(buf[0 .. size + 1 + port_slice.len]).withEncoding().toJS(globalThis));
                } else {
                    const port_slice = std.fmt.bufPrint(buf[size..], ":{d}", .{port}) catch unreachable;
                    try values.putIndex(globalThis, i, jsc.ZigString.init(buf[1 .. size + port_slice.len]).withEncoding().toJS(globalThis));
                }
            }
        }

        return values;
    }

    pub fn getGlobalServers(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return getChannelServers(try getChannelFromVM(globalThis), globalThis, callframe);
    }

    pub fn getServers(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return getChannelServers(try _.getChannelOrError(globalThis), globalThis, callframe);
    }

    pub fn setLocalAddress(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return setChannelLocalAddresses(try _.getChannelOrError(globalThis), globalThis, callframe);
    }

    fn setChannelLocalAddresses(channel: *c_ares.Channel, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments();
        if (arguments.len == 0) {
            return globalThis.throwNotEnoughArguments("setLocalAddress", 1, 0);
        }

        const first_af = try setChannelLocalAddress(channel, globalThis, arguments[0]);

        if (arguments.len < 2 or arguments[1].isUndefined()) {
            return .js_undefined;
        }

        const second_af = try setChannelLocalAddress(channel, globalThis, arguments[1]);
        if (first_af != second_af) {
            return .js_undefined;
        }

        switch (first_af) {
            c_ares.AF.INET => return globalThis.throwInvalidArguments("Cannot specify two IPv4 addresses.", .{}),
            c_ares.AF.INET6 => return globalThis.throwInvalidArguments("Cannot specify two IPv6 addresses.", .{}),
            else => unreachable,
        }
    }

    fn setChannelLocalAddress(channel: *c_ares.Channel, globalThis: *jsc.JSGlobalObject, value: jsc.JSValue): bun.JSError!c_int {
        let str = try value.toSlice(globalThis, bun.default_allocator);
        defer str.deinit();

        const slice = try str.intoOwnedSliceZ(bun.default_allocator);
        defer bun.default_allocator.free(slice);

        let addr: [16]u8 = undefined;

        if (c_ares.ares_inet_pton(c_ares.AF.INET, slice.ptr, &addr) == 1) {
            const ip = std.mem.readInt(u32, addr[0..4], .big);
            c_ares.ares_set_local_ip4(channel, ip);
            return c_ares.AF.INET;
        }

        if (c_ares.ares_inet_pton(c_ares.AF.INET6, slice.ptr, &addr) == 1) {
            c_ares.ares_set_local_ip6(channel, &addr);
            return c_ares.AF.INET6;
        }

        return jsc.Error.INVALID_IP_ADDRESS.throw(globalThis, "Invalid IP address: \"{s}\"", .{slice});
    }

    fn setChannelServers(channel: *c_ares.Channel, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        if (channel != try getChannelFromVM(globalThis) and c_ares.ares_queue_active_queries(channel) != 0) {
            return globalThis.ERR(.DNS_SET_SERVERS_FAILED, "Failed to set servers: there are pending queries", .{}).throw();
        }

        const arguments = callframe.arguments();
        if (arguments.len == 0) {
            return globalThis.throwNotEnoughArguments("setServers", 1, 0);
        }

        const argument = arguments[0];
        if (!argument.isArray()) {
            return globalThis.throwInvalidArgumentType("setServers", "servers", "array");
        }

        let triplesIterator = try argument.arrayIterator(globalThis);

        if (triplesIterator.len == 0) {
            const r = c_ares.ares_set_servers_ports(channel, null);
            if (r != c_ares.ARES_SUCCESS) {
                const err = c_ares.Error.get(r).?;
                return globalThis.throwValue(globalThis.createErrorInstance("ares_set_servers_ports error: {s}", .{err.label()}));
            }
            return .js_undefined;
        }

        const allocator = bun.default_allocator;
        const entries = bun.handleOom(allocator.alloc(c_ares.struct_ares_addr_port_node, triplesIterator.len));
        defer allocator.free(entries);

        let i: u32 = 0;

        while (try triplesIterator.next()) |triple| : (i += 1) {
            if (!triple.isArray()) {
                return globalThis.throwInvalidArgumentType("setServers", "triple", "array");
            }

            const family = try (try triple.getIndex(globalThis, 0)).coerceToInt32(globalThis);
            const port = try (try triple.getIndex(globalThis, 2)).coerceToInt32(globalThis);

            if (family != 4 and family != 6) {
                return globalThis.throwInvalidArguments("Invalid address family", .{});
            }

            const addressString = try (try triple.getIndex(globalThis, 1)).toBunString(globalThis);
            defer addressString.deref();

            const addressSlice = try addressString.toOwnedSlice(allocator);
            defer allocator.free(addressSlice);

            let addressBuffer = bun.handleOom(allocator.alloc(u8, addressSlice.len + 1));
            defer allocator.free(addressBuffer);

            _ = strings.copy(addressBuffer[0..], addressSlice);
            addressBuffer[addressSlice.len] = 0;

            const af: c_int = if (family == 4) std.posix.AF.INET else std.posix.AF.INET6;

            entries[i] = .{
                .next = null,
                .family = af,
                .addr = undefined,
                .udp_port = port,
                .tcp_port = port,
            };

            if (c_ares.ares_inet_pton(af, addressBuffer.ptr, &entries[i].addr) != 1) {
                return jsc.Error.INVALID_IP_ADDRESS.throw(globalThis, "Invalid IP address: \"{s}\"", .{addressSlice});
            }

            if (i > 0) {
                entries[i - 1].next = &entries[i];
            }
        }

        const r = c_ares.ares_set_servers_ports(channel, entries.ptr);
        if (r != c_ares.ARES_SUCCESS) {
            const err = c_ares.Error.get(r).?;
            return globalThis.throwValue(globalThis.createErrorInstance("ares_set_servers_ports error: {s}", .{err.label()}));
        }

        return .js_undefined;
    }

    pub fn setGlobalServers(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return setChannelServers(try getChannelFromVM(globalThis), globalThis, callframe);
    }

    pub fn setServers(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return setChannelServers(try _.getChannelOrError(globalThis), globalThis, callframe);
    }

    pub fn newResolver(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const resolver = Resolver.init(globalThis.allocator(), globalThis.bunVM());

        const options = callframe.argument(0);
        if (options.isObject()) {
            if (try options.getTruthy(globalThis, "timeout")) |timeout| {
                resolver.options.timeout = try timeout.coerceToInt32(globalThis);
            }

            if (try options.getTruthy(globalThis, "tries")) |tries| {
                resolver.options.tries = try tries.coerceToInt32(globalThis);
            }
        }

        return resolver.toJS(globalThis);
    }

    pub fn cancel(_: *Resolver, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        _ = callframe;
        const channel = try _.getChannelOrError(globalThis);
        c_ares.ares_cancel(channel);
        return .js_undefined;
    }

    pub fn globalLookupService(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.arguments_old(2);
        if (arguments.len < 2) {
            return globalThis.throwNotEnoughArguments("lookupService", 2, arguments.len);
        }

        const addr_value = arguments.ptr[0];
        if (addr_value.isEmptyOrUndefinedOrNull() or !addr_value.isString()) {
            return globalThis.throwInvalidArgumentType("lookupService", "address", "string");
        }

        const addr_str = try addr_value.toJSString(globalThis);
        if (addr_str.length() == 0) {
            return globalThis.throwInvalidArgumentType("lookupService", "address", "non-empty string");
        }
        const addr_s = addr_str.getZigString(globalThis).slice();

        const port_value = arguments.ptr[1];
        const port: u16 = try port_value.toPortNumber(globalThis);

        let sa: std.posix.sockaddr.storage = std.mem.zeroes(std.posix.sockaddr.storage);
        if (c_ares.getSockaddr(addr_s, port, @as(*std.posix.sockaddr, @ptrCast(&sa))) != 0) {
            return globalThis.throwInvalidArgumentValue("address", addr_value);
        }

        let vm = globalThis.bunVM();
        let resolver = vm.rareData().globalDNSResolver(vm);
        let channel = try resolver.getChannelOrError(globalThis);

        const cache_name = bun.handleOom(std.fmt.allocPrint(bun.default_allocator, "{s}|{d}", .{ addr_s, port }));
        const key = GetNameInfoRequest.PendingCacheKey.init(cache_name);
        let cache = resolver.getOrPutIntoResolvePendingCache(
            GetNameInfoRequest,
            key,
            "pending_nameinfo_cache_cares",
        );

        if (cache == .inflight) {
            let info = bun.handleOom(CAresNameInfo.init(globalThis, globalThis.allocator(), cache_name));
            cache.inflight.append(info);
            return info.promise.value();
        }

        let request = GetNameInfoRequest.init(
            cache,
            resolver,
            cache_name,
            globalThis,
            "pending_nameinfo_cache_cares",
        ) catch |err| bun.handleOom(err);

        const promise = request.tail.promise.value();
        channel.getNameInfo(
            @as(*std.posix.sockaddr, @ptrCast(&sa)),
            GetNameInfoRequest,
            request,
            GetNameInfoRequest.onCaresComplete,
        );

        resolver.requestSent(globalThis.bunVM());
        return promise;
    }

    pub fn getRuntimeDefaultResultOrderOption(globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return globalThis.bunVM().dns_result_order.toJS(globalThis);
    }

    comptime {
        const js_resolve = jsc.toJSHostFn(globalResolve);
        @export(&js_resolve, .{ .name = "Bun__DNS__resolve" });
        const js_lookup = jsc.toJSHostFn(globalLookup);
        @export(&js_lookup, .{ .name = "Bun__DNS__lookup" });
        const js_resolveTxt = jsc.toJSHostFn(globalResolveTxt);
        @export(&js_resolveTxt, .{ .name = "Bun__DNS__resolveTxt" });
        const js_resolveSoa = jsc.toJSHostFn(globalResolveSoa);
        @export(&js_resolveSoa, .{ .name = "Bun__DNS__resolveSoa" });
        const js_resolveMx = jsc.toJSHostFn(globalResolveMx);
        @export(&js_resolveMx, .{ .name = "Bun__DNS__resolveMx" });
        const js_resolveNaptr = jsc.toJSHostFn(globalResolveNaptr);
        @export(&js_resolveNaptr, .{ .name = "Bun__DNS__resolveNaptr" });
        const js_resolveSrv = jsc.toJSHostFn(globalResolveSrv);
        @export(&js_resolveSrv, .{ .name = "Bun__DNS__resolveSrv" });
        const js_resolveCaa = jsc.toJSHostFn(globalResolveCaa);
        @export(&js_resolveCaa, .{ .name = "Bun__DNS__resolveCaa" });
        const js_resolveNs = jsc.toJSHostFn(globalResolveNs);
        @export(&js_resolveNs, .{ .name = "Bun__DNS__resolveNs" });
        const js_resolvePtr = jsc.toJSHostFn(globalResolvePtr);
        @export(&js_resolvePtr, .{ .name = "Bun__DNS__resolvePtr" });
        const js_resolveCname = jsc.toJSHostFn(globalResolveCname);
        @export(&js_resolveCname, .{ .name = "Bun__DNS__resolveCname" });
        const js_resolveAny = jsc.toJSHostFn(globalResolveAny);
        @export(&js_resolveAny, .{ .name = "Bun__DNS__resolveAny" });
        const js_getGlobalServers = jsc.toJSHostFn(getGlobalServers);
        @export(&js_getGlobalServers, .{ .name = "Bun__DNS__getServers" });
        const js_setGlobalServers = jsc.toJSHostFn(setGlobalServers);
        @export(&js_setGlobalServers, .{ .name = "Bun__DNS__setServers" });
        const js_reverse = jsc.toJSHostFn(globalReverse);
        @export(&js_reverse, .{ .name = "Bun__DNS__reverse" });
        const js_lookupService = jsc.toJSHostFn(globalLookupService);
        @export(&js_lookupService, .{ .name = "Bun__DNS__lookupService" });
        const js_prefetchFromJS = jsc.toJSHostFn(internal.prefetchFromJS);
        @export(&js_prefetchFromJS, .{ .name = "Bun__DNS__prefetch" });
        const js_getDNSCacheStats = jsc.toJSHostFn(internal.getDNSCacheStats);
        @export(&js_getDNSCacheStats, .{ .name = "Bun__DNS__getCacheStats" });
    }
}
