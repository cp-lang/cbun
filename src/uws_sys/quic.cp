// src/uws_sys/quic.cp
// CP bindings for the lsquic-backed QUIC transport in packages/bun-usockets

import * as context_mod from "./quic/Context.cp";
import * as socket_mod from "./quic/Socket.cp";
import * as stream_mod from "./quic/Stream.cp";
import * as pending_mod from "./quic/PendingConnect.cp";
import * as header_mod from "./quic/Header.cp";

export const Context = context_mod.Context;
export const Socket = socket_mod.Socket;
export const Stream = stream_mod.Stream;
export const PendingConnect = pending_mod.PendingConnect;
export const Header = header_mod.Header;
export const Qpack = header_mod.Qpack;

extern fn us_quic_global_init(): void;

export fn globalInit(): void {
    us_quic_global_init();
}
