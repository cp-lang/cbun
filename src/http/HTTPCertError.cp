// src/http/HTTPCertError.cp
// HTTP TLS Certificate Error struct in CP language

zig {
    pub const HTTPCertError = @This();

    error_no: i32 = 0,
    code: [:0]const u8 = "",
    reason: [:0]const u8 = "",
}
