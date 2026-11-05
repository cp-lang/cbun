zig {
    const bun = @import("bun");
}

export @extern @value class Header {
    zig {
        name: [*]const u8,
        name_len: c_uint,
        value: [*]const u8,
        value_len: c_uint,
        qpack_index: c_int = -1,

        pub fn init(name_: []const u8, value_: []const u8, idx: ?Qpack) Header {
            return .{
                .name = name_.ptr,
                .name_len = @intCast(name_.len),
                .value = value_.ptr,
                .value_len = @intCast(value_.len),
                .qpack_index = if (idx) |i| @intFromEnum(i) else -1,
            };
        }
    }
}

export enum Qpack: u8 {
    authority = 0,
    path = 1,
    content_disposition = 3,
    content_length = 4,
    cookie = 5,
    date = 6,
    etag = 7,
    if_modified_since = 8,
    if_none_match = 9,
    last_modified = 10,
    link = 11,
    location = 12,
    referer = 13,
    set_cookie = 14,
    method_get = 17,
    scheme_https = 23,
    accept = 29,
    accept_encoding = 31,
    accept_ranges = 32,
    cache_control = 36,
    content_encoding = 43,
    content_type = 44,
    range = 55,
    vary = 59,
    accept_language = 72,
    authorization = 84,
    forwarded = 88,
    if_range = 89,
    origin = 90,
    server = 92,
    user_agent = 95,
    x_forwarded_for = 96,
    _,

    zig {
        pub const Class = union(enum) {
            forbidden,
            host,
            indexed: struct { name: []const u8, index: Qpack },

            fn idx(comptime name: []const u8, i: Qpack) Class {
                return .{ .indexed = .{ .name = name, .index = i } };
            }
        };

        pub fn classify(name: []const u8) ?Class {
            return map.getAnyCase(name);
        }

        const map = bun.ComptimeStringMap(Class, .{
            .{ "connection", .forbidden },
            .{ "host", .host },
            .{ "keep-alive", .forbidden },
            .{ "proxy-connection", .forbidden },
            .{ "transfer-encoding", .forbidden },
            .{ "upgrade", .forbidden },

            .{ "accept", Class.idx("accept", .accept) },
            .{ "accept-encoding", Class.idx("accept-encoding", .accept_encoding) },
            .{ "accept-language", Class.idx("accept-language", .accept_language) },
            .{ "accept-ranges", Class.idx("accept-ranges", .accept_ranges) },
            .{ "authorization", Class.idx("authorization", .authorization) },
            .{ "cache-control", Class.idx("cache-control", .cache_control) },
            .{ "content-disposition", Class.idx("content-disposition", .content_disposition) },
            .{ "content-encoding", Class.idx("content-encoding", .content_encoding) },
            .{ "content-length", Class.idx("content-length", .content_length) },
            .{ "content-type", Class.idx("content-type", .content_type) },
            .{ "cookie", Class.idx("cookie", .cookie) },
            .{ "date", Class.idx("date", .date) },
            .{ "etag", Class.idx("etag", .etag) },
            .{ "forwarded", Class.idx("forwarded", .forwarded) },
            .{ "if-modified-since", Class.idx("if-modified-since", .if_modified_since) },
            .{ "if-none-match", Class.idx("if-none-match", .if_none_match) },
            .{ "if-range", Class.idx("if-range", .if_range) },
            .{ "last-modified", Class.idx("last-modified", .last_modified) },
            .{ "link", Class.idx("link", .link) },
            .{ "location", Class.idx("location", .location) },
            .{ "origin", Class.idx("origin", .origin) },
            .{ "range", Class.idx("range", .range) },
            .{ "referer", Class.idx("referer", .referer) },
            .{ "server", Class.idx("server", .server) },
            .{ "set-cookie", Class.idx("set-cookie", .set_cookie) },
            .{ "user-agent", Class.idx("user-agent", .user_agent) },
            .{ "vary", Class.idx("vary", .vary) },
            .{ "x-forwarded-for", Class.idx("x-forwarded-for", .x_forwarded_for) },
        });
    }
}
