// c-ares DNS resolver bindings in CP language

zig {
    const std = @import("std");
    const iovec = @import("std").os.iovec;
    const bun = @import("bun");
    const jsc = bun.jsc;
    const strings = bun.strings;
    const libuv = bun.windows.libuv;
    const c = @import("std").c;
    const ares_socklen_t = c.socklen_t;
    const fd_set = c.fd_set;
    const struct_sockaddr = std.posix.sockaddr;
    const struct_in_addr = std.posix.sockaddr.in;
    pub const socklen_t = c.socklen_t;
    pub const ares_ssize_t = isize;
    pub const ares_socket_t = if (bun.Environment.isWindows) std.os.windows.ws2_32.SOCKET else i32;
    pub const hostent_int = if (bun.Environment.isWindows) i16 else i32;
    pub const ares_sock_state_cb = ?*const fn (?*anyopaque, ares_socket_t, i32, i32) callconv(.c) void;
    pub const AF = std.posix.AF;
}

pub @opaque class struct_apattern {}

pub const ARES_SOCKET_BAD: ares_socket_t = @if (bun.Environment.isWindows) std.os.windows.ws2_32.INVALID_SOCKET else -@as(i32, 1);

pub enum NSClass : i32 {
    ns_c_invalid = 0,
    ns_c_in = 1,
    ns_c_2 = 2,
    ns_c_chaos = 3,
    ns_c_hs = 4,
    ns_c_none = 254,
    ns_c_any = 255,
    ns_c_max = 65536,
}

pub enum NSType : i32 {
    ns_t_invalid = 0,
    ns_t_a = 1,
    ns_t_ns = 2,
    ns_t_md = 3,
    ns_t_mf = 4,
    ns_t_cname = 5,
    ns_t_soa = 6,
    ns_t_mb = 7,
    ns_t_mg = 8,
    ns_t_mr = 9,
    ns_t_null = 10,
    ns_t_wks = 11,
    ns_t_ptr = 12,
    ns_t_hinfo = 13,
    ns_t_minfo = 14,
    ns_t_mx = 15,
    ns_t_txt = 16,
    ns_t_rp = 17,
    ns_t_afsdb = 18,
    ns_t_x25 = 19,
    ns_t_isdn = 20,
    ns_t_rt = 21,
    ns_t_nsap = 22,
    ns_t_nsap_ptr = 23,
    ns_t_sig = 24,
    ns_t_key = 25,
    ns_t_px = 26,
    ns_t_gpos = 27,
    ns_t_aaaa = 28,
    ns_t_loc = 29,
    ns_t_nxt = 30,
    ns_t_eid = 31,
    ns_t_nimloc = 32,
    ns_t_srv = 33,
    ns_t_atma = 34,
    ns_t_naptr = 35,
    ns_t_kx = 36,
    ns_t_cert = 37,
    ns_t_a6 = 38,
    ns_t_dname = 39,
    ns_t_sink = 40,
    ns_t_opt = 41,
    ns_t_apl = 42,
    ns_t_ds = 43,
    ns_t_sshfp = 44,
    ns_t_rrsig = 46,
    ns_t_nsec = 47,
    ns_t_dnskey = 48,
    ns_t_tkey = 249,
    ns_t_tsig = 250,
    ns_t_ixfr = 251,
    ns_t_axfr = 252,
    ns_t_mailb = 253,
    ns_t_maila = 254,
    ns_t_any = 255,
    ns_t_uri = 256,
    ns_t_caa = 257,
    ns_t_max = 65536,
}

pub @c struct struct_ares_server_failover_options {
    retry_chance: u16 = 0,
    retry_delay: usize = 0,
}

const ARES_EVSYS_DEFAULT: i32 = 0;
const ARES_EVSYS_WIN32: i32 = 1;
const ARES_EVSYS_EPOLL: i32 = 2;
const ARES_EVSYS_KQUEUE: i32 = 3;
const ARES_EVSYS_POLL: i32 = 4;
const ARES_EVSYS_SELECT: i32 = 5;
pub type ares_evsys_t = u32;

pub @c struct Options {
    flags: i32 = 0,
    timeout: i32 = 0,
    tries: i32 = 0,
    ndots: i32 = 0,
    udp_port: u16 = 0,
    tcp_port: u16 = 0,
    socket_send_buffer_size: i32 = 0,
    socket_receive_buffer_size: i32 = 0,
    servers: [*c]struct_in_addr = null,
    nservers: i32 = 0,
    domains: [*]([*:0]u8!)?! = null,
    ndomains: i32 = 0,
    lookups: [*:0]u8?! = null,
    sock_state_cb: ares_sock_state_cb = null,
    sock_state_cb_data: *any?! = null,
    sortlist: *struct_apattern?! = null,
    nsort: i32 = 0,
    ednspsz: i32 = 0,
    resolvconf_path: [*:0]u8?! = null,
    hosts_path: [*:0]u8?! = null,
    udp_max_queries: i32 = 0,
    maxtimeout: i32 = 0,
    qcache_max_ttl: u32 = 0,
    evsys: ares_evsys_t = 0,
    server_failover_opts: struct_ares_server_failover_options = std.mem.zeroes(struct_ares_server_failover_options),
}

pub @c struct struct_hostent {
    h_name: [*:0]u8?! = null,
    h_aliases: [*:null]([*:0]u8?!)?! = null,
    h_addrtype: hostent_int = 0,
    h_length: hostent_int = 0,
    h_addr_list: [*:null]([*:0]u8?!)?! = null,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").hostentToJSResponse;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_hostent) void;
        }

        pub fn hostCallbackWrapper(
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_host_callback {
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, hostent: ?*struct_hostent) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, hostent);
                }
            }.handle;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var start: [*c]struct_hostent = undefined;
                    if (comptime strings.eqlComptime(_lookup, "cname")) {
                        var addrttls: [256]struct_ares_addrttl = undefined;
                        var naddrttls: i32 = 256;

                        const result = ares_parse_a_reply(buffer, buffer_length, &start, &addrttls, &naddrttls);
                        if (result != ARES_SUCCESS) {
                            function(this, Error.get(result), timeouts, null);
                            return;
                        }
                        function(this, null, timeouts, start);
                    } else if (comptime strings.eqlComptime(_lookup, "ns")) {
                        const result = ares_parse_ns_reply(buffer, buffer_length, &start);
                        if (result != ARES_SUCCESS) {
                            function(this, Error.get(result), timeouts, null);
                            return;
                        }
                        function(this, null, timeouts, start);
                    } else if (comptime strings.eqlComptime(_lookup, "ptr")) {
                        const result = ares_parse_ptr_reply(buffer, buffer_length, null, 0, AF.INET, &start);
                        if (result != ARES_SUCCESS) {
                            function(this, Error.get(result), timeouts, null);
                            return;
                        }
                        function(this, null, timeouts, start);
                    } else {
                        @compileError(std.fmt.comptimePrint("Unsupported struct_hostent record type: {s}", .{_lookup}));
                    }
                }
            }.handle;
        }
    }

    pub fn release(*_): void {
        ares_free_hostent(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub class hostent_with_ttls {
    hostent: *struct_hostent,
    ttls: [256]i32 = [-1; 256],

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").hostentWithTtlsToJSResponse;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*hostent_with_ttls) void;
        }

        pub fn hostCallbackWrapper(
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_host_callback {
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, hostent: ?*hostent_with_ttls) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, hostent);
                }
            }.handle;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    switch (parse(_lookup, buffer, buffer_length)) {
                        .result => |result| function(this, null, timeouts, result),
                        .err => |err| function(this, err, timeouts, null),
                    }
                }
            }.handle;
        }

        pub fn parse(comptime _lookup: []const u8, buffer: [*c]u8, buffer_length: c_int) jsc.Node.Maybe(*hostent_with_ttls, Error) {
            var start: ?*struct_hostent = null;

            if (comptime strings.eqlComptime(_lookup, "a")) {
                var addrttls: [256]struct_ares_addrttl = undefined;
                var naddrttls: c_int = 256;

                const result = ares_parse_a_reply(buffer, buffer_length, &start, &addrttls, &naddrttls);
                if (result != ARES_SUCCESS) {
                    return .{ .err = Error.get(result).? };
                }
                var with_ttls = bun.handleOom(bun.default_allocator.create(hostent_with_ttls));
                with_ttls.hostent = start.?;
                for (addrttls[0..@intCast(naddrttls)], 0..) |ttl, i| {
                    with_ttls.ttls[i] = ttl.ttl;
                }
                return .{ .result = with_ttls };
            }

            if (comptime strings.eqlComptime(_lookup, "aaaa")) {
                var addr6ttls: [256]struct_ares_addr6ttl = undefined;
                var naddr6ttls: c_int = 256;

                const result = ares_parse_aaaa_reply(buffer, buffer_length, &start, &addr6ttls, &naddr6ttls);
                if (result != ARES_SUCCESS) {
                    return .{ .err = Error.get(result).? };
                }
                var with_ttls = bun.handleOom(bun.default_allocator.create(hostent_with_ttls));
                with_ttls.hostent = start.?;
                for (addr6ttls[0..@intCast(naddr6ttls)], 0..) |ttl, i| {
                    with_ttls.ttls[i] = ttl.ttl;
                }
                return .{ .result = with_ttls };
            }

            @compileError(std.fmt.comptimePrint("Unsupported hostent_with_ttls record type: {s}", .{_lookup}));
        }
    }

    pub fn release(*_): void {
        _.hostent.release();
        bun.default_allocator.destroy(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_nameinfo {
    node: [*c]u8,
    service: [*c]u8,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").nameinfoToJSResponse;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, node: ?struct_nameinfo) void;
        }

        pub fn CallbackWrapper(
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_nameinfo_callback {
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, node: [*c]u8, service: [*c]u8) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, .{ .node = node, .service = service });
                    return;
                }
            }.handle;
        }
    }
}

pub type struct_timeval = std.posix.timeval;
pub @opaque class struct_Channeldata {}

pub @c struct AddrInfo_cname {
    ttl: i32,
    alias: [*c]u8,
    name: [*c]u8,
    next: [*c]AddrInfo_cname,
}

pub @c struct AddrInfo_node {
    ttl: i32 = 0,
    flags: i32 = 0,
    family: i32 = 0,
    socktype: i32 = 0,
    protocol: i32 = 0,
    addrlen: ares_socklen_t,
    addr: *struct_sockaddr?! = null,
    next: *AddrInfo_node?! = null,

    fn count(*_): u32 {
        let len: u32 = 0;
        let mut node: *AddrInfo_node?! = _;
        while (node != null) {
            len += 1;
            node = node?.next;
        }
        return len;
    }
}

pub @c struct AddrInfo {
    cnames_: [*c]AddrInfo_cname = null,
    node: *AddrInfo_node?! = null,
    name_: [*:0]u8?! = null,

    zig {
        pub const toJSArray = @import("../runtime/dns_jsc/cares_jsc.zig").addrInfoToJSArray;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*AddrInfo) void;
        }

        pub fn callbackWrapper(
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_addrinfo_callback {
            return &struct {
                pub fn handleAddrInfo(ctx: ?*anyopaque, status: c_int, timeouts: c_int, addr_info: ?*AddrInfo) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    function(this, Error.get(status), timeouts, addr_info);
                }
            }.handleAddrInfo;
        }
    }

    fn name(_): string {
        let name_ = _.name_ ? return "";
        return bun.span(name_);
    }

    fn cnames(_): AddrInfo_node[] {
        let cnames_ = _.cnames_ ? return [];
        return bun.span(cnames_);
    }

    pub fn release(*_): void {
        ares_freeaddrinfo(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct AddrInfo_hints {
    ai_flags: i32 = 0,
    ai_family: i32 = 0,
    ai_socktype: i32 = 0,
    ai_protocol: i32 = 0,

    fn isEmpty(_): bool {
        return _.ai_flags == 0 && _.ai_family == 0 && _.ai_socktype == 0 && _.ai_protocol == 0;
    }
}

pub class ChannelOptions {
    timeout: i32? = null,
    tries: i32? = null,
}

zig {
    var ares_has_loaded = std.atomic.Value(bool).init(false);
    fn libraryInit() void {
        if (ares_has_loaded.swap(true, .monotonic))
            return;

        const rc = ares_library_init_mem(
            ARES_LIB_INIT_ALL,
            bun.mimalloc.mi_malloc,
            bun.mimalloc.mi_free,
            bun.mimalloc.mi_realloc,
        );
        if (rc != ARES_SUCCESS) {
            std.debug.panic("ares_library_init_mem failed: {d}", .{rc});
            unreachable;
        }
    }
}

pub @opaque class Channel {
    pub fn release(*_): void {
        ares_destroy(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }

    @inline pub fn process(*_, fd: ares_socket_t, readable: bool, writable: bool): void {
        let mut r_fd: ares_socket_t = ARES_SOCKET_BAD;
        if (readable) {
            r_fd = fd;
        }
        let mut w_fd: ares_socket_t = ARES_SOCKET_BAD;
        if (writable) {
            w_fd = fd;
        }
        ares_process_fd(_, r_fd, w_fd);
    }

    zig {
        pub fn init(comptime Container: type, this: *Container, options: ChannelOptions) ?Error {
            var channel: *Channel = undefined;

            libraryInit();

            const SockStateWrap = struct {
                pub fn onSockState(ctx: ?*anyopaque, socket: ares_socket_t, readable: c_int, writable: c_int) callconv(.c) void {
                    const container = bun.cast(*Container, ctx.?);
                    Container.onDNSSocketState(container, socket, readable != 0, writable != 0);
                }
            };

            var opts = bun.zero(Options);

            opts.flags = ARES_FLAG_NOCHECKRESP;
            opts.sock_state_cb = &SockStateWrap.onSockState;
            opts.sock_state_cb_data = @as(*anyopaque, @ptrCast(this));
            opts.timeout = options.timeout orelse -1;
            opts.tries = options.tries orelse 4;

            const optmask: c_int =
                ARES_OPT_FLAGS | ARES_OPT_TIMEOUTMS |
                ARES_OPT_SOCK_STATE_CB | ARES_OPT_TRIES;

            if (Error.get(ares_init_options(&channel, &opts, optmask))) |err| {
                ares_library_cleanup();
                return err;
            }

            this.channel = channel;
            return null;
        }

        pub fn getAddrInfo(this: *Channel, host: []const u8, port: u16, hints: []const AddrInfo_hints, comptime Type: type, ctx: *Type, comptime callback: AddrInfo.Callback(Type)) void {
            var host_buf: [1024]u8 = undefined;
            var port_buf: [52]u8 = undefined;
            const host_ptr: ?[*:0]const u8 = brk: {
                const len = @min(host.len, host_buf.len - 1);
                @memcpy(host_buf[0..len], host[0..len]);
                host_buf[len] = 0;
                break :brk host_buf[0..len :0].ptr;
            };

            const port_ptr: ?[*:0]const u8 = brk: {
                if (port == 0) {
                    break :brk null;
                }

                break :brk (std.fmt.bufPrintZ(&port_buf, "{d}", .{port}) catch unreachable).ptr;
            };

            var hints_buf: [3]AddrInfo_hints = bun.zero([3]AddrInfo_hints);
            for (hints[0..@min(hints.len, 2)], 0..) |hint, i| {
                hints_buf[i] = hint;
            }
            const hints_: [*c]const AddrInfo_hints = if (hints.len > 0) &hints_buf else null;
            ares_getaddrinfo(this, host_ptr, port_ptr, hints_, AddrInfo.callbackWrapper(Type, callback), ctx);
        }

        pub fn resolve(this: *Channel, name: []const u8, comptime lookup_name: []const u8, comptime Type: type, ctx: *Type, comptime cares_type: type, comptime callback: cares_type.Callback(Type)) void {
            if (name.len >= 1023 or (name.len == 0 and !(bun.strings.eqlComptime(lookup_name, "ns") or bun.strings.eqlComptime(lookup_name, "soa")))) {
                return cares_type.callbackWrapper(lookup_name, Type, callback).?(ctx, ARES_EBADNAME, 0, null, 0);
            }

            var name_buf: [1024]u8 = undefined;
            const name_ptr: [*:0]const u8 = brk: {
                const len = @min(name.len, name_buf.len - 1);
                @memcpy(name_buf[0..len], name[0..len]);

                name_buf[len] = 0;
                break :brk name_buf[0..len :0];
            };

            const field_name = comptime std.fmt.comptimePrint("ns_t_{s}", .{lookup_name});
            ares_query(this, name_ptr, NSClass.ns_c_in, @field(NSType, field_name), cares_type.callbackWrapper(lookup_name, Type, callback), ctx);
        }

        pub fn getHostByAddr(this: *Channel, ip_addr: []const u8, comptime Type: type, ctx: *Type, comptime callback: struct_hostent.Callback(Type)) void {
            const buf_size = 46;
            var addr_buf: [buf_size]u8 = undefined;
            const addr_ptr: ?[*:0]const u8 = brk: {
                if (ip_addr.len == 0 or ip_addr.len >= buf_size) {
                    break :brk null;
                }
                const len = @min(ip_addr.len, addr_buf.len - 1);
                @memcpy(addr_buf[0..len], ip_addr[0..len]);

                addr_buf[len] = 0;
                break :brk addr_buf[0..len :0];
            };

            var addr: [16]u8 = undefined;
            if (addr_ptr != null) {
                if (ares_inet_pton(AF.INET, addr_ptr, &addr) > 0) {
                    ares_gethostbyaddr(this, &addr, 4, AF.INET, struct_hostent.hostCallbackWrapper(Type, callback), ctx);
                    return;
                } else if (ares_inet_pton(AF.INET6, addr_ptr, &addr) > 0) {
                    return ares_gethostbyaddr(this, &addr, 16, AF.INET6, struct_hostent.hostCallbackWrapper(Type, callback), ctx);
                }
            }
            struct_hostent.hostCallbackWrapper(Type, callback).?(ctx, ARES_ENOTIMP, 0, null);
        }

        pub fn getNameInfo(this: *Channel, sa: *std.posix.sockaddr, comptime Type: type, ctx: *Type, comptime callback: struct_nameinfo.Callback(Type)) void {
            return ares_getnameinfo(
                this,
                sa,
                if (sa.*.family == AF.INET) @sizeOf(std.posix.sockaddr.in) else @sizeOf(std.posix.sockaddr.in6),
                ARES_NI_NAMEREQD | ARES_NI_LOOKUPHOST | ARES_NI_LOOKUPSERVICE,
                struct_nameinfo.CallbackWrapper(Type, callback),
                ctx,
            );
        }
    }
}

pub type ares_callback = (@c fn (*any?!, i32, i32, [*c]u8, i32): void)?;
pub type ares_host_callback = (@c fn (*any?!, i32, i32, *struct_hostent?!): void)?;
pub type ares_nameinfo_callback = (@c fn (*any?!, i32, i32, [*c]u8, [*c]u8): void)?;
pub type ares_sock_create_callback = (@c fn (ares_socket_t, i32, *any?!): i32)?;
pub type ares_sock_config_callback = (@c fn (ares_socket_t, i32, *any?!): i32)?;
pub type ares_addrinfo_callback = @c fn (*any?!, i32, i32, *AddrInfo?!): void;

pub @c struct ares_socket_functions {
    socket: (@c fn (i32, i32, i32, *any?!): ares_socket_t)? = null,
    close: (@c fn (ares_socket_t, *any?!): i32)? = null,
    connect: (@c fn (ares_socket_t, [*c]struct_sockaddr, ares_socklen_t, *any?!): i32)? = null,
    recvfrom: (@c fn (ares_socket_t, *any?!, usize, i32, [*c]struct_sockaddr, [*c]ares_socklen_t, *any?!): ares_ssize_t)? = null,
    sendv: (@c fn (ares_socket_t, [*c]iovec, i32, *any?!): ares_ssize_t)? = null,
}

zig {
    pub const union_unnamed_2 = extern union {
        _S6_u8: [16]u8,
    };
}

pub @c struct struct_ares_in6_addr {
    _S6_un: union_unnamed_2,
}

pub @c struct struct_ares_addrttl {
    ipaddr: u32,
    ttl: i32,
}

pub @c struct struct_ares_addr6ttl {
    ip6addr: struct_ares_in6_addr,
    ttl: i32,
}

pub @c struct struct_ares_caa_reply {
    next: *struct_ares_caa_reply?!,
    critical: i32,
    property: [*c]u8,
    plength: usize,
    value: [*c]u8,
    length: usize,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").caaReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").caaReplyToJS;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_ares_caa_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var start: [*c]struct_ares_caa_reply = undefined;
                    const result = ares_parse_caa_reply(buffer, buffer_length, &start);
                    if (result != ARES_SUCCESS) {
                        function(this, Error.get(result), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, start);
                }
            }.handle;
        }
    }

    pub fn release(*_): void {
        ares_free_data(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_ares_srv_reply {
    next: *struct_ares_srv_reply?!,
    host: [*c]u8,
    priority: u16,
    weight: u16,
    port: u16,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").srvReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").srvReplyToJS;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_ares_srv_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handleSrv(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var srv_start: [*c]struct_ares_srv_reply = undefined;
                    const result = ares_parse_srv_reply(buffer, buffer_length, &srv_start);
                    if (result != ARES_SUCCESS) {
                        function(this, Error.get(result), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, srv_start);
                }
            }.handleSrv;
        }
    }

    pub fn release(*_): void {
        ares_free_data(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_ares_mx_reply {
    next: *struct_ares_mx_reply?!,
    host: [*c]u8,
    priority: u16,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").mxReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").mxReplyToJS;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_ares_mx_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handle(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var start: [*c]struct_ares_mx_reply = undefined;
                    const result = ares_parse_mx_reply(buffer, buffer_length, &start);
                    if (result != ARES_SUCCESS) {
                        function(this, Error.get(result), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, start);
                }
            }.handle;
        }
    }

    pub fn release(*_): void {
        ares_free_data(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_ares_txt_reply {
    next: *struct_ares_txt_reply?!,
    txt: [*c]u8,
    length: usize,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").txtReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").txtReplyToJS;
        pub const toJSForAny = @import("../runtime/dns_jsc/cares_jsc.zig").txtReplyToJSForAny;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_ares_txt_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handleTxt(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var srv_start: [*c]struct_ares_txt_reply = undefined;
                    const result = ares_parse_txt_reply(buffer, buffer_length, &srv_start);
                    if (result != ARES_SUCCESS) {
                        function(this, Error.get(result), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, srv_start);
                }
            }.handleTxt;
        }
    }

    pub fn release(*_): void {
        ares_free_data(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_ares_txt_ext {
    next: [*c]struct_ares_txt_ext,
    txt: [*c]u8,
    length: usize,
    record_start: u8,
}

pub @c struct struct_ares_naptr_reply {
    next: *struct_ares_naptr_reply?!,
    flags: [*c]u8,
    service: [*c]u8,
    regexp: [*c]u8,
    replacement: [*c]u8,
    order: u16,
    preference: u16,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").naptrReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").naptrReplyToJS;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_ares_naptr_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handleNaptr(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var naptr_start: [*c]struct_ares_naptr_reply = undefined;
                    const result = ares_parse_naptr_reply(buffer, buffer_length, &naptr_start);
                    if (result != ARES_SUCCESS) {
                        function(this, Error.get(result), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, naptr_start);
                }
            }.handleNaptr;
        }
    }

    pub fn release(*_): void {
        ares_free_data(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_ares_soa_reply {
    nsname: [*c]u8,
    hostmaster: [*c]u8,
    serial: u32,
    refresh: u32,
    retry: u32,
    expire: u32,
    minttl: u32,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").soaReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").soaReplyToJS;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_ares_soa_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handleSoa(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var soa_start: [*c]struct_ares_soa_reply = undefined;
                    const result = ares_parse_soa_reply(buffer, buffer_length, &soa_start);
                    if (result != ARES_SUCCESS) {
                        function(this, Error.get(result), timeouts, null);
                        return;
                    }
                    function(this, null, timeouts, soa_start);
                }
            }.handleSoa;
        }
    }

    pub fn release(*_): void {
        ares_free_data(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

pub @c struct struct_ares_uri_reply {
    next: [*c]struct_ares_uri_reply,
    priority: u16,
    weight: u16,
    uri: [*c]u8,
    ttl: i32,
}

pub class struct_any_reply {
    a_reply: *hostent_with_ttls?! = null,
    aaaa_reply: *hostent_with_ttls?! = null,
    mx_reply: *struct_ares_mx_reply?! = null,
    ns_reply: *struct_hostent?! = null,
    txt_reply: *struct_ares_txt_reply?! = null,
    srv_reply: *struct_ares_srv_reply?! = null,
    ptr_reply: *struct_hostent?! = null,
    naptr_reply: *struct_ares_naptr_reply?! = null,
    soa_reply: *struct_ares_soa_reply?! = null,
    caa_reply: *struct_ares_caa_reply?! = null,

    zig {
        pub const toJSResponse = @import("../runtime/dns_jsc/cares_jsc.zig").anyReplyToJSResponse;
        pub const toJS = @import("../runtime/dns_jsc/cares_jsc.zig").anyReplyToJS;

        pub fn Callback(comptime Type: type) type {
            return fn (*Type, status: ?Error, timeouts: i32, results: ?*struct_any_reply) void;
        }

        pub fn callbackWrapper(
            comptime _lookup: []const u8,
            comptime Type: type,
            comptime function: Callback(Type),
        ) ares_callback {
            _ = _lookup;
            return &struct {
                pub fn handleAny(ctx: ?*anyopaque, status: c_int, timeouts: c_int, buffer: [*c]u8, buffer_length: c_int) callconv(.c) void {
                    const this = bun.cast(*Type, ctx.?);
                    if (status != ARES_SUCCESS) {
                        function(this, Error.get(status), timeouts, null);
                        return;
                    }

                    var any_success = false;
                    var last_error: ?c_int = null;
                    var reply = bun.handleOom(bun.default_allocator.create(struct_any_reply));
                    reply.* = .{};

                    switch (hostent_with_ttls.parse("a", buffer, buffer_length)) {
                        .result => |result| {
                            reply.a_reply = result;
                            any_success = true;
                        },
                        .err => |err| last_error = @intFromEnum(err),
                    }

                    switch (hostent_with_ttls.parse("aaaa", buffer, buffer_length)) {
                        .result => |result| {
                            reply.aaaa_reply = result;
                            any_success = true;
                        },
                        .err => |err| last_error = @intFromEnum(err),
                    }

                    var result = ares_parse_mx_reply(buffer, buffer_length, &reply.mx_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_ns_reply(buffer, buffer_length, &reply.ns_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_txt_reply(buffer, buffer_length, &reply.txt_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_srv_reply(buffer, buffer_length, &reply.srv_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_ptr_reply(buffer, buffer_length, null, 0, AF.INET, &reply.ptr_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_naptr_reply(buffer, buffer_length, &reply.naptr_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_soa_reply(buffer, buffer_length, &reply.soa_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    result = ares_parse_caa_reply(buffer, buffer_length, &reply.caa_reply);
                    if (result == ARES_SUCCESS) {
                        any_success = true;
                    } else {
                        last_error = result;
                    }

                    if (!any_success) {
                        reply.release();
                        function(this, Error.get(last_error.?), timeouts, null);
                        return;
                    }

                    function(this, null, timeouts, reply);
                }
            }.handleAny;
        }
    }

    pub fn release(*_): void {
        _.a_reply?.release();
        _.aaaa_reply?.release();
        _.mx_reply?.release();
        _.ns_reply?.release();
        _.txt_reply?.release();
        _.srv_reply?.release();
        _.ptr_reply?.release();
        _.naptr_reply?.release();
        _.soa_reply?.release();
        _.caa_reply?.release();
        bun.default_allocator.destroy(_);
    }

    pub fn deinit(*_): void {
        _.release();
    }
}

zig {
    pub const union_unnamed_3 = extern union {
        addr4: struct_in_addr,
        addr6: struct_ares_in6_addr,
    };
}

pub @c struct struct_ares_addr_node {
    next: *struct_ares_addr_node?!,
    family: i32,
    addr: union_unnamed_3,
}

zig {
    pub const union_unnamed_4 = extern union {
        addr4: struct_in_addr,
        addr6: struct_ares_in6_addr,
    };
}

pub @c struct struct_ares_addr_port_node {
    next: *struct_ares_addr_port_node?!,
    family: i32,
    addr: union_unnamed_4,
    udp_port: i32,
    tcp_port: i32,
}

@c {
    fn ares_library_init(flags: i32): i32;
    fn ares_library_init_mem(flags: i32, amalloc: (@c fn (usize): *any?!)?, afree: (@c fn (*any?!): void)?, arealloc: (@c fn (*any?!, usize): *any?!)?): i32;
    fn ares_library_initialized(): i32;
    fn ares_library_cleanup(): void;
    fn ares_version(version: [*c]i32): [*c]const u8;
    fn ares_init(channelptr: **Channel!): i32;
    fn ares_init_options(channelptr: **Channel!, options: *Options?!, optmask: i32): i32;
    fn ares_save_options(channel: *Channel, options: *Options?!, optmask: *i32): i32;
    fn ares_destroy_options(options: *Options): void;
    fn ares_dup(dest: *Channel?!, src: *Channel): i32;
    fn ares_destroy(channel: *Channel): void;
    fn ares_cancel(channel: *Channel): void;
    fn ares_set_local_ip4(channel: *Channel, local_ip: u32): void;
    fn ares_set_local_ip6(channel: *Channel, local_ip6: [*c]const u8): void;
    fn ares_set_local_dev(channel: *Channel, local_dev_name: [*c]const u8): void;
    fn ares_set_socket_callback(channel: *Channel, callback: ares_sock_create_callback, user_data: *any?!): void;
    fn ares_set_socket_configure_callback(channel: *Channel, callback: ares_sock_config_callback, user_data: *any?!): void;
    fn ares_set_sortlist(channel: *Channel, sortstr: [*c]const u8): i32;
    fn ares_getaddrinfo(channel: *Channel, node: [*:0]const u8?!, service: [*:0]const u8?!, hints: [*c]const AddrInfo_hints, callback: ares_addrinfo_callback, arg: *any?!): void;
    fn ares_freeaddrinfo(ai: *AddrInfo): void;
    fn ares_set_socket_functions(channel: *Channel, funcs: *const ares_socket_functions?!, user_data: *any?!): void;
    fn ares_send(channel: *Channel, qbuf: [*c]const u8, qlen: i32, callback: ares_callback, arg: *any?!): void;
    fn ares_query(channel: *Channel, name: [*c]const u8, dnsclass: NSClass, @"type": NSType, callback: ares_callback, arg: *any?!): void;
    fn ares_search(channel: *Channel, name: [*c]const u8, dnsclass: i32, @"type": i32, callback: ares_callback, arg: *any?!): void;
    fn ares_gethostbyname(channel: *Channel, name: [*c]const u8, family: i32, callback: ares_host_callback, arg: *any?!): void;
    fn ares_gethostbyname_file(channel: *Channel, name: [*c]const u8, family: i32, host: [*:null](*struct_hostent?!)!): i32;
    fn ares_gethostbyaddr(channel: *Channel, addr: *const any?!, addrlen: i32, family: i32, callback: ares_host_callback, arg: *any?!): void;
    fn ares_getnameinfo(channel: *Channel, sa: [*c]const struct_sockaddr, salen: ares_socklen_t, flags: i32, callback: ares_nameinfo_callback, arg: *any?!): void;
    fn ares_getsock(channel: *Channel, socks: [*c]ares_socket_t, numsocks: i32): i32;
    fn ares_timeout(channel: *Channel, maxtv: *struct_timeval?!, tv: *struct_timeval?!): *struct_timeval?!;
    fn ares_process_fd(channel: *Channel, read_fd: ares_socket_t, write_fd: ares_socket_t): void;
    fn ares_create_query(name: [*c]const u8, dnsclass: i32, @"type": i32, id: u16, rd: i32, buf: [*c][*c]u8, buflen: [*c]i32, max_udp_size: i32): i32;
    fn ares_mkquery(name: [*c]const u8, dnsclass: i32, @"type": i32, id: u16, rd: i32, buf: [*c][*c]u8, buflen: [*c]i32): i32;
    fn ares_expand_name(encoded: [*c]const u8, abuf: [*c]const u8, alen: i32, s: [*c][*c]u8, enclen: [*c]isize): i32;
    fn ares_expand_string(encoded: [*c]const u8, abuf: [*c]const u8, alen: i32, s: [*c][*c]u8, enclen: [*c]isize): i32;
    fn ares_queue_active_queries(channel: *const Channel): usize;

    fn ares_parse_a_reply(abuf: [*c]const u8, alen: i32, host: [*c](*struct_hostent?!), addrttls: [*c]struct_ares_addrttl, naddrttls: [*c]i32): i32;
    fn ares_parse_aaaa_reply(abuf: [*c]const u8, alen: i32, host: [*c](*struct_hostent?!), addrttls: [*c]struct_ares_addr6ttl, naddrttls: [*c]i32): i32;
    fn ares_parse_caa_reply(abuf: [*c]const u8, alen: i32, caa_out: [*c][*c]struct_ares_caa_reply): i32;
    fn ares_parse_ptr_reply(abuf: [*c]const u8, alen: i32, addr: *const any?!, addrlen: i32, family: i32, host: [*c](*struct_hostent?!)): i32;
    fn ares_parse_ns_reply(abuf: [*c]const u8, alen: i32, host: [*c](*struct_hostent?!)): i32;
    fn ares_parse_srv_reply(abuf: [*c]const u8, alen: i32, srv_out: [*c][*c]struct_ares_srv_reply): i32;
    fn ares_parse_mx_reply(abuf: [*c]const u8, alen: i32, mx_out: [*c][*c]struct_ares_mx_reply): i32;
    fn ares_parse_txt_reply(abuf: [*c]const u8, alen: i32, txt_out: [*c][*c]struct_ares_txt_reply): i32;
    fn ares_parse_txt_reply_ext(abuf: [*c]const u8, alen: i32, txt_out: [*c][*c]struct_ares_txt_ext): i32;
    fn ares_parse_naptr_reply(abuf: [*c]const u8, alen: i32, naptr_out: [*c][*c]struct_ares_naptr_reply): i32;
    fn ares_parse_soa_reply(abuf: [*c]const u8, alen: i32, soa_out: [*c][*c]struct_ares_soa_reply): i32;
    fn ares_parse_uri_reply(abuf: [*c]const u8, alen: i32, uri_out: [*c][*c]struct_ares_uri_reply): i32;
    fn ares_free_string(str: *any?!): void;
    fn ares_free_hostent(host: *struct_hostent?!): void;
    fn ares_free_data(dataptr: *any?!): void;
    fn ares_strerror(code: i32): [*c]const u8;

    fn ares_set_servers(channel: *Channel, servers: [*c]struct_ares_addr_node): i32;
    fn ares_set_servers_ports(channel: *Channel, servers: [*c]struct_ares_addr_port_node): i32;
    fn ares_set_servers_csv(channel: *Channel, servers: [*c]const u8): i32;
    fn ares_set_servers_ports_csv(channel: *Channel, servers: [*c]const u8): i32;
    fn ares_get_servers(channel: *Channel, servers: *(*struct_ares_addr_port_node?!)): i32;
    fn ares_get_servers_ports(channel: *Channel, servers: *(*struct_ares_addr_port_node?!)): i32;
    fn ares_inet_ntop(af: i32, src: *const any?!, dst: [*]u8!, size: ares_socklen_t): [*:0]u8?!;
    fn ares_inet_pton(af: i32, src: [*c]const u8, dst: *any?!): i32;
}

pub const ARES_SUCCESS = 0;
pub const ARES_ENODATA = 1;
pub const ARES_EFORMERR = 2;
pub const ARES_ESERVFAIL = 3;
pub const ARES_ENOTFOUND = 4;
pub const ARES_ENOTIMP = 5;
pub const ARES_EREFUSED = 6;
pub const ARES_EBADQUERY = 7;
pub const ARES_EBADNAME = 8;
pub const ARES_EBADFAMILY = 9;
pub const ARES_EBADRESP = 10;
pub const ARES_ECONNREFUSED = 11;
pub const ARES_ETIMEOUT = 12;
pub const ARES_EOF = 13;
pub const ARES_EFILE = 14;
pub const ARES_ENOMEM = 15;
pub const ARES_EDESTRUCTION = 16;
pub const ARES_EBADSTR = 17;
pub const ARES_EBADFLAGS = 18;
pub const ARES_ENONAME = 19;
pub const ARES_EBADHINTS = 20;
pub const ARES_ENOTINITIALIZED = 21;
pub const ARES_ELOADIPHLPAPI = 22;
pub const ARES_EADDRGETNETWORKPARAMS = 23;
pub const ARES_ECANCELLED = 24;
pub const ARES_ESERVICE = 25;
pub const ARES_ENOSERVER = 26;

pub enum Error : i32 {
    ENODATA = ARES_ENODATA,
    EFORMERR = ARES_EFORMERR,
    ESERVFAIL = ARES_ESERVFAIL,
    ENOTFOUND = ARES_ENOTFOUND,
    ENOTIMP = ARES_ENOTIMP,
    EREFUSED = ARES_EREFUSED,
    EBADQUERY = ARES_EBADQUERY,
    EBADNAME = ARES_EBADNAME,
    EBADFAMILY = ARES_EBADFAMILY,
    EBADRESP = ARES_EBADRESP,
    ECONNREFUSED = ARES_ECONNREFUSED,
    ETIMEOUT = ARES_ETIMEOUT,
    EOF = ARES_EOF,
    EFILE = ARES_EFILE,
    ENOMEM = ARES_ENOMEM,
    EDESTRUCTION = ARES_EDESTRUCTION,
    EBADSTR = ARES_EBADSTR,
    EBADFLAGS = ARES_EBADFLAGS,
    ENONAME = ARES_ENONAME,
    EBADHINTS = ARES_EBADHINTS,
    ENOTINITIALIZED = ARES_ENOTINITIALIZED,
    ELOADIPHLPAPI = ARES_ELOADIPHLPAPI,
    EADDRGETNETWORKPARAMS = ARES_EADDRGETNETWORKPARAMS,
    ECANCELLED = ARES_ECANCELLED,
    ESERVICE = ARES_ESERVICE,
    ENOSERVER = ARES_ENOSERVER,

    zig {
        pub const Deferred = @import("../runtime/dns_jsc/cares_jsc.zig").ErrorDeferred;
        pub const toDeferred = @import("../runtime/dns_jsc/cares_jsc.zig").errorToDeferred;
        pub const toJSWithSyscall = @import("../runtime/dns_jsc/cares_jsc.zig").errorToJSWithSyscall;
        pub const toJSWithSyscallAndHostname = @import("../runtime/dns_jsc/cares_jsc.zig").errorToJSWithSyscallAndHostname;

        pub fn initEAI(rc: i32) ?Error {
            if (comptime bun.Environment.isWindows) {
                if (rc == libuv.UV_EAI_NODATA or rc == libuv.UV_EAI_NONAME) {
                    return Error.ENOTFOUND;
                }

                return switch (rc) {
                    0 => null,
                    libuv.UV_EAI_AGAIN => Error.ETIMEOUT,
                    libuv.UV_EAI_ADDRFAMILY => Error.EBADFAMILY,
                    libuv.UV_EAI_BADFLAGS => Error.EBADFLAGS,
                    libuv.UV_EAI_BADHINTS => Error.EBADHINTS,
                    libuv.UV_EAI_CANCELED => Error.ECANCELLED,
                    libuv.UV_EAI_FAIL => Error.ENOTFOUND,
                    libuv.UV_EAI_FAMILY => Error.EBADFAMILY,
                    libuv.UV_EAI_MEMORY => Error.ENOMEM,
                    libuv.UV_EAI_NODATA => Error.ENODATA,
                    libuv.UV_EAI_NONAME => Error.ENONAME,
                    libuv.UV_EAI_OVERFLOW => Error.ENOMEM,
                    libuv.UV_EAI_PROTOCOL => Error.EBADQUERY,
                    libuv.UV_EAI_SERVICE => Error.ESERVICE,
                    libuv.UV_EAI_SOCKTYPE => Error.ECONNREFUSED,
                    else => Error.ENOTFOUND,
                };
            }

            const eai: std.posix.system.EAI = @enumFromInt(rc);

            if (eai == .NODATA or eai == .NONAME) {
                return Error.ENOTFOUND;
            }

            if (comptime bun.Environment.isLinux) {
                if (eai == .SOCKTYPE) return Error.ECONNREFUSED;
            }
            if (comptime bun.Environment.isGlibc) {
                switch (eai) {
                    .IDN_ENCODE => return Error.EBADSTR,
                    .ALLDONE => return Error.ENOTFOUND,
                    .INPROGRESS => return Error.ETIMEOUT,
                    .CANCELED => return Error.ECANCELLED,
                    .NOTCANCELED => return Error.ECANCELLED,
                    else => {},
                }
            }

            return switch (eai) {
                @as(std.posix.system.EAI, @enumFromInt(0)) => return null,
                .ADDRFAMILY => Error.EBADFAMILY,
                .BADFLAGS => Error.EBADFLAGS,
                .FAIL => Error.EBADRESP,
                .FAMILY => Error.EBADFAMILY,
                .MEMORY => Error.ENOMEM,
                .SERVICE => Error.ESERVICE,
                .SYSTEM => Error.ESERVFAIL,
                else => bun.todo(@src(), Error.ENOTIMP),
            };
        }

        pub const code = bun.enumMap(Error, .{
            .{ .ENODATA, "DNS_ENODATA" },
            .{ .EFORMERR, "DNS_EFORMERR" },
            .{ .ESERVFAIL, "DNS_ESERVFAIL" },
            .{ .ENOTFOUND, "DNS_ENOTFOUND" },
            .{ .ENOTIMP, "DNS_ENOTIMP" },
            .{ .EREFUSED, "DNS_EREFUSED" },
            .{ .EBADQUERY, "DNS_EBADQUERY" },
            .{ .EBADNAME, "DNS_ENOTFOUND" },
            .{ .EBADFAMILY, "DNS_EBADFAMILY" },
            .{ .EBADRESP, "DNS_EBADRESP" },
            .{ .ECONNREFUSED, "DNS_ECONNREFUSED" },
            .{ .ETIMEOUT, "DNS_ETIMEOUT" },
            .{ .EOF, "DNS_EOF" },
            .{ .EFILE, "DNS_EFILE" },
            .{ .ENOMEM, "DNS_ENOMEM" },
            .{ .EDESTRUCTION, "DNS_EDESTRUCTION" },
            .{ .EBADSTR, "DNS_EBADSTR" },
            .{ .EBADFLAGS, "DNS_EBADFLAGS" },
            .{ .ENONAME, "DNS_ENOTFOUND" },
            .{ .EBADHINTS, "DNS_EBADHINTS" },
            .{ .ENOTINITIALIZED, "DNS_ENOTINITIALIZED" },
            .{ .ELOADIPHLPAPI, "DNS_ELOADIPHLPAPI" },
            .{ .EADDRGETNETWORKPARAMS, "DNS_EADDRGETNETWORKPARAMS" },
            .{ .ECANCELLED, "DNS_ECANCELLED" },
            .{ .ESERVICE, "DNS_ESERVICE" },
            .{ .ENOSERVER, "DNS_ENOSERVER" },
        });

        pub const label = bun.enumMap(Error, .{
            .{ .ENODATA, "No data record of requested type" },
            .{ .EFORMERR, "Malformed DNS query" },
            .{ .ESERVFAIL, "Server failed to complete the DNS operation" },
            .{ .ENOTFOUND, "Domain name not found" },
            .{ .ENOTIMP, "DNS resolver does not implement requested operation" },
            .{ .EREFUSED, "DNS operation refused" },
            .{ .EBADQUERY, "Misformatted DNS query" },
            .{ .EBADNAME, "Misformatted domain name" },
            .{ .EBADFAMILY, "Misformatted DNS query (family)" },
            .{ .EBADRESP, "Misformatted DNS reply" },
            .{ .ECONNREFUSED, "Could not contact DNS servers" },
            .{ .ETIMEOUT, "Timeout while contacting DNS servers" },
            .{ .EOF, "End of file" },
            .{ .EFILE, "Error reading file" },
            .{ .ENOMEM, "Out of memory" },
            .{ .EDESTRUCTION, "Channel is being destroyed" },
            .{ .EBADSTR, "Misformatted string" },
            .{ .EBADFLAGS, "Illegal flags specified" },
            .{ .ENONAME, "Given hostname is not numeric" },
            .{ .EBADHINTS, "Illegal hints flags specified" },
            .{ .ENOTINITIALIZED, "Library initialization not yet performed" },
            .{ .ELOADIPHLPAPI, "ELOADIPHLPAPI TODO WHAT DOES THIS MEAN" },
            .{ .EADDRGETNETWORKPARAMS, "EADDRGETNETWORKPARAMS" },
            .{ .ECANCELLED, "DNS query cancelled" },
            .{ .ESERVICE, "Service not available" },
            .{ .ENOSERVER, "No DNS servers were configured" },
        });

        pub fn get(rc: i32) ?Error {
            if (rc == ARES_ENODATA or rc == ARES_ENONAME) {
                return get(ARES_ENOTFOUND);
            }

            return switch (rc) {
                0 => null,
                1...ARES_ENOSERVER => @as(Error, @enumFromInt(rc)),
                -ARES_ENOSERVER...-1 => @as(Error, @enumFromInt(-rc)),
                else => unreachable,
            };
        }
    }
}

pub const ARES_FLAG_USEVC = @as(i32, 1) << @as(i32, 0);
pub const ARES_FLAG_PRIMARY = @as(i32, 1) << @as(i32, 1);
pub const ARES_FLAG_IGNTC = @as(i32, 1) << @as(i32, 2);
pub const ARES_FLAG_NORECURSE = @as(i32, 1) << @as(i32, 3);
pub const ARES_FLAG_STAYOPEN = @as(i32, 1) << @as(i32, 4);
pub const ARES_FLAG_NOSEARCH = @as(i32, 1) << @as(i32, 5);
pub const ARES_FLAG_NOALIASES = @as(i32, 1) << @as(i32, 6);
pub const ARES_FLAG_NOCHECKRESP = @as(i32, 1) << @as(i32, 7);
pub const ARES_FLAG_NO_DFLT_SVR = @as(i32, 1) << @as(i32, 9);
pub const ARES_FLAG_EDNS = @as(i32, 1) << @as(i32, 8);
pub const ARES_OPT_FLAGS = @as(i32, 1) << @as(i32, 0);
pub const ARES_OPT_TIMEOUT = @as(i32, 1) << @as(i32, 1);
pub const ARES_OPT_TRIES = @as(i32, 1) << @as(i32, 2);
pub const ARES_OPT_NDOTS = @as(i32, 1) << @as(i32, 3);
pub const ARES_OPT_UDP_PORT = @as(i32, 1) << @as(i32, 4);
pub const ARES_OPT_TCP_PORT = @as(i32, 1) << @as(i32, 5);
pub const ARES_OPT_SERVERS = @as(i32, 1) << @as(i32, 6);
pub const ARES_OPT_DOMAINS = @as(i32, 1) << @as(i32, 7);
pub const ARES_OPT_LOOKUPS = @as(i32, 1) << @as(i32, 8);
pub const ARES_OPT_SOCK_STATE_CB = @as(i32, 1) << @as(i32, 9);
pub const ARES_OPT_SORTLIST = @as(i32, 1) << @as(i32, 10);
pub const ARES_OPT_SOCK_SNDBUF = @as(i32, 1) << @as(i32, 11);
pub const ARES_OPT_SOCK_RCVBUF = @as(i32, 1) << @as(i32, 12);
pub const ARES_OPT_TIMEOUTMS = @as(i32, 1) << @as(i32, 13);
pub const ARES_OPT_ROTATE = @as(i32, 1) << @as(i32, 14);
pub const ARES_OPT_EDNSPSZ = @as(i32, 1) << @as(i32, 15);
pub const ARES_OPT_NOROTATE = @as(i32, 1) << @as(i32, 16);
pub const ARES_OPT_RESOLVCONF = @as(i32, 1) << @as(i32, 17);
pub const ARES_OPT_HOSTS_FILE = @as(i32, 1) << @as(i32, 18);
pub const ARES_NI_NOFQDN = @as(i32, 1) << @as(i32, 0);
pub const ARES_NI_NUMERICHOST = @as(i32, 1) << @as(i32, 1);
pub const ARES_NI_NAMEREQD = @as(i32, 1) << @as(i32, 2);
pub const ARES_NI_NUMERICSERV = @as(i32, 1) << @as(i32, 3);
pub const ARES_NI_DGRAM = @as(i32, 1) << @as(i32, 4);
pub const ARES_NI_TCP = @as(i32, 0);
pub const ARES_NI_UDP = ARES_NI_DGRAM;
pub const ARES_NI_SCTP = @as(i32, 1) << @as(i32, 5);
pub const ARES_NI_DCCP = @as(i32, 1) << @as(i32, 6);
pub const ARES_NI_NUMERICSCOPE = @as(i32, 1) << @as(i32, 7);
pub const ARES_NI_LOOKUPHOST = @as(i32, 1) << @as(i32, 8);
pub const ARES_NI_LOOKUPSERVICE = @as(i32, 1) << @as(i32, 9);
pub const ARES_NI_IDN = @as(i32, 1) << @as(i32, 10);
pub const ARES_NI_IDN_ALLOW_UNASSIGNED = @as(i32, 1) << @as(i32, 11);
pub const ARES_NI_IDN_USE_STD3_ASCII_RULES = @as(i32, 1) << @as(i32, 12);
pub const ARES_AI_CANONNAME = @as(i32, 1) << @as(i32, 0);
pub const ARES_AI_NUMERICHOST = @as(i32, 1) << @as(i32, 1);
pub const ARES_AI_PASSIVE = @as(i32, 1) << @as(i32, 2);
pub const ARES_AI_NUMERICSERV = @as(i32, 1) << @as(i32, 3);
pub const ARES_AI_V4MAPPED = @as(i32, 1) << @as(i32, 4);
pub const ARES_AI_ALL = @as(i32, 1) << @as(i32, 5);
pub const ARES_AI_ADDRCONFIG = @as(i32, 1) << @as(i32, 6);
pub const ARES_AI_NOSORT = @as(i32, 1) << @as(i32, 7);
pub const ARES_AI_ENVHOSTS = @as(i32, 1) << @as(i32, 8);
pub const ARES_AI_IDN = @as(i32, 1) << @as(i32, 10);
pub const ARES_AI_IDN_ALLOW_UNASSIGNED = @as(i32, 1) << @as(i32, 11);
pub const ARES_AI_IDN_USE_STD3_ASCII_RULES = @as(i32, 1) << @as(i32, 12);
pub const ARES_AI_CANONIDN = @as(i32, 1) << @as(i32, 13);
pub const ARES_AI_MASK = (((((ARES_AI_CANONNAME | ARES_AI_NUMERICHOST) | ARES_AI_PASSIVE) | ARES_AI_NUMERICSERV) | ARES_AI_V4MAPPED) | ARES_AI_ALL) | ARES_AI_ADDRCONFIG;
pub const ARES_GETSOCK_MAXNUM = @as(i32, 16);

@inline pub fn ARES_GETSOCK_READABLE(bits: auto, num: auto): i32 {
    return bits & (@as(i32, 1) << num);
}

@inline pub fn ARES_GETSOCK_WRITABLE(bits: auto, num: auto): i32 {
    return bits & (@as(i32, 1) << (num + ARES_GETSOCK_MAXNUM));
}

pub const ARES_LIB_INIT_NONE = @as(i32, 0);
pub const ARES_LIB_INIT_WIN32 = @as(i32, 1) << @as(i32, 0);
pub const ARES_LIB_INIT_ALL = ARES_LIB_INIT_WIN32;
pub const ares_socket_typedef = "";
pub const ares_addrinfo_cname = AddrInfo_cname;
pub const ares_addrinfo_node = AddrInfo_node;
pub const ares_addrinfo = AddrInfo;
pub const ares_addrinfo_hints = AddrInfo_hints;
pub const ares_in6_addr = struct_ares_in6_addr;
pub const ares_addrttl = struct_ares_addrttl;
pub const ares_addr6ttl = struct_ares_addr6ttl;
pub const ares_caa_reply = struct_ares_caa_reply;
pub const ares_srv_reply = struct_ares_srv_reply;
pub const ares_mx_reply = struct_ares_mx_reply;
pub const ares_txt_reply = struct_ares_txt_reply;
pub const ares_txt_ext = struct_ares_txt_ext;
pub const ares_naptr_reply = struct_ares_naptr_reply;
pub const ares_soa_reply = struct_ares_soa_reply;
pub const ares_uri_reply = struct_ares_uri_reply;
pub const ares_addr_node = struct_ares_addr_node;
pub const ares_addr_port_node = struct_ares_addr_port_node;

pub fn getSockaddr(addr: string, port: u16, sa: *struct_sockaddr): i32 {
    const buf_size: usize = 128;
    if (addr.length == 0 || addr.length >= buf_size) {
        return -1;
    }
    let mut buf: [128]u8 = [0; 128];
    let len = @min(addr.length, buf_size - 1);
    @memcpy(buf[0..len], addr[0..len]);
    buf[len] = 0;
    const addr_ptr: [*:0]const u8 = (&buf) as [*:0]const u8;

    let in4 = @ptrCast(@alignCast(sa)) as *struct_in_addr;
    if (ares_inet_pton(AF.INET, addr_ptr, &in4.addr) == 1) {
        in4.family = AF.INET;
        in4.port = std.mem.nativeToBig(u16, port);
        return 0;
    }

    let in6 = @ptrCast(@alignCast(sa)) as *std.posix.sockaddr.in6;
    if (ares_inet_pton(AF.INET6, addr_ptr, &in6.addr) == 1) {
        in6.family = AF.INET6;
        in6.port = std.mem.nativeToBig(u16, port);
        return 0;
    }

    return -1;
}
