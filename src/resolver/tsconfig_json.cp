import cache from "../bundler/cache.zig";
import options from "../bundler/options.zig";
import std from "std";
import bun from "bun";
const Environment = bun.Environment;
const assert = bun.assert;
const js_ast = bun.ast;
const js_lexer = bun.js_lexer;
const logger = bun.logger;
const strings = bun.strings;

const PathsMap = bun.StringArrayHashMap([]string);

fn FlagSet(comptime Type: type): type {
    return std.EnumSet(std.meta.FieldEnum(Type));
}

const JSXFieldSet = FlagSet(options.JSX.Pragma);

const ImportsNotUsedAsValueList = bun.ComptimeStringMap(ImportsNotUsedAsValueEnum, [
    ["preserve", .preserve],
    ["error", .@"err"],
    ["remove", .remove],
]);

pub enum ImportsNotUsedAsValueEnum {
    preserve,
    @"err",
    remove,
    invalid,

    pub const List = ImportsNotUsedAsValueList;
}

pub struct TSConfigJSON {
    pub const @"new" = bun.TrivialNew(TSConfigJSON);
    pub const ImportsNotUsedAsValue = ImportsNotUsedAsValueEnum;

    abs_path: string,
    base_url: string = "",
    base_url_for_paths: string = "",
    extends: string = "",
    paths: PathsMap,
    jsx: options.JSX.Pragma = options.JSX.Pragma{},
    jsx_flags: JSXFieldSet = JSXFieldSet{},
    use_define_for_class_fields: bool? = null,
    preserve_imports_not_used_as_values: bool? = false,
    emit_decorator_metadata: bool = false,
    experimental_decorators: bool = false,

    pub fn hasBaseURL(this): bool {
        return this.base_url.len > 0;
    }

    pub fn mergeJSX(this, let current: options.JSX.Pragma): options.JSX.Pragma {
        if (this.jsx_flags.contains(.factory)) {
            current.factory = this.jsx.factory;
        }

        if (this.jsx_flags.contains(.fragment)) {
            current.fragment = this.jsx.fragment;
        }

        if (this.jsx_flags.contains(.import_source)) {
            current.import_source = this.jsx.import_source;
        }

        if (this.jsx_flags.contains(.runtime)) {
            current.runtime = this.jsx.runtime;
        }

        if (this.jsx_flags.contains(.development)) {
            current.development = this.jsx.development;
        }

        return current;
    }

    static fn strReplacingTemplates(allocator: std.mem.Allocator, input: string, source: *const logger.Source): string ! bun.OOM {
        let remaining = input;
        let string_builder = bun.StringBuilder{};
        const configDir = source.path.sourceDir();

        while (index := strings.indexOf(remaining, "${configDir}")) {
            string_builder.count(remaining[0..index]);
            string_builder.count(configDir);
            remaining = remaining[index + "${configDir}".len ..];
        }

        if (remaining.len == input.len) {
            return input;
        }

        string_builder.countZ(remaining);
        string_builder.allocate(allocator)?;

        remaining = input;
        while (index := strings.indexOf(remaining, "${configDir}")) {
            _ = string_builder.append(remaining[0..index]);
            _ = string_builder.append(configDir);
            remaining = remaining[index + "${configDir}".len ..];
        }

        _ = string_builder.appendZ(remaining);

        return string_builder.allocatedSlice()[0 .. string_builder.len - 1];
    }

    pub static fn parse(
        allocator: std.mem.Allocator,
        log: *logger.Log,
        source: *const logger.Source,
        json_cache: *cache.Json,
    ): (*TSConfigJSON)?! {
        const json: js_ast.Expr = (json_cache.parseTSConfig(log, source, allocator) catch null) ?? return null;

        bun.analytics.Features.tsconfig += 1;

        let result: TSConfigJSON = TSConfigJSON{ abs_path: source.path.text, paths: PathsMap.init(allocator) };
        errdefer allocator.free(result.paths);
        if (extends_value := json.asProperty("extends")) {
            if (!source.path.isNodeModule()) {
                if (str := extends_value.expr.asString(allocator) ?? null) {
                    result.extends = str;
                }
            }
        }
        let has_base_url = false;

        if (compiler_opts := json.asProperty("compilerOptions")) {
            if (base_url_prop := compiler_opts.expr.asProperty("baseUrl")) {
                if (base_url := base_url_prop.expr.asString(allocator)) {
                    result.base_url = strReplacingTemplates(allocator, base_url, source) catch return null;
                    has_base_url = true;
                }
            }

            if (emit_decorator_metadata_prop := compiler_opts.expr.asProperty("emitDecoratorMetadata")) {
                if (val := emit_decorator_metadata_prop.expr.asBool()) {
                    result.emit_decorator_metadata = val;
                }
            }

            if (experimental_decorators_prop := compiler_opts.expr.asProperty("experimentalDecorators")) {
                if (val := experimental_decorators_prop.expr.asBool()) {
                    result.experimental_decorators = val;
                }
            }

            if (jsx_prop := compiler_opts.expr.asProperty("jsxFactory")) {
                if (str := jsx_prop.expr.asString(allocator)) {
                    result.jsx.factory = parseMemberExpressionForJSX(log, source, jsx_prop.loc, str, allocator)?;
                    result.jsx_flags.insert(.factory);
                }
            }

            if (jsx_prop := compiler_opts.expr.asProperty("jsxFragmentFactory")) {
                if (str := jsx_prop.expr.asString(allocator)) {
                    result.jsx.fragment = parseMemberExpressionForJSX(log, source, jsx_prop.loc, str, allocator)?;
                    result.jsx_flags.insert(.fragment);
                }
            }

            if (jsx_prop := compiler_opts.expr.asProperty("jsx")) {
                if (str := jsx_prop.expr.asString(allocator)) {
                    const str_lower = allocator.alloc(u8, str.len) catch unreachable;
                    defer allocator.free(str_lower);
                    _ = strings.copyLowercase(str, str_lower);
                    if (runtime := options.JSX.RuntimeMap.get(str_lower)) {
                        result.jsx.runtime = runtime.runtime;
                        result.jsx_flags.insert(.runtime);

                        if (dev := runtime.development) {
                            result.jsx.development = dev;
                            result.jsx_flags.insert(.development);
                        }
                    }
                }
            }

            if (jsx_prop := compiler_opts.expr.asProperty("jsxImportSource")) {
                if (str := jsx_prop.expr.asString(allocator)) {
                    if (str.len >= "solid-js".len && strings.eqlComptime(str[0.."solid-js".len], "solid-js")) {
                        result.jsx.runtime = .solid;
                        result.jsx_flags.insert(.runtime);
                    }

                    result.jsx.package_name = str;
                    result.jsx.setImportSource(allocator);
                    result.jsx_flags.insert(.import_source);
                }
            }

            if (use_define_value_prop := compiler_opts.expr.asProperty("useDefineForClassFields")) {
                if (val := use_define_value_prop.expr.asBool()) {
                    result.use_define_for_class_fields = val;
                }
            }

            if (jsx_prop := compiler_opts.expr.asProperty("importsNotUsedAsValues")) {
                if (str := jsx_prop.expr.asString(allocator)) {
                    switch (ImportsNotUsedAsValue.List.get(str) ?? ImportsNotUsedAsValue.invalid) {
                        .preserve, .@"err" => {
                            result.preserve_imports_not_used_as_values = true;
                        },
                        .remove => {},
                        else => {
                            log.addRangeWarningFmt(source, source.rangeOfString(jsx_prop.loc), allocator, "Invalid value \"{s}\" for \"importsNotUsedAsValues\"", [str]) catch {};
                        },
                    }
                }
            }

            if (prefixes := compiler_opts.expr.asProperty("moduleSuffixes")) {
                if (!source.path.isNodeModule()) {
                    if (let array := prefixes.expr.asArray()) {
                        while (*element := array.next()) {
                            if (str := element.asString(allocator)) {
                                if (str.len > 0) {
                                    log.addWarning(source, prefixes.loc, "moduleSuffixes is not supported yet") catch {};
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (paths_prop := compiler_opts.expr.asProperty("paths")) {
                switch (paths_prop.expr.data) {
                    .e_object => {
                        defer {
                            bun.analytics.Features.tsconfig_paths += 1;
                        }
                        let paths = paths_prop.expr.data.e_object;
                        result.base_url_for_paths = if (result.base_url.len > 0) result.base_url else ".";
                        result.paths = PathsMap.init(allocator);
                        for (property of paths.properties.slice()) {
                            const key_prop = property.key ?? continue;
                            const key = key_prop.asString(allocator) ?? continue;

                            if (!TSConfigJSON.isValidTSConfigPathPattern(key, log, source, key_prop.loc, allocator)) {
                                continue;
                            }

                            const value_prop = property.value ?? continue;

                            switch (value_prop.data) {
                                .e_array => {
                                    const array = value_prop.data.e_array.slice();

                                    if (array.len > 0) {
                                        let values = allocator.alloc(string, array.len) catch unreachable;
                                        errdefer allocator.free(values);
                                        let count: usize = 0;
                                        for (expr of array) {
                                            if (str_ := expr.asString(allocator)) {
                                                const str = strReplacingTemplates(allocator, str_, source) catch return null;
                                                errdefer allocator.free(str);
                                                if (TSConfigJSON.isValidTSConfigPathPattern(
                                                    str,
                                                    log,
                                                    source,
                                                    expr.loc,
                                                    allocator,
                                                ) &&
                                                    (has_base_url ||
                                                        TSConfigJSON.isValidTSConfigPathNoBaseURLPattern(
                                                            str,
                                                            log,
                                                            source,
                                                            allocator,
                                                            expr.loc,
                                                        )))
                                                {
                                                    values[count] = str;
                                                    count += 1;
                                                }
                                            }
                                        }
                                        if (count > 0) {
                                            if (count < values.len) {
                                                values = allocator.realloc(values, count) catch values;
                                            }
                                            result.paths.put(
                                                key,
                                                values[0..count],
                                            ) catch unreachable;
                                        } else {
                                            allocator.free(values);
                                        }
                                    }
                                },
                                else => {
                                    log.addRangeWarningFmt(
                                        source,
                                        source.rangeOfString(key_prop.loc),
                                        allocator,
                                        "Substitutions for pattern \"{s}\" should be an array",
                                        [key],
                                    ) catch {};
                                },
                            }
                        }
                    },
                    else => {},
                }
            }
        }

        if (Environment.isDebug && has_base_url) {
            assert(result.base_url.len > 0);
        }

        return TSConfigJSON.@"new"(result);
    }

    pub static fn isValidTSConfigPathPattern(text: string, log: *logger.Log, source: *const logger.Source, loc: logger.Loc, allocator: std.mem.Allocator): bool {
        let found_asterisk = false;
        for (c of text) {
            if (c == '*') {
                if (found_asterisk) {
                    const r = source.rangeOfString(loc);
                    log.addRangeWarningFmt(source, r, allocator, "Invalid pattern \"{s}\", must have at most one \"*\" character", [text]) catch {};
                    return false;
                }
                found_asterisk = true;
            }
        }

        return true;
    }

    pub static fn parseMemberExpressionForJSX(log: *logger.Log, source: *const logger.Source, loc: logger.Loc, text: string, allocator: std.mem.Allocator): ([]string)! {
        if (text.len == 0) {
            return &([]);
        }
        const parts_count = std.mem.count(u8, text, ".") + @as(usize, @intFromBool(text[text.len - 1] != '.'));
        let parts = std.array_list.Managed(string).initCapacity(allocator, parts_count) catch unreachable;

        if (parts_count == 1) {
            if (!js_lexer.isIdentifier(text)) {
                const warn = source.rangeOfString(loc);
                log.addRangeWarningFmt(source, warn, allocator, "Invalid JSX member expression: \"{s}\"", [text]) catch {};
                parts.deinit();
                return &([]);
            }

            parts.appendAssumeCapacity(text);
            return parts.items;
        }

        let iter = std.mem.tokenizeScalar(u8, text, '.');

        while (part := iter.next()) {
            if (!js_lexer.isIdentifier(part)) {
                const warn = source.rangeOfString(loc);
                log.addRangeWarningFmt(source, warn, allocator, "Invalid JSX member expression: \"{s}\"", [part]) catch {};
                parts.deinit();
                return &([]);
            }
            parts.appendAssumeCapacity(part);
        }

        return parts.items;
    }

    pub static fn isSlash(c: u8): bool {
        return c == '/' || c == '\\';
    }

    pub static fn isValidTSConfigPathNoBaseURLPattern(text: string, log: *logger.Log, source: *const logger.Source, allocator: std.mem.Allocator, loc: logger.Loc): bool {
        let c0: u8 = 0;
        let c1: u8 = 0;
        let c2: u8 = 0;
        const n = text.len;

        switch (n) {
            0 => {
                return false;
            },
            1 => {
                return text[0] == '.';
            },
            2 => {
                return text[0] == '.' && (text[1] == '.' || text[1] == '\\' || text[1] == '/');
            },
            else => {
                c0 = text[0];
                c1 = text[1];
                c2 = text[2];
            },
        }

        if (c0 == '.' && (TSConfigJSON.isSlash(c1) || (c1 == '.' && TSConfigJSON.isSlash(c2)))) {
            return true;
        }

        if (c1 == ':' && TSConfigJSON.isSlash(c2)) {
            switch (c0) {
                'a'...'z', 'A'...'Z' => {
                    return true;
                },
                else => {},
            }
        }

        if (TSConfigJSON.isSlash(c0)) {
            return true;
        }

        const r = source.rangeOfString(loc);
        log.addRangeWarningFmt(source, r, allocator, "Non-relative path \"{s}\" is not allowed when \"baseUrl\" is not set (did you forget a leading \"./\"?)", [text]) catch {};
        return false;
    }

    pub fn deinit(*this) {
        this.paths.deinit();
        bun.destroy(this);
    }
}
