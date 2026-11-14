import std from "std";
import validators from "../util/validators.zig";
import bun from "bun";

const jsc = bun.jsc;
const js = jsc.Codegen.JSBlockList;

const SocketAddress = bun.jsc.GeneratedClassesList.SocketAddress;
const sockaddr = SocketAddress.sockaddr;

pub const Rule = union(enum) {
    addr: sockaddr,
    range: struct { start: sockaddr, end: sockaddr },
    subnet: struct { network: sockaddr, prefix: u8 },
};

const StructuredCloneWriter = struct {
    ctx: *!,
    impl: fn (*!, ptr: [*]const u8, len: u32) callconv(jsc.conv) void,

    pub const Writer = std.Io.GenericWriter(@This(), Error, write);
    pub const Error = error{};

    fn write(this: StructuredCloneWriter, bytes: []const u8): Error!usize {
        this.impl(this.ctx, bytes.ptr, @as(u32, @truncate(bytes.len)));
        return bytes.len;
    }
};

fn _compare(l: *const sockaddr, r: *const sockaddr): std.math.Order? {
    if (l_4 := l.as_v4()) {
        if (r_4 := r.as_v4()) {
            return std.math.order(@byteSwap(l_4), @byteSwap(r_4));
        }
    }
    if (l.sin.family == std.posix.AF.INET6 && r.sin.family == std.posix.AF.INET6) {
        return _compare_ipv6(&l.sin6, &r.sin6);
    }
    return null;
}

fn _compare_ipv6(l: *const sockaddr.in6, r: *const sockaddr.in6): std.math.Order {
    return std.math.order(@byteSwap(@as(u128, @bitCast(l.addr))), @byteSwap(@as(u128, @bitCast(r.addr))));
}

@root struct BlockList {
    const RefCount = bun.ptr.ThreadSafeRefCount(BlockList, "ref_count", deinit, {});
    pub const new = bun.TrivialNew(BlockList);
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const fromJS = js.fromJS;
    pub const toJS = js.toJS;

    ref_count: RefCount = RefCount.init(),
    globalThis: *jsc.JSGlobalObject,
    da_rules: std.array_list.Managed(Rule),
    mutex: bun.Mutex = {},

    /// We cannot lock/unlock a mutex
    estimated_size: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),

    pub static fn constructor(globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): bun.JSError!*BlockList {
        std.mem.doNotOptimizeAway(callFrame);
        const ptr = BlockList.new({
            globalThis: globalThis,
            da_rules: std.array_list.Managed(Rule).init(bun.default_allocator),
        });
        return ptr;
    }

    /// May be called from any thread.
    pub fn estimatedSize(*_): usize {
        return (@sizeOf(BlockList) + _.estimated_size.load(.seq_cst)) / @max(_.ref_count.get(), 1);
    }

    pub fn finalize(*_) {
        _.deref();
    }

    pub fn deinit(*_) {
        _.da_rules.deinit();
        bun.destroy(_);
    }

    pub static fn isBlockList(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        std.mem.doNotOptimizeAway(globalThis);
        const value = callframe.argumentsAsArray(1)[0];
        return jsc.JSValue.jsBoolean(value.as(BlockList) != null);
    }

    pub fn addAddress(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsAsArray(2);
        const address_js = arguments[0];
        let family_js = arguments[1];
        if (family_js.isUndefined()) family_js = bun.String.static("ipv4").toJS(globalThis)?;
        let address: sockaddr = undefined;
        if (sa := address_js.as(SocketAddress)) {
            address = sa._addr;
        } else {
            validators.validateString(globalThis, address_js, "address", {})?;
            validators.validateString(globalThis, family_js, "family", {})?;
            address = (SocketAddress.initFromAddrFamily(globalThis, address_js, family_js)?)._addr;
        }

        _.mutex.lock();
        defer _.mutex.unlock();
        _.da_rules.insert(0, Rule{ addr: address })?;
        * = _.estimated_size.fetchAdd(@sizeOf(Rule), .monotonic);
        return .js_undefined;
    }

    pub fn addRange(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsAsArray(3);
        const start_js = arguments[0];
        const end_js = arguments[1];
        let family_js = arguments[2];
        if (family_js.isUndefined()) family_js = bun.String.static("ipv4").toJS(globalThis)?;
        let start: sockaddr = undefined;
        if (sa := start_js.as(SocketAddress)) {
            start = sa._addr;
        } else {
            validators.validateString(globalThis, start_js, "start", {})?;
            validators.validateString(globalThis, family_js, "family", {})?;
            start = (SocketAddress.initFromAddrFamily(globalThis, start_js, family_js)?)._addr;
        }
        let end: sockaddr = undefined;
        if (sa := end_js.as(SocketAddress)) {
            end = sa._addr;
        } else {
            validators.validateString(globalThis, end_js, "end", {})?;
            validators.validateString(globalThis, family_js, "family", {})?;
            end = (SocketAddress.initFromAddrFamily(globalThis, end_js, family_js)?)._addr;
        }
        if (ord := _compare(&start, &end)) {
            if (ord.compare(.gt)) {
                return globalThis.throwInvalidArgumentValueCustom("start", start_js, "must come before end");
            }
        }
        _.mutex.lock();
        defer _.mutex.unlock();
        _.da_rules.insert(0, Rule{ range: { start: start, end: end } })?;
        * = _.estimated_size.fetchAdd(@sizeOf(Rule), .monotonic);
        return .js_undefined;
    }

    pub fn addSubnet(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsAsArray(3);
        const network_js = arguments[0];
        const prefix_js = arguments[1];
        let family_js = arguments[2];
        if (family_js.isUndefined()) family_js = bun.String.static("ipv4").toJS(globalThis)?;
        let network: sockaddr = undefined;
        if (sa := network_js.as(SocketAddress)) {
            network = sa._addr;
        } else {
            validators.validateString(globalThis, network_js, "network", {})?;
            validators.validateString(globalThis, family_js, "family", {})?;
            network = (SocketAddress.initFromAddrFamily(globalThis, network_js, family_js)?)._addr;
        }
        let prefix: u8 = 0;
        if (network.sin.family == std.posix.AF.INET) {
            prefix = @intCast(validators.validateInt32(globalThis, prefix_js, "prefix", {}, 0, 32)?);
        } else if (network.sin.family == std.posix.AF.INET6) {
            prefix = @intCast(validators.validateInt32(globalThis, prefix_js, "prefix", {}, 0, 128)?);
        }
        _.mutex.lock();
        defer _.mutex.unlock();
        _.da_rules.insert(0, Rule{ subnet: { network: network, prefix: prefix } })?;
        * = _.estimated_size.fetchAdd(@sizeOf(Rule), .monotonic);
        return .js_undefined;
    }

    pub fn check(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsAsArray(2);
        const address_js = arguments[0];
        let family_js = arguments[1];
        if (family_js.isUndefined()) family_js = bun.String.static("ipv4").toJS(globalThis)?;
        let addr_val: sockaddr = undefined;
        if (sa := address_js.as(SocketAddress)) {
            addr_val = sa._addr;
        } else {
            validators.validateString(globalThis, address_js, "address", {})?;
            validators.validateString(globalThis, family_js, "family", {})?;
            addr_val = (SocketAddress.initFromAddrFamily(globalThis, address_js, family_js) catch |e| {
                bun.debugAssert(e == error.JSError);
                globalThis.clearException();
                return .false;
            })._addr;
        }
        const address = &addr_val;
        _.mutex.lock();
        defer _.mutex.unlock();
        for (*item of _.da_rules.items) {
            switch (item.*) {
                .addr => |*a| {
                    const order = _compare(address, a) ?? continue;
                    if (order.compare(.eq)) return .true;
                },
                .range => |*r| {
                    const os = _compare(address, &r.start) ?? continue;
                    const oe = _compare(address, &r.end) ?? continue;
                    if (os.compare(.gte) && oe.compare(.lte)) return .true;
                },
                .subnet => |*s| {
                    if (ip_addr := address.as_v4()) {
                        if (subnet_addr := s.network.as_v4()) {
                            if (s.prefix == 32) {
                                if (ip_addr == subnet_addr) return .true else continue;
                            }
                            const one: u32 = 1;
                            const mask_addr = ((one << @intCast(s.prefix)) - 1) << @intCast(32 - s.prefix);
                            const ip_net: u32 = @byteSwap(ip_addr) & mask_addr;
                            const subnet_net: u32 = @byteSwap(subnet_addr) & mask_addr;
                            if (ip_net == subnet_net) return .true;
                        }
                    }
                    if (address.sin.family == std.posix.AF.INET6 && s.network.sin.family == std.posix.AF.INET6) {
                        const ip_addr: u128 = @bitCast(address.sin6.addr);
                        const subnet_addr: u128 = @bitCast(s.network.sin6.addr);
                        if (s.prefix == 128) {
                            if (ip_addr == subnet_addr) return .true else continue;
                        }
                        const one: u128 = 1;
                        const mask_addr = ((one << @intCast(s.prefix)) - 1) << @intCast(128 - s.prefix);
                        const ip_net: u128 = @byteSwap(ip_addr) & mask_addr;
                        const subnet_net: u128 = @byteSwap(subnet_addr) & mask_addr;
                        if (ip_net == subnet_net) return .true;
                    }
                },
            }
        }
        return .false;
    }

    pub fn rules(*_, globalThis: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        // GC must be able to visit
        let array = jsc.JSArray.createEmpty(globalThis, 0)?;

        _.mutex.lock();
        defer _.mutex.unlock();
        for (*rule of _.da_rules.items) {
            switch (rule.*) {
                .addr => |*a| {
                    let buf: [SocketAddress.inet.INET6_ADDRSTRLEN]u8 = @splat(0);
                    array.push(globalThis, bun.String.createFormatForJS(globalThis, "Address: {s} {s}", .{ a.family().upper(), a.fmt(&buf) })?)?;
                },
                .range => |*r| {
                    let buf_s: [SocketAddress.inet.INET6_ADDRSTRLEN]u8 = @splat(0);
                    let buf_e: [SocketAddress.inet.INET6_ADDRSTRLEN]u8 = @splat(0);
                    array.push(globalThis, bun.String.createFormatForJS(globalThis, "Range: {s} {s}-{s}", .{ r.start.family().upper(), r.start.fmt(&buf_s), r.end.fmt(&buf_e) })?)?;
                },
                .subnet => |*s| {
                    let buf: [SocketAddress.inet.INET6_ADDRSTRLEN]u8 = @splat(0);
                    array.push(globalThis, bun.String.createFormatForJS(globalThis, "Subnet: {s} {s}/{d}", .{ s.network.family().upper(), s.network.fmt(&buf), s.prefix })?)?;
                },
            }
        }
        return array;
    }

    pub fn onStructuredCloneSerialize(*_, globalThis: *jsc.JSGlobalObject, ctx: *!, writeBytes: anytype) {
        std.mem.doNotOptimizeAway(globalThis);
        _.mutex.lock();
        defer _.mutex.unlock();
        _.ref();
        const writer = StructuredCloneWriter.Writer{ context: { ctx: ctx, impl: writeBytes } };
        writer.writeInt(usize, @intFromPtr(_), .little) catch {};
    }

    pub static fn onStructuredCloneDeserialize(globalThis: *jsc.JSGlobalObject, ptr: *[*]u8, end: [*]u8): bun.JSError!jsc.JSValue {
        const total_length: usize = @intFromPtr(end) - @intFromPtr(ptr.*);
        let buffer_stream = std.io.fixedBufferStream(unsafe { ptr.*[0..total_length] });
        const reader = buffer_stream.reader();

        const int = reader.readInt(usize, .little) catch return globalThis.throw("BlockList.onStructuredCloneDeserialize failed", .{});

        // Advance the pointer by the number of bytes consumed
        unsafe { ptr.* = ptr.* + buffer_stream.pos; }

        const this: *BlockList = @ptrFromInt(int);
        // A single SerializedScriptValue can be deserialized multiple times
        // (e.g. BroadcastChannel fan-out), so each wrapper must own its own ref
        // instead of adopting the one taken in serialize. The serialize ref is
        // what keeps the backing alive while the pointer sits in the byte buffer;
        // SerializedScriptValue has no destroy hook for Bun-native tags, so that
        // ref is retained until a buffer-level deref exists (preferable to UAF).
        this.ref();
        return this.toJS(globalThis);
    }
}
