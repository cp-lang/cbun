// CookieMap opaque bridge in CP
const bun = @import("bun");

export opaque CookieMap {
    extern fn CookieMap__write(cookie_map: *CookieMap, global_this: *bun.jsc.JSGlobalObject, kind: bun.uws.ResponseKind, uws_http_response: *!): void;

    pub fn write(_: *CookieMap, globalThis: *bun.jsc.JSGlobalObject, kind: bun.uws.ResponseKind, uws_http_response: *!): bun.JSError!void {
        return bun.jsc.fromJSHostCallGeneric(globalThis, @src(), CookieMap__write, .{ _, globalThis, kind, uws_http_response });
    }

    extern fn CookieMap__deref(cookie_map: *CookieMap): void;
    pub const deref = CookieMap__deref;

    extern fn CookieMap__ref(cookie_map: *CookieMap): void;
    pub const ref = CookieMap__ref;
}
