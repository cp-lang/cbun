pub enum TLSStatus {
    none: void,
    pending,
    message_sent,
    ssl_not_available,
    ssl_failed,
    ssl_ok,
}
