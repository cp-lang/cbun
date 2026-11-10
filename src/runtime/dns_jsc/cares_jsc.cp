// src/runtime/dns_jsc/cares_jsc.cp
//! JSC bridges for c-ares reply structs. Keeps `src/cares_sys/` free of
//! `JSValue`/`JSGlobalObject`/`CallFrame` types — the original methods on
//! each `struct_ares_*_reply` are aliased to the free fns here.

const std = @import("std");
const bun = @import("bun");
const c_ares = bun.c_ares;
const jsc = bun.jsc;
const strings = bun.strings;
const GetAddrInfo = bun.dns.GetAddrInfo;

// ── struct_hostent ─────────────────────────────────────────────────────────
pub fn hostentToJSResponse(reply: *c_ares.struct_hostent, parent_alloc: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = parent_alloc;
    if (comptime strings.eqlComptime(lookup_name, "cname")) {
        // A cname lookup always returns a single record but we follow the common API here.
        if (reply.h_name == null) {
            return jsc.JSValue.createEmptyArray(globalThis, 0)?;
        }
        const item = bun.String.borrowUTF8(reply.h_name.?[0..bun.len(reply.h_name.?)]);
        return bun.String.toJSArray(globalThis, &[item]);
    }

    if (reply.h_aliases == null) {
        return jsc.JSValue.createEmptyArray(globalThis, 0)?;
    }

    let count: u32 = 0;
    while (reply.h_aliases.?[count] != null) {
        count += 1;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;
    count = 0;

    while (alias := reply.h_aliases.?[count]) {
        const alias_len = bun.len(alias);
        const alias_slice = alias[0..alias_len];
        array.putIndex(globalThis, count, jsc.ZigString.fromUTF8(alias_slice).toJS(globalThis))?;
        count += 1;
    }

    return array;
}

// ── hostent_with_ttls ──────────────────────────────────────────────────────
pub fn hostentWithTtlsToJSResponse(reply: *c_ares.hostent_with_ttls, parent_alloc: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = parent_alloc;
    if (comptime strings.eqlComptime(lookup_name, "a") || strings.eqlComptime(lookup_name, "aaaa")) {
        if (reply.hostent.h_addr_list == null) {
            return jsc.JSValue.createEmptyArray(globalThis, 0)?;
        }

        let count: u32 = 0;
        while (reply.hostent.h_addr_list.?[count] != null) {
            count += 1;
        }

        const array = jsc.JSValue.createEmptyArray(globalThis, count)?;
        count = 0;

        const addressKey = jsc.ZigString.static("address").withEncoding();
        const ttlKey = jsc.ZigString.static("ttl").withEncoding();

        while (addr := reply.hostent.h_addr_list.?[count]) {
            const addrString = (if (reply.hostent.h_addrtype == c_ares.AF.INET6)
                bun.dns.addressToJS(&std.net.Address.initIp6(addr[0..16].*, 0, 0, 0), globalThis)
            else
                bun.dns.addressToJS(&std.net.Address.initIp4(addr[0..4].*, 0), globalThis)) catch return globalThis.throwOutOfMemoryValue();

            const ttl: ?i32 = if (count < reply.ttls.len) reply.ttls[count] else null;
            const ttlVal = if (ttl != null) jsc.JSValue.jsNumber(ttl.?) else jsc.JSValue.js_undefined;
            const resultObject = jsc.JSValue.createObject2(globalThis, &addressKey, &ttlKey, addrString, ttlVal)?;
            array.putIndex(globalThis, count, resultObject)?;
            count += 1;
        }

        return array;
    } else {
        @compileError(std.fmt.comptimePrint("Unsupported hostent_with_ttls record type: {s}", .{lookup_name}));
    }
}

// ── struct_nameinfo ────────────────────────────────────────────────────────
pub fn nameinfoToJSResponse(reply: *c_ares.struct_nameinfo, parent_alloc: std.mem.Allocator, globalThis: *jsc.JSGlobalObject): !jsc.JSValue {
    _ = parent_alloc;
    const array = jsc.JSValue.createEmptyArray(globalThis, 2)?;

    if (reply.node != null) {
        const node_len = bun.len(reply.node);
        const node_slice = reply.node[0..node_len];
        array.putIndex(globalThis, 0, jsc.ZigString.fromUTF8(node_slice).toJS(globalThis))?;
    } else {
        array.putIndex(globalThis, 0, jsc.JSValue.js_undefined)?;
    }

    if (reply.service != null) {
        const service_len = bun.len(reply.service);
        const service_slice = reply.service[0..service_len];
        array.putIndex(globalThis, 1, jsc.ZigString.fromUTF8(service_slice).toJS(globalThis))?;
    } else {
        array.putIndex(globalThis, 1, jsc.JSValue.js_undefined)?;
    }

    return array;
}

// ── AddrInfo ───────────────────────────────────────────────────────────────
pub fn addrInfoToJSArray(addr_info: *c_ares.AddrInfo, globalThis: *jsc.JSGlobalObject): !jsc.JSValue {
    if (addr_info.node == null) {
        return jsc.JSValue.createEmptyArray(globalThis, 0)?;
    }
    const node = addr_info.node.?;
    const array = jsc.JSValue.createEmptyArray(globalThis, node.count())?;

    let j: u32 = 0;
    let current: ?*c_ares.AddrInfo_node = addr_info.node;
    while (current != null) {
        const this_node = current.?;
        current = this_node.next;
        const addr_val: std.net.Address = if (this_node.family == c_ares.AF.INET) blk: {
            const in_ptr: *const std.posix.sockaddr.in = @ptrCast(this_node.addr.?);
            break :blk std.net.Address{ .in = .{ .sa = in_ptr.* } };
        } else if (this_node.family == c_ares.AF.INET6) blk: {
            const in6_ptr: *const std.posix.sockaddr.in6 = @ptrCast(this_node.addr.?);
            break :blk std.net.Address{ .in6 = .{ .sa = in6_ptr.* } };
        } else unreachable;
        const res = GetAddrInfo.Result{
            .address = addr_val,
            .ttl = this_node.ttl,
        };
        const js_res = GetAddrInfo.Result.toJS(&res, globalThis) catch |err| return err;
        array.putIndex(globalThis, j, js_res)?;
        j += 1;
    }

    return array;
}

// ── struct_ares_caa_reply ──────────────────────────────────────────────────
pub fn caaReplyToJSResponse(reply: *c_ares.struct_ares_caa_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    let count: usize = 0;
    let caa: ?*c_ares.struct_ares_caa_reply = reply;
    while (caa != null) {
        count += 1;
        caa = caa.?.next;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;

    caa = reply;
    let i: u32 = 0;
    while (caa != null) {
        const node = caa.?;
        array.putIndex(globalThis, i, node.toJS(globalThis, allocator))?;
        caa = node.next;
        i += 1;
    }

    return array;
}

pub fn caaReplyToJS(reply: *c_ares.struct_ares_caa_reply, globalThis: *jsc.JSGlobalObject, alloc: std.mem.Allocator): jsc.JSValue {
    _ = alloc;
    const obj = jsc.JSValue.createEmptyObject(globalThis, 2);

    obj.put(globalThis, jsc.ZigString.static("critical"), jsc.JSValue.jsNumber(reply.critical));

    const property = reply.property[0..reply.plength];
    const value = reply.value[0..reply.length];
    const property_str = jsc.ZigString.fromUTF8(property);
    obj.put(globalThis, &property_str, jsc.ZigString.fromUTF8(value).toJS(globalThis));

    return obj;
}

// ── struct_ares_srv_reply ──────────────────────────────────────────────────
pub fn srvReplyToJSResponse(reply: *c_ares.struct_ares_srv_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    let count: usize = 0;
    let srv: ?*c_ares.struct_ares_srv_reply = reply;
    while (srv != null) {
        count += 1;
        srv = srv.?.next;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;

    srv = reply;
    let i: u32 = 0;
    while (srv != null) {
        const node = srv.?;
        array.putIndex(globalThis, i, node.toJS(globalThis, allocator))?;
        srv = node.next;
        i += 1;
    }

    return array;
}

pub fn srvReplyToJS(reply: *c_ares.struct_ares_srv_reply, globalThis: *jsc.JSGlobalObject, alloc: std.mem.Allocator): jsc.JSValue {
    _ = alloc;
    const obj = jsc.JSValue.createEmptyObject(globalThis, 4);

    obj.put(globalThis, jsc.ZigString.static("priority"), jsc.JSValue.jsNumber(reply.priority));
    obj.put(globalThis, jsc.ZigString.static("weight"), jsc.JSValue.jsNumber(reply.weight));
    obj.put(globalThis, jsc.ZigString.static("port"), jsc.JSValue.jsNumber(reply.port));

    const len = bun.len(reply.host);
    const host = reply.host[0..len];
    obj.put(globalThis, jsc.ZigString.static("name"), jsc.ZigString.fromUTF8(host).toJS(globalThis));

    return obj;
}

// ── struct_ares_mx_reply ───────────────────────────────────────────────────
pub fn mxReplyToJSResponse(reply: *c_ares.struct_ares_mx_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    let count: usize = 0;
    let mx: ?*c_ares.struct_ares_mx_reply = reply;
    while (mx != null) {
        count += 1;
        mx = mx.?.next;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;

    mx = reply;
    let i: u32 = 0;
    while (mx != null) {
        const node = mx.?;
        array.putIndex(globalThis, i, node.toJS(globalThis, allocator))?;
        mx = node.next;
        i += 1;
    }

    return array;
}

pub fn mxReplyToJS(reply: *c_ares.struct_ares_mx_reply, globalThis: *jsc.JSGlobalObject, alloc: std.mem.Allocator): jsc.JSValue {
    _ = alloc;
    const obj = jsc.JSValue.createEmptyObject(globalThis, 2);
    obj.put(globalThis, jsc.ZigString.static("priority"), jsc.JSValue.jsNumber(reply.priority));

    const host_len = bun.len(reply.host);
    const host = reply.host[0..host_len];
    obj.put(globalThis, jsc.ZigString.static("exchange"), jsc.ZigString.fromUTF8(host).toJS(globalThis));

    return obj;
}

// ── struct_ares_txt_reply ──────────────────────────────────────────────────
pub fn txtReplyToJSResponse(reply: *c_ares.struct_ares_txt_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    let count: usize = 0;
    let txt: ?*c_ares.struct_ares_txt_reply = reply;
    while (txt != null) {
        count += 1;
        txt = txt.?.next;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;

    txt = reply;
    let i: u32 = 0;
    while (txt != null) {
        const node = txt.?;
        const js_item = node.toJS(globalThis, allocator) catch |err| return err;
        array.putIndex(globalThis, i, js_item)?;
        txt = node.next;
        i += 1;
    }

    return array;
}

pub fn txtReplyToJS(reply: *c_ares.struct_ares_txt_reply, globalThis: *jsc.JSGlobalObject, alloc: std.mem.Allocator): !jsc.JSValue {
    _ = alloc;
    const array = jsc.JSValue.createEmptyArray(globalThis, 1)?;
    const value = reply.txt[0..reply.length];
    array.putIndex(globalThis, 0, jsc.ZigString.fromUTF8(value).toJS(globalThis))?;
    return array;
}

pub fn txtReplyToJSForAny(reply: *c_ares.struct_ares_txt_reply, parent_alloc: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = parent_alloc;
    _ = lookup_name;
    let count: usize = 0;
    let txt: ?*c_ares.struct_ares_txt_reply = reply;
    while (txt != null) {
        count += 1;
        txt = txt.?.next;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;

    txt = reply;
    let i: u32 = 0;
    while (txt != null) {
        const node = txt.?;
        array.putIndex(globalThis, i, jsc.ZigString.fromUTF8(node.txt[0..node.length]).toJS(globalThis))?;
        txt = node.next;
        i += 1;
    }

    const obj = jsc.JSObject.create(.{
        .entries = array,
    }, globalThis)?;
    return obj.toJS();
}

// ── struct_ares_naptr_reply ────────────────────────────────────────────────
pub fn naptrReplyToJSResponse(reply: *c_ares.struct_ares_naptr_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    let count: usize = 0;
    let naptr: ?*c_ares.struct_ares_naptr_reply = reply;
    while (naptr != null) {
        count += 1;
        naptr = naptr.?.next;
    }

    const array = jsc.JSValue.createEmptyArray(globalThis, count)?;

    naptr = reply;
    let i: u32 = 0;
    while (naptr != null) {
        const node = naptr.?;
        array.putIndex(globalThis, i, node.toJS(globalThis, allocator))?;
        naptr = node.next;
        i += 1;
    }

    return array;
}

pub fn naptrReplyToJS(reply: *c_ares.struct_ares_naptr_reply, globalThis: *jsc.JSGlobalObject, alloc: std.mem.Allocator): jsc.JSValue {
    _ = alloc;
    const obj = jsc.JSValue.createEmptyObject(globalThis, 6);

    obj.put(globalThis, jsc.ZigString.static("preference"), jsc.JSValue.jsNumber(reply.preference));
    obj.put(globalThis, jsc.ZigString.static("order"), jsc.JSValue.jsNumber(reply.order));

    const flags_len = bun.len(reply.flags);
    const flags = reply.flags[0..flags_len];
    obj.put(globalThis, jsc.ZigString.static("flags"), jsc.ZigString.fromUTF8(flags).toJS(globalThis));

    const service_len = bun.len(reply.service);
    const service = reply.service[0..service_len];
    obj.put(globalThis, jsc.ZigString.static("service"), jsc.ZigString.fromUTF8(service).toJS(globalThis));

    const regexp_len = bun.len(reply.regexp);
    const regexp = reply.regexp[0..regexp_len];
    obj.put(globalThis, jsc.ZigString.static("regexp"), jsc.ZigString.fromUTF8(regexp).toJS(globalThis));

    const replacement_len = bun.len(reply.replacement);
    const replacement = reply.replacement[0..replacement_len];
    obj.put(globalThis, jsc.ZigString.static("replacement"), jsc.ZigString.fromUTF8(replacement).toJS(globalThis));

    return obj;
}

// ── struct_ares_soa_reply ──────────────────────────────────────────────────
pub fn soaReplyToJSResponse(reply: *c_ares.struct_ares_soa_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    return reply.toJS(globalThis, allocator);
}

pub fn soaReplyToJS(reply: *c_ares.struct_ares_soa_reply, globalThis: *jsc.JSGlobalObject, alloc: std.mem.Allocator): jsc.JSValue {
    _ = alloc;
    const obj = jsc.JSValue.createEmptyObject(globalThis, 7);

    obj.put(globalThis, jsc.ZigString.static("serial"), jsc.JSValue.jsNumber(reply.serial));
    obj.put(globalThis, jsc.ZigString.static("refresh"), jsc.JSValue.jsNumber(reply.refresh));
    obj.put(globalThis, jsc.ZigString.static("retry"), jsc.JSValue.jsNumber(reply.retry));
    obj.put(globalThis, jsc.ZigString.static("expire"), jsc.JSValue.jsNumber(reply.expire));
    obj.put(globalThis, jsc.ZigString.static("minttl"), jsc.JSValue.jsNumber(reply.minttl));

    const nsname_len = bun.len(reply.nsname);
    const nsname = reply.nsname[0..nsname_len];
    obj.put(globalThis, jsc.ZigString.static("nsname"), jsc.ZigString.fromUTF8(nsname).toJS(globalThis));

    const hostmaster_len = bun.len(reply.hostmaster);
    const hostmaster = reply.hostmaster[0..hostmaster_len];
    obj.put(globalThis, jsc.ZigString.static("hostmaster"), jsc.ZigString.fromUTF8(hostmaster).toJS(globalThis));

    return obj;
}

// ── struct_any_reply ───────────────────────────────────────────────────────
pub fn anyReplyToJSResponse(reply: *c_ares.struct_any_reply, parent_allocator: std.mem.Allocator, globalThis: *jsc.JSGlobalObject, comptime lookup_name: []const u8): !jsc.JSValue {
    _ = lookup_name;
    let stack = std.heap.stackFallback(2048, parent_allocator);
    let arena_allocator = bun.ArenaAllocator.init(stack.get());
    defer arena_allocator.deinit();

    const allocator = arena_allocator.allocator();
    return reply.toJS(globalThis, allocator);
}

fn anyReplyAppend(globalThis: *jsc.JSGlobalObject, array: jsc.JSValue, i: *u32, response: jsc.JSValue, comptime lookup_name: []const u8): !void {
    const transformed = if (response.isString())
        (jsc.JSObject.create(.{
            .value = response,
        }, globalThis) catch |err| return err).toJS()
    else blk: {
        bun.assert(response.isObject());
        break :blk response;
    };

    let upper: [lookup_name.len]u8 = undefined;
    let idx: usize = 0;
    while (idx < lookup_name.len) {
        upper[idx] = std.ascii.toUpper(lookup_name[idx]);
        idx += 1;
    }

    const upper_js = bun.String.ascii(&upper).toJS(globalThis) catch |err| return err;
    transformed.put(globalThis, "type", upper_js);
    array.putIndex(globalThis, i.*, transformed) catch |err| return err;
    i.* += 1;
}

fn anyReplyAppendAll(globalThis: *jsc.JSGlobalObject, allocator: std.mem.Allocator, array: jsc.JSValue, i: *u32, reply: auto, comptime lookup_name: []const u8): !void {
    const response: jsc.JSValue = if (comptime @hasDecl(@TypeOf(reply.*), "toJSForAny"))
        (reply.toJSForAny(allocator, globalThis, lookup_name) catch |err| return err)
    else
        (reply.toJSResponse(allocator, globalThis, lookup_name) catch |err| return err);

    if (response.isArray()) {
        let iterator = response.arrayIterator(globalThis) catch |err| return err;
        while (item := (iterator.next() catch |err| return err)) {
            anyReplyAppend(globalThis, array, i, item, lookup_name) catch |err| return err;
        }
    } else {
        anyReplyAppend(globalThis, array, i, response, lookup_name) catch |err| return err;
    }
}

pub fn anyReplyToJS(reply: *c_ares.struct_any_reply, globalThis: *jsc.JSGlobalObject, allocator: std.mem.Allocator): !jsc.JSValue {
    const array = jsc.JSValue.createEmptyArray(globalThis, blk: {
        let len: usize = 0;
        @for (field of @typeInfo(c_ares.struct_any_reply).@"struct".fields) {
            if (bun.strings.hasSuffix(field.name, "_reply")) {
                if (@field(reply, field.name) != null) {
                    len += 1;
                }
            }
        }
        break :blk len;
    }) catch |err| return err;

    let i: u32 = 0;

    @for (field of @typeInfo(c_ares.struct_any_reply).@"struct".fields) {
        if (bun.strings.hasSuffix(field.name, "_reply")) {
            if (rep := @field(reply, field.name)) {
                const lookup_name = field.name[0 .. field.name.len - "_reply".len];
                anyReplyAppendAll(globalThis, allocator, array, &i, rep, lookup_name) catch |err| return err;
            }
        }
    }

    return array;
}

// ── Error ──────────────────────────────────────────────────────────────────
export struct ErrorDeferred {
    errno: c_ares.Error;
    syscall: []const u8;
    hostname: ?bun.String;
    promise: jsc.JSPromise.Strong;

    pub const new = bun.TrivialNew(@This());

    pub static fn init(errno: c_ares.Error, syscall: []const u8, hostname: ?bun.String, promise: jsc.JSPromise.Strong): *ErrorDeferred {
        return ErrorDeferred.new(.{
            .errno = errno,
            .syscall = syscall,
            .hostname = hostname,
            .promise = promise,
        });
    }

    pub fn reject(*this, globalThis: *jsc.JSGlobalObject): !void {
        const syscall_arg = this.syscall;
        const code_arg = this.errno.code()[4..];
        const msg = if (this.hostname != null) blk: {
            const host_arg = this.hostname.?;
            break :blk bun.handleOom(bun.String.createFormat("{s} {s} {f}", .{ syscall_arg, code_arg, host_arg }));
        } else blk: {
            break :blk bun.handleOom(bun.String.createFormat("{s} {s}", .{ syscall_arg, code_arg }));
        };

        const system_error = jsc.SystemError{
            .errno = @intFromEnum(this.errno),
            .code = bun.String.static(this.errno.code()),
            .message = msg,
            .syscall = bun.String.cloneUTF8(this.syscall),
            .hostname = this.hostname orelse bun.String.empty,
        };

        const instance = system_error.toErrorInstanceWithAsyncStack(globalThis, this.promise.get());
        const dns_name = bun.String.static("DNSException").toJS(globalThis) catch |err| return err;
        instance.put(globalThis, "name", dns_name);

        defer this.deinit();
        defer this.hostname = null;
        return this.promise.reject(globalThis, instance);
    }

    pub fn rejectLater(*this, globalThis: *jsc.JSGlobalObject) {
        const Context = struct {
            deferred: *ErrorDeferred,
            globalThis: *jsc.JSGlobalObject,
            pub fn callback(context: *@This()) !void {
                defer bun.default_allocator.destroy(context);
                context.deferred.reject(context.globalThis) catch |err| return err;
            }
        };

        let context = bun.handleOom(bun.default_allocator.create(Context));
        context.deferred = this;
        context.globalThis = globalThis;
        globalThis.bunVM().enqueueTask(jsc.ManagedTask.New(Context, Context.callback).init(context));
    }

    pub fn deinit(*this) {
        if (hostname := this.hostname) {
            hostname.deref();
        }
        this.promise.deinit();
        bun.destroy(this);
    }
}

pub fn errorToDeferred(err: c_ares.Error, syscall: []const u8, hostname: ?[]const u8, promise: *jsc.JSPromise.Strong): *ErrorDeferred {
    const host_string: ?bun.String = if (hostname != null)
        bun.String.cloneUTF8(hostname.?)
    else
        null;
    defer promise.* = .{};
    return ErrorDeferred.init(err, syscall, host_string, promise.*);
}

pub fn errorToJSWithSyscall(err: c_ares.Error, globalThis: *jsc.JSGlobalObject, comptime syscall: cstring): !jsc.JSValue {
    const err_code = err.code()[4..];
    const sys_err = jsc.SystemError{
        .errno = @intFromEnum(err),
        .code = bun.String.static(err_code),
        .syscall = bun.String.static(syscall),
        .message = bun.handleOom(bun.String.createFormat("{s} {s}", .{ syscall, err_code })),
    };
    const instance = sys_err.toErrorInstance(globalThis);
    const dns_name = bun.String.static("DNSException").toJS(globalThis) catch |e| return e;
    instance.put(globalThis, "name", dns_name);
    return instance;
}

pub fn errorToJSWithSyscallAndHostname(err: c_ares.Error, globalThis: *jsc.JSGlobalObject, comptime syscall: cstring, hostname: []const u8): !jsc.JSValue {
    const err_code = err.code()[4..];
    const sys_err = jsc.SystemError{
        .errno = @intFromEnum(err),
        .code = bun.String.static(err.code()),
        .message = bun.handleOom(bun.String.createFormat("{s} {s} {s}", .{ syscall, err_code, hostname })),
        .syscall = bun.String.static(syscall),
        .hostname = bun.String.cloneUTF8(hostname),
    };
    const instance = sys_err.toErrorInstance(globalThis);
    const dns_name = bun.String.static("DNSException").toJS(globalThis) catch |e| return e;
    instance.put(globalThis, "name", dns_name);
    return instance;
}

// ── canonicalizeIP host fn ─────────────────────────────────────────────────
! {
    const Bun__canonicalizeIP = jsc.toJSHostFn(Bun__canonicalizeIP_);
    @export(&Bun__canonicalizeIP, .{ .name = "Bun__canonicalizeIP" });
}

pub fn Bun__canonicalizeIP_(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): !jsc.JSValue {
    jsc.markBinding(@src());

    const arguments = callframe.arguments();

    if (arguments.len == 0) {
        return globalThis.throwInvalidArguments("canonicalizeIP() expects a string but received no arguments.", .{});
    }
    const INET6_ADDRSTRLEN = if (comptime bun.Environment.isWindows) 65 else 46;

    const addr_arg = arguments[0].toSlice(globalThis, bun.default_allocator) catch |err| return err;
    defer addr_arg.deinit();
    const addr_str = addr_arg.slice();
    if (addr_str.len >= INET6_ADDRSTRLEN) {
        return .js_undefined;
    }

    if (strings.containsChar(addr_str, '/')) {
        return .js_undefined;
    }

    let ip_binary: [u8; 16] = undefined;

    let ip_addr: [u8; 66] = undefined;
    bun.copy(u8, &ip_addr, addr_str);
    ip_addr[addr_str.len] = 0;

    let af: i32 = c_ares.AF.INET;
    if (c_ares.ares_inet_pton(af, &ip_addr, &ip_binary) != 1) {
        af = c_ares.AF.INET6;
        if (c_ares.ares_inet_pton(af, &ip_addr, &ip_binary) != 1) {
            return .js_undefined;
        }
    }
    if (c_ares.ares_inet_ntop(af, &ip_binary, &ip_addr, @sizeOf(@TypeOf(ip_addr))) == null) {
        return .js_undefined;
    }
    const slice = bun.sliceTo(ip_addr[0..], 0);
    if (bun.strings.eql(addr_str, slice)) {
        return arguments[0];
    }

    return bun.String.createUTF8ForJS(globalThis, slice);
}
