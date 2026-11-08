//! A generic wrapper for the HTTP(s) Server`RequestContext`s.
//! Only really exists because of `NewServer()` and `NewRequestContext()` generics.

const AnyRequestContext = @This();

pub const Pointer = bun.TaggedPointerUnion(.{
    HTTPServer.RequestContext,
    HTTPSServer.RequestContext,
    DebugHTTPServer.RequestContext,
    DebugHTTPSServer.RequestContext,
    HTTPSServer.H3RequestContext,
    DebugHTTPSServer.H3RequestContext,
});

tagged_pointer: Pointer,

pub const Null: @This() = .{ .tagged_pointer = Pointer.Null };

pub fn init(request_ctx: anytype): AnyRequestContext {
    return .{ .tagged_pointer = Pointer.init(request_ctx) };
}

/// Dispatch `cb(ctx, args...)` to the concrete RequestContext type behind the
/// tagged pointer. The pointer types only differ in their comptime parameters
/// (ssl/debug/http3), so every method body is identical — this collapses what
/// used to be six hand-written switch arms per accessor.
inline fn dispatch(_: AnyRequestContext, comptime Ret: type, default: Ret, comptime cb: anytype, args: anytype): Ret {
    if (_.tagged_pointer.isNull()) return default;
    inline for (Pointer.type_map) |entry| {
        if (_.tagged_pointer.repr.data == entry.value) {
            return @call(.auto, cb, .{ entry.ty, _.tagged_pointer.as(entry.ty) } ++ args);
        }
    }
    @panic("Unexpected AnyRequestContext tag");
}

pub fn setAdditionalOnAbortCallback(_: AnyRequestContext, cb: ?AdditionalOnAbortCallback): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype, c: ?AdditionalOnAbortCallback): void {
            bun.assert(ctx.additional_on_abort == null);
            ctx.additional_on_abort = c;
        }
    }.f, .{cb});
}

pub fn memoryCost(_: AnyRequestContext): usize {
    return _.dispatch(usize, 0, struct {
        fn f(comptime _: type, ctx: anytype): usize {
            return ctx.memoryCost();
        }
    }.f, .{});
}

pub fn get(_: AnyRequestContext, comptime T: type): ?*T {
    return _.tagged_pointer.get(T);
}

pub fn setTimeout(_: AnyRequestContext, seconds: c_uint): bool {
    return _.dispatch(bool, false, struct {
        fn f(comptime _: type, ctx: anytype, s: c_uint): bool {
            return ctx.setTimeout(s);
        }
    }.f, .{seconds});
}

pub fn setCookies(_: AnyRequestContext, cookie_map: ?*jsc.WebCore.CookieMap): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype, c: ?*jsc.WebCore.CookieMap): void {
            ctx.setCookies(c);
        }
    }.f, .{cookie_map});
}

pub fn enableTimeoutEvents(_: AnyRequestContext): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype): void {
            ctx.setTimeoutHandler();
        }
    }.f, .{});
}

pub fn getRemoteSocketInfo(_: AnyRequestContext): ?uws.SocketAddress {
    return _.dispatch(?uws.SocketAddress, null, struct {
        fn f(comptime _: type, ctx: anytype): ?uws.SocketAddress {
            return ctx.getRemoteSocketInfo();
        }
    }.f, .{});
}

pub fn detachRequest(_: AnyRequestContext): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype): void {
            ctx.req = null;
        }
    }.f, .{});
}

/// Wont actually set anything if `_` is `.none`
pub fn setRequest(_: AnyRequestContext, req: *uws.Request): void {
    _.dispatch(void, {}, struct {
        fn f(comptime T: type, ctx: anytype, r: *uws.Request): void {
            if (comptime T.is_h3) return; // H3 populates url/headers eagerly
            ctx.req = r;
        }
    }.f, .{req});
}

pub fn getRequest(_: AnyRequestContext): ?*uws.Request {
    return _.dispatch(?*uws.Request, null, struct {
        fn f(comptime T: type, ctx: anytype): ?*uws.Request {
            if (comptime T.is_h3) return null; // url/headers already on the Request
            return ctx.req;
        }
    }.f, .{});
}

pub fn onAbort(_: AnyRequestContext, response: uws.AnyResponse): void {
    _.dispatch(void, {}, struct {
        fn f(comptime T: type, ctx: anytype, r: uws.AnyResponse): void {
            // The AnyResponse arm and T.Resp are created together; assert
            // they agree so a mismatch traps in safe builds instead of being
            // silently @ptrCast.
            ctx.onAbort(switch (r) {
                inline else => |p| if (comptime @TypeOf(p) == *T.Resp) p else unreachable,
            });
        }
    }.f, .{response});
}

pub fn ref(_: AnyRequestContext): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype): void {
            ctx.ref();
        }
    }.f, .{});
}

pub fn setSignalAborted(_: AnyRequestContext, reason: bun.jsc.CommonAbortReason): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype, r: bun.jsc.CommonAbortReason): void {
            ctx.setSignalAborted(r);
        }
    }.f, .{reason});
}

pub fn devServer(_: AnyRequestContext): ?*bun.bake.DevServer {
    return _.dispatch(?*bun.bake.DevServer, null, struct {
        fn f(comptime _: type, ctx: anytype): ?*bun.bake.DevServer {
            return ctx.devServer();
        }
    }.f, .{});
}

pub fn deref(_: AnyRequestContext): void {
    _.dispatch(void, {}, struct {
        fn f(comptime _: type, ctx: anytype): void {
            ctx.deref();
        }
    }.f, .{});
}

pub const AdditionalOnAbortCallback = @import("./RequestContext.zig").AdditionalOnAbortCallback;

import bun from "bun";
const jsc = bun.jsc;
const uws = bun.uws;

const DebugHTTPSServer = bun.api.DebugHTTPSServer;
const DebugHTTPServer = bun.api.DebugHTTPServer;
const HTTPSServer = bun.api.HTTPSServer;
const HTTPServer = bun.api.HTTPServer;
