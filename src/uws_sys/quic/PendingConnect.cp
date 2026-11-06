import { Socket } from "./Socket.cp";

zig {
    const c = struct {
        extern fn us_quic_pending_connect_addrinfo(pc: *PendingConnect) *anyopaque;
        extern fn us_quic_pending_connect_resolved(pc: *PendingConnect) ?*Socket;
        extern fn us_quic_pending_connect_cancel(pc: *PendingConnect) void;
    };
}

export @opaque class PendingConnect {
    zig {
        pub const addrinfo = c.us_quic_pending_connect_addrinfo;
        pub const resolved = c.us_quic_pending_connect_resolved;
        pub const cancel = c.us_quic_pending_connect_cancel;
    }
}
