zig {
    const bun = @import("bun");
    const uws = bun.uws;
    const Socket = uws.quic.Socket;
}

export @opaque class PendingConnect {
    zig {
        extern fn us_quic_pending_connect_addrinfo(pc: *PendingConnect) *anyopaque;
        pub const addrinfo = us_quic_pending_connect_addrinfo;

        extern fn us_quic_pending_connect_resolved(pc: *PendingConnect) ?*Socket;
        pub const resolved = us_quic_pending_connect_resolved;

        extern fn us_quic_pending_connect_cancel(pc: *PendingConnect) void;
        pub const cancel = us_quic_pending_connect_cancel;
    }
}
