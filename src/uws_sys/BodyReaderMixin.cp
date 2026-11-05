zig {
    const std = @import("std");
    const bun = @import("bun");
    const uws = bun.uws;

    pub fn BodyReaderMixin(
        Wrap: type,
        field: []const u8,
        onBody: fn (*Wrap, body: []const u8, resp: uws.AnyResponse) anyerror!void,
        onError: fn (*Wrap) void,
    ) type {
        return struct {
            body: std.array_list.Managed(u8),

            pub fn init(allocator: std.mem.Allocator) @This() {
                return .{ .body = .init(allocator) };
            }

            pub fn readBody(ctx: *@This(), resp: anytype) void {
                const Mixin = @This();
                const Response = @TypeOf(resp);
                const handlers = struct {
                    fn onDataGeneric(mixin: *Mixin, r: Response, chunk: []const u8, last: bool) void {
                        const any = uws.AnyResponse.init(r);
                        onData(mixin, any, chunk, last) catch |e| switch (e) {
                            error.OutOfMemory => return mixin.onOOM(any),
                            else => return mixin.onInvalid(any),
                        };
                    }
                    fn onAborted(mixin: *Mixin, _: Response) void {
                        mixin.body.clearAndFree();
                        onError(@fieldParentPtr(field, mixin));
                    }
                };
                resp.onData(*@This(), handlers.onDataGeneric, ctx);
                resp.onAborted(*@This(), handlers.onAborted, ctx);
            }

            fn onData(ctx: *@This(), resp: uws.AnyResponse, chunk: []const u8, last: bool) !void {
                if (last) {
                    var body = ctx.body;
                    defer body.deinit();
                    ctx.body = .init(ctx.body.allocator);
                    resp.clearOnData();
                    if (body.items.len > 0) {
                        try body.appendSlice(chunk);
                        try onBody(@fieldParentPtr(field, ctx), body.items, resp);
                    } else {
                        try onBody(@fieldParentPtr(field, ctx), chunk, resp);
                    }
                } else {
                    try ctx.body.appendSlice(chunk);
                }
            }

            fn onOOM(ctx: *@This(), r: uws.AnyResponse) void {
                var body = ctx.body;
                ctx.body = .init(ctx.body.allocator);
                body.deinit();
                r.clearAborted();
                r.clearOnData();
                r.clearOnWritable();

                r.writeStatus("500 Internal Server Error");
                r.endWithoutBody(false);

                onError(@fieldParentPtr(field, ctx));
            }

            fn onInvalid(ctx: *@This(), r: uws.AnyResponse) void {
                var body = ctx.body;
                ctx.body = .init(body.allocator);
                body.deinit();

                r.clearAborted();
                r.clearOnData();
                r.clearOnWritable();

                r.writeStatus("400 Bad Request");
                r.endWithoutBody(false);

                onError(@fieldParentPtr(field, ctx));
            }
        };
    }
}
