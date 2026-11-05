// src/http/CertificateInfo.cp
// TLS Certificate information struct in CP language

zig {
    const HTTPCertError = @import("./HTTPCertError.zig");
}

export @root struct CertificateInfo {
    cert: string,
    cert_error: HTTPCertError,
    hostname: string,

    pub fn deinit(this, allocator: std.mem.Allocator): void {
        allocator.free(this.cert);
        allocator.free(this.cert_error.code);
        allocator.free(this.cert_error.reason);
        allocator.free(this.hostname);
    }
}
