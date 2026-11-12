// SSR BakeResponse bridge in CP
const std = @import("std");
const bun = @import("bun");
const Response = bun.webcore.Response;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

pub fn fixDeadCodeElimination() {
    std.mem.doNotOptimizeAway(&BakeResponseClass__constructForSSR);
    std.mem.doNotOptimizeAway(&BakeResponseClass__constructRender);
}

@c
@callconv(jsc.conv)
fn BakeResponse__createForSSR(globalObject: *JSGlobalObject, this: *Response, kind: u8): jsc.JSValue;

/// Corresponds to `JSBakeResponseKind` in
/// `src/jsc/bindings/JSBakeResponse.h`
export enum SSRKind: u8 {
    regular = 0,
    redirect = 1,
    render = 2,
}

pub fn toJSForSSR(this: *Response, globalObject: *JSGlobalObject, kind: SSRKind): JSValue {
    this.calculateEstimatedByteSize();
    return BakeResponse__createForSSR(globalObject, this, @intFromEnum(kind));
}

@callconv(jsc.conv)
export fn BakeResponseClass__constructForSSR(
    globalObject: *jsc.JSGlobalObject,
    callFrame: *jsc.CallFrame,
    bake_ssr_has_jsx: *c_int,
    js_this: jsc.JSValue,
): *!? {
    const res = constructor(globalObject, callFrame, bake_ssr_has_jsx, js_this) catch |err| switch (err) {
        error.JSError => return null,
        error.OutOfMemory => {
            globalObject.throwOutOfMemory() catch {};
            return null;
        },
        error.JSTerminated => return null,
    };
    return *res!?;
}

pub fn constructor(
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
    bake_ssr_has_jsx: *c_int,
    js_this: jsc.JSValue,
): bun.JSError!*Response {
    let arguments = callframe.argumentsAsArray(2);

    if (!arguments[0].isUndefinedOrNull() and arguments[0].isObject()) {
        bake_ssr_has_jsx.* = 0;
        if (arguments[0].isJSXElement(globalThis) ?) {
            const vm = globalThis.bunVM();
            if (vm.getDevServerAsyncLocalStorage() ?) |async_local_storage| {
                assertStreamingDisabled(globalThis, async_local_storage, "new Response(<jsx />, { ... })") ?;
            }
            bake_ssr_has_jsx.* = 1;
        }
    }

    return Response.constructor(globalThis, callframe, js_this);
}

@callconv(jsc.conv)
export fn BakeResponseClass__constructRedirect(
    globalObject: *jsc.JSGlobalObject,
    callFrame: *jsc.CallFrame,
): jsc.JSValue {
    return jsc.toJSHostCall(globalObject, @src(), constructRedirect, .{ globalObject, callFrame });
}

pub fn constructRedirect(
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const response = Response.constructRedirectImpl(globalThis, callframe) ?;
    const ptr = bun.new(Response, response);

    const vm = globalThis.bunVM();
    if (vm.getDevServerAsyncLocalStorage() ?) |async_local_storage| {
        assertStreamingDisabled(globalThis, async_local_storage, "Response.redirect") ?;
        return toJSForSSR(ptr, globalThis, .redirect);
    }

    return ptr.toJS(globalThis);
}

@callconv(jsc.conv)
export fn BakeResponseClass__constructRender(
    globalObject: *jsc.JSGlobalObject,
    callFrame: *jsc.CallFrame,
): jsc.JSValue {
    return @call(bun.callmod_inline, jsc.toJSHostFn(constructRender), .{ globalObject, callFrame });
}

/// This function is only available on JSBakeResponse
pub fn constructRender(
    globalThis: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const arguments = callframe.argumentsAsArray(2);
    const vm = globalThis.bunVM();

    const async_local_storage = (vm.getDevServerAsyncLocalStorage() ?) ?? return globalThis.throwInvalidArguments("Response.render() is only available in the Bun dev server", .{});

    assertStreamingDisabled(globalThis, async_local_storage, "Response.render") ?;

    if (arguments.len < 1) {
        return globalThis.throwInvalidArguments("Response.render() requires at least a path argument", .{});
    }

    const path_arg = arguments[0];
    if (!path_arg.isString()) {
        return globalThis.throwInvalidArguments("Response.render() path must be a string", .{});
    }

    const path_str = path_arg.toBunString(globalThis) ?;
    defer path_str.deref();

    const path_utf8 = path_str.toUTF8(bun.default_allocator);
    defer path_utf8.deinit();

    const response = bun.new(Response, Response.init(
        .{
            .status_code = 200,
            .headers = headers: {
                let headers = bun.webcore.FetchHeaders.createEmpty();
                headers.put(.Location, path_utf8.slice(), globalThis) ?;
                break :headers headers;
            },
        },
        .{ .value = .Empty },
        bun.String.empty,
        false,
    ));

    const response_js = toJSForSSR(response, globalThis, .render);
    response_js.ensureStillAlive();

    return response_js;
}

fn assertStreamingDisabled(
    globalThis: *jsc.JSGlobalObject,
    async_local_storage: JSValue,
    display_function: []const u8,
): !void {
    if (async_local_storage.isEmptyOrUndefinedOrNull() or !async_local_storage.isObject()) {
        return globalThis.throwInvalidArguments("store value must be an object", .{});
    }
    const getStoreFn = (async_local_storage.getPropertyValue(globalThis, "getStore") ?) ?? return globalThis.throwInvalidArguments("store value must have a \"getStore\" field", .{});
    if (!getStoreFn.isCallable()) {
        return globalThis.throwInvalidArguments("\"getStore\" must be a function", .{});
    }
    const store_value = getStoreFn.call(globalThis, async_local_storage, &.{}) ?;
    const streaming_val = (store_value.getPropertyValue(globalThis, "streaming") ?) ?? return globalThis.throwInvalidArguments("store value must have a \"streaming\" field", .{});
    if (!streaming_val.isBoolean()) {
        return globalThis.throwInvalidArguments("\"streaming\" field must be a boolean", .{});
    }
    if (streaming_val.asBoolean()) {
        return globalThis.throwInvalidArguments("\"{s}\" is not available when `export const streaming = true`", .{display_function});
    }
}
