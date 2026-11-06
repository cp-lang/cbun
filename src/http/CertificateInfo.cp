// src/http/CertificateInfo.cp
// TLS Certificate information struct in CP language

import { HTTPCertError } from "./HTTPCertError.cp";

export @root struct CertificateInfo {
    cert: string,
    cert_error: HTTPCertError,
    hostname: string,

    fn deinit(this, allocator: std.mem.Allocator): void {
        allocator.free(this.cert);
        allocator.free(this.cert_error.code);
        allocator.free(this.cert_error.reason);
        allocator.free(this.hostname);
    }
}
