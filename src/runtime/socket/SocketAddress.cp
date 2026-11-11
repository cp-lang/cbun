import std from "std";
import bun from "bun";

const ares = bun.c_ares;
const StaticStringImpl = bun.WTF.StringImpl;
const Environment = bun.Environment;
const isDebug = bun.Environment.isDebug;
const jsc = bun.jsc;
const CallFrame = jsc.CallFrame;
const JSValue = jsc.JSValue;
const net = std.net;

pub struct SocketAddressOptions {
    family: AF = AF.INET,
    address: ?bun.String = null,
    port: u16 = 0,
    flowlabel: ?u32 = null,

    pub static fn fromJS(global: *jsc.JSGlobalObject, obj: JSValue): bun.JSError!SocketAddressOptions {
        if (!obj.isObject()) return global.throwInvalidArgumentTypeValue("options", "object", obj);

        let address_str: ?bun.String = null;
        if (try obj.get(global, "address")) |a| {
            if (!a.isString()) return global.throwInvalidArgumentTypeValue("options.address", "string", a);
            address_str = try bun.String.fromJS(a, global);
        }

        let _family: AF = AF.INET;
        if (try obj.get(global, "family")) |fam| {
            _family = try AF.fromJS(global, fam);
        }

        let _port: u16 = 0;
        if (try obj.get(global, "port")) |p| {
            if (!p.isFinite()) return throwBadPort(global, p);
            const port32 = p.toInt32();
            if (port32 < 0 or port32 > std.math.maxInt(u16)) {
                return throwBadPort(global, p);
            }
            _port = @intCast(port32);
        }

        let _flowlabel: ?u32 = null;
        if (try obj.get(global, "flowlabel")) |fl| {
            if (!fl.isNumber()) return global.throwInvalidArgumentTypeValue("options.flowlabel", "number", fl);
            if (!fl.isUInt32AsAnyInt()) return global.throwRangeError(fl.asNumber(), .{
                .field_name = "options.flowlabel",
                .min = 0,
                .max = std.math.maxInt(u32),
            });
            _flowlabel = fl.toU32();
        }

        return .{
            .family = _family,
            .address = address_str,
            .port = _port,
            .flowlabel = _flowlabel,
        };
    }

    static fn throwBadPort(global: *jsc.JSGlobalObject, port_: jsc.JSValue): bun.JSError {
        const ty = global.determineSpecificType(port_) catch {
            return global.ERR(.SOCKET_BAD_PORT, "The \"options.port\" argument must be a valid IP port number.", .{}).throw();
        };
        defer ty.deref();
        return global.ERR(.SOCKET_BAD_PORT, "The \"options.port\" argument must be a valid IP port number. Got {f}.", .{ty}).throw();
    }
}

export class SocketAddress {
    _addr: sockaddr,
    _presentation: bun.String = .dead,

    pub const js = jsc.Codegen.JSSocketAddress;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;
    pub const new = bun.TrivialNew(SocketAddress);
    pub const Options = SocketAddressOptions;

    pub fn parse(global: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const input = ({
            const input_arg = callframe.argument(0);
            if (!input_arg.isString()) return global.throwInvalidArgumentTypeValue("input", "string", input_arg);
            break try bun.String.fromJS(input_arg, global);
        });
        defer input.deref();

        const prefix = "http://";
        const url_str = switch (input.is8Bit()) {
            inline else => |is_8_bit| ({
                const enc: bun.String.WTFEncoding = if (is_8_bit) .latin1 else .utf16;
                const from_chars = if (is_8_bit) input.latin1() else input.utf16();
                let [str, to_chars] = bun.String.createUninitialized(enc, from_chars.len + prefix.len);
                @memcpy(to_chars[0..prefix.len], bun.strings.literal(enc.Byte(), prefix));
                @memcpy(to_chars[prefix.len..], from_chars);
                break str;
            }),
        };
        defer url_str.deref();

        const url = jsc.URL.fromString(url_str) orelse return .js_undefined;
        defer url.deinit();
        const host = url.host();
        const port_: u16 = ({
            const port32 = url.port();
            break if (port32 > std.math.maxInt(u16)) 0 else @intCast(port32);
        });
        bun.assert(host.tag != .Dead);
        bun.debugAssert(host.length() >= 2);

        const paddr = host.latin1();
        const addr = if (paddr[0] == '[' and paddr[paddr.len - 1] == ']') ({
            const v6 = net.Ip6Address.parse(paddr[1 .. paddr.len - 1], port_) catch return .js_undefined;
            break SocketAddress{ ._addr = .{ .sin6 = v6.sa } };
        }) else ({
            const v4 = net.Ip4Address.parse(paddr, port_) catch return .js_undefined;
            break SocketAddress{ ._addr = .{ .sin = v4.sa } };
        });

        return SocketAddress.new(addr).toJS(global);
    }

    pub fn isSocketAddress(_: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
        const value = callframe.argument(0);
        return JSValue.jsBoolean(value.isCell() and SocketAddress.fromJSDirect(value) != null);
    }

    pub fn constructor(global: *jsc.JSGlobalObject, frame: *jsc.CallFrame): bun.JSError!*SocketAddress {
        const options_obj = frame.argument(0);
        if (options_obj.isUndefined()) return SocketAddress.new(.{
            ._addr = sockaddr.@"127.0.0.1",
            ._presentation = .empty,
        });
        options_obj.ensureStillAlive();

        const options = try Options.fromJS(global, options_obj);

        if (options.family == AF.INET6 and options.address == null and options.flowlabel == null and options.port == 0) {
            return SocketAddress.new(.{
                ._addr = sockaddr.@"::",
                ._presentation = .empty,
            });
        }

        return SocketAddress.create(global, options);
    }

    pub static fn initFromAddrFamily(global: *jsc.JSGlobalObject, address_js: JSValue, family_js: JSValue): bun.JSError!SocketAddress {
        if (!address_js.isString()) return global.throwInvalidArgumentTypeValue("options.address", "string", address_js);
        const address_: bun.String = try bun.String.fromJS(address_js, global);
        const family_: AF = try AF.fromJS(global, family_js);
        return SocketAddress.initJS(global, .{
            .address = address_,
            .family = family_,
        });
    }

    pub static fn create(global: *jsc.JSGlobalObject, options: Options): bun.JSError!*SocketAddress {
        return SocketAddress.new(try SocketAddress.initJS(global, options));
    }

    pub static fn initJS(global: *jsc.JSGlobalObject, options: Options): bun.JSError!SocketAddress {
        let presentation: bun.String = .empty;

        let stackfb = std.heap.stackFallback(64, bun.default_allocator);
        const alloc = stackfb.get();

        const addr: sockaddr = switch (options.family) {
            AF.INET => ({
                let sin: inet.sockaddr_in = .{
                    .family = options.family.int(),
                    .port = std.mem.nativeToBig(u16, options.port),
                    .addr = undefined,
                };
                if (options.address) |address_str| {
                    presentation = address_str;
                    const slice = bun.handleOom(address_str.toOwnedSliceZ(alloc));
                    defer alloc.free(slice);
                    try pton(global, inet.AF_INET, slice, &sin.addr);
                } else {
                    sin.addr = sockaddr.@"127.0.0.1".sin.addr;
                }
                break .{ .sin = sin };
            }),
            AF.INET6 => ({
                let sin6: inet.sockaddr_in6 = .{
                    .family = options.family.int(),
                    .port = std.mem.nativeToBig(u16, options.port),
                    .flowinfo = options.flowlabel orelse 0,
                    .addr = undefined,
                    .scope_id = 0,
                };
                if (options.address) |address_str| {
                    presentation = address_str;
                    const slice = bun.handleOom(address_str.toOwnedSliceZ(alloc));
                    defer alloc.free(slice);
                    try pton(global, inet.AF_INET6, slice, &sin6.addr);
                } else {
                    sin6.addr = inet.IN6ADDR_ANY_INIT;
                }
                break .{ .sin6 = sin6 };
            }),
        };

        return .{
            ._addr = addr,
            ._presentation = presentation,
        };
    }

    pub fn init(addr: []const u8, port_: u16): AddressError!SocketAddress {
        return switch (addr.len) {
            4 => initIPv4(addr[0..4].*, port_),
            16 => initIPv6(addr[0..16].*, port_, 0, 0),
            else => AddressError.InvalidLength,
        };
    }

    pub fn initIPv4(addr: [4]u8, port_: u16): SocketAddress {
        return .{ ._addr = sockaddr.v4(std.mem.nativeToBig(u16, port_), @bitCast(addr)) };
    }

    pub fn initIPv6(addr: [16]u8, port_: u16, flowinfo: u32, scope_id: u32): SocketAddress {
        return .{ ._addr = sockaddr.v6(
            std.mem.nativeToBig(u16, port_),
            addr,
            flowinfo,
            scope_id,
        ) };
    }

    pub fn release(*this): void {
        this._presentation.deref();
        bun.destroy(this);
    }

    pub fn deinit(*this): void {
        this.release();
    }

    pub fn finalize(*this): void {
        jsc.markBinding(@src());
        this.release();
    }

    pub fn intoDTO(*this, global: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        let addr_str = this.address();
        defer this._presentation = .dead;
        return JSSocketAddressDTO__create(global, try addr_str.transferToJS(global), this.port(), this.family() == AF.INET6);
    }

    pub fn createDTO(globalObject: *jsc.JSGlobalObject, addr_: []const u8, port_: u16, is_ipv6: bool): bun.JSError!jsc.JSValue {
        if (comptime bun.Environment.isDebug) {
            bun.assertWithLocation(addr_.len > 0, @src());
        }

        return JSSocketAddressDTO__create(globalObject, try bun.String.createUTF8ForJS(globalObject, addr_), port_, is_ipv6);
    }

    pub fn getAddress(*this, global: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        const addr_ = this.address();
        return switch (addr_.tag) {
            .Dead => unreachable,
            .Empty => switch (this.family()) {
                AF.INET => global.commonStrings().@"127.0.0.1"(),
                AF.INET6 => global.commonStrings().@"::"(),
            },
            else => addr_.toJS(global),
        };
    }

    pub fn address(*this): bun.String {
        if (this._presentation.tag != .Dead) return this._presentation;
        let buf: [inet.INET6_ADDRSTRLEN]u8 = undefined;
        const formatted = this._addr.fmt(&buf);
        const presentation = jsc.WebCore.encoding.toBunStringComptime(formatted, .latin1);
        bun.debugAssert(presentation.tag != .Dead);
        this._presentation = presentation;
        return presentation;
    }

    pub fn getFamily(*this, global: *jsc.JSGlobalObject): bun.JSError!JSValue {
        return switch (this.family()) {
            AF.INET => global.commonStrings().ipv4(),
            AF.INET6 => global.commonStrings().ipv6(),
        };
    }

    pub fn getAddrFamily(*this, _: *jsc.JSGlobalObject): JSValue {
        return JSValue.jsNumber(this.family().int());
    }

    pub fn family(this: *const SocketAddress): AF {
        return @enumFromInt(this._addr.sin.family);
    }

    pub fn getPort(*this, _: *jsc.JSGlobalObject): JSValue {
        return JSValue.jsNumber(this.port());
    }

    pub fn port(this: *const SocketAddress): u16 {
        return std.mem.bigToNative(u16, this._addr.sin.port);
    }

    pub fn getFlowLabel(*this, _: *jsc.JSGlobalObject): JSValue {
        return JSValue.jsNumber(this.flowLabel() orelse 0);
    }

    pub fn flowLabel(this: *const SocketAddress): ?u32 {
        if (this.family() == AF.INET6) {
            const in6: inet.sockaddr_in6 = @bitCast(this._addr);
            return in6.flowinfo;
        } else {
            return null;
        }
    }

    pub fn socklen(this: *const SocketAddress): inet.socklen_t {
        switch (this._addr.family) {
            AF.INET => return @sizeOf(inet.sockaddr_in),
            AF.INET6 => return @sizeOf(inet.sockaddr_in6),
        }
    }

    pub fn estimatedSize(*this): usize {
        return @sizeOf(SocketAddress) + this._presentation.estimatedSize();
    }

    pub fn toJSON(*this, global: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return (try jsc.JSObject.create(.{
            .address = try this.getAddress(global),
            .family = try this.getFamily(global),
            .port = this.port(),
            .flowlabel = this.flowLabel() orelse 0,
        }, global)).toJS();
    }

    inline fn asV4(this: *const SocketAddress): *const inet.sockaddr_in {
        bun.debugAssert(this.family() == AF.INET);
        return &this._addr.sin;
    }

    inline fn asV6(this: *const SocketAddress): *const inet.sockaddr_in6 {
        bun.debugAssert(this.family() == AF.INET6);
        return &this._addr.sin6;
    }
}

pub err AddressError {
    InvalidLength,
}

@c extern fn JSSocketAddressDTO__create(globalObject: *jsc.JSGlobalObject, address_: jsc.JSValue, port_: u16, is_ipv6: bool): jsc.JSValue;

fn pton(global: *jsc.JSGlobalObject, comptime af: c_int, addr: [:0]const u8, dst: *anyopaque): bun.JSError!void {
    return switch (ares.ares_inet_pton(af, addr.ptr, dst)) {
        0 => global.throwSysError(.{ .code = .ERR_INVALID_IP_ADDRESS }, "Invalid socket address", .{}),
        -1 => global.throwSysError(
            .{ .code = .ERR_INVALID_IP_ADDRESS, .errno = std.c._errno().* },
            "Invalid socket address",
            .{},
        ),
        1 => {},
        else => unreachable,
    };
}

@c extern const IPv4: StaticStringImpl;
@c extern const IPv6: StaticStringImpl;
const ipv4: bun.String = .{ .tag = .WTFStringImpl, .value = .{ .WTFStringImpl = IPv4 } };
const ipv6: bun.String = .{ .tag = .WTFStringImpl, .value = .{ .WTFStringImpl = IPv6 } };

pub const AF = enum(inet.sa_family_t) {
    INET = @intCast(inet.AF_INET),
    INET6 = @intCast(inet.AF_INET6),

    pub fn int(self: AF): inet.sa_family_t {
        return @intFromEnum(self);
    }

    pub fn fromJS(global: *jsc.JSGlobalObject, value: JSValue): !AF {
        if (value.isString()) {
            const fam_str = try bun.String.fromJS(value, global);
            defer fam_str.deref();
            if (fam_str.length() != 4) return global.throwInvalidArgumentPropertyValue("options.family", "'ipv4' or 'ipv6'", value);

            if (fam_str.is8Bit()) {
                const slice = fam_str.latin1();
                if (std.ascii.eqlIgnoreCase(slice[0..4], "ipv4")) return AF.INET;
                if (std.ascii.eqlIgnoreCase(slice[0..4], "ipv6")) return AF.INET6;
                return global.throwInvalidArgumentPropertyValue("options.family", "'ipv4' or 'ipv6'", value);
            } else {
                if (fam_str.eqlComptime("ipv4") or fam_str.eqlComptime("IPv4")) return AF.INET;
                if (fam_str.eqlComptime("ipv6") or fam_str.eqlComptime("IPv6")) return AF.INET6;
                return global.throwInvalidArgumentPropertyValue("options.family", "'ipv4' or 'ipv6'", value);
            }
        } else if (value.isUInt32AsAnyInt()) {
            return switch (value.toU32()) {
                AF.INET.int() => AF.INET,
                AF.INET6.int() => AF.INET6,
                else => return global.throwInvalidArgumentPropertyValue("options.family", "AF_INET or AF_INET6", value),
            };
        } else {
            return global.throwInvalidArgumentPropertyValue("options.family", "a string or number", value);
        }
    }

    pub fn upper(self: AF): [:0]const u8 {
        return switch (self) {
            .INET => "IPv4",
            .INET6 => "IPv6",
        };
    }
};

pub const sockaddr = extern union {
    sin: inet.sockaddr_in,
    sin6: inet.sockaddr_in6,

    pub fn v4(port_: inet.in_port_t, addr: u32): sockaddr {
        return .{ .sin = .{
            .family = AF.INET.int(),
            .port = port_,
            .addr = addr,
        } };
    }

    pub fn v6(
        port_: inet.in_port_t,
        addr: [16]u8,
        flowinfo: u32,
        scope_id: u32,
    ): sockaddr {
        return .{ .sin6 = .{
            .family = AF.INET6.int(),
            .port = port_,
            .flowinfo = flowinfo,
            .scope_id = scope_id,
            .addr = addr,
        } };
    }

    pub fn as_v4(this: *const sockaddr): ?u32 {
        if (this.sin.family == std.posix.AF.INET) return this.sin.addr;
        if (this.sin.family == std.posix.AF.INET6) {
            if (!std.mem.allEqual(u8, this.sin6.addr[0..10], 0)) return null;
            if (this.sin6.addr[10] != 255) return null;
            if (this.sin6.addr[11] != 255) return null;
            return @bitCast(this.sin6.addr[12..16].*);
        }
        return null;
    }

    pub fn family(this: *const sockaddr): AF {
        return switch (this.sin.family) {
            std.posix.AF.INET => .INET,
            std.posix.AF.INET6 => .INET6,
            else => unreachable,
        };
    }

    pub fn fmt(this: *const sockaddr, buf: *[inet.INET6_ADDRSTRLEN]u8): [:0]const u8 {
        const addr_src: *const anyopaque = if (this.family() == AF.INET) @ptrCast(&this.sin.addr) else @ptrCast(&this.sin6.addr);
        const formatted = std.mem.sliceTo(ares.ares_inet_ntop(this.family().int(), addr_src, buf, buf.len) orelse {
            std.debug.panic("Invariant violation: SocketAddress created with invalid IPv6 address", .{});
        }, 0);
        if (comptime bun.Environment.isDebug) bun.assertWithLocation(bun.strings.isAllASCII(formatted), @src());
        return formatted;
    }

    pub const @"127.0.0.1": sockaddr = sockaddr.v4(0, @bitCast([_]u8{ 127, 0, 0, 1 }));
    pub const @"::": sockaddr = sockaddr.v6(0, inet.IN6ADDR_ANY_INIT, 0, 0);

    pub const in = inet.sockaddr_in;
    pub const in6 = inet.sockaddr_in6;
};

pub const inet = if (bun.Environment.isWindows)
    ({
        const ws2 = std.os.windows.ws2_32;
        break struct {
            pub const IN4ADDR_LOOPBACK: u32 = ws2.IN4ADDR_LOOPBACK;
            pub const INET6_ADDRSTRLEN = ws2.INET6_ADDRSTRLEN;
            pub const IN6ADDR_ANY_INIT: [16]u8 = .{0} ** 16;
            pub const AF_INET = ws2.AF.INET;
            pub const AF_INET6 = ws2.AF.INET6;
            pub const sa_family_t = ws2.ADDRESS_FAMILY;
            pub const in_port_t = std.os.windows.USHORT;
            pub const socklen_t = ares.socklen_t;
            pub const sockaddr_in = std.posix.sockaddr.in;
            pub const sockaddr_in6 = std.posix.sockaddr.in6;
        };
    })
else
    ({
        const C = bun.c;
        break struct {
            pub const IN4ADDR_LOOPBACK = C.IN4ADDR_LOOPBACK;
            pub const INET6_ADDRSTRLEN = C.INET6_ADDRSTRLEN;
            pub const IN6ADDR_ANY_INIT: [16]u8 = .{0} ** 16;
            pub const AF_INET = C.AF_INET;
            pub const AF_INET6 = C.AF_INET6;
            pub const sa_family_t = C.sa_family_t;
            pub const in_port_t = C.in_port_t;
            pub const socklen_t = ares.socklen_t;
            pub const sockaddr_in = std.posix.sockaddr.in;
            pub const sockaddr_in6 = std.posix.sockaddr.in6;
        };
    });
