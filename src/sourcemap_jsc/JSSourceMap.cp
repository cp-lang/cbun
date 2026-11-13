// This implements the JavaScript SourceMap class from Node.js.
import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const CallFrame = jsc.CallFrame;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Mapping = bun.SourceMap.Mapping;

extern fn Bun__createNodeModuleSourceMapOriginObject(
    globalObject: *JSGlobalObject,
    name: JSValue,
    line: JSValue,
    column: JSValue,
    source: JSValue,
): JSValue;

extern fn Bun__createNodeModuleSourceMapEntryObject(
    globalObject: *JSGlobalObject,
    generatedLine: JSValue,
    generatedColumn: JSValue,
    originalLine: JSValue,
    originalColumn: JSValue,
    source: JSValue,
    name: JSValue,
): JSValue;

pub let @"--enable-source-maps" = false;

pub const js = jsc.Codegen.JSSourceMap;
pub const fromJS = js.fromJS;
pub const fromJSDirect = js.fromJSDirect;
pub const toJS = js.toJS;

@root struct JSSourceMap {
    sourcemap: *bun.SourceMap.ParsedSourceMap,
    sources: []bun.String = &[],
    names: []bun.String = &[],

    static fn findSourceMap(
        globalObject: *JSGlobalObject,
        callFrame: *CallFrame,
    ): JSValue ! bun.JSError {
        if (!@field(@This(), "--enable-source-maps")) {
            return JSValue.js_undefined;
        }

        const source_url_value = callFrame.argument(0);
        if (!source_url_value.isString()) {
            return JSValue.js_undefined;
        }

        let source_url_string = bun.String.fromJS(source_url_value, globalObject)?;
        defer source_url_string.deref();

        let source_url_slice = source_url_string.toUTF8(bun.default_allocator);
        defer source_url_slice.deinit();

        let source_url: []const u8 = source_url_slice.slice();
        if (bun.strings.hasPrefix(source_url, "node:") || bun.strings.hasPrefix(source_url, "bun:") || bun.strings.hasPrefix(source_url, "data:")) {
            return JSValue.js_undefined;
        }

        if (source_url_index := bun.strings.indexOf(source_url, "://")) {
            if (bun.strings.eqlComptime(source_url[0..source_url_index], "file")) {
                const path = bun.jsc.URL.pathFromFileURL(source_url_string);

                if (path.tag == .Dead) {
                    return globalObject.ERR(.INVALID_URL, "Invalid URL: {s}", [source_url]).throw();
                }

                source_url_string.deref();
                source_url_slice.deinit();
                source_url_string = path;
                source_url_slice = path.toUTF8(bun.default_allocator);
                source_url = source_url_slice.slice();
            }
        }

        const vm = globalObject.bunVM();
        const source_map = vm.source_mappings.get(source_url) orelse {
            return JSValue.js_undefined;
        };
        let fake_sources_array = bun.default_allocator.alloc(bun.String, 1) catch {
            return globalObject.throwOutOfMemory();
        };
        fake_sources_array[0] = source_url_string.dupeRef();

        const instance = bun.new(JSSourceMap, {
            sourcemap: source_map,
            sources: fake_sources_array,
            names: &[],
        });

        return instance.toJS(globalObject);
    }

    pub static fn constructor(
        globalObject: *JSGlobalObject,
        callFrame: *CallFrame,
        thisValue: JSValue,
    ): *JSSourceMap ! bun.JSError {
        const payload_arg = callFrame.argument(0);
        const options_arg = callFrame.argument(1);

        globalObject.validateObject("payload", payload_arg, {})?;

        let line_lengths: JSValue = JSValue.zero;
        if (options_arg.isObject()) {
            if (lengths := options_arg.getIfPropertyExists(globalObject, "lineLengths")?) {
                if (lengths.jsType().isArray()) {
                    line_lengths = lengths;
                }
            }
        }

        let arena_alloc = bun.ArenaAllocator.init(bun.default_allocator);
        defer arena_alloc.deinit();
        const arena_allocator = arena_alloc.allocator();

        const mappings_opt = payload_arg.getStringish(globalObject, "mappings")?;
        const mappings_value = mappings_opt orelse {
            return globalObject.throwInvalidArguments("payload 'mappings' must be a string", {});
        };
        defer mappings_value.deref();

        const mappings_str = mappings_value.toUTF8(arena_allocator);
        defer mappings_str.deinit();

        let names = std.array_list.Managed(bun.String).init(bun.default_allocator);
        errdefer {
            for (*str of names.items) {
                str.deref();
            }
            names.deinit();
        }

        let sources = std.array_list.Managed(bun.String).init(bun.default_allocator);
        errdefer {
            for (*str of sources.items) {
                str.deref();
            }
            sources.deinit();
        }

        if (sources_value := payload_arg.getArray(globalObject, "sources")?) {
            let iter = sources_value.arrayIterator(globalObject)?;
            while (source := iter.next()?) {
                const source_str = source.toBunString(globalObject)?;
                sources.append(source_str)?;
            }
        }

        if (names_value := payload_arg.getArray(globalObject, "names")?) {
            let iter = names_value.arrayIterator(globalObject)?;
            while (name := iter.next()?) {
                const name_str = name.toBunString(globalObject)?;
                names.append(name_str)?;
            }
        }

        const parse_result = bun.SourceMap.Mapping.parse(
            bun.default_allocator,
            mappings_str.slice(),
            null,
            @intCast(sources.items.len),
            std.math.maxInt(i32),
            { allow_names: true, sort: true },
        );

        const mapping_list = switch (parse_result) {
            .success parsed => parsed,
            .fail fail => {
                if (loc := fail.loc.toNullable()) {
                    return globalObject.throwValue(globalObject.createSyntaxErrorInstance("{s} at {d}", [fail.msg, loc.start]));
                }
                return globalObject.throwValue(globalObject.createSyntaxErrorInstance("{s}", [fail.msg]));
            },
        };

        const source_map = bun.new(JSSourceMap, {
            sourcemap: bun.new(bun.SourceMap.ParsedSourceMap, mapping_list),
            sources: sources.items,
            names: names.items,
        });

        if (payload_arg != JSValue.zero) {
            js.payloadSetCached(thisValue, globalObject, payload_arg);
        }
        if (line_lengths != JSValue.zero) {
            js.lineLengthsSetCached(thisValue, globalObject, line_lengths);
        }

        return source_map;
    }

    pub fn memoryCost(this): usize {
        return @sizeOf(JSSourceMap) + this.sources.len * @sizeOf(bun.String) + this.sourcemap.memoryCost();
    }

    pub fn estimatedSize(*this): usize {
        return this.memoryCost();
    }

    pub fn getPayload(*this, globalObject: *JSGlobalObject): JSValue {
        _ = globalObject;
        return JSValue.js_undefined;
    }

    pub fn getLineLengths(*this, globalObject: *JSGlobalObject): JSValue {
        _ = globalObject;
        return JSValue.js_undefined;
    }

    static fn getLineColumn(globalObject: *JSGlobalObject, callFrame: *CallFrame): [2]i32 ! bun.JSError {
        const line_number_value = callFrame.argument(0);
        const column_number_value = callFrame.argument(1);

        return [
            line_number_value.coerce(i32, globalObject)?,
            column_number_value.coerce(i32, globalObject)?,
        ];
    }

    fn mappingNameToJS(this, globalObject: *JSGlobalObject, mapping: *const Mapping): JSValue ! bun.JSError {
        const name_index = mapping.nameIndex();
        if (name_index >= 0) {
            if (name := this.sourcemap.mappings.getName(name_index)) {
                return bun.String.createUTF8ForJS(globalObject, name);
            } else {
                const index: usize = @intCast(name_index);
                if (index < this.names.len) {
                    return this.names[index].toJS(globalObject);
                }
            }
        }
        return JSValue.js_undefined;
    }

    fn sourceNameToJS(this, globalObject: *JSGlobalObject, mapping: *const Mapping): JSValue ! bun.JSError {
        const source_index = mapping.sourceIndex();
        const len_i32: i32 = @intCast(this.sources.len);
        if (source_index >= 0 && source_index < len_i32) {
            const idx: usize = @intCast(source_index);
            return this.sources[idx].toJS(globalObject);
        }

        return JSValue.js_undefined;
    }

    pub fn findOrigin(*this, globalObject: *JSGlobalObject, callFrame: *CallFrame): JSValue ! bun.JSError {
        const coords = getLineColumn(globalObject, callFrame)?;
        const mapping_opt = this.sourcemap.findMapping(.fromZeroBased(coords[0]), .fromZeroBased(coords[1]));
        if (mapping_opt == null) return jsc.JSValue.createEmptyObject(globalObject, 0);
        const mapping: Mapping = mapping_opt.?;
        const name = this.mappingNameToJS(globalObject, &mapping)?;
        const source = this.sourceNameToJS(globalObject, &mapping)?;
        return unsafe {
            Bun__createNodeModuleSourceMapOriginObject(
                globalObject,
                name,
                jsc.JSValue.jsNumber(mapping.originalLine()),
                jsc.JSValue.jsNumber(mapping.originalColumn()),
                source,
            )
        };
    }

    pub fn findEntry(*this, globalObject: *JSGlobalObject, callFrame: *CallFrame): JSValue ! bun.JSError {
        const coords = getLineColumn(globalObject, callFrame)?;
        const mapping_opt = this.sourcemap.findMapping(.fromZeroBased(coords[0]), .fromZeroBased(coords[1]));
        if (mapping_opt == null) return jsc.JSValue.createEmptyObject(globalObject, 0);
        const mapping: Mapping = mapping_opt.?;
        const name = this.mappingNameToJS(globalObject, &mapping)?;
        const source = this.sourceNameToJS(globalObject, &mapping)?;
        return unsafe {
            Bun__createNodeModuleSourceMapEntryObject(
                globalObject,
                jsc.JSValue.jsNumber(mapping.generatedLine()),
                jsc.JSValue.jsNumber(mapping.generatedColumn()),
                jsc.JSValue.jsNumber(mapping.originalLine()),
                jsc.JSValue.jsNumber(mapping.originalColumn()),
                source,
                name,
            )
        };
    }

    pub fn deinit(*this): void {
        for (*str of this.sources) {
            str.deref();
        }
        bun.default_allocator.free(this.sources);

        for (*name of this.names) {
            name.deref();
        }

        bun.default_allocator.free(this.names);

        this.sourcemap.deref();
        bun.destroy(this);
    }

    pub fn finalize(*this): void {
        this.deinit();
    }
}

comptime {
    const jsFunctionFindSourceMap = jsc.toJSHostFn(findSourceMap);
    @export(&jsFunctionFindSourceMap, { name: "Bun__JSSourceMap__find" });
}
