// src/http/HTTPCertError.cp
// HTTP TLS Certificate Error struct in CP language

export @root struct HTTPCertError {
    error_no: i32 = 0,
    code: cstring = "",
    reason: cstring = "",
}
