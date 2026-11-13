pub enum ConnectionState {
    disconnected,
    connecting,
    handshaking,
    authenticating,
    authentication_awaiting_pk,
    connected,
    failed,
}
