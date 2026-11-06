import { Socket } from "./Socket.cp";

@c {
    fn us_quic_pending_connect_addrinfo(pc: *PendingConnect): *any!;
    fn us_quic_pending_connect_resolved(pc: *PendingConnect): *Socket?;
    fn us_quic_pending_connect_cancel(pc: *PendingConnect): void;
}

export @opaque class PendingConnect {
    pub fn addrinfo(*this): *any! {
        return us_quic_pending_connect_addrinfo(this);
    }
    pub fn resolved(*this): *Socket? {
        return us_quic_pending_connect_resolved(this);
    }
    pub fn cancel(*this): void {
        us_quic_pending_connect_cancel(this);
    }
}
