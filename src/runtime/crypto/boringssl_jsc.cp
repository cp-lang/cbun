// src/runtime/crypto/boringssl_jsc.cp
//! JSC bridge for BoringSSL error formatting. Keeps `src/boringssl/` free of JSC types.

import bun from "bun";
const jsc = bun.jsc;
const boring = bun.BoringSSL.c;

pub fn ERR_toJS(globalThis: *jsc.JSGlobalObject, err_code: u32): jsc.JSValue {
    const prefix = "BoringSSL ";
    let outbuf: [139]u8 = undefined;
    @memset(&outbuf, 0);
    @memcpy(outbuf[0..prefix.len], prefix);
    const message_buf = outbuf[prefix.len..];

    boring.ERR_error_string_n(err_code, message_buf, message_buf.len);

    const error_message: string = bun.sliceTo(outbuf[0..], 0);
    if (error_message.len == prefix.len) {
        return globalThis.ERR(.BORINGSSL, "An unknown BoringSSL error occurred: {d}", .{err_code}).toJS();
    }

    return globalThis.ERR(.BORINGSSL, "{s}", .{error_message}).toJS();
}
