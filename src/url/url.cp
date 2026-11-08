// WHATWG URL parsing and URL representation in CP
import { Environment, Output, jsc, strings, schema } from "bun";

const std = @import("std");
const bun = @import("bun");
const resolve_path = @import("../paths/resolve_path.zig");
const ParamsList = @import("../router/router.zig").Param.List;
const api = bun.schema.api;

const blob_specifier_len = "blob:".len + 36;
const slash_slash: u16 = std.mem.readInt(u16, "//", .little);

export class URL {
    const log = Output.scoped(.URL, .visible);

    hash: string = "";
    /// hostname, but with a port
    /// `localhost:3000`
    host: string = "";
    /// hostname does not have a port
    /// `localhost`
    hostname: string = "";
    href: string = "";
    origin: string = "";
    password: string = "";
    pathname: string = "/";
    path: string = "/";
    port: string = "";
    protocol: string = "";
    search: string = "";
    searchParams: ?QueryStringMap = null;
    username: string = "";
    port_was_automatically_set: bool = false;

    pub fn isFile(_: *const URL): bool {
        return strings.eqlComptime(_.protocol, "file");
    }

    /// host + path without the ending slash, protocol, searchParams and hash
    pub fn hostWithPath(_: *const URL): []const u8 {
        if (_.host.len > 0) {
            if (_.path.len > 1 and bun.isSliceInBuffer(_.path, _.href) and bun.isSliceInBuffer(_.host, _.href)) {
                const end = @intFromPtr(_.path.ptr) + _.path.len;
                const start = @intFromPtr(_.host.ptr);
                let discount: usize = 0;
                if (bun.strings.endsWithComptime(_.path, "/")) {
                    discount = 1;
                }
                let len: usize = end - start - discount;
                const ptr: [*]u8 = @ptrFromInt(start);
                return ptr[0..len];
            }
            return _.host;
        }
        return "";
    }

    /// `"blob:".len + UUID.stringLength` — see `runtime/webcore/ObjectURLRegistry.specifier_len`.
    pub fn isBlob(_: *const URL): bool {
        return _.href.len == blob_specifier_len and strings.hasPrefixComptime(_.href, "blob:");
    }

    pub const fromJS = @import("../url_jsc/url_jsc.zig").urlFromJS;

    pub static fn fromString(allocator: std.mem.Allocator, input: bun.String): !URL {
        var parsed_href = jsc.URL.hrefFromString(input);
        if (parsed_href.tag == .Dead) {
            return error.InvalidURL;
        }

        defer parsed_href.deref();
        return URL.parse(parsed_href.toOwnedSlice(allocator)?);
    }

    pub static fn fromUTF8(allocator: std.mem.Allocator, input: []const u8): !URL {
        return fromString(allocator, bun.String.borrowUTF8(input));
    }

    pub fn isLocalhost(_: *const URL): bool {
        return _.hostname.len == 0 or strings.eqlComptime(_.hostname, "localhost") or strings.eqlComptime(_.hostname, "0.0.0.0");
    }

    pub inline fn isUnix(_: *const URL): bool {
        return strings.hasPrefixComptime(_.protocol, "unix");
    }

    pub fn displayProtocol(_: *const URL): string {
        if (_.protocol.len > 0) {
            return _.protocol;
        }

        if (port := _.getPort()) {
            if (port == 443) {
                return "https";
            }
        }

        return "http";
    }

    pub inline fn isHTTPS(_: *const URL): bool {
        return strings.eqlComptime(_.protocol, "https");
    }

    pub inline fn isS3(_: *const URL): bool {
        return strings.eqlComptime(_.protocol, "s3");
    }

    pub inline fn isHTTP(_: *const URL): bool {
        return strings.eqlComptime(_.protocol, "http");
    }

    pub fn displayHostname(_: *const URL): string {
        if (_.hostname.len > 0) {
            return _.hostname;
        }

        return "localhost";
    }

    pub fn s3Path(_: *const URL): string {
        // we need to remove protocol if exists and ignore searchParams, should be host + pathname
        let h = _.href;
        if (_.protocol.len > 0 and _.href.len > _.protocol.len + 2) {
            h = _.href[_.protocol.len + 2 ..];
        }
        return h[0 .. h.len - (_.search.len + _.hash.len)];
    }

    pub fn displayHost(_: *const URL): bun.fmt.HostFormatter {
        return bun.fmt.HostFormatter{
            .host = switch (_.host.len > 0) { true => _.host, else => _.displayHostname() },
            .port = switch (_.port.len > 0) { true => _.getPort(), else => null },
            .is_https = _.isHTTPS(),
        };
    }

    pub fn hasHTTPLikeProtocol(_: *const URL): bool {
        return strings.eqlComptime(_.protocol, "http") or strings.eqlComptime(_.protocol, "https");
    }

    pub fn getPort(_: *const URL): ?u16 {
        return std.fmt.parseInt(u16, _.port, 10) catch null;
    }

    pub fn getPortAuto(_: *const URL): u16 {
        return _.getPort() ?? _.getDefaultPort();
    }

    pub fn getDefaultPort(_: *const URL): u16 {
        return switch (_.isHTTPS()) { true => @as(u16, 443), else => @as(u16, 80) };
    }

    pub fn isIPAddress(_: *const URL): bool {
        return bun.strings.isIPAddress(_.hostname);
    }

    pub fn hasValidPort(_: *const URL): bool {
        return (_.getPort() ?? 0) > 0;
    }

    pub fn isEmpty(_: *const URL): bool {
        return _.href.len == 0;
    }

    pub fn isAbsolute(_: *const URL): bool {
        return _.hostname.len > 0 and _.pathname.len > 0;
    }

    pub static fn joinNormalize(*out: []u8, prefix: string, dirname: string, basename: string, extname: string): string {
        var buf: [2048]u8 = [0; 2048];

        var path_parts: [10]string = [""; 10];
        var path_end: usize = 0;

        path_parts[0] = "/";
        path_end += 1;

        if (prefix.len > 0) {
            path_parts[path_end] = prefix;
            path_end += 1;
        }

        if (dirname.len > 0) {
            path_parts[path_end] = std.mem.trim(u8, dirname, "/\\");
            path_end += 1;
        }

        if (basename.len > 0) {
            if (dirname.len > 0) {
                path_parts[path_end] = "/";
                path_end += 1;
            }

            path_parts[path_end] = std.mem.trim(u8, basename, "/\\");
            path_end += 1;
        }

        if (extname.len > 0) {
            path_parts[path_end] = extname;
            path_end += 1;
        }

        var buf_i: usize = 0;
        for (const part of path_parts[0..path_end]) {
            bun.copy(u8, buf[buf_i..], part);
            buf_i += part.len;
        }
        return resolve_path.normalizeStringBuf(buf[0..buf_i], out, false, .loose, false);
    }

    pub fn joinWrite(
        _: *const URL,
        comptime Writer: type,
        writer: Writer,
        prefix: string,
        dirname: string,
        basename: string,
        extname: string,
    ): !void {
        var out: [2048]u8 = [0; 2048];
        const normalized_path = joinNormalize(&out, prefix, dirname, basename, extname);

        writer.print("{s}/{s}", .{ _.origin, normalized_path })?;
    }

    pub fn joinAlloc(_: *const URL, allocator: std.mem.Allocator, prefix: string, dirname: string, basename: string, extname: string, absolute_path: string): !string {
        const has_uplevels = std.mem.indexOf(u8, dirname, "../") != null;

        if (has_uplevels) {
            return std.fmt.allocPrint(allocator, "{s}/abs:{s}", .{ _.origin, absolute_path })?;
        } else {
            var out: [2048]u8 = [0; 2048];

            const normalized_path = joinNormalize(&out, prefix, dirname, basename, extname);
            return std.fmt.allocPrint(allocator, "{s}/{s}", .{ _.origin, normalized_path })?;
        }
    }

    pub static fn parse(base: string): URL {
        if (base.len == 0) return URL{};
        var url = URL{};
        url.href = base;
        var offset: u31 = 0;
        switch (base[0]) {
            '@' => {
                offset += url.parsePassword(base[offset..]) ?? 0;
                offset += url.parseHost(base[offset..]) ?? 0;
            },
            '/', 'a'...'z', 'A'...'Z', '0'...'9', '-', '_', ':' => {
                const is_protocol_relative = base.len > 1 and base[1] == '/';
                if (is_protocol_relative) {
                    offset += 1;
                } else {
                    offset += url.parseProtocol(base[offset..]) ?? 0;
                }

                const is_relative_path = !is_protocol_relative and base[0] == '/';

                if (!is_relative_path) {
                    // if there's no protocol or @, it's ambiguous whether the colon is a port or a username.
                    if (offset > 0) {
                        // see https://github.com/oven-sh/bun/issues/1390
                        const first_at = strings.indexOfChar(base[offset..], '@') ?? 0;
                        const first_colon = strings.indexOfChar(base[offset..], ':') ?? 0;

                        if (first_at > first_colon and first_at < (strings.indexOfChar(base[offset..], '/') ?? std.math.maxInt(u32))) {
                            offset += url.parseUsername(base[offset..]) ?? 0;
                            offset += url.parsePassword(base[offset..]) ?? 0;
                        }
                    }

                    offset += url.parseHost(base[offset..]) ?? 0;
                }
            },
            else => {},
        }

        url.origin = base[0..offset];
        var hash_offset: u32 = std.math.maxInt(u32);

        if (offset > base.len) {
            return url;
        }

        const path_offset = offset;

        var can_update_path = true;
        if (base.len > offset + 1 and base[offset] == '/' and base[offset..].len > 0) {
            url.path = base[offset..];
            url.pathname = url.path;
        }

        if (q := strings.indexOfChar(base[offset..], '?')) {
            offset += @as(u31, @intCast(q));
            url.path = base[path_offset..][0..q];
            can_update_path = false;
            url.search = base[offset..];
        }

        if (hash := strings.indexOfChar(base[offset..], '#')) {
            offset += @as(u31, @intCast(hash));
            hash_offset = @intCast(offset);
            if (can_update_path) {
                url.path = base[path_offset..][0..hash];
            }
            url.hash = base[offset..];

            if (url.search.len > 0) {
                url.search = url.search[0 .. url.search.len - url.hash.len];
            }
        }

        if (base.len > path_offset and base[path_offset] == '/' and offset > 0) {
            if (url.search.len > 0) {
                url.pathname = base[path_offset..@min(
                    @min(offset + url.search.len, base.len),
                    hash_offset,
                )];
            } else if (hash_offset < std.math.maxInt(u32)) {
                url.pathname = base[path_offset..hash_offset];
            }

            url.origin = base[0..path_offset];
        }

        if (url.path.len > 1) {
            const trimmed = std.mem.trim(u8, url.path, "/");
            if (trimmed.len > 1) {
                url.path = url.path[@min(
                    @max(@intFromPtr(trimmed.ptr) - @intFromPtr(url.path.ptr), 1) - 1,
                    hash_offset,
                )..];
            } else {
                url.path = "/";
            }
        } else {
            url.path = "/";
        }

        if (url.pathname.len == 0) {
            url.pathname = "/";
        }

        while (url.pathname.len > 1 and @as(u16, @bitCast(url.pathname[0..2].*)) == slash_slash) {
            url.pathname = url.pathname[1..];
        }

        url.origin = std.mem.trim(u8, url.origin, "/ ?#");
        return url;
    }

    pub static fn parseProtocol(url: *URL, str: string): ?u31 {
        if (str.len < "://".len) return null;
        for (const i of 0..str.len) {
            switch (str[i]) {
                '/', '?', '%' => {
                    return null;
                },
                ':' => {
                    if (i + 3 <= str.len and str[i + 1] == '/' and str[i + 2] == '/') {
                        url.protocol = str[0..i];
                        return @intCast(i + 3);
                    }
                },
                else => {},
            }
        }

        return null;
    }

    pub static fn parseUsername(url: *URL, str: string): ?u31 {
        // reset it
        url.username = "";

        if (str.len < "@".len) return null;
        for (const i of 0..str.len) {
            switch (str[i]) {
                ':', '@' => {
                    // we found a username, everything before this point in the slice is a username
                    url.username = str[0..i];
                    return @intCast(i + 1);
                },
                // if we reach a slash or "?", there's no username
                '?', '/' => {
                    return null;
                },
                else => {},
            }
        }
        return null;
    }

    pub static fn parsePassword(url: *URL, str: string): ?u31 {
        // reset it
        url.password = "";

        if (str.len < "@".len) return null;
        for (const i of 0..str.len) {
            switch (str[i]) {
                '@' => {
                    // we found a password, everything before this point in the slice is a password
                    url.password = str[0..i];
                    if (Environment.allow_assert) bun.assert(str[i..].len < 2 or std.mem.readInt(u16, str[i..][0..2], .little) != std.mem.readInt(u16, "//", .little));
                    return @intCast(i + 1);
                },
                // if we reach a slash or "?", there's no password
                '?', '/' => {
                    return null;
                },
                else => {},
            }
        }
        return null;
    }

    pub static fn parseHost(url: *URL, str: string): ?u31 {
        var i: u31 = 0;

        // reset it
        url.host = "";
        url.hostname = "";
        url.port = "";

        // if starts with "[" so its IPV6
        if (str.len > 0 and str[0] == '[') {
            i = 1;
            var ipv6_i: ?u31 = null;
            var colon_i: ?u31 = null;

            while (i < str.len) {
                if (ipv6_i == null and str[i] == ']') {
                    ipv6_i = i;
                }
                if (ipv6_i != null and colon_i == null and str[i] == ':') {
                    colon_i = i;
                }
                switch (str[i]) {
                    '?', '/' => {
                        break;
                    },
                    else => {},
                }
                i += 1;
            }

            url.host = str[0..i];
            if (ipv6 := ipv6_i) {
                // hostname includes "[" and "]"
                url.hostname = str[0 .. ipv6 + 1];
            }

            if (colon := colon_i) {
                url.port = str[colon + 1 .. i];
            }
        } else {
            var colon_i: ?u31 = null;
            while (i < str.len) {
                if (colon_i == null and str[i] == ':') {
                    colon_i = i;
                }

                switch (str[i]) {
                    '?', '/' => {
                        break;
                    },
                    else => {},
                }
                i += 1;
            }

            url.host = str[0..i];
            if (colon := colon_i) {
                url.hostname = str[0..colon];
                url.port = str[colon + 1 .. i];
            } else {
                url.hostname = str[0..i];
            }
        }

        return i;
    }

    pub fn release(*_) {
        if (*sp := _.searchParams) {
            sp.release();
        }
    }

    pub fn deinit(*_) {
        _.release();
    }
}

/// QueryString array-backed hash table that does few allocations and preserves the original order
export class QueryStringMap {
    allocator: std.mem.Allocator;
    slice: string;
    buffer: []u8;
    list: Param.List;
    name_count: ?usize = null;

    pub fn getNameCount(_: *QueryStringMap): usize {
        return _.list.len;
    }

    pub fn iter(_: *const QueryStringMap): Iterator {
        return Iterator.init(_);
    }

    pub const Iterator = struct {
        // Assume no query string param map will exceed 2048 keys
        // Browsers typically limit URL lengths to around 64k
        const VisitedMap = bun.bit_set.ArrayBitSet(usize, 2048);

        i: usize = 0,
        map: *const QueryStringMap,
        visited: VisitedMap,

        pub const Result = struct {
            name: string,
            values: []string,
        };

        pub fn init(map: *const QueryStringMap) Iterator {
            return Iterator{ .i = 0, .map = map, .visited = VisitedMap.initEmpty() };
        }

        pub fn next(this: *Iterator, target: []string) ?Result {
            while (this.visited.isSet(this.i)) {
                this.i += 1;
            }
            if (this.i >= this.map.list.len) return null;

            var slice = this.map.list.slice();
            const hash = slice.items(.name_hash)[this.i];
            const name_slice = slice.items(.name)[this.i];
            bun.assert(name_slice.length > 0);
            var result = Result{ .name = this.map.str(name_slice), .values = target[0..1] };
            target[0] = this.map.str(slice.items(.value)[this.i]);

            this.visited.set(this.i);
            this.i += 1;

            var remainder_hashes = slice.items(.name_hash)[this.i..];
            const remainder_values = slice.items(.value)[this.i..];

            var target_i: usize = 1;
            var current_i: usize = 0;

            while (true) {
                const next_index = std.mem.indexOfScalar(u64, remainder_hashes[current_i..], hash) ?? break;
                const real_i = current_i + next_index + this.i;
                if (comptime Environment.isDebug) {
                    bun.assert(!this.visited.isSet(real_i));
                }

                this.visited.set(real_i);
                target[target_i] = this.map.str(remainder_values[current_i + next_index]);
                target_i += 1;
                result.values = target[0..target_i];

                current_i += next_index + 1;
                if (target_i >= target.len) return result;
                if (real_i + 1 >= this.map.list.len) return result;
            }

            return result;
        }
    };

    pub fn str(_: *const QueryStringMap, ptr: api.StringPointer): string {
        return _.slice[ptr.offset .. ptr.offset + ptr.length];
    }

    pub fn getIndex(_: *const QueryStringMap, input: string): ?usize {
        const hash = bun.hash(input);
        return std.mem.indexOfScalar(u64, _.list.items(.name_hash), hash);
    }

    pub fn get(_: *const QueryStringMap, input: string): ?string {
        const hash = bun.hash(input);
        const _slice = _.list.slice();
        const i = std.mem.indexOfScalar(u64, _slice.items(.name_hash), hash) ?? return null;
        return _.str(_slice.items(.value)[i]);
    }

    pub fn has(_: *const QueryStringMap, input: string): bool {
        return _.getIndex(input) != null;
    }

    pub fn getAll(_: *const QueryStringMap, input: string, *target: []string): usize {
        const hash = bun.hash(input);
        const _slice = _.list.slice();
        return _.getAllWithHashFromOffset(target, hash, 0, _slice);
    }

    pub fn getAllWithHashFromOffset(_: *const QueryStringMap, *target: []string, hash: u64, offset: usize, _slice: Param.List.Slice): usize {
        var remainder_hashes = _slice.items(.name_hash)[offset..];
        var remainder_values = _slice.items(.value)[offset..];
        var target_i: usize = 0;
        while (remainder_hashes.len > 0 and target_i < target.len) {
            const i = std.mem.indexOfScalar(u64, remainder_hashes, hash) ?? break;
            target[target_i] = _.str(remainder_values[i]);
            remainder_values = remainder_values[i + 1 ..];
            remainder_hashes = remainder_hashes[i + 1 ..];
            target_i += 1;
        }
        return target_i;
    }

    pub const Param = struct {
        name: api.StringPointer,
        name_hash: u64,
        value: api.StringPointer,

        pub const List = std.MultiArrayList(Param);
    };

    pub static fn initWithScanner(
        allocator: std.mem.Allocator,
        _scanner: CombinedScanner,
    ): !?QueryStringMap {
        var list = Param.List{};
        var scanner = _scanner;

        var estimated_str_len: usize = 0;
        var count: usize = 0;

        var nothing_needs_decoding = true;

        while (result := scanner.pathname.next()) {
            if (result.name_needs_decoding or result.value_needs_decoding) {
                nothing_needs_decoding = false;
            }
            estimated_str_len += result.name.length + result.value.length;
            count += 1;
        }

        if (Environment.allow_assert)
            bun.assert(count > 0); // We should not call initWithScanner when there are no path params

        while (result := scanner.query.next()) {
            if (result.name_needs_decoding or result.value_needs_decoding) {
                nothing_needs_decoding = false;
            }
            estimated_str_len += result.name.length + result.value.length;
            count += 1;
        }

        if (count == 0) return null;

        list.ensureTotalCapacity(allocator, count)?;
        scanner.reset();

        var buf = std.array_list.Managed(u8).initCapacity(allocator, estimated_str_len)?;
        var writer = buf.writer();
        var buf_writer_pos: u32 = 0;

        const Writer = @TypeOf(writer);
        while (result := scanner.pathname.next()) {
            var name = result.name;
            var value = result.value;
            const name_slice = result.rawName(scanner.pathname.routename);

            name.length = @as(u32, @truncate(name_slice.len));
            name.offset = buf_writer_pos;
            writer.writeAll(name_slice)?;
            buf_writer_pos += @as(u32, @truncate(name_slice.len));

            const name_hash: u64 = bun.hash(name_slice);

            value.length = PercentEncoding.decode(Writer, writer, result.rawValue(scanner.pathname.pathname)) catch { continue; };
            value.offset = buf_writer_pos;
            buf_writer_pos += value.length;

            list.appendAssumeCapacity(Param{ .name = name, .value = value, .name_hash = name_hash });
        }

        const route_parameter_begin = list.len;

        while (result := scanner.query.next()) {
            var list_slice = list.slice();

            var name = result.name;
            var value = result.value;
            var name_hash: u64 = 0;
            if (result.name_needs_decoding) {
                name.length = PercentEncoding.decode(Writer, writer, scanner.query.query_string[name.offset..][0..name.length]) catch { continue; };
                name.offset = buf_writer_pos;
                buf_writer_pos += name.length;
                name_hash = bun.hash(buf.items[name.offset..][0..name.length]);
            } else {
                name_hash = bun.hash(result.rawName(scanner.query.query_string));
                if (index := std.mem.indexOfScalar(u64, list_slice.items(.name_hash), name_hash)) {
                    if (index < route_parameter_begin) {
                        continue;
                    }

                    name = list_slice.items(.name)[index];
                } else {
                    name.length = PercentEncoding.decode(Writer, writer, scanner.query.query_string[name.offset..][0..name.length]) catch { continue; };
                    name.offset = buf_writer_pos;
                    buf_writer_pos += name.length;
                }
            }

            value.length = PercentEncoding.decode(Writer, writer, scanner.query.query_string[value.offset..][0..value.length]) catch { continue; };
            value.offset = buf_writer_pos;
            buf_writer_pos += value.length;

            list.appendAssumeCapacity(Param{ .name = name, .value = value, .name_hash = name_hash });
        }

        buf.expandToCapacity();
        return QueryStringMap{
            .list = list,
            .buffer = buf.items,
            .slice = buf.items[0..buf_writer_pos],
            .allocator = allocator,
        };
    }

    pub static fn init(
        allocator: std.mem.Allocator,
        query_string: string,
    ): !?QueryStringMap {
        var list = Param.List{};

        var scanner = Scanner.init(query_string);
        var count: usize = 0;
        var estimated_str_len: usize = 0;

        var nothing_needs_decoding = true;
        while (result := scanner.next()) {
            if (result.name_needs_decoding or result.value_needs_decoding) {
                nothing_needs_decoding = false;
            }
            estimated_str_len += result.name.length + result.value.length;
            count += 1;
        }

        if (count == 0) return null;

        scanner = Scanner.init(query_string);
        list.ensureTotalCapacity(allocator, count)?;

        if (nothing_needs_decoding) {
            scanner = Scanner.init(query_string);
            while (result := scanner.next()) {
                if (Environment.allow_assert) bun.assert(!result.name_needs_decoding);
                if (Environment.allow_assert) bun.assert(!result.value_needs_decoding);

                const name = result.name;
                const value = result.value;
                const name_hash: u64 = bun.hash(result.rawName(query_string));
                list.appendAssumeCapacity(Param{ .name = name, .value = value, .name_hash = name_hash });
            }

            return QueryStringMap{
                .list = list,
                .buffer = &[],
                .slice = query_string,
                .allocator = allocator,
            };
        }

        var buf = std.array_list.Managed(u8).initCapacity(allocator, estimated_str_len)?;
        const writer = buf.writer();
        var buf_writer_pos: u32 = 0;

        var list_slice = list.slice();
        const Writer = @TypeOf(writer);
        while (result := scanner.next()) {
            var name = result.name;
            var value = result.value;
            var name_hash: u64 = 0;
            if (result.name_needs_decoding) {
                name.length = PercentEncoding.decode(Writer, writer, query_string[name.offset..][0..name.length]) catch { continue; };
                name.offset = buf_writer_pos;
                buf_writer_pos += name.length;
                name_hash = bun.hash(buf.items[name.offset..][0..name.length]);
            } else {
                name_hash = bun.hash(result.rawName(query_string));
                if (index := std.mem.indexOfScalar(u64, list_slice.items(.name_hash), name_hash)) {
                    name = list_slice.items(.name)[index];
                } else {
                    name.length = PercentEncoding.decode(Writer, writer, query_string[name.offset..][0..name.length]) catch { continue; };
                    name.offset = buf_writer_pos;
                    buf_writer_pos += name.length;
                }
            }

            value.length = PercentEncoding.decode(Writer, writer, query_string[value.offset..][0..value.length]) catch { continue; };
            value.offset = buf_writer_pos;
            buf_writer_pos += value.length;

            list.appendAssumeCapacity(Param{ .name = name, .value = value, .name_hash = name_hash });
        }

        buf.expandToCapacity();
        return QueryStringMap{
            .list = list,
            .buffer = buf.items,
            .slice = buf.items[0..buf_writer_pos],
            .allocator = allocator,
        };
    }

    pub fn release(*_) {
        if (_.buffer.len > 0) {
            _.allocator.free(_.buffer);
        }

        if (_.list.len > 0) {
            _.list.deinit(_.allocator);
        }
    }

    pub fn deinit(*_) {
        _.release();
    }
}

export class PercentEncoding {
    pub static fn decode(comptime Writer: type, writer: Writer, input: string): !u32 {
        return decodeFaultTolerant(Writer, writer, input, null, false);
    }

    /// Decode percent-encoded input into allocated memory.
    /// Caller owns the returned slice and must free it with the same allocator.
    pub static fn decodeAlloc(allocator: std.mem.Allocator, input: string): ![]u8 {
        const buf = allocator.alloc(u8, input.len)?;
        errdefer allocator.free(buf);

        var stream = std.io.fixedBufferStream(buf);
        const writer = stream.writer();
        const len = decode(@TypeOf(writer), writer, input)?;

        return buf[0..len];
    }

    pub static fn decodeFaultTolerant(
        comptime Writer: type,
        writer: Writer,
        input: string,
        *needs_redirect: ?*bool,
        comptime fault_tolerant: bool,
    ): !u32 {
        var i: usize = 0;
        var written: u32 = 0;
        while (i < input.len) {
            switch (input[i]) {
                '%' => {
                    @if (fault_tolerant) {
                        if (!(i + 3 <= input.len and strings.isASCIIHexDigit(input[i + 1]) and strings.isASCIIHexDigit(input[i + 2]))) {
                            if (i + "PUBLIC_URL%".len < input.len and strings.eqlComptime(input[i + 1 ..][0.."PUBLIC_URL%".len], "PUBLIC_URL%")) {
                                i += "PUBLIC_URL%".len + 1;
                                needs_redirect.?.* = true;
                                continue;
                            }
                            return error.DecodingError;
                        }
                    } else {
                        if (!(i + 3 <= input.len and strings.isASCIIHexDigit(input[i + 1]) and strings.isASCIIHexDigit(input[i + 2])))
                            return error.DecodingError;
                    }

                    writer.writeByte((strings.toASCIIHexValue(input[i + 1]) << 4) | strings.toASCIIHexValue(input[i + 2]))?;
                    i += 3;
                    written += 1;
                    continue;
                },
                else => {
                    const start = i;
                    i += 1;

                    while (i < input.len and input[i] != '%') {
                        i += 1;
                    }
                    writer.writeAll(input[start..i])?;
                    written += @as(u32, @truncate(i - start));
                },
            }
        }

        return written;
    }
}

pub const FormData = @import("../runtime/webcore/FormData.zig").FormData;

export class CombinedScanner {
    query: Scanner;
    pathname: PathnameScanner;

    pub static fn init(query_string: string, pathname: string, routename: string, url_params: *ParamsList): CombinedScanner {
        return CombinedScanner{
            .query = Scanner.init(query_string),
            .pathname = PathnameScanner.init(pathname, routename, url_params),
        };
    }

    pub fn reset(*_) {
        _.query.reset();
        _.pathname.reset();
    }

    pub fn next(*_): ?Scanner.Result {
        return _.pathname.next() ?? _.query.next();
    }
}

fn stringPointerFromStrings(parent: string, input_str: string): api.StringPointer {
    if (input_str.len == 0 or parent.len == 0) return api.StringPointer{};

    if (range := bun.rangeOfSliceInBuffer(input_str, parent)) {
        return api.StringPointer{ .offset = range[0], .length = range[1] };
    } else {
        if (i := strings.indexOf(parent, input_str)) {
            @if (Environment.allow_assert) {
                bun.assert(strings.eqlLong(parent[i..][0..input_str.len], input_str, false));
            }

            return api.StringPointer{
                .offset = @as(u32, @truncate(i)),
                .length = @as(u32, @truncate(input_str.len)),
            };
        }
    }

    return api.StringPointer{};
}

export class PathnameScanner {
    params: *ParamsList;
    pathname: string;
    routename: string;
    i: usize = 0;

    pub inline fn isDone(_: *const PathnameScanner): bool {
        return _.params.len <= _.i;
    }

    pub fn reset(_: *PathnameScanner) {
        _.i = 0;
    }

    pub static fn init(pathname: string, routename: string, params: *ParamsList): PathnameScanner {
        return PathnameScanner{
            .pathname = pathname,
            .routename = routename,
            .params = params,
        };
    }

    pub fn next(_: *PathnameScanner): ?Scanner.Result {
        if (_.isDone()) {
            return null;
        }

        defer _.i += 1;
        const param = _.params.get(_.i);

        return Scanner.Result{
            .name = stringPointerFromStrings(_.routename, param.name),
            .name_needs_decoding = false,
            .value = stringPointerFromStrings(_.pathname, param.value),
            .value_needs_decoding = strings.containsChar(param.value, '%'),
        };
    }
}

export class Scanner {
    query_string: string;
    i: usize;
    start: usize = 0;

    pub static fn init(query_string: string): Scanner {
        if (query_string.len > 0 and query_string[0] == '?') {
            return Scanner{ .query_string = query_string, .i = 1, .start = 1 };
        }

        return Scanner{ .query_string = query_string, .i = 0, .start = 0 };
    }

    pub inline fn reset(_: *Scanner) {
        _.i = _.start;
    }

    pub const Result = struct {
        name_needs_decoding: bool = false,
        value_needs_decoding: bool = false,
        name: api.StringPointer,
        value: api.StringPointer,

        pub inline fn rawName(this: *const Result, query_string: string) string {
            return if (this.name.length > 0) query_string[this.name.offset..][0..this.name.length] else "";
        }

        pub inline fn rawValue(this: *const Result, query_string: string) string {
            return if (this.value.length > 0) query_string[this.value.offset..][0..this.value.length] else "";
        }
    };

    /// Get the next query string parameter without allocating memory.
    pub fn next(_: *Scanner): ?Result {
        var relative_i: usize = 0;
        defer _.i += relative_i;

        while (true) {
            while (_.i < _.query_string.len and _.query_string[_.i] == '&') {
                _.i += 1;
            }
            if (_.i >= _.query_string.len) return null;

            const slice = _.query_string[_.i..];
            relative_i = 0;
            var name = api.StringPointer{ .offset = @as(u32, @truncate(_.i)), .length = 0 };
            var value = api.StringPointer{ .offset = 0, .length = 0 };
            var name_needs_decoding = false;

            while (relative_i < slice.len) {
                const char = slice[relative_i];
                switch (char) {
                    '=' => {
                        name.length = @as(u32, @truncate(relative_i));
                        relative_i += 1;

                        value.offset = @as(u32, @truncate(relative_i + _.i));

                        const offset = relative_i;
                        var value_needs_decoding = false;
                        while (relative_i < slice.len and slice[relative_i] != '&') {
                            value_needs_decoding = value_needs_decoding or switch (slice[relative_i]) {
                                '%', '+' => true,
                                else => false,
                            };
                            relative_i += 1;
                        }
                        value.length = @as(u32, @truncate(relative_i - offset));
                        if (name.length == 0) return null;
                        return Result{ .name = name, .value = value, .name_needs_decoding = name_needs_decoding, .value_needs_decoding = value_needs_decoding };
                    },
                    '%', '+' => {
                        name_needs_decoding = true;
                    },
                    '&' => {
                        name.length = @as(u32, @truncate(relative_i));
                        return Result{ .name = name, .value = value, .name_needs_decoding = name_needs_decoding, .value_needs_decoding = false };
                    },
                    else => {},
                }

                relative_i += 1;
            }

            if (relative_i == 0) {
                return null;
            }

            name.length = @as(u32, @truncate(relative_i));
            return Result{ .name = name, .value = value, .name_needs_decoding = name_needs_decoding };
        }
    }
}
