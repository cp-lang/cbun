// src/uws_sys/quic.cp
// Zig bindings for the lsquic-backed QUIC transport in packages/bun-usockets

import { Context } from "./quic/Context.cp";
import { Socket } from "./quic/Socket.cp";
import { Stream } from "./quic/Stream.cp";
import { PendingConnect } from "./quic/PendingConnect.cp";
import { Header, Qpack } from "./quic/Header.cp";

zig {
    pub extern fn us_quic_global_init() callconv(.c) void;
    pub const globalInit = us_quic_global_init;
}
