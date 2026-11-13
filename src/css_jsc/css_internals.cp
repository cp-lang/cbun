import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const jsc = bun.jsc;
const JSGlobalObject = bun.jsc.JSGlobalObject;
const JSValue = bun.jsc.JSValue;
const ImportRecordList = bun.BabyList(bun.ImportRecord);

enum TestKind {
    normal,
    minify,
    prefix,
}

enum TestCategory {
    normal,
    parser_options,
}

pub fn minifyErrorTestWithOptions(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .minify, .parser_options);
}

pub fn minifyTestWithOptions(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .minify, .parser_options);
}

pub fn prefixTestWithOptions(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .prefix, .parser_options);
}

pub fn testWithOptions(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .normal, .parser_options);
}

pub fn minifyTest(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .minify, .normal);
}

pub fn prefixTest(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .prefix, .normal);
}

pub fn _test(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    return testingImpl(globalThis, callframe, .normal, .normal);
}

pub fn testingImpl(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame, @comptime test_kind: TestKind, @comptime test_category: TestCategory): jsc.JSValue ! bun.JSError {
    let arena_alloc = bun.ArenaAllocator.init(bun.default_allocator);
    defer arena_alloc.deinit();
    const alloc = arena_alloc.allocator();

    const arguments_ = callframe.arguments_old(3);
    let arguments = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments_.slice());
    const source_arg_opt = arguments.nextEat();
    if (source_arg_opt == null) return globalThis.throw("minifyTestWithOptions: expected 2 arguments, got 0", []);
    const source_arg = source_arg_opt.?;
    if (!source_arg.isString()) {
        return globalThis.throw("minifyTestWithOptions: expected source to be a string", []);
    }
    const source_bunstr = source_arg.toBunString(globalThis)?;
    defer source_bunstr.deref();
    const source = source_bunstr.toUTF8(bun.default_allocator);
    defer source.deinit();

    const expected_arg_opt = arguments.nextEat();
    if (expected_arg_opt == null) return globalThis.throw("minifyTestWithOptions: expected 2 arguments, got 1", []);
    const expected_arg = expected_arg_opt.?;
    if (!expected_arg.isString()) {
        return globalThis.throw("minifyTestWithOptions: expected `expected` arg to be a string", []);
    }
    const expected_bunstr = expected_arg.toBunString(globalThis)?;
    defer expected_bunstr.deref();
    const expected = expected_bunstr.toUTF8(bun.default_allocator);
    defer expected.deinit();

    const browser_options_arg = arguments.nextEat();

    let log: bun.logger.Log = bun.logger.Log.init(alloc);
    defer log.deinit();

    let browsers: bun.css.targets.Browsers? = null;
    const parser_options = parser_options: {
        let opts: bun.css.ParserOptions = bun.css.ParserOptions.default(alloc, &log);
        switch (test_category) {
            .normal => {
                if (optargs := browser_options_arg) {
                    if (optargs.isObject()) {
                        browsers = targetsFromJS(globalThis, optargs)?;
                    }
                }
            },
            .parser_options => {
                if (optargs := browser_options_arg) {
                    if (optargs.isObject()) {
                        parserOptionsFromJS(globalThis, alloc, &opts, optargs)?;
                    }
                }
            },
        }
        break :parser_options opts;
    };

    let import_records = ImportRecordList{};
    switch (bun.css.StyleSheet(bun.css.DefaultAtRule).parse(
        alloc,
        source.slice(),
        parser_options,
        &import_records,
        bun.bundle_v2.Index.invalid,
    )) {
        .result => |ret| {
            let [stylesheet, extra]: [bun.css.StyleSheet(bun.css.DefaultAtRule), bun.css.StyleSheet(bun.css.DefaultAtRule).Extra] = ret;
            let minify_options: bun.css.MinifyOptions = bun.css.MinifyOptions.default();
            minify_options.targets.browsers = browsers;
            switch (stylesheet.minify(alloc, minify_options, &extra)) {
                .result => {},
                .err => |*e| {
                    return globalThis.throwValue(e.toErrorInstance(globalThis)?);
                },
            }

            const symbols = bun.ast.Symbol.Map{};
            let local_names = bun.css.LocalsResultsMap{};
            const result = switch (stylesheet.toCss(
                alloc,
                bun.css.PrinterOptions{
                    minify: switch (test_kind) {
                        .minify => true,
                        .normal => false,
                        .prefix => false,
                    },
                    targets: {
                        browsers: minify_options.targets.browsers,
                    },
                },
                bun.css.ImportInfo.initOutsideOfBundler(&import_records),
                &local_names,
                &symbols,
            )) {
                .result => |res| res,
                .err => |*e| {
                    return globalThis.throwValue(e.toErrorInstance(globalThis)?);
                },
            };

            return bun.String.fromBytes(result.code).toJS(globalThis);
        },
        .err => |e| {
            if (log.hasErrors()) {
                return log.toJS(globalThis, bun.default_allocator, "parsing failed:");
            }
            return globalThis.throw("parsing failed: {f}", [e.kind]);
        },
    }
}

fn parserOptionsFromJS(globalThis: *jsc.JSGlobalObject, allocator: Allocator, opts: *bun.css.ParserOptions, jsobj: JSValue): void ! bun.JSError {
    _ = allocator;
    if (val := jsobj.getTruthy(globalThis, "flags")?) {
        if (val.isArray()) {
            let iter = val.arrayIterator(globalThis)?;
            while (item := iter.next()?) {
                const bunstr = item.toBunString(globalThis)?;
                defer bunstr.deref();
                const str = bunstr.toUTF8(bun.default_allocator);
                defer str.deinit();
                if (std.mem.eql(u8, str.slice(), "DEEP_SELECTOR_COMBINATOR")) {
                    opts.flags.deep_selector_combinator = true;
                } else {
                    return globalThis.throw("invalid flag: {s}", [str.slice()]);
                }
            }
        } else {
            return globalThis.throw("flags must be an array", []);
        }
    }
}

fn targetsFromJS(globalThis: *jsc.JSGlobalObject, jsobj: JSValue): bun.css.targets.Browsers ! bun.JSError {
    let targets = bun.css.targets.Browsers{};

    if (val := jsobj.getTruthy(globalThis, "android")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.android = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "chrome")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.chrome = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "edge")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.edge = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "firefox")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.firefox = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "ie")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.ie = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "ios_saf")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.ios_saf = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "opera")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.opera = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "safari")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.safari = bun.intFromFloat(u32, value);
            }
        }
    }
    if (val := jsobj.getTruthy(globalThis, "samsung")?) {
        if (val.isInt32()) {
            if (value := val.getNumber()) {
                targets.samsung = bun.intFromFloat(u32, value);
            }
        }
    }

    return targets;
}

pub fn attrTest(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    let arena_alloc = bun.ArenaAllocator.init(bun.default_allocator);
    defer arena_alloc.deinit();
    const alloc = arena_alloc.allocator();

    const arguments_ = callframe.arguments_old(4);
    let arguments = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments_.slice());
    const source_arg_opt = arguments.nextEat();
    if (source_arg_opt == null) return globalThis.throw("attrTest: expected 3 arguments, got 0", []);
    const source_arg = source_arg_opt.?;
    if (!source_arg.isString()) {
        return globalThis.throw("attrTest: expected source to be a string", []);
    }
    const source_bunstr = source_arg.toBunString(globalThis)?;
    defer source_bunstr.deref();
    const source = source_bunstr.toUTF8(bun.default_allocator);
    defer source.deinit();

    const expected_arg_opt = arguments.nextEat();
    if (expected_arg_opt == null) return globalThis.throw("attrTest: expected 3 arguments, got 1", []);
    const expected_arg = expected_arg_opt.?;
    if (!expected_arg.isString()) {
        return globalThis.throw("attrTest: expected `expected` arg to be a string", []);
    }
    const expected_bunstr = expected_arg.toBunString(globalThis)?;
    defer expected_bunstr.deref();
    const expected = expected_bunstr.toUTF8(bun.default_allocator);
    defer expected.deinit();

    const minify_arg_opt = arguments.nextEat();
    if (minify_arg_opt == null) return globalThis.throw("attrTest: expected 3 arguments, got 2", []);
    const minify_arg = minify_arg_opt.?;
    const minify = minify_arg.isBoolean() && minify_arg.toBoolean();

    let targets = bun.css.targets.Targets{};
    if (arg := arguments.nextEat()) {
        if (arg.isObject()) {
            targets.browsers = targetsFromJS(globalThis, arg)?;
        }
    }

    let log: bun.logger.Log = bun.logger.Log.init(alloc);
    defer log.deinit();

    const parser_options = bun.css.ParserOptions.default(alloc, &log);

    let import_records = ImportRecordList{};
    switch (bun.css.StyleAttribute.parse(alloc, source.slice(), parser_options, &import_records, bun.bundle_v2.Index.invalid)) {
        .result => |stylesheet_| {
            let stylesheet = stylesheet_;
            let minify_options: bun.css.MinifyOptions = bun.css.MinifyOptions.default();
            minify_options.targets = targets;
            stylesheet.minify(alloc, minify_options);

            const result = stylesheet.toCss(
                alloc,
                bun.css.PrinterOptions{
                    minify: minify,
                    targets: targets,
                },
                bun.css.ImportInfo.initOutsideOfBundler(&import_records),
            ) catch e => {
                bun.handleErrorReturnTrace(e, @errorReturnTrace());
                return jsc.JSValue.js_undefined;
            };

            return bun.String.fromBytes(result.code).toJS(globalThis);
        },
        .err => |e| {
            if (log.hasAny()) {
                return log.toJS(globalThis, bun.default_allocator, "parsing failed:");
            }
            return globalThis.throw("parsing failed: {f}", [e.kind]);
        },
    }
}
