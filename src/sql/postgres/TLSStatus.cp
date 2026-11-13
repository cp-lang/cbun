pub enum TLSStatus {
    none,
    pending,
    message_sent: u8,
    ssl_not_available,
    ssl_ok,
}
