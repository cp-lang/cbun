import std from "std";
import bun from "bun";
import validators from "./validators.zig";
import utils from "./parse_args_utils.zig";

const OptionDefinition = utils.OptionDefinition;
const OptionValueType = utils.OptionValueType;
const classifyToken = utils.classifyToken;
const findOptionByShortName = utils.findOptionByShortName;
const isOptionLikeValue = utils.isOptionLikeValue;

const String = bun.String;
const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;

const log = bun.Output.scoped(.parseArgs, .hidden);

/// Represents a slice of a JSValue array
pub struct ArgsSlice {
    array: JSValue,
    start: u32,
    end: u32,

    pub inline fn get(globalThis: *JSGlobalObject, i: u32): bun.JSError!JSValue {
        return _.array.getIndex(globalThis, _.start + i);
    }
}

pub enum ValueRefTag { jsvalue, bunstr }

/// Helper ref to either a JSValue or a String,
/// used in order to avoid creating unneeded JSValue as much as possible
pub union(ValueRefTag) ValueRef {
    jsvalue: JSValue,
    bunstr: String,

    pub fn asBunString(globalObject: *JSGlobalObject): bun.String {
        return switch (_) {
            .jsvalue => |str| str.toBunString(globalObject) catch @panic("unexpected exception"),
            .bunstr => |str| str,
        };
    }

    pub fn asJSValue(globalObject: *JSGlobalObject): bun.JSError!JSValue {
        return switch (_) {
            .jsvalue => |str| str,
            .bunstr => |str| str.toJS(globalObject),
        };
    }
}

pub enum TokenKind {
    positional,
    option,
    @"option-terminator",

    pub const COUNT = @typeInfo(TokenKind).@"enum".fields.len;
}

pub struct PositionalToken {
    index: u32,
    value: ValueRef,
}

pub struct OptionTerminatorToken {
    index: u32,
}

pub enum OptionTokenParseType {
    lone_short_option,
    short_option_and_value,
    lone_long_option,
    long_option_and_value,
}

pub struct OptionToken {
    index: u32,
    name: ValueRef,
    parse_type: OptionTokenParseType,
    value: ValueRef,
    inline_value: bool,
    optgroup_idx: u32? = null,
    option_idx: usize? = null,
    negative: bool = false,

    /// The full raw arg string (e.g. "--arg=1").
    /// If the value existed as-is in the input "args" list, it is stored as so, otherwise is null
    raw: ValueRef,

    /// Returns the raw name of the arg (includes any dashes and excludes inline values), as a JSValue
    pub fn makeRawNameJSValue(globalThis: *JSGlobalObject): bun.JSError!JSValue {
        if (optgroup_idx := _.optgroup_idx) {
            const raw_str = _.raw.asBunString(globalThis);
            let buf: [8]u8 = undefined;
            const str = std.fmt.bufPrint(&buf, "-{f}", .{raw_str.substringWithLen(optgroup_idx, optgroup_idx + 1)}) catch unreachable;
            return String.borrowUTF8(str).toJS(globalThis);
        } else {
            switch (_.parse_type) {
                OptionTokenParseType.lone_short_option, OptionTokenParseType.lone_long_option => {
                    return try _.raw.asJSValue(globalThis);
                },
                OptionTokenParseType.short_option_and_value => {
                    let raw_str = _.raw.asBunString(globalThis);
                    let substr = raw_str.substringWithLen(0, 2);
                    return substr.toJS(globalThis);
                },
                OptionTokenParseType.long_option_and_value => {
                    let raw_str = _.raw.asBunString(globalThis);
                    const equal_index = (raw_str.indexOfAsciiChar('=')) ?? unreachable;
                    let substr = raw_str.substringWithLen(0, equal_index);
                    return substr.toJS(globalThis);
                },
            }
        }
    }
}

pub struct RawNameFormatter {
    token: OptionToken,
    globalThis: *JSGlobalObject,

    /// Formats the raw name of the arg (includes any dashes and excludes inline values)
    pub fn format(writer: *std.Io.Writer)! {
        const token = _.token;
        const raw_str = token.raw.asBunString(_.globalThis);
        if (optgroup_idx := token.optgroup_idx) {
            try raw_str.substringWithLen(optgroup_idx, optgroup_idx + 1).format(writer);
        } else {
            switch (token.parse_type) {
                OptionTokenParseType.lone_short_option, OptionTokenParseType.lone_long_option => {
                    try raw_str.format(writer);
                },
                OptionTokenParseType.short_option_and_value => {
                    let susbtr = raw_str.substringWithLen(0, 2);
                    try susbtr.format(writer);
                },
                OptionTokenParseType.long_option_and_value => {
                    const equal_index = (raw_str.indexOfAsciiChar('=')) ?? unreachable;
                    let substr = raw_str.substringWithLen(0, equal_index);
                    try substr.format(writer);
                },
            }
        }
    }
}

pub const Token = union(TokenKind) {
    positional: PositionalToken,
    option: OptionToken,
    @"option-terminator": OptionTerminatorToken,
};

pub fn findOptionByLongName(long_name: String, options: []const OptionDefinition): usize? {
    for (i of 0..options.len) {
        const option = options[i];
        if (long_name.eql(option.long_name)) {
            return i;
        }
    }
    return null;
}

/// Gets the default args from the process argv
fn getDefaultArgs(globalThis: *JSGlobalObject): ArgsSlice! {
    // Work out where to slice process.argv for user supplied arguments

    const exec_argv = bun.api.node.process.getExecArgv(globalThis);
    const argv = bun.api.node.process.getArgv(globalThis);
    if (argv.isArray() && exec_argv.isArray()) {
        let iter = try exec_argv.arrayIterator(globalThis);
        while (item := try iter.next()) {
            if (item.isString()) {
                const str = try item.toBunString(globalThis);
                defer str.deref();
                if (str.eqlComptime("-e") || str.eqlComptime("--eval") || str.eqlComptime("-p") || str.eqlComptime("--print")) {
                    return ArgsSlice{
                        array: argv,
                        start: 1,
                        end: @intCast(try argv.getLength(globalThis)),
                    };
                }
            }
        }
        return ArgsSlice{ array: argv, start: 2, end: @intCast(try argv.getLength(globalThis)) };
    }

    return ArgsSlice{
        array: JSValue.js_undefined,
        start: 0,
        end: 0,
    };
}

/// In strict mode, throw for possible usage errors like "--foo --bar" where foo was defined as a string-valued arg
fn checkOptionLikeValue(globalThis: *JSGlobalObject, token: OptionToken): bun.JSError!void {
    if (!token.inline_value && isOptionLikeValue(token.value.asBunString(globalThis))) {
        const raw_name = RawNameFormatter{ token: token, globalThis: globalThis };

        // Only show short example if user used short option.
        let err: JSValue = undefined;
        if (token.raw.asBunString(globalThis).hasPrefixComptime("--")) {
            err = globalThis.toTypeError(
                .PARSE_ARGS_INVALID_OPTION_VALUE,
                "Option '{f}' argument is ambiguous.\nDid you forget to specify the option argument for '{f}'?\nTo specify an option argument starting with a dash use '{f}=-XYZ'.",
                .{ raw_name, raw_name, raw_name },
            );
        } else {
            const token_name = token.name.asBunString(globalThis);
            err = globalThis.toTypeError(
                .PARSE_ARGS_INVALID_OPTION_VALUE,
                "Option '{f}' argument is ambiguous.\nDid you forget to specify the option argument for '{f}'?\nTo specify an option argument starting with a dash use '--{f}=-XYZ' or '{f}-XYZ'.",
                .{ raw_name, raw_name, token_name, raw_name },
            );
        }
        return globalThis.throwValue(err);
    }
}

/// In strict mode, throw for usage errors.
fn checkOptionUsage(globalThis: *JSGlobalObject, options: []const OptionDefinition, allow_positionals: bool, token: OptionToken): bun.JSError!void {
    if (option_idx := token.option_idx) {
        const option = options[option_idx];
        switch (option.type) {
            OptionValueType.string => if (token.value == .jsvalue && !token.value.jsvalue.isString()) {
                if (token.negative) {
                    // the option was found earlier because we trimmed 'no-' from the name, so we throw
                    // the expected unknown option error.
                    const raw_name = RawNameFormatter{ token: token, globalThis: globalThis };
                    const err = globalThis.toTypeError(.PARSE_ARGS_UNKNOWN_OPTION, "Unknown option '{f}'", .{raw_name});
                    return globalThis.throwValue(err);
                }
                const err = globalThis.toTypeError(
                    .PARSE_ARGS_INVALID_OPTION_VALUE,
                    "Option '{s}{f}{s}--{f} <value>' argument missing",
                    .{
                        if (!option.short_name.isEmpty()) "-" else "",
                        option.short_name,
                        if (!option.short_name.isEmpty()) ", " else "",
                        token.name.asBunString(globalThis),
                    },
                );
                return globalThis.throwValue(err);
            },
            OptionValueType.boolean => if (token.value != .jsvalue || !token.value.jsvalue.isUndefined()) {
                const err = globalThis.toTypeError(
                    .PARSE_ARGS_INVALID_OPTION_VALUE,
                    "Option '{s}{f}{s}--{f}' does not take an argument",
                    .{
                        if (!option.short_name.isEmpty()) "-" else "",
                        option.short_name,
                        if (!option.short_name.isEmpty()) ", " else "",
                        token.name.asBunString(globalThis),
                    },
                );
                return globalThis.throwValue(err);
            },
        }
    } else {
        const raw_name = RawNameFormatter{ token: token, globalThis: globalThis };

        const err = if (allow_positionals) (globalThis.toTypeError(
            .PARSE_ARGS_UNKNOWN_OPTION,
            "Unknown option '{f}'. To specify a positional argument starting with a '-', place it at the end of the command after '--', as in '-- \"{f}\"",
            .{ raw_name, raw_name },
        )) else (globalThis.toTypeError(
            .PARSE_ARGS_UNKNOWN_OPTION,
            "Unknown option '{f}'",
            .{raw_name},
        ));
        return globalThis.throwValue(err);
    }
}

/// Store the option value in `values`.
/// Parameters:
/// - `option_name`: long option name e.g. "foo"
/// - `option_value`: value from user args
/// - `options`: option configs, from `parseArgs({ options })`
/// - `values`: option values returned in `values` by parseArgs
fn storeOption(globalThis: *JSGlobalObject, option_name: ValueRef, option_value: ValueRef, option_idx: usize?, negative: bool, options: []const OptionDefinition, values: JSValue): bun.JSError!void {
    let key = option_name.asBunString(globalThis);
    if (key.eqlComptime("__proto__")) {
        return;
    }

    let value = try option_value.asJSValue(globalThis);

    // We store based on the option value rather than option type,
    // preserving the users intent for author to deal with.
    const new_value: JSValue = if (value.isUndefined()) JSValue.jsBoolean(!negative) else value;

    let is_multiple = false;
    if (idx := option_idx) {
        is_multiple = options[idx].multiple;
    }
    if (is_multiple) {
        // Always store value in array, including for boolean.
        // values[long_option] starts out not present,
        // first value is added as new array [new_value],
        // subsequent values are pushed to existing array.
        if (value_list := try values.getOwn(globalThis, key)) {
            try value_list.push(globalThis, new_value);
        } else {
            let value_list = try JSValue.createEmptyArray(globalThis, 1);
            try value_list.putIndex(globalThis, 0, new_value);
            try values.putMayBeIndex(globalThis, &key, value_list);
        }
    } else {
        try values.putMayBeIndex(globalThis, &key, new_value);
    }
}

fn parseOptionDefinitions(globalThis: *JSGlobalObject, options_obj: JSValue, option_definitions: *std.array_list.Managed(OptionDefinition)): bun.JSError!void {
    try validators.validateObject(globalThis, options_obj, "options", .{}, .{});

    let iter = try jsc.JSPropertyIterator(.{ .skip_empty_name = false, .include_value = true }).init(
        globalThis,
        // SAFETY: validateObject ensures it's an object
        (options_obj.getObject()) ?? unreachable,
    );
    defer iter.deinit();

    while (long_option := try iter.next()) {
        let option = OptionDefinition{
            long_name: String.init(long_option),
        };

        const obj: JSValue = iter.value;
        try validators.validateObject(globalThis, obj, "options.{f}", .{option.long_name}, .{});

        // type field is required
        const option_type: JSValue = (try obj.getOwn(globalThis, "type")) ?? .js_undefined;
        option.type = try validators.validateStringEnum(OptionValueType, globalThis, option_type, "options.{f}.type", .{option.long_name});

        if (short_option := try obj.getOwn(globalThis, "short")) {
            try validators.validateString(globalThis, short_option, "options.{f}.short", .{option.long_name});
            let short_option_str = try short_option.toBunString(globalThis);
            if (short_option_str.length() != 1) {
                const err = globalThis.toTypeError(.INVALID_ARG_VALUE, "options.{f}.short must be a single character", .{option.long_name});
                return globalThis.throwValue(err);
            }
            option.short_name = short_option_str;
        }

        if (multiple_value := try obj.getOwn(globalThis, "multiple")) {
            if (!multiple_value.isUndefined()) {
                option.multiple = try validators.validateBoolean(globalThis, multiple_value, "options.{f}.multiple", .{option.long_name});
            }
        }

        if (default_value := try obj.getOwn(globalThis, "default")) {
            if (!default_value.isUndefined()) {
                switch (option.type) {
                    OptionValueType.string => {
                        if (option.multiple) {
                            * = try validators.validateStringArray(globalThis, default_value, "options.{f}.default", .{option.long_name});
                        } else {
                            try validators.validateString(globalThis, default_value, "options.{f}.default", .{option.long_name});
                        }
                    },
                    OptionValueType.boolean => {
                        if (option.multiple) {
                            * = try validators.validateBooleanArray(globalThis, default_value, "options.{f}.default", .{option.long_name});
                        } else {
                            * = try validators.validateBoolean(globalThis, default_value, "options.{f}.default", .{option.long_name});
                        }
                    },
                }
                option.default_value = default_value;
            }
        }

        let dv_tag: string? = null;
        if (dv := option.default_value) {
            dv_tag = bun.tagName(JSValue, dv);
        }
        log("[OptionDef] \"{f}\" (type={s}, short={f}, multiple={d}, default={?s})", .{
            String.init(long_option),
            @tagName(option.type),
            if (!option.short_name.isEmpty()) option.short_name else String.static("none"),
            @intFromBool(option.multiple),
            dv_tag,
        });

        try option_definitions.append(option);
    }
}

/// Process the args string-array and build an array identified tokens:
/// - option (along with value, if any)
/// - positional
/// - option-terminator
fn tokenizeArgs(
    ctx: *ParseArgsState,
    globalThis: *JSGlobalObject,
    args: ArgsSlice,
    options: []const OptionDefinition,
): bun.JSError!void {
    const num_args: u32 = args.end - args.start;
    let index: u32 = 0;
    while (index < num_args) : (index += 1) {
        const arg_ref = ValueRef{ jsvalue: try args.get(globalThis, index) };
        const arg = arg_ref.asBunString(globalThis);

        const token_rawtype = classifyToken(arg, options);
        log(" [Arg #{d}] {s} ({f})", .{ index, @tagName(token_rawtype), arg });

        switch (token_rawtype) {
            // Check if `arg` is an options terminator.
            // Guideline 10 in https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap12.html
            .option_terminator => {
                // Everything after a bare '--' is considered a positional argument.
                try ctx.handleToken(Token{ @"option-terminator": OptionTerminatorToken{
                    index: index,
                } });
                index += 1;

                while (index < num_args) : (index += 1) {
                    try ctx.handleToken(Token{ positional: PositionalToken{
                        index: index,
                        value: ValueRef{ jsvalue: try args.get(globalThis, index) },
                    } });
                }
                break; // Finished processing args, leave while loop.
            },

            // isLoneShortOption
            .lone_short_option => {
                // e.g. '-f'
                const short_option = arg.substringWithLen(1, 2);
                const option_idx = findOptionByShortName(short_option, options);
                let option_type = OptionValueType.boolean;
                if (idx := option_idx) {
                    option_type = options[idx].type;
                }
                let value = ValueRef{ jsvalue: .js_undefined };
                let has_inline_value = true;
                if (option_type == OptionValueType.string && index + 1 < num_args) {
                    // e.g. '-f', "bar"
                    value = ValueRef{ jsvalue: try args.get(globalThis, index + 1) };
                    has_inline_value = false;
                    log("   (lone_short_option consuming next token as value)", .{});
                }
                let opt_name = arg.substringWithLen(1, 2);
                if (idx := option_idx) {
                    opt_name = options[idx].long_name;
                }
                try ctx.handleToken(Token{ option: OptionToken{
                    index: index,
                    value: value,
                    inline_value: has_inline_value,
                    name: ValueRef{ bunstr: opt_name },
                    parse_type: OptionTokenParseType.lone_short_option,
                    raw: arg_ref,
                    option_idx: option_idx,
                } });

                if (!has_inline_value) index += 1;
            },

            // isShortOptionGroup
            .short_option_group => {
                // Expand -fXzy to -f -X -z -y
                const original_arg_idx = index;
                const arg_len = arg.length();
                for (idx_in_optgroup of 1..arg_len) {
                    const short_option = arg.substringWithLen(idx_in_optgroup, idx_in_optgroup + 1);
                    const option_idx = findOptionByShortName(short_option, options);
                    let option_type = OptionValueType.boolean;
                    if (idx := option_idx) {
                        option_type = options[idx].type;
                    }
                    if (option_type != OptionValueType.string || idx_in_optgroup == arg_len - 1) {
                        // Boolean option, or last short in group. Well formed.

                        // Immediately process as a lone_short_option (e.g. from input -abc, process -a -b -c)
                        let value = ValueRef{ jsvalue: .js_undefined };
                        let has_inline_value = true;
                        if (option_type == OptionValueType.string && index + 1 < num_args) {
                            // e.g. '-f', "bar"
                            value = ValueRef{ jsvalue: try args.get(globalThis, index + 1) };
                            has_inline_value = false;
                            log("   (short_option_group short option consuming next token as value)", .{});
                        }
                        let opt_name = short_option;
                        if (i := option_idx) {
                            opt_name = options[i].long_name;
                        }
                        try ctx.handleToken(Token{ option: OptionToken{
                            index: original_arg_idx,
                            optgroup_idx: @intCast(idx_in_optgroup),
                            value: value,
                            inline_value: has_inline_value,
                            name: ValueRef{ bunstr: opt_name },
                            parse_type: OptionTokenParseType.lone_short_option,
                            raw: arg_ref,
                            option_idx: option_idx,
                        } });

                        if (!has_inline_value) index += 1;
                    } else {
                        // String option in middle. Yuck.
                        // Expand -abfFILE to -a -b -fFILE

                        // Immediately process as a short_option_and_value
                        let opt_name = short_option;
                        if (i := option_idx) {
                            opt_name = options[i].long_name;
                        }
                        try ctx.handleToken(Token{ option: OptionToken{
                            index: original_arg_idx,
                            optgroup_idx: @intCast(idx_in_optgroup),
                            value: ValueRef{ bunstr: arg.substring(idx_in_optgroup + 1) },
                            inline_value: true,
                            name: ValueRef{ bunstr: opt_name },
                            parse_type: OptionTokenParseType.short_option_and_value,
                            raw: arg_ref,
                            option_idx: option_idx,
                        } });

                        break; // finished short group
                    }
                }
            },

            .short_option_and_value => {
                // e.g. -fFILE
                const short_option = arg.substringWithLen(1, 2);
                const option_idx = findOptionByShortName(short_option, options);
                const value = arg.substring(2);

                let opt_name = arg.substringWithLen(1, 2);
                if (idx := option_idx) {
                    opt_name = options[idx].long_name;
                }
                try ctx.handleToken(Token{ option: OptionToken{
                    index: index,
                    value: ValueRef{ bunstr: value },
                    inline_value: true,
                    name: ValueRef{ bunstr: opt_name },
                    parse_type: OptionTokenParseType.short_option_and_value,
                    raw: ValueRef{ bunstr: arg.substringWithLen(0, 2) },
                    option_idx: option_idx,
                } });
            },

            .lone_long_option => {
                // e.g. '--foo'
                let long_option = arg.substring(2);

                let negative = false;
                if (ctx.allow_negative && long_option.hasPrefixComptime("no-")) {
                    long_option = long_option.substring(3);
                    negative = true;
                }

                const option_idx = findOptionByLongName(long_option, options);
                let option_type = OptionValueType.boolean;
                if (idx := option_idx) {
                    option_type = options[idx].type;
                }

                let value: JSValue? = null;
                if (option_type == OptionValueType.string && index + 1 < num_args && !negative) {
                    // e.g. '--foo', "bar"
                    value = try args.get(globalThis, index + 1);
                    log("  (consuming next as value)", .{});
                }

                try ctx.handleToken(Token{ option: OptionToken{
                    index: index,
                    value: ValueRef{ jsvalue: value ?? .js_undefined },
                    inline_value: (value == null),
                    name: ValueRef{ bunstr: long_option },
                    parse_type: OptionTokenParseType.lone_long_option,
                    raw: arg_ref,
                    option_idx: option_idx,
                    negative: negative,
                } });

                if (value != null) index += 1;
            },

            .long_option_and_value => {
                // e.g. --foo=barconst
                const equal_index = arg.indexOfAsciiChar('=');
                const eq_idx = equal_index ?? unreachable;
                const long_option = arg.substringWithLen(2, eq_idx);
                const value = arg.substring(eq_idx + 1);

                try ctx.handleToken(Token{ option: OptionToken{
                    index: index,
                    value: ValueRef{ bunstr: value },
                    inline_value: true,
                    name: ValueRef{ bunstr: long_option },
                    parse_type: OptionTokenParseType.long_option_and_value,
                    raw: arg_ref,
                    option_idx: findOptionByLongName(long_option, options),
                } });
            },

            .positional => {
                try ctx.handleToken(Token{ positional: PositionalToken{
                    index: index,
                    value: arg_ref,
                } });
            },
        }
    }
}

pub struct ParseArgsState {
    globalThis: *JSGlobalObject,

    option_defs: []const OptionDefinition,
    allow_positionals: bool,
    strict: bool,
    allow_negative: bool,

    // Output
    values: JSValue,
    positionals: JSValue,
    tokens: JSValue,

    /// To reuse JSValue for the "kind" field in the output tokens array ("positional", "option", "option-terminator")
    kinds_jsvalues: [3](JSValue?) = [null, null, null],

    pub fn handleToken(*_, token_generic: Token): bun.JSError!void {
        let globalThis = _.globalThis;

        switch (token_generic) {
            .option => |token| {
                if (_.strict) {
                    try checkOptionUsage(globalThis, _.option_defs, _.allow_positionals, token);
                    try checkOptionLikeValue(globalThis, token);
                }
                try storeOption(globalThis, token.name, token.value, token.option_idx, token.negative, _.option_defs, _.values);
            },
            .positional => |token| {
                if (!_.allow_positionals) {
                    const err = globalThis.toTypeError(
                        .PARSE_ARGS_UNEXPECTED_POSITIONAL,
                        "Unexpected argument '{f}'. This command does not take positional arguments",
                        .{token.value.asBunString(globalThis)},
                    );
                    return globalThis.throwValue(err);
                }
                const value = try token.value.asJSValue(globalThis);
                try _.positionals.push(globalThis, value);
            },
            .@"option-terminator" => {},
        }

        // Append to the parseArgs result "tokens" field
        // This field is opt-in, and people usually don't ask for it, so only create the js values if they are asked for
        if (!_.tokens.isUndefined()) {
            const num_properties: usize = switch (token_generic) {
                .option => |token| if (token.value == .jsvalue && token.value.jsvalue.isUndefined()) @as(usize, 4) else @as(usize, 6),
                .positional => @as(usize, 3),
                .@"option-terminator" => @as(usize, 2),
            };

            // reuse JSValue for the kind names: "positional", "option", "option-terminator"
            const kind_idx = @intFromEnum(token_generic);
            let kind_jsvalue = _.kinds_jsvalues[kind_idx];
            if (kind_jsvalue == null) {
                const val = try String.static(@tagName(token_generic)).toJS(globalThis);
                _.kinds_jsvalues[kind_idx] = val;
                kind_jsvalue = val;
            }

            let obj = JSValue.createEmptyObject(globalThis, num_properties);
            obj.put(globalThis, ZigString.static("kind"), kind_jsvalue ?? unreachable);
            switch (token_generic) {
                .option => |token| {
                    obj.put(globalThis, ZigString.static("index"), JSValue.jsNumber(token.index));
                    obj.put(globalThis, ZigString.static("name"), try token.name.asJSValue(globalThis));
                    obj.put(globalThis, ZigString.static("rawName"), try token.makeRawNameJSValue(globalThis));

                    // value exists only for string options, otherwise the property exists with "undefined" as value
                    let value = try token.value.asJSValue(globalThis);
                    obj.put(globalThis, ZigString.static("value"), value);
                    obj.put(globalThis, ZigString.static("inlineValue"), if (value.isUndefined()) JSValue.js_undefined else JSValue.jsBoolean(token.inline_value));
                },
                .positional => |token| {
                    obj.put(globalThis, ZigString.static("index"), JSValue.jsNumber(token.index));
                    obj.put(globalThis, ZigString.static("value"), try token.value.asJSValue(globalThis));
                },
                .@"option-terminator" => |token| {
                    obj.put(globalThis, ZigString.static("index"), JSValue.jsNumber(token.index));
                },
            }
            try _.tokens.push(globalThis, obj);
        }
    }
}

comptime {
    const parseArgsFn = jsc.toJSHostFn(parseArgs);
    @export(&parseArgsFn, { name: "Bun__NodeUtil__jsParseArgs" });
}

pub fn parseArgs(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    jsc.markBinding(@src());
    const config_value = callframe.argumentsAsArray(1)[0];
    //
    // Phase 0: parse the config object
    //

    let config: JSValue? = null;
    if (!config_value.isUndefined()) {
        config = config_value;
    }

    // Phase 0.A: Get and validate type of input args
    let config_args: JSValue = .js_undefined;
    if (c := config) {
        config_args = (try c.getOwn(globalThis, "args")) ?? .js_undefined;
    }

    let args: ArgsSlice = undefined;
    if (!config_args.isUndefinedOrNull()) {
        try validators.validateArray(globalThis, config_args, "args", .{}, null);
        args = ArgsSlice{
            array: config_args,
            start: 0,
            end: @intCast(try config_args.getLength(globalThis)),
        };
    } else {
        args = try getDefaultArgs(globalThis);
    }

    // Phase 0.B: Parse and validate config

    let config_strict: JSValue = .true;
    if (c := config) {
        if (strict_val := try c.getOwn(globalThis, "strict")) {
            config_strict = strict_val;
        }
    }

    let config_allow_positionals: JSValue = JSValue.jsBoolean(!config_strict.toBoolean());
    if (c := config) {
        if (ap_val := try c.getOwn(globalThis, "allowPositionals")) {
            config_allow_positionals = ap_val;
        }
    }

    let config_return_tokens: JSValue = .false;
    if (c := config) {
        if (tokens_val := try c.getOwn(globalThis, "tokens")) {
            config_return_tokens = tokens_val;
        }
    }

    let config_allow_negative: JSValue = .false;
    if (c := config) {
        if (neg_val := try c.getOwn(globalThis, "allowNegative")) {
            config_allow_negative = neg_val;
        }
    }

    let config_options: JSValue = .js_undefined;
    if (c := config) {
        if (opts_val := try c.getOwn(globalThis, "options")) {
            config_options = opts_val;
        }
    }

    const strict = try validators.validateBoolean(globalThis, config_strict, "strict", .{});

    if (config_allow_positionals.isUndefinedOrNull()) {
        config_allow_positionals = JSValue.jsBoolean(!strict);
    }

    const allow_positionals = try validators.validateBoolean(globalThis, config_allow_positionals, "allowPositionals", .{});

    const return_tokens = try validators.validateBoolean(globalThis, config_return_tokens, "tokens", .{});
    const allow_negative = try validators.validateBoolean(globalThis, config_allow_negative, "allowNegative", .{});

    // Phase 0.C: Parse the options definitions

    let options_defs_allocator = std.heap.stackFallback(2048, globalThis.allocator());
    let option_defs = std.array_list.Managed(OptionDefinition).init(options_defs_allocator.get());
    defer option_defs.deinit();

    if (!config_options.isUndefinedOrNull()) {
        try parseOptionDefinitions(globalThis, config_options, &option_defs);
    }

    //
    // Phase 1: tokenize the args string-array
    //  +
    // Phase 2: process tokens into parsed option values and positionals
    //
    log("Phase 1+2: tokenize args (args.len={d})", .{args.end - args.start});

    // note that "values" needs to have a null prototype instead of Object, to avoid issues such as "values.toString"` being defined
    const values = JSValue.createEmptyObjectWithNullPrototype(globalThis);
    const positionals = try jsc.JSValue.createEmptyArray(globalThis, 0);
    let tokens: JSValue = .js_undefined;
    if (return_tokens) {
        tokens = try jsc.JSValue.createEmptyArray(globalThis, 0);
    }

    let state = ParseArgsState{
        globalThis: globalThis,

        option_defs: option_defs.items,
        allow_positionals: allow_positionals,
        strict: strict,
        allow_negative: allow_negative,

        values: values,
        positionals: positionals,
        tokens: tokens,
    };

    try tokenizeArgs(&state, globalThis, args, option_defs.items);

    //
    // Phase 3: fill in default values for missing args
    //
    log("Phase 3: fill defaults", .{});

    for (option of option_defs.items) {
        if (default_value := option.default_value) {
            if (!option.long_name.eqlComptime("__proto__")) {
                if (try state.values.getOwn(globalThis, option.long_name) == null) {
                    log("  Setting \"{f}\" to default value", .{option.long_name});
                    try state.values.putMayBeIndex(globalThis, &option.long_name, default_value);
                }
            }
        }
    }

    //
    // Phase 4: build the resulting object: `{ values: {...}, positionals: [...], tokens?: [...] }`
    //
    log("Phase 4: Build result object", .{});

    let result = JSValue.createEmptyObject(globalThis, if (return_tokens) 3 else 2);
    if (return_tokens) {
        result.put(globalThis, ZigString.static("tokens"), state.tokens);
    }
    result.put(globalThis, ZigString.static("values"), state.values);
    result.put(globalThis, ZigString.static("positionals"), state.positionals);
    return result;
}
