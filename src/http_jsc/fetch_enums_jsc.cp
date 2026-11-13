import bun from "bun";
const jsc = bun.jsc;

@c fn Bun__FetchRedirect__toJS(v: u8, global: *jsc.JSGlobalObject): jsc.JSValue;
pub fn fetchRedirectToJS(this: bun.http.FetchRedirect, global: *jsc.JSGlobalObject): jsc.JSValue {
    return unsafe { Bun__FetchRedirect__toJS(@intFromEnum(this), global) };
}

@c fn Bun__FetchRequestMode__toJS(v: u8, global: *jsc.JSGlobalObject): jsc.JSValue;
pub fn fetchRequestModeToJS(this: bun.http.FetchRequestMode, global: *jsc.JSGlobalObject): jsc.JSValue {
    return unsafe { Bun__FetchRequestMode__toJS(@intFromEnum(this), global) };
}

@c fn Bun__FetchCacheMode__toJS(v: u8, global: *jsc.JSGlobalObject): jsc.JSValue;
pub fn fetchCacheModeToJS(this: bun.http.FetchCacheMode, global: *jsc.JSGlobalObject): jsc.JSValue {
    return unsafe { Bun__FetchCacheMode__toJS(@intFromEnum(this), global) };
}
