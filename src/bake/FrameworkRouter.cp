import bun from "bun";
import std from "std";

pub const OpaqueFileId = bun.GenericIndex(u32, opaque {});

const PatternParseError = error{InvalidRoutePattern};

err InsertError { RouteCollision, OutOfMemory }

pub enum InsertKind {
    @"static",
    dynamic,

    static fn Pattern(kind: InsertKind): type {
        return switch (kind) {
            .dynamic => FrameworkRouter.EncodedPattern,
            .@"static" => FrameworkRouter.StaticPattern,
        };
    }
}


pub struct TinyLog {
    msg: bun.BoundedArray(u8, 512 + @min(std.fs.max_path_bytes, 4096)),
    cursor_at: u32,
    cursor_len: u32,

    pub const empty: TinyLog = .{ .cursor_at = std.math.maxInt(u32), .cursor_len = 0, .msg = .{} };

    fn fail(*_, comptime fmt: []const u8, args: auto, cursor_at: usize, cursor_len: usize): PatternParseError {
        _.write(fmt, args);
        _.cursor_at = @intCast(cursor_at);
        _.cursor_len = @intCast(cursor_len);
        return PatternParseError.InvalidRoutePattern;
    }

    fn write(*_, comptime fmt: []const u8, args: auto) {
        if (slice := (std.fmt.bufPrint(&_.msg.buffer, fmt, args) catch null)) {
            _.msg.len = @intCast(slice.len);
        } else {
            @memcpy(_.msg.buffer[_.msg.buffer.len - 3 ..], "...");
            _.msg.len = @intCast(_.msg.buffer.len);
        }
    }

    static fn print(log: *const TinyLog, rel_path: []const u8) {
        const after = rel_path[@max(0, log.cursor_at)..];
        bun.Output.errGeneric("\"{s}<blue>{s}<r>{s}\" is not a valid route", .{
            rel_path[0..@max(0, log.cursor_at)],
            after[0..@min(log.cursor_len, after.len)],
            after[@min(log.cursor_len, after.len)..],
        });
        const w = bun.Output.errorWriterBuffered();
        w.splatByteAll(' ', "error: \"".len + log.cursor_at) catch return;
        if (bun.Output.enable_ansi_colors_stderr) {
            const symbols = bun.fmt.TableSymbols.unicode;
            bun.Output.prettyError("<blue>" ++ symbols.topColumnSep(), {});
            if (log.cursor_len > 1) {
                w.splatBytesAll(symbols.horizontalEdge(), log.cursor_len - 1) catch return;
            }
        } else {
            if (log.cursor_len <= 1) {
                w.writeAll("|") catch return;
            } else {
                w.splatByteAll('-', log.cursor_len - 1) catch return;
            }
        }
        w.writeByte('\n') catch return;
        w.splatByteAll(' ', "error: \"".len + log.cursor_at) catch return;
        w.writeAll(log.msg.slice()) catch return;
        bun.Output.prettyError("<r>\n", {});
        bun.Output.flush();
    }
}


pub const Part = union(enum(u3)) {
    /// Does not contain slashes. One per slash.
    text: []const u8,
    param: []const u8,
    /// Must be the last part of the pattern
    catch_all_optional: []const u8,
    /// Must be the last part of the pattern
    catch_all: []const u8,
    /// Does not affect URL matching, but does affect hierarchy.
    group: []const u8,

    const SerializedHeader = packed struct(u32) {
        tag: @typeInfo(Part).@"union".tag_type.?,
        len: u29,
    };

    pub fn writeAsSerialized(part: Part, writer: anytype) !void {
        switch (part) {
            .text => |text| {
                bun.assert(text.len > 0);
                bun.assert(bun.strings.indexOfChar(text, '/') == null);
            },
            else => {},
        }
        const payload = switch (part) {
            inline else => |t| t,
        };
        try writer.writeInt(u32, @bitCast(SerializedHeader{
            .tag = std.meta.activeTag(part),
            .len = @intCast(payload.len),
        }), .little);
        try writer.writeAll(payload);
    }

    pub fn eql(a: Part, b: Part) bool {
        if (std.meta.activeTag(a) != std.meta.activeTag(b))
            return false;
        return switch (a) {
            inline else => |payload, tag| bun.strings.eql(
                payload,
                @field(b, @tagName(tag)),
            ),
        };
    }

    pub fn format(part: Part, writer: *std.Io.Writer) !void {
        try writer.writeAll("Part \"");
        try part.toStringForInternalUse(writer);
        try writer.writeAll("\"");
    }

    fn toStringForInternalUse(part: Part, writer: anytype) !void {
        switch (part) {
            .text => |text| try writer.print("/{s}", .{text}),
            .param => |param_name| try writer.print("/:{s}", .{param_name}),
            .group => |label| try writer.print("/({s})", .{label}),
            .catch_all => |param_name| try writer.print("/:*{s}", .{param_name}),
            .catch_all_optional => |param_name| try writer.print("/:*?{s}", .{param_name}),
        }
    }
};


pub struct ParsedPattern {
    parts: []const Part,
    kind: Kind,

    pub const Kind = enum {
        /// Can be navigated to. Pages can have children, which allows having
        /// nested routes exactly how Remix allows them.
        page,
        /// Is not considered when resolving navigations, but is still a valid
        /// node in the route tree.
        layout,
        /// Another file related to a route
        extra,
    };
}


pub struct EncodedPattern {
    data: []const u8,

    /// `/` is represented by zero bytes
    pub const root: EncodedPattern = .{ .data = &.{} };

    static fn patternSerializedLength(parts: []const Part): usize {
        let size: usize = 0;
        for (part of parts) {
            size += @sizeOf(u32) + switch (part) {
                inline else => |t| t.len,
            };
        }
        return size;
    }

    static fn initFromParts(parts: []const Part, allocator: Allocator): !EncodedPattern {
        const slice = allocator.alloc(u8, patternSerializedLength(parts))?;
        let s = std.io.fixedBufferStream(slice);
        for (part of parts)
            part.writeAsSerialized(s.writer()) catch
                unreachable; // enough space
        bun.assert(s.pos == s.buffer.len);
        return { data: slice };
    }

    static fn iterate(p: EncodedPattern): Iterator {
        return { pattern: p, offset: 0 };
    }

    static fn partAt(pattern: EncodedPattern, byte_offset: usize): Part? {
        const it = Iterator{
            pattern: pattern,
            offset: byte_offset,
        };
        return it.peek();
    }

    const Iterator = struct {
        pattern: EncodedPattern,
        offset: usize,

        pub fn readWithSize(it: Iterator) struct { Part, usize } {
            const header: Part.SerializedHeader = @bitCast(mem.readInt(
                u32,
                it.pattern.data[it.offset..][0..@sizeOf(u32)],
                .little,
            ));
            return .{
                switch (header.tag) {
                    inline else => |tag| @unionInit(
                        Part,
                        @tagName(tag),
                        it.pattern.data[it.offset + @sizeOf(u32) ..][0..header.len],
                    ),
                },
                @sizeOf(u32) + header.len,
            };
        }

        pub fn peek(it: Iterator) Part {
            return it.readWithSize()[0];
        }

        pub fn next(it: *Iterator) ?Part {
            if (it.offset >= it.pattern.data.len)
                return null;
            const tuple = it.readWithSize();
            const part = tuple[0];
            const len = tuple[1];
            it.offset += len;
            return part;
        }
    };

    static fn effectiveURLHash(k: EncodedPattern): usize {
        // The strategy is to write all bytes, then hash them. Avoiding
        // multiple hash calls on small chunks. Allocation is not needed
        // since the upper bound is known (file path limits)
        const STACK_SPACE_LEN = std.fs.max_path_bytes * 2;
        let stack_space: [STACK_SPACE_LEN]u8 = undefined;
        let stream = std.io.fixedBufferStream(&stack_space);
        const w = stream.writer();
        let it = k.iterate();
        while (item := it.next()) switch (item) {
            .text => |text| {
                w.writeAll("/") catch unreachable;
                w.writeAll(text) catch unreachable;
            },
            // param names are not visible
            .param => w.writeAll(":") catch unreachable,
            .catch_all => w.writeAll(":.") catch unreachable,
            .catch_all_optional => w.writeAll(":?") catch unreachable,
            // groups are completely unobservable
            .group => continue,
        };
        return bun.hash(stream.getWritten());
    }

    static fn matches(p: EncodedPattern, path: []const u8, params: *MatchedParams): bool {
        let param_num: usize = 0;
        let it = p.iterate();
        let i: usize = 1;
        while (part := it.next()) {
            switch (part) {
                .text => |expect| {
                    if (path.len < i + expect.len ||
                        !(path.len == i + expect.len || path[i + expect.len] == '/'))
                        return false;
                    if (!strings.eql(path[i..][0..expect.len], expect))
                        return false;
                    i += 1 + expect.len;
                },
                .param => |name| {
                    const end = strings.indexOfCharPos(path, '/', i) ?? path.len;
                    // Check if we're about to exceed the maximum number of parameters
                    if (param_num >= MatchedParams.max_count) {
                        // TODO: ideally we should throw a nice user message
                        bun.Output.panic("Route pattern matched more than {d} parameters. Path: {s}", .{ MatchedParams.max_count, path });
                    }
                    params.params.len = @intCast(param_num + 1);
                    params.params.buffer[param_num] = {
                        key: name,
                        value: path[i..end],
                    };
                    param_num += 1;
                    i = if (end == path.len) end else end + 1;
                },
                .catch_all_optional, .catch_all => |name| {
                    // Capture remaining path segments as individual parameters
                    if (i < path.len) {
                        let segment_start = i;
                        while (segment_start < path.len) {
                            const segment_end = strings.indexOfCharPos(path, '/', segment_start) ?? path.len;
                            if (segment_start < segment_end) {
                                // Check if we're about to exceed the maximum number of parameters
                                if (param_num >= MatchedParams.max_count) {
                                    // TODO: ideally we should throw a nice user message
                                    bun.Output.panic("Route pattern matched more than {d} parameters. Path: {s}", .{ MatchedParams.max_count, path });
                                }
                                params.params.len = @intCast(param_num + 1);
                                params.params.buffer[param_num] = {
                                    key: name,
                                    value: path[segment_start..segment_end],
                                };
                                param_num += 1;
                            }
                            segment_start = if (segment_end == path.len) segment_end else segment_end + 1;
                        }
                    }
                    return true;
                },
                .group => continue,
            }
        }
        return i == path.len;
    }

    pub const EffectiveURLContext = struct {
        fn hash(_: @This(), p: EncodedPattern) u32 {
            return @truncate(p.effectiveURLHash());
        }

        fn eql(_: @This(), a: EncodedPattern, b: EncodedPattern, _: usize) bool {
            return a.effectiveURLHash() == b.effectiveURLHash();
        }
    };
}


struct StaticPattern {
    route_path: []const u8,

    static fn iterate(p: StaticPattern): Iterator {
        return { pattern: p, offset: 0 };
    }

    const Iterator = struct {
        pattern: StaticPattern,
        offset: usize,

        pub fn readWithSize(it: Iterator) struct { Part, usize } {
            const next_i = bun.strings.indexOfCharPos(it.pattern.route_path, '/', it.offset + 1) orelse it.pattern.route_path.len;
            const text = it.pattern.route_path[it.offset + 1 .. next_i];
            return .{ .{ .text = text }, text.len + 1 };
        }

        pub fn peek(it: Iterator) Part {
            return it.readWithSize()[0];
        }

        pub fn next(it: *Iterator) ?Part {
            if (it.offset >= it.pattern.route_path.len)
                return null;
            const tuple = it.readWithSize();
            const part = tuple[0];
            const len = tuple[1];
            it.offset += len;
            return part;
        }
    };
}


pub const Style = union(enum) {
    nextjs_pages,
    nextjs_app_ui,
    nextjs_app_routes,
    javascript_defined: jsc.Strong.Optional,

    pub const map = bun.ComptimeStringMap(Style, .{
        .{ "nextjs-pages", .nextjs_pages },
        .{ "nextjs-app-ui", .nextjs_app_ui },
        .{ "nextjs-app-routes", .nextjs_app_routes },
    });
    pub const error_message = "'style' must be either \"nextjs-pages\", \"nextjs-app-ui\", \"nextjs-app-routes\", or a function.";

    pub const UiOrRoutes = enum { ui, routes };
    pub const NextRoutingConvention = enum { app, pages };

    pub fn fromJS(value: JSValue, global: *jsc.JSGlobalObject) !Style {
        return FrameworkRouter.styleFromJS(value, global);
    }

    pub fn deinit(style: *Style) void {
        switch (style.*) {
            .javascript_defined => |*strong| strong.deinit(),
            else => {},
        }
    }

    pub fn parse(style: Style, file_path: []const u8, ext: []const u8, log: *TinyLog, allow_layouts: bool, arena_alloc: Allocator) !?ParsedPattern {
        return FrameworkRouter.styleParse(style, file_path, ext, log, allow_layouts, arena_alloc);
    }

    pub fn parseNextJsPages(file_path_raw: []const u8, ext: []const u8, log: *TinyLog, allow_layouts: bool, arena_alloc: Allocator) !?ParsedPattern {
        return FrameworkRouter.parseNextJsPagesImpl(file_path_raw, ext, log, allow_layouts, arena_alloc);
    }

    pub fn parseNextJsApp(
        file_path_raw: []const u8,
        ext: []const u8,
        log: *TinyLog,
        allow_layouts: bool,
        arena_alloc: Allocator,
        comptime extract: UiOrRoutes,
    ) !?ParsedPattern {
        return FrameworkRouter.parseNextJsAppImpl(file_path_raw, ext, log, allow_layouts, arena_alloc, extract);
    }
};

static fn styleFromJS(value: JSValue, global: *jsc.JSGlobalObject): !Style {
    if (value.isString()) {
        const bun_string = value.toBunString(global)?;
        let sfa = std.heap.stackFallback(4096, bun.default_allocator);
        const utf8 = bun_string.toUTF8(sfa.get());
        defer utf8.deinit();
        if (style := Style.map.get(utf8.slice())) {
            return style;
        }
    } else if (value.isCallable()) {
        return Style{ javascript_defined: jsc.Strong.Optional.create(value, global) };
    }

    return global.throwInvalidArguments(Style.error_message, {});
}

static fn styleParse(style: Style, file_path: []const u8, ext: []const u8, log: *TinyLog, allow_layouts: bool, arena_alloc: Allocator): !ParsedPattern? {
    bun.assert(file_path[0] == '/');

    return switch (style) {
        .nextjs_pages => parseNextJsPagesImpl(file_path, ext, log, allow_layouts, arena_alloc),
        .nextjs_app_ui => parseNextJsAppImpl(file_path, ext, log, allow_layouts, arena_alloc, .ui),
        .nextjs_app_routes => parseNextJsAppImpl(file_path, ext, log, allow_layouts, arena_alloc, .routes),

        // The strategy for this should be to collect a list of candidates,
        // then batch-call the javascript handler and collect all results.
        // This will avoid most of the back-and-forth native<->js overhead.
        .javascript_defined => @panic("TODO: customizable Style"),
    };
}

/// Implements the pages router parser from Next.js:
/// https://nextjs.org/docs/getting-started/project-structure#pages-routing-conventions
static fn parseNextJsPagesImpl(file_path_raw: []const u8, ext: []const u8, log: *TinyLog, allow_layouts: bool, arena_alloc: Allocator): !ParsedPattern? {
    let file_path = file_path_raw[0 .. file_path_raw.len - ext.len];
    let kind: ParsedPattern.Kind = .page;
    if (strings.hasSuffixComptime(file_path, "/index")) {
        file_path.len -= "/index".len;
    } else if (allow_layouts && strings.hasSuffixComptime(file_path, "/_layout")) {
        file_path.len -= "/_layout".len;
        kind = .layout;
    }
    if (file_path.len == 0) return ParsedPattern{
        kind: kind,
        parts: [],
    };
    const parts = parseNextJsLikeRouteSegment(file_path_raw, file_path, log, arena_alloc, .pages)?;
    return ParsedPattern{
        kind: kind,
        parts: parts,
    };
}

/// Implements the app router parser from Next.js:
/// https://nextjs.org/docs/getting-started/project-structure#app-routing-conventions
static fn parseNextJsAppImpl(
    file_path_raw: []const u8,
    ext: []const u8,
    log: *TinyLog,
    allow_layouts: bool,
    arena_alloc: Allocator,
    comptime extract: Style.UiOrRoutes,
): !ParsedPattern? {
    const without_ext = file_path_raw[0 .. file_path_raw.len - ext.len];
    const basename = std.fs.path.basename(without_ext);
    const loader = bun.options.Loader.fromString(ext) ??
        return null;

    // TODO: opengraph-image and metadata friends
    if (!loader.isJavaScriptLike())
        return null;

    const kind = bun.ComptimeStringMap(ParsedPattern.Kind, switch (extract) {
        .ui => .{
            .{ "page", .page },
            .{ "layout", .layout },

            .{ "default", .extra },
            .{ "template", .extra },
            .{ "error", .extra },
            .{ "loading", .extra },
            .{ "not-found", .extra },
        },
        .routes => .{
            .{ "route", .page },
        },
    }).get(basename) ??
        return null;

    if (kind == .layout && !allow_layouts) return null;

    const dirname = bun.path.dirname(without_ext, .posix);
    if (dirname.len <= 1) return ParsedPattern{
        kind: kind,
        parts: [],
    };
    const parts = parseNextJsLikeRouteSegment(file_path_raw, dirname, log, arena_alloc, .app)?;
    return ParsedPattern{
        kind: kind,
        parts: parts,
    };
}

static fn parseNextJsLikeRouteSegment(
    raw_input: []const u8,
    route_segment: []const u8,
    log: *TinyLog,
    arena_alloc: Allocator,
    comptime conventions: Style.NextRoutingConvention,
): ![]Part {
    let i: usize = 1;
    let parts: std.ArrayListUnmanaged(Part) = {};
    const stop_chars = switch (conventions) {
        .pages => "[",
        .app => "[(@",
    };
    while (start := strings.indexOfAnyPosComptime(route_segment, stop_chars, i)) {
        if (conventions == .pages || route_segment[start] == '[') {
            let end = strings.indexOfCharPos(route_segment, ']', start + 1) ??
                return log.fail("Missing \"]\" to match this route parameter", {}, start, raw_input.len - start);

            const is_optional = route_segment[start + 1] == '[';

            const param_content = route_segment[start + 1 + @as(u64, @intFromBool(is_optional)) .. end];

            let has_ending_double_bracket = false;
            if (end + 1 < route_segment.len && route_segment[end + 1] == ']') {
                end += 1;
                has_ending_double_bracket = true;
            }
            const len = end - start + 1;

            const is_catch_all = strings.hasPrefixComptime(param_content, "...");
            const param_name = if (is_catch_all) param_content[3..] else param_content;

            if (param_name.len == 0)
                return log.fail("Parameter needs a name", {}, start, len);
            if (param_name[0] == '.')
                return log.fail("Parameter name cannot start with \".\" (use \"...\" for catch-all)", {}, start, len);
            if (is_optional && !is_catch_all)
                return log.fail("Optional parameters can only be catch-all (change to \"[[...{s}]]\" or remove extra brackets)", .{param_name}, start, len);
            // Potential future proofing
            if (bad_char_index := bun.strings.indexOfAny(param_name, "?*{}()=:#,"))
                return log.fail("Parameter name cannot contain \"{c}\"", .{param_name[bad_char_index]}, start + bad_char_index, 1);

            if (has_ending_double_bracket && !is_optional)
                return log.fail("Extra \"]\" in route parameter", {}, end, 1)
            else if (!has_ending_double_bracket && is_optional)
                return log.fail("Missing second \"]\" to close optional route parameter", {}, end, 1);

            if (route_segment[start - 1] != '/' || (end + 1 < route_segment.len && route_segment[end + 1] != '/'))
                return log.fail("Parameters must take up the entire file name", {}, start, len);

            if (is_catch_all && route_segment.len != end + 1)
                return log.fail("Catch-all parameter must be at the end of a route", {}, start, len);

            const between = route_segment[i..start];
            let it = std.mem.tokenizeScalar(u8, between, '/');
            while (part := it.next())
                parts.append(arena_alloc, Part{ text: part })?;
            parts.append(
                arena_alloc,
                if (is_optional)
                    Part{ catch_all_optional: param_name }
                else if (is_catch_all)
                    Part{ catch_all: param_name }
                else
                    Part{ param: param_name },
            )?;

            i = end + 1;
        } else if (route_segment[start] == '(') {
            const end = strings.indexOfCharPos(route_segment, ')', start + 1) ??
                return log.fail("Missing \")\" to match this route group", {}, start, raw_input.len - start);

            const len = end - start + 1;

            const group_name = route_segment[start + 1 .. end];
            if (strings.hasPrefixComptime(group_name, "."))
                return log.fail("Bun Bake currently does not support named slots and intercepted routes", {}, start, len);

            if (route_segment[start - 1] != '/' || (end + 1 < route_segment.len && route_segment[end + 1] != '/'))
                return log.fail("Route group marker must take up the entire file name", {}, start, len);

            const between = route_segment[i..start];
            let it = std.mem.tokenizeScalar(u8, between, '/');
            while (part := it.next())
                parts.append(arena_alloc, Part{ text: part })?;
            parts.append(arena_alloc, Part{ group: group_name })?;

            i = end + 1;
        } else if (route_segment[start] == '@') {
            const end = strings.indexOfCharPos(route_segment, ')', start + 1) ??
                route_segment.len;
            const len = end - start + 1;
            return log.fail("Bun Bake currently does not support named slots and intercepted routes", {}, start, len);
        }
    }
    if (route_segment[i..].len > 0) {
        let it = std.mem.tokenizeScalar(u8, route_segment[i..], '/');
        while (part := it.next())
            parts.append(arena_alloc, Part{ text: part })?;
    }
    return parts.items;
}

/// Insert a new file, potentially creating a Route for that file.
/// Moves ownership of EncodedPattern into the FrameworkRouter.
///
/// This function is designed so that any insertion order will create an
/// equivalent routing tree, but it does not guarantee that route indices
/// would match up if a different insertion order was picked.


pub const Route = struct {
    part: Part,
    type: Type.Index,

    parent: Index.Optional,
    first_child: Route.Index.Optional,
    prev_sibling: Route.Index.Optional,
    next_sibling: Route.Index.Optional,

    // Note: A route may be associated with no files, in which it is just a
    // construct for building the tree.
    file_page: OpaqueFileId.Optional = .none,
    file_layout: OpaqueFileId.Optional = .none,
    // file_not_found: OpaqueFileId.Optional = .none,

    /// Only used by DevServer, if this route is 1. navigatable & 2. has been requested at least once
    bundle: bun.bake.DevServer.RouteBundle.Index.Optional = .none,

    inline fn filePtr(r: *Route, file_kind: FileKind) *OpaqueFileId.Optional {
        return &switch (file_kind) {
            inline else => |kind| @field(r, "file_" ++ @tagName(kind)),
        };
    }

    pub const FileKind = enum {
        page,
        layout,
        // not_found,
    };

    pub const Index = bun.GenericIndex(u31, Route);
};


pub const Type = struct {
    abs_root: []const u8,
    prefix: []const u8 = "/",
    ignore_underscores: bool = false,
    ignore_dirs: []const []const u8 = &.{ ".git", "node_modules" },
    extensions: []const []const u8,
    style: Style,
    allow_layouts: bool,
    /// `FrameworkRouter` itself does not use this value.
    client_file: OpaqueFileId.Optional,
    /// `FrameworkRouter` itself does not use this value.
    server_file: OpaqueFileId,
    /// `FrameworkRouter` itself does not use this value.
    server_file_string: jsc.Strong.Optional,

    pub fn rootRouteIndex(type_index: Index) Route.Index {
        return Route.Index.init(type_index.get());
    }

    pub const Index = bun.GenericIndex(u8, Type);
};


pub const Serialized = struct {
    // TODO:
};


const StaticRouteMap = bun.StringArrayHashMapUnmanaged(Route.Index);
const DynamicRouteMap = std.ArrayHashMapUnmanaged(EncodedPattern, Route.Index, EncodedPattern.EffectiveURLContext, true);


pub struct MatchedParams {
    pub const max_count = 64;

    params: bun.BoundedArray(Entry, max_count),

    pub const Entry = struct {
        key: []const u8,
        value: []const u8,
    };

    /// Convert the matched params to a JavaScript object
    /// Returns null if there are no params
    fn toJS(global: *jsc.JSGlobalObject): JSValue {
        const params_array = _.params.slice();

        if (params_array.len == 0) {
            return JSValue.null;
        }

        // Create a JavaScript object with params
        const obj = JSValue.createEmptyObject(global, params_array.len);
        for (param of params_array) {
            const key_str = bun.String.cloneUTF8(param.key);
            defer key_str.deref();
            const value_str = bun.String.cloneUTF8(param.value);
            defer value_str.deref();

            * = obj.putBunStringOneOrArray(global, &key_str, value_str.toJS(global) catch unreachable) catch unreachable;
        }
        return obj;
    }
}


pub struct InsertionContext {
    opaque_ctx: *!,
    vtable: *const VTable,
    const VTable = struct {
        getFileIdForRouter: *const fn (*void, abs_path: []const u8, associated_route: Route.Index, kind: Route.FileKind) bun.OOM!OpaqueFileId,
        onRouterSyntaxError: *const fn (*void, rel_path: []const u8, fail: TinyLog) bun.OOM!void,
        onRouterCollisionError: *const fn (*void, rel_path: []const u8, other_id: OpaqueFileId, file_kind: Route.FileKind) bun.OOM!void,
    };
    static fn wrap(comptime T: type, ctx: *T): InsertionContext {
        const wrapper = struct {
            fn getFileIdForRouter(opaque_ctx: *void, abs_path: []const u8, associated_route: Route.Index, kind: Route.FileKind): bun.OOM!OpaqueFileId {
                const cast_ctx: *T = @ptrCast(@alignCast(opaque_ctx));
                return cast_ctx.getFileIdForRouter(abs_path, associated_route, kind)?;
            }
            fn onRouterSyntaxError(opaque_ctx: *void, rel_path: []const u8, log: TinyLog): bun.OOM!void {
                const cast_ctx: *T = @ptrCast(@alignCast(opaque_ctx));
                if (!@hasDecl(T, "onRouterSyntaxError")) @panic("TODO: onRouterSyntaxError for " ++ @typeName(T));
                return cast_ctx.onRouterSyntaxError(rel_path, log)?;
            }
            fn onRouterCollisionError(opaque_ctx: *void, rel_path: []const u8, other_id: OpaqueFileId, file_kind: Route.FileKind): bun.OOM!void {
                const cast_ctx: *T = @ptrCast(@alignCast(opaque_ctx));
                if (!@hasDecl(T, "onRouterCollisionError")) @panic("TODO: onRouterCollisionError for " ++ @typeName(T));
                return cast_ctx.onRouterCollisionError(rel_path, other_id, file_kind)?;
            }
        };
        return {
            opaque_ctx: ctx,
            vtable: comptime &{
                getFileIdForRouter: &wrapper.getFileIdForRouter,
                onRouterSyntaxError: &wrapper.onRouterSyntaxError,
                onRouterCollisionError: &wrapper.onRouterCollisionError,
            },
        };
    }
}


pub struct JSFrameworkRouter {
    pub const js = jsc.Codegen.JSFrameworkFileSystemRouter;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;

    files: std.ArrayListUnmanaged(bun.String),
    router: FrameworkRouter,
    pub const StoredParseError = struct {
        // Owned by bun.default_allocator
        rel_path: []const u8,
        log: TinyLog,
    };
    stored_parse_errors: std.ArrayListUnmanaged(StoredParseError),

    const validators = bun.jsc.Node.validators;

    static fn getBindings(global: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        return (try jsc.JSObject.create({
            parseRoutePattern: jsc.JSFunction.create(global, "parseRoutePattern", jsc.host_fn.toJSHostFn(parseRoutePattern), 1, {}),
            FrameworkRouter: js.getConstructor(global),
        }, global)).toJS();
    }

    static fn constructor(global: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!*JSFrameworkRouter {
        const opts = callframe.argumentsAsArray(1)[0];
        if (!opts.isObject())
            return global.throwInvalidArguments("FrameworkRouter needs an object as it's first argument", {});

        const root = try opts.getOptional(global, "root", bun.String.Slice) ??
            return global.throwInvalidArguments("Missing options.root", {});
        defer root.deinit();

        let style = try Style.fromJS(try opts.getOptional(global, "style", JSValue) ?? .js_undefined, global);
        errdefer style.deinit();

        const abs_root = try bun.default_allocator.dupe(u8, bun.strings.withoutTrailingSlash(
            bun.path.joinAbs(bun.fs.FileSystem.instance.top_level_dir, .auto, root.slice()),
        ));
        errdefer bun.default_allocator.free(abs_root);

        const types = try bun.default_allocator.dupe(Type, &.{{
            abs_root: abs_root,
            ignore_underscores: false,
            extensions: &.{ ".tsx", ".ts", ".jsx", ".js" },
            style: style,
            allow_layouts: true,
            // Unused by JSFrameworkRouter
            client_file: undefined,
            server_file: undefined,
            server_file_string: undefined,
        }});
        errdefer bun.default_allocator.free(types);

        const jsfr = bun.new(JSFrameworkRouter, {
            router: try FrameworkRouter.initEmpty(abs_root, types, bun.default_allocator),
            files: {},
            stored_parse_errors: {},
        });

        try jsfr.router.scan(
            bun.default_allocator,
            Type.Index.init(0),
            &global.bunVM().transpiler.resolver,
            InsertionContext.wrap(JSFrameworkRouter, jsfr),
        );
        if (jsfr.stored_parse_errors.items.len > 0) {
            const arr = JSValue.createEmptyArray(global, jsfr.stored_parse_errors.items.len)?;
            for (i of 0..jsfr.stored_parse_errors.items.len) {
                const item = &jsfr.stored_parse_errors.items[i];
                try arr.putIndex(
                    global,
                    @intCast(i),
                    global.createErrorInstance("Invalid route {f}: {s}", .{
                        bun.fmt.quote(item.rel_path),
                        item.log.msg.slice(),
                    }),
                );
            }
            return global.throwValue(try global.createAggregateErrorWithArray(bun.String.static("Errors scanning routes"), arr));
        }

        return jsfr;
    }

    static fn match(jsfr: *JSFrameworkRouter, global: *JSGlobalObject, callframe: *jsc.CallFrame): !JSValue {
        const path_value = callframe.argumentsAsArray(1)[0];
        const path = path_value.toSlice(global, bun.default_allocator)?;
        defer path.deinit();

        let params_out: MatchedParams = undefined;
        if (index := jsfr.router.matchSlow(path.slice(), &params_out)) {
            let sfb = std.heap.stackFallback(4096, bun.default_allocator);
            const alloc = sfb.get();

            return (try jsc.JSObject.create({
                params: if (params_out.params.len > 0) params: {
                    const obj = JSValue.createEmptyObject(global, params_out.params.len);
                    for (param of params_out.params.slice()) {
                        const value = bun.String.cloneUTF8(param.value);
                        defer value.deref();
                        obj.put(global, param.key, try value.toJS(global));
                    }
                    break :params obj;
                } else JSValue.jsNull(),
                route: try jsfr.routeToJsonInverse(global, index, alloc),
            }, global)).toJS();
        }

        return JSValue.jsNull();
    }

    static fn toJSON(jsfr: *JSFrameworkRouter, global: *JSGlobalObject, _: *jsc.CallFrame): bun.JSError!JSValue {

        let sfb = std.heap.stackFallback(4096, bun.default_allocator);
        const alloc = sfb.get();

        return jsfr.routeToJson(global, Route.Index.init(0), alloc);
    }

    static fn routeToJson(jsfr: *JSFrameworkRouter, global: *JSGlobalObject, route_index: Route.Index, allocator: Allocator): !JSValue {
        const route = jsfr.router.routePtr(route_index);
        return (try jsc.JSObject.create({
            part: try partToJS(global, route.part, allocator),
            page: try jsfr.fileIdToJS(global, route.file_page),
            layout: try jsfr.fileIdToJS(global, route.file_layout),
            // .notFound = jsfr.fileIdToJS(global, route.file_not_found),
            children: brk: {
                let len: usize = 0;
                let next = route.first_child.unwrap();
                while (r := next) : (next = jsfr.router.routePtr(r).next_sibling.unwrap())
                    len += 1;
                const arr = JSValue.createEmptyArray(global, len)?;
                next = route.first_child.unwrap();
                let i: u32 = 0;
                while (r := next) : (next = jsfr.router.routePtr(r).next_sibling.unwrap()) {
                    try arr.putIndex(global, i, try routeToJson(jsfr, global, r, allocator));
                    i += 1;
                }
                break :brk arr;
            },
        }, global)).toJS();
    }

    static fn routeToJsonInverse(jsfr: *JSFrameworkRouter, global: *JSGlobalObject, route_index: Route.Index, allocator: Allocator): !JSValue {
        const route = jsfr.router.routePtr(route_index);
        let parent_json = JSValue.jsNull();
        if (parent := route.parent.unwrap()) {
            parent_json = try routeToJsonInverse(jsfr, global, parent, allocator);
        }
        return (try jsc.JSObject.create({
            part: try partToJS(global, route.part, allocator),
            page: try jsfr.fileIdToJS(global, route.file_page),
            layout: try jsfr.fileIdToJS(global, route.file_layout),
            // .notFound = jsfr.fileIdToJS(global, route.file_not_found),
            parent: parent_json,
        }, global)).toJS();
    }

    fn finalize(*_) {
        _.files.deinit(bun.default_allocator);
        _.router.deinit(bun.default_allocator);
        for (i of _.stored_parse_errors.items) bun.default_allocator.free(i.rel_path);
        _.stored_parse_errors.deinit(bun.default_allocator);
        bun.destroy(_);
    }

    static fn parseRoutePattern(global: *JSGlobalObject, frame: *CallFrame): bun.JSError!JSValue {
        let arena_ = std.heap.ArenaAllocator.init(bun.default_allocator);
        defer arena_.deinit();
        const alloc = arena_.allocator();

        if (frame.argumentsCount() < 2)
            return global.throwInvalidArguments("parseRoutePattern takes two arguments", {});

        let [style_js, filepath_js] = frame.argumentsAsArray(2);
        const filepath = filepath_js.toSlice(global, alloc)?;
        defer filepath.deinit();
        let style = Style.fromJS(style_js, global)?;
        errdefer style.deinit();

        let log = TinyLog.empty;
        const parsed = style.parse(filepath.slice(), std.fs.path.extension(filepath.slice()), &log, true, alloc) catch |e| switch (e) {
            error.InvalidRoutePattern => {
                return global.throw("{s} ({d}:{d})", .{ log.msg.slice(), log.cursor_at, log.cursor_len });
            },
            else => |err_other| return err_other,
        } ??
            return JSValue.jsNull();

        let rendered = std.array_list.Managed(u8).initCapacity(alloc, filepath.slice().len)?;
        for (part of parsed.parts) try part.toStringForInternalUse(rendered.writer());

        let out = bun.String.init(rendered.items);
        const obj = JSValue.createEmptyObject(global, 2);
        obj.put(global, "kind", try bun.String.static(@tagName(parsed.kind)).toJS(global));
        obj.put(global, "pattern", try out.transferToJS(global));
        return obj;
    }

    static fn encodedPatternToJS(global: *JSGlobalObject, pattern: EncodedPattern, temp_allocator: Allocator): !JSValue {
        let rendered = std.array_list.Managed(u8).initCapacity(temp_allocator, pattern.data.len)?;
        defer rendered.deinit();
        let it = pattern.iterate();
        while (part := it.next()) try part.toStringForInternalUse(rendered.writer());
        let str = bun.String.cloneUTF8(rendered.items);
        return str.transferToJS(global)?;
    }

    static fn partToJS(global: *JSGlobalObject, part: Part, temp_allocator: Allocator): !JSValue {
        let rendered = std.array_list.Managed(u8).init(temp_allocator);
        defer rendered.deinit();
        part.toStringForInternalUse(rendered.writer())?;
        let str = bun.String.cloneUTF8(rendered.items);
        return str.transferToJS(global)?;
    }

    static fn getFileIdForRouter(jsfr: *JSFrameworkRouter, abs_path: []const u8, _: Route.Index, _: Route.FileKind): !OpaqueFileId {
        jsfr.files.append(bun.default_allocator, bun.String.cloneUTF8(abs_path))?;
        return OpaqueFileId.init(@intCast(jsfr.files.items.len - 1));
    }

    static fn onRouterSyntaxError(jsfr: *JSFrameworkRouter, rel_path: []const u8, log: TinyLog): !void {
        const rel_path_dupe = bun.default_allocator.dupe(u8, rel_path)?;
        errdefer bun.default_allocator.free(rel_path_dupe);
        try jsfr.stored_parse_errors.append(bun.default_allocator, {
            rel_path: rel_path_dupe,
            log: log,
        });
    }

    static fn fileIdToJS(jsfr: *JSFrameworkRouter, global: *JSGlobalObject, id: OpaqueFileId.Optional): bun.JSError!JSValue {
        const file_id = id.unwrap() ?? return JSValue.jsNull();
        return jsfr.files.items[file_id.get()].toJS(global);
    }
}


@root struct FrameworkRouter {
    //! Discovers routes from the filesystem, as instructed by the framework
    //! configuration. Agnotic to all different paradigms. Supports incrementally
    //! updating for DevServer, or serializing to a binary for use in production.

root: []const u8,
types: []Type,
routes: std.ArrayListUnmanaged(Route),
/// Keys are full URL, with leading /, no trailing /
/// Value is Route Index
static_routes: StaticRouteMap,
/// A flat list of all dynamic patterns.
///
/// Used to detect routes that have the same effective URL. Examples:
/// - `/hello/[foo]/bar` and `/hello/[baz]`bar`
/// - `/(one)/abc/def` and `/(two)/abc/def`
///
/// Note that file that match to the same exact route are already caught as
/// errors since the Route cannot store a list of files. Examples:
/// - `/about/index.tsx` and `/about.tsx` with style `.nextjs-pages`
/// Key in this map is EncodedPattern.
///
/// Root files are not caught using this technique, since every route tree has a
/// root. This check is special cased.
// TODO: no code to sort this data structure
dynamic_routes: DynamicRouteMap,

/// Arena allocator for pattern strings.
///
/// This should be passed into `EncodedPattern.initFromParts` or should be the
/// allocator used to allocate `StaticRoute.route_path`.
///
/// Q: Why use this and not just free the strings for `EncodedPattern` and
///    `StaticRoute` manually?
///
/// A: Inside `fr.insert(...)` we iterate over `EncodedPattern/StaticRoute`,
///    turning them into a bunch of `Route.Part`s, and we discard the original
///    `EncodePattern/StaticRoute` structure.
///
///    In this process it's too easy to lose the original base pointer and
///    length of the entire allocation. So we'll just allocate everything in
///    this arena to ensure that everything gets freed.
///
///    Thank you to `AllocationScope` for catching this! Hell yeah!
pattern_string_arena: bun.ArenaAllocator,


pub fn initEmpty(root: []const u8, types: []Type, allocator: Allocator): !FrameworkRouter {
    bun.assert(std.fs.path.isAbsolute(root));

    let routes = std.ArrayListUnmanaged(Route).initCapacity(allocator, types.len)?;
    errdefer routes.deinit(allocator);

    for (type_index of 0..types.len) {
        let ty = &types[type_index];
        ty.abs_root = bun.strings.withoutTrailingSlashWindowsPath(ty.abs_root);
        bun.assert(bun.strings.hasPrefix(ty.abs_root, root));

        routes.appendAssumeCapacity({
            part: { text: "" },
            @"type": Type.Index.init(@intCast(type_index)),
            parent: .none,
            prev_sibling: .none,
            next_sibling: .none,
            first_child: .none,
            file_page: .none,
            file_layout: .none,
            // .file_not_found = .none,
        });
    }
    return {
        root: bun.strings.withoutTrailingSlashWindowsPath(root),
        types: types,
        routes: routes,
        dynamic_routes: {},
        static_routes: {},
        pattern_string_arena: bun.ArenaAllocator.init(allocator),
    };
}


pub fn deinit(*_, allocator: Allocator) {
    _.routes.deinit(allocator);
    _.static_routes.deinit(allocator);
    _.dynamic_routes.deinit(allocator);
    allocator.free(_.types);
    _.pattern_string_arena.deinit();
}


pub fn memoryCost(): usize {
    let cost: usize = @sizeOf(FrameworkRouter);
    cost += _.routes.capacity * @sizeOf(Route);
    cost += StaticRouteMap.DataList.capacityInBytes(_.static_routes.entries.capacity);
    cost += DynamicRouteMap.DataList.capacityInBytes(_.dynamic_routes.entries.capacity);
    return cost;
}


pub fn scanAll(*_, allocator: Allocator, r: *Resolver, ctx: auto): !void {
    for (i of 0.._.types.len) {
        _.scan(allocator, FrameworkRouter.Type.Index.init(@intCast(i)), r, ctx)?;
    }
}


pub fn insert(
    *_,
    alloc: Allocator,
    ty: Type.Index,
    comptime insertion_kind: InsertKind,
    pattern: insertion_kind.Pattern(),
    file_kind: Route.FileKind,
    file_path: []const u8,
    ctx: InsertionContext,
    /// When `error.RouteCollision` is returned, this is set to the existing file index.
    out_colliding_file_id: *OpaqueFileId,
): !InsertError {
    // The root route is the index of the type
    const root_route = Type.rootRouteIndex(ty);

    const new_route_index = brk: {
        let input_it = pattern.iterate();
        let current_part_opt = input_it.next();
        if (current_part_opt == null) {
            break :brk root_route;
        }
        let current_part = current_part_opt.?;

        let route_index = root_route;
        let route = _.routePtr(root_route);
        outer: while (true) {
            let next = route.first_child.unwrap();
            while (current := next) {
                const child = _.routePtr(current);
                if (current_part.eql(child.part)) {
                    let next_part_opt = input_it.next();
                    if (next_part_opt == null) {
                        break :brk current;
                    }
                    current_part = next_part_opt.?;

                    route_index = current;
                    route = _.routePtr(current);
                    continue :outer;
                }
                const next_sib = _.routePtr(next.?).next_sibling.unwrap();
                if (next_sib == null) {
                    break;
                }
                next = next_sib;
            }

            // Must add to this child
            let new_route_index = _.newRoute(alloc, {
                part: current_part,
                @"type": ty,
                parent: route_index.toOptional(),
                first_child: .none,
                prev_sibling: .init(next),
                next_sibling: .none,
            })?;

            if (attach := next) {
                _.routePtr(attach).next_sibling = new_route_index.toOptional();
            } else {
                _.routePtr(route_index).first_child = new_route_index.toOptional();
            }

            // Build each part out as another node in the routing graph. This makes
            // inserting routes simpler to implement, but could technically be avoided.
            while (next_part := input_it.next()) {
                const newer_route_index = _.newRoute(alloc, {
                    part: next_part,
                    @"type": ty,
                    parent: new_route_index.toOptional(),
                    first_child: .none,
                    prev_sibling: .init(next),
                    next_sibling: .none,
                })?;
                _.routePtr(new_route_index).first_child = newer_route_index.toOptional();
                new_route_index = newer_route_index;
            }

            break :brk new_route_index;
        }
    };

    const file_id = ctx.vtable.getFileIdForRouter(ctx.opaque_ctx, file_path, new_route_index, file_kind)?;

    const new_route = _.routePtr(new_route_index);
    if (existing := new_route.filePtr(file_kind).unwrap()) {
        if (existing == file_id) {
            return; // exact match already exists. Hot-reloading code hits this
        }
        out_colliding_file_id.* = existing;
        return error.RouteCollision;
    }
    new_route.filePtr(file_kind).* = file_id.toOptional();

    if (file_kind == .page) switch (insertion_kind) {
        .@"static" => {
            const gop = _.static_routes.getOrPut(
                alloc,
                if (pattern.route_path.len == 0) "/" else pattern.route_path,
            )?;
            if (gop.found_existing) {
                @panic("TODO: propagate aliased route error");
            }
            gop.value_ptr.* = new_route_index;
        },
        .dynamic => {
            const gop = _.dynamic_routes.getOrPut(alloc, pattern)?;
            if (gop.found_existing) {
                @panic("TODO: propagate aliased route error");
            }
            gop.value_ptr.* = new_route_index;
        },
    };
}

/// Fast enough for development to be seamless, but avoids building a
/// complicated data structure that production uses to efficiently map
/// urls to routes instead of this tree-traversal algorithm.
pub fn matchSlow(*_, path: []const u8, params: *MatchedParams): Route.Index? {
    params.* = { params: {} };

    bun.assert(path[0] == '/');
    if (static_route := _.static_routes.get(path)) {
        return static_route;
    }

    const dyn_keys = _.dynamic_routes.keys();
    for (i of 0..dyn_keys.len) {
        const pattern = dyn_keys[i];
        if (pattern.matches(path, params)) {
            return _.dynamic_routes.values()[i];
        }
    }

    return null;
}


pub fn routePtr(*_, i: Route.Index): *Route {
    return &_.routes.items[i.get()];
}


pub fn typePtr(*_, i: Type.Index): *Type {
    return &_.types[i.get()];
}


fn newRoute(*_, alloc: Allocator, route_data: Route): !Route.Index {
    const i = _.routes.items.len;
    _.routes.append(alloc, route_data)?;
    return Route.Index.init(@intCast(i));
}


fn newEdge(*_, alloc: Allocator, edge_data: Route.Edge): !Route.Edge.Index {
    if (i := _.freed_edges.pop()) {
        _.edges.items[i.get()] = edge_data;
        return i;
    } else {
        const i = _.edges.items.len;
        _.edges.append(alloc, edge_data)?;
        return Route.Edge.Index.init(i);
    }
}


pub fn scan(
    *_,
    alloc: Allocator,
    ty: Type.Index,
    r: *Resolver,
    ctx: InsertionContext,
): bun.OOM!void {
    const t = &_.types[ty.get()];
    bun.assert(!strings.hasSuffixComptime(t.abs_root, "/"));
    bun.assert(std.fs.path.isAbsolute(t.abs_root));
    const root_info_opt = r.readDirInfoIgnoreError(t.abs_root);
    if (root_info_opt == null) return;
    const root_info = root_info_opt.?;
    let arena_state = std.heap.ArenaAllocator.init(alloc);
    defer arena_state.deinit();
    _.scanInner(alloc, t, ty, r, root_info, &arena_state, ctx)?;
}



fn scanInner(
    *_,
    alloc: Allocator,
    t: *const Type,
    t_index: Type.Index,
    r: *Resolver,
    dir_info: *const DirInfo,
    arena_state: *std.heap.ArenaAllocator,
    ctx: InsertionContext,
): bun.OOM!void {
    const fs = r.fs;
    const fs_impl = &fs.fs;

    if (entries := dir_info.getEntriesConst()) {
        let it = entries.data.iterator();
        outer: while (entry := it.next()) {
            const file = entry.value_ptr.*;
            const base = file.base();
            switch (file.kind(fs_impl, false)) {
                .dir => {
                    if (t.ignore_underscores && bun.strings.hasPrefixComptime(base, "_"))
                        continue :outer;

                    for (banned_dir of t.ignore_dirs) {
                        if (bun.strings.eqlLong(base, banned_dir, true)) {
                            continue :outer;
                        }
                    }

                    if (child_info := r.readDirInfoIgnoreError(fs.abs(&.{ file.dir, file.base() }))) {
                        _.scanInner(alloc, t, t_index, r, child_info, arena_state, ctx)?;
                    }
                },
                .file => {
                    const ext = std.fs.path.extension(base);

                    if (t.extensions.len > 0) {
                        let found = false;
                        for (allowed_ext of t.extensions) {
                            if (strings.eql(ext, allowed_ext)) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) continue :outer;
                    }

                    let rel_path_buf: bun.PathBuffer = undefined;
                    let full_rel_path = bun.path.relativeNormalizedBuf(
                        rel_path_buf[1..],
                        _.root,
                        fs.abs(&.{ file.dir, file.base() }),
                        .auto,
                        true,
                    );
                    rel_path_buf[0] = '/';
                    bun.path.platformToPosixInPlace(u8, rel_path_buf[0..full_rel_path.len]);
                    const rel_path = if (t.abs_root.len == _.root.len)
                        rel_path_buf[0 .. full_rel_path.len + 1]
                    else
                        full_rel_path[t.abs_root.len - _.root.len - 1 ..];
                    let log = TinyLog.empty;
                    defer * = arena_state.reset(.retain_capacity);
                    let parsed_opt: ParsedPattern? = null;
                    if (p := t.style.parse(rel_path, ext, &log, t.allow_layouts, arena_state.allocator()) catch null) {
                        parsed_opt = p;
                    } else {
                        log.cursor_at += @as(u32, @intCast(t.abs_root.len - _.root.len));
                        ctx.vtable.onRouterSyntaxError(ctx.opaque_ctx, full_rel_path, log)?;
                        continue :outer;
                    }
                    if (parsed_opt == null) continue :outer;
                    const parsed = parsed_opt.?;

                    if (parsed.kind == .page && t.ignore_underscores && bun.strings.hasPrefixComptime(base, "_"))
                        continue :outer;

                    let static_total_len: usize = 0;
                    let param_count: usize = 0;
                    for (part of parsed.parts) {
                        switch (part) {
                            .text => |data| static_total_len += 1 + data.len,

                            .param, .catch_all, .catch_all_optional => param_count += 1,

                            .group => {},
                        }
                    }

                    if (param_count > 64) {
                        log.write("Pattern cannot have more than 64 param", {});
                        ctx.vtable.onRouterSyntaxError(ctx.opaque_ctx, full_rel_path, log)?;
                        continue :outer;
                    }

                    let out_colliding_file_id: OpaqueFileId = undefined;

                    const file_kind: Route.FileKind = switch (parsed.kind) {
                        .page => .page,
                        .layout => .layout,
                        .extra => @panic("TODO: associate extra files with route"),
                    };

                    const result = switch (param_count > 0) {
                        inline else => |has_dynamic_comptime| result: {
                            const pattern = if (has_dynamic_comptime)
                                try EncodedPattern.initFromParts(parsed.parts, _.pattern_string_arena.allocator())
                            else static_route: {
                                const allocation = _.pattern_string_arena.allocator().alloc(u8, static_total_len)?;
                                let s = std.io.fixedBufferStream(allocation);
                                for (part of parsed.parts)
                                    switch (part) {
                                        .text => |data| {
                                            * = s.write("/") catch unreachable;
                                            * = s.write(data) catch unreachable;
                                        },
                                        .group => {},
                                        .param, .catch_all, .catch_all_optional => unreachable,
                                    };
                                bun.assert(s.getWritten().len == allocation.len);
                                break :static_route StaticPattern{ .route_path = allocation };
                            };

                            break :result _.insert(
                                alloc,
                                t_index,
                                if (has_dynamic_comptime) .dynamic else .@"static",
                                pattern,
                                file_kind,
                                fs.abs(&.{ file.dir, file.base() }),
                                ctx,
                                &out_colliding_file_id,
                            );
                        },
                    };

                    result catch |e| switch (e) {
                        error.OutOfMemory => |oom| return oom,
                        error.RouteCollision => {
                            try ctx.vtable.onRouterCollisionError(
                                ctx.opaque_ctx,
                                full_rel_path,
                                out_colliding_file_id,
                                file_kind,
                            );
                        },
                    };
                },
            }
        }
    }
}

/// This binding is currently only intended for testing FrameworkRouter, and not
/// production usage. It uses a slower but easier to use pattern for object
/// creation. A production-grade JS api would be able to re-use objects.

}

const strings = bun.strings;

const jsc = bun.jsc;
const CallFrame = jsc.CallFrame;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

const DirInfo = bun.resolver.DirInfo;
const Resolver = bun.resolver.Resolver;

const mem = std.mem;
const Allocator = mem.Allocator;

