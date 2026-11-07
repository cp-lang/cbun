// src/runtime/api/bun/x509.cp
// X.509 certificate subjectAltName validation and JavaScriptCore object bindings in CP

zig {
    const bun = @import("bun");
    const BoringSSL = bun.BoringSSL.c;
    const jsc = bun.jsc;
    const JSGlobalObject = jsc.JSGlobalObject;
    const JSValue = jsc.JSValue;

    extern fn Bun__X509__toJSLegacyEncoding(cert: *BoringSSL.X509, globalObject: *JSGlobalObject) JSValue;
    extern fn Bun__X509__toJS(cert: *BoringSSL.X509, globalObject: *JSGlobalObject) JSValue;
}

export fn isSafeAltName(name: string, utf8: bool): bool {
    for c of name {
        switch c {
            '"', '\\', ',', '\'' => {
                return false;
            },
            else => {
                if utf8 {
                    if c < ' ' || c == 0x7f {
                        return false;
                    }
                } else {
                    if c < ' ' || c > '~' {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

export fn toJS(cert: *BoringSSL.X509, globalObject: *JSGlobalObject): JSValue!bun.JSError {
    return bun.jsc.fromJSHostCall(globalObject, @src(), Bun__X509__toJSLegacyEncoding, [cert, globalObject]);
}

export fn toJSObject(cert: *BoringSSL.X509, globalObject: *JSGlobalObject): JSValue!bun.JSError {
    return Bun__X509__toJS(cert, globalObject);
}
