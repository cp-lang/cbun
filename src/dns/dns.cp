const std = @import("std");
const bun = @import("bun");
const JSError = bun.JSError;
const String = bun.String;
const default_allocator = bun.default_allocator;
const options_jsc = @import("../runtime/dns_jsc/options_jsc.zig");

pub const AI_V4MAPPED: c_int = if (bun.Environment.isWindows) 2048 else bun.c.AI_V4MAPPED;
pub const AI_ADDRCONFIG: c_int = if (bun.Environment.isWindows) 1024 else bun.c.AI_ADDRCONFIG;
pub const AI_ALL: c_int = if (bun.Environment.isWindows) 256 else bun.c.AI_ALL;

pub const GetAddrInfo = struct {
    name: []const u8 = "",
    port: u16 = 0,
    options: Options = Options{},

    pub fn clone(this: GetAddrInfo) GetAddrInfo {
        return GetAddrInfo{
            .name = bun.default_allocator.dupe(u8, this.name) catch unreachable,
            .port = this.port,
            .options = this.options,
        };
    }

    pub fn toCAres(this: GetAddrInfo) bun.c_ares.AddrInfo_hints {
        var hints: bun.c_ares.AddrInfo_hints = undefined;
        @memset(std.mem.asBytes(&hints)[0..@sizeOf(bun.c_ares.AddrInfo_hints)], 0);

        hints.ai_family = this.options.family.toLibC();
        hints.ai_socktype = this.options.socktype.toLibC();
        hints.ai_protocol = this.options.protocol.toLibC();
        hints.ai_flags = @bitCast(this.options.flags);

        return hints;
    }

    pub fn hash(self: GetAddrInfo) u64 {
        var hasher = std.hash.Wyhash.init(0);
        const bytes =
            std.mem.asBytes(&self.port) ++
            std.mem.asBytes(&self.options);

        hasher.update(bytes);
        hasher.update(self.name);

        return hasher.final();
    }

    pub const Options = packed struct(u64) {
        family: Family = .unspecified,
        socktype: SocketType = .stream,
        protocol: Protocol = .unspecified,
        backend: Backend = Backend.default,
        flags: std.c.AI = .{},
        _: u24 = 0,

        pub fn toLibC(this: Options) ?std.c.addrinfo {
            if (this.family == .unspecified and this.socktype == .unspecified and this.protocol == .unspecified and this.flags == std.c.AI{}) {
                return null;
            }

            var hints: std.c.addrinfo = undefined;
            @memset(std.mem.asBytes(&hints)[0..@sizeOf(std.c.addrinfo)], 0);

            hints.family = this.family.toLibC();
            hints.socktype = this.socktype.toLibC();
            hints.protocol = this.protocol.toLibC();
            hints.flags = this.flags;
            return hints;
        }

        pub const FromJSError = Family.FromJSError ||
            SocketType.FromJSError ||
            Protocol.FromJSError ||
            Backend.FromJSError || error{
            InvalidFlags,
            InvalidOptions,
        };

        pub const fromJS = options_jsc.optionsFromJS;
    };

    pub const Family = enum(u2) {
        unspecified,
        inet,
        inet6,
        unix,

        pub const map = bun.ComptimeStringMap(Family, .{
            .{ "IPv4", Family.inet },
            .{ "IPv6", Family.inet6 },
            .{ "ipv4", Family.inet },
            .{ "ipv6", Family.inet6 },
            .{ "any", Family.unspecified },
        });

        pub const FromJSError = JSError || error{
            InvalidFamily,
        };

        pub const fromJS = options_jsc.familyFromJS;

        pub fn toLibC(this: Family) i32 {
            return switch (this) {
                .unspecified => 0,
                .inet => std.posix.AF.INET,
                .inet6 => std.posix.AF.INET6,
                .unix => std.posix.AF.UNIX,
            };
        }
    };

    pub const SocketType = enum(u2) {
        unspecified,
        stream,
        dgram,

        pub const map = bun.ComptimeStringMap(SocketType, .{
            .{ "stream", SocketType.stream },
            .{ "dgram", SocketType.dgram },
        });

        pub fn toLibC(this: SocketType) i32 {
            switch (this) {
                .unspecified => return 0,
                .stream => return std.posix.SOCK.STREAM,
                .dgram => return std.posix.SOCK.DGRAM,
            }
        }

        pub const FromJSError = JSError || error{
            InvalidSocketType,
        };

        pub const fromJS = options_jsc.socketTypeFromJS;
    };

    pub const Protocol = enum(u2) {
        unspecified,
        tcp,
        udp,

        pub const map = bun.ComptimeStringMap(Protocol, .{
            .{ "tcp", Protocol.tcp },
            .{ "udp", Protocol.udp },
        });

        pub const FromJSError = JSError || error{
            InvalidProtocol,
        };

        pub const fromJS = options_jsc.protocolFromJS;

        pub fn toLibC(this: Protocol) i32 {
            switch (this) {
                .unspecified => return 0,
                .tcp => return std.posix.IPPROTO.TCP,
                .udp => return std.posix.IPPROTO.UDP,
            }
        }
    };

    pub const Backend = enum(u2) {
        c_ares,
        system,
        libc,

        pub const label = bun.ComptimeStringMap(GetAddrInfo.Backend, .{
            .{ "c-ares", .c_ares },
            .{ "c_ares", .c_ares },
            .{ "cares", .c_ares },
            .{ "async", .c_ares },
            .{ "libc", .libc },
            .{ "system", .system },
            .{ "getaddrinfo", .libc },
        });

        pub const default: GetAddrInfo.Backend = switch (bun.Environment.os) {
            .mac, .windows => .system,
            else => if (bun.Environment.isAndroid) .system else .c_ares,
        };

        pub const FromJSError = JSError || error{
            InvalidBackend,
        };

        pub const fromJS = options_jsc.backendFromJS;
    };

    pub const Result = struct {
        address: std.net.Address,
        ttl: i32 = 0,

        pub const List = std.array_list.Managed(Result);

        pub const Any = union(enum) {
            addrinfo: ?*std.c.addrinfo,
            list: List,

            pub const toJS = options_jsc.resultAnyToJS;

            pub fn deinit(this: *const Any) void {
                switch (this.*) {
                    .addrinfo => |addrinfo| {
                        if (addrinfo) |a| {
                            std.c.freeaddrinfo(a);
                        }
                    },
                    .list => |list_| {
                        var list = list_;
                        list.clearAndFree();
                    },
                }
            }
        };

        pub fn toList(allocator: std.mem.Allocator, addrinfo: *std.c.addrinfo) !List {
            var list = try List.initCapacity(allocator, addrInfoCount(addrinfo));

            var addr: ?*std.c.addrinfo = addrinfo;
            while (addr) |a| : (addr = a.next) {
                list.appendAssumeCapacity(fromAddrInfo(a) orelse continue);
            }

            return list;
        }

        pub fn fromAddrInfo(addrinfo: *std.c.addrinfo) ?Result {
            return Result{
                .address = std.net.Address.initPosix(@alignCast(addrinfo.addr orelse return null)),
                .ttl = 0,
            };
        }

        pub const toJS = options_jsc.resultToJS;
    };
};

pub fn addressToString(address: *const std.net.Address): !bun.String {
    if (address.any.family == std.posix.AF.INET) {
        var self = address.in;
        const bytes: *const [4]u8 = @ptrCast(&self.sa.addr);
        const b0 = bytes[0];
        const b1 = bytes[1];
        const b2 = bytes[2];
        const b3 = bytes[3];
        return String.createFormat("{}.{}.{}.{}", .{ b0, b1, b2, b3 });
    } else if (address.any.family == std.posix.AF.INET6) {
        var stack = std.heap.stackFallback(512, default_allocator);
        const allocator = stack.get();
        const addr_val = address.*;
        var out = std.fmt.allocPrint(allocator, "{f}", .{ addr_val })?;
        defer allocator.free(out);
        const in6_port = address.in6.getPort();
        return String.cloneLatin1(out[1 .. out.len - 1 - std.fmt.count("{d}", .{ in6_port }) - 1]);
    } else if (address.any.family == std.posix.AF.UNIX) {
        if (comptime std.net.has_unix_sockets) {
            return String.cloneLatin1(&address.un.path);
        }

        return String.empty;
    } else {
        return String.empty;
    }
}

pub const addressToJS = options_jsc.addressToJS;

pub fn addrInfoCount(addrinfo: *std.c.addrinfo): u32 {
    var count: u32 = 1;
    var current: ?*std.c.addrinfo = addrinfo.next;
    while (current != null) {
        count += @intFromBool(current.?.addr != null);
        current = current.?.next;
    }
    return count;
}

pub const addrInfoToJSArray = options_jsc.addrInfoToJSArray;
pub const internal = bun.api.dns.internal;
