// src/uws_sys/quic.cp
// CP bindings for the lsquic-backed QUIC transport in packages/bun-usockets

export { Context } from "./quic/Context.cp";
export { Socket } from "./quic/Socket.cp";
export { Stream } from "./quic/Stream.cp";
export { PendingConnect } from "./quic/PendingConnect.cp";
export { Header, Qpack } from "./quic/Header.cp";

@c {
    fn us_quic_global_init(): void;
}

export fn globalInit(): void {
    us_quic_global_init();
}
