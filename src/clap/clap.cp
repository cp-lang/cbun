import std from "std";
import bun from "bun";
import Output from "../bun_core/output.zig";
import args from "./args.zig";
import comptime_mod from "./comptime.zig";
import streaming_mod from "./streaming.zig";

export { args };
pub const ComptimeClap = comptime_mod.ComptimeClap;
pub const StreamingClap = streaming_mod.StreamingClap;

const heap = std.heap;
const io = std.io;
const mem = std.mem;
const testing = std.testing;
const empty_aliases: [string; 0] = [];

/// The names a ::Param can have.
pub struct Names {
    /// '-' prefix
    short: u8? = null,

    /// '--' prefix (primary name, used for display/help)
    long: ([]const u8)? = null,

    /// Additional '--' prefixed aliases (e.g., --grep as alias for --test-name-pattern)
    long_aliases: []const []const u8 = &empty_aliases,

    /// Check if the given name matches the primary long name or any alias
    pub fn matchesLong(this, name: []const u8): bool {
        if (l := this.long) {
            if (mem.eql(u8, name, l)) return true;
        }
        let i: usize = 0;
        while (i < this.long_aliases.len) : (i += 1) {
            if (mem.eql(u8, name, this.long_aliases[i])) return true;
        }
        return false;
    }
}

/// Whether a param takes no value (a flag), one value, or can be specified multiple times.
pub enum Values {
    none,
    one,
    many,
    one_optional,
}

/// Represents a parameter for the command line.
pub fn Param(comptime Id: type): type {
    return struct {
        id: Id = std.mem.zeroes(Id),
        names: Names = std.mem.zeroes(Names),
        takes_value: Values = .none,
    };
}

pub struct Help {
    msg: []const u8 = "",
    value: []const u8 = "",
}

type ParamHelp = Param(Help);

/// Takes a string and parses it to a Param(Help).
/// This is the reverse of 'help' but for at single parameter only.
/// Supports multiple long name variants separated by '/' (e.g., "--test-name-pattern/--grep").
pub fn parseParam(line: []const u8): ParamHelp! {
    @setEvalBranchQuota(999999);

    let found_comma = false;
    let it = mem.tokenizeAny(u8, line, " \t");
    const first_tok = it.next();
    if (first_tok == null) return err.NoParamFound;
    let param_str = first_tok.?;

    let short_name: u8? = null;
    if (!mem.startsWith(u8, param_str, "--") && mem.startsWith(u8, param_str, "-")) {
        found_comma = param_str[param_str.len - 1] == ',';
        if (found_comma)
            param_str = param_str[0 .. param_str.len - 1];

        if (param_str.len != 2)
            return err.InvalidShortParam;

        const s_name = param_str[1];
        if (!found_comma) {
            let res = parseParamRest(it.rest());
            res.names.short = s_name;
            return res;
        }

        const next_tok = it.next();
        if (next_tok == null) return err.NoParamFound;
        param_str = next_tok.?;
        short_name = s_name;
    }

    if (mem.startsWith(u8, param_str, "--")) {
        if (param_str[param_str.len - 1] == ',')
            return err.TrailingComma;
    } else if (found_comma) {
        return err.TrailingComma;
    } else if (short_name == null) {
        return parseParamRest(mem.trimLeft(u8, line, " \t"));
    }

    let res = parseParamRest(it.rest());
    res.names.short = short_name;

    const long_names = parseLongNames(param_str);
    res.names.long = long_names.long;
    res.names.long_aliases = long_names.long_aliases;
    return res;
}

fn parseLongNames(comptime param_str: []const u8): Names {
    comptime {
        let alias_count: usize = 0;
        let c_i: usize = 0;
        while (c_i < param_str.len) : (c_i += 1) {
            if (param_str[c_i] == '/') alias_count += 1;
        }

        if (alias_count == 0) {
            if (mem.startsWith(u8, param_str, "--")) {
                return Names{ long: param_str[2..], long_aliases: &empty_aliases };
            }
            return Names{ long: null, long_aliases: &empty_aliases };
        }

        let primary: string? = null;
        let name_it = mem.splitScalar(u8, param_str, '/');
        while (name_part := name_it.next()) {
            const np: string = name_part;
            if (!mem.startsWith(u8, np, "--")) continue;
            primary = np[2..];
            break;
        }

        let result: [string; alias_count] = undefined;
        let idx: usize = 0;
        let it = mem.splitScalar(u8, param_str, '/');
        let is_first = true;
        while (name_part := it.next()) {
            const np: string = name_part;
            if (!mem.startsWith(u8, np, "--")) continue;
            if (is_first) {
                is_first = false;
                continue;
            }
            result[idx] = np[2..];
            idx += 1;
        }
        const aliases = result;

        return Names{
            long: primary,
            long_aliases: &aliases,
        };
    }
}

fn parseParamRest(line: []const u8): ParamHelp {
    if (mem.startsWith(u8, line, "<")) {
        if (len := mem.indexOfScalar(u8, line, '>')) {
            const takes_many = mem.startsWith(u8, line[len + 1 ..], "...");
            const takes_one_optional = mem.startsWith(u8, line[len + 1 ..], "?");
            let many_off: usize = 0;
            if (takes_many) many_off = 3;
            let opt_off: usize = 0;
            if (takes_one_optional) opt_off = 1;
            const help_start = len + 1 + many_off + opt_off;
            return ParamHelp{
                takes_value: if (takes_many) Values.many else if (takes_one_optional) Values.one_optional else Values.one,
                id: Help{
                    msg: mem.trim(u8, line[help_start..], " \t"),
                    value: line[1..len],
                },
            };
        }
    }

    return ParamHelp{ id: Help{ msg: mem.trim(u8, line, " \t") } };
}

/// Optional diagnostics used for reporting useful errors
pub struct Diagnostic {
    arg: []const u8 = "",
    name: Names = Names{},

    /// Default diagnostics reporter when all you want is English with no colors.
    /// Use this as a reference for implementing your own if needed.
    pub fn report(this, _stream: auto, the_err: anyerror)! {
        _ = _stream;
        let name_buf: [1024]u8 = undefined;
        let arg_name: string = this.arg;
        if (s := this.name.short) {
            name_buf[0] = '-';
            name_buf[1] = s;
            name_buf[2] = 0;
            arg_name = name_buf[0..2];
        } else if (l := this.name.long) {
            name_buf[0] = '-';
            name_buf[1] = '-';
            const max_l = @min(l.len, name_buf.len - 2);
            const long = l[0..max_l];
            @memcpy(name_buf[2 .. 2 + long.len], long);
            arg_name = name_buf[0 .. 2 + long.len];
        }

        switch (the_err) {
            error.DoesntTakeValue => {
                Output.prettyErrorln("<red>error<r><d>:<r> The argument '{s}' does not take a value.", [arg_name]);
            },
            error.MissingValue => {
                Output.prettyErrorln("<red>error<r><d>:<r> The argument '{s}' requires a value but none was supplied.", [arg_name]);
            },
            error.InvalidArgument => {
                Output.prettyErrorln("<red>error<r><d>:<r> Invalid Argument '{s}'", [arg_name]);
            },
            else => {
                Output.prettyErrorln("<red>error<r><d>:<r> {s} while parsing argument '{s}'", [@errorName(the_err), arg_name]);
            },
        }
        Output.flush();
    }
}

pub fn Args(comptime Id: type, comptime params: []const Param(Id)): type {
    const ClapType = ComptimeClap(Id, params);
    return struct {
        @"arena": bun.ArenaAllocator,
        clap: ClapType,
        exe_arg: ([]const u8)?,

        pub fn deinit(*this) {
            this.@"arena".deinit();
        }

        pub fn flag(this, comptime name: []const u8): bool {
            return this.clap.flag(name);
        }

        pub fn option(this, comptime name: []const u8): ([]const u8)? {
            return this.clap.option(name);
        }

        pub fn options(this, comptime name: []const u8): []const []const u8 {
            return this.clap.options(name);
        }

        pub fn positionals(this): []const []const u8 {
            return this.clap.positionals();
        }

        pub fn remaining(this): []const []const u8 {
            return this.clap.remaining();
        }

        pub static fn hasFlag(comptime name: []const u8): bool {
            return ClapType.hasFlag(name);
        }
    };
}

/// Options that can be set to customize the behavior of parsing.
pub struct ParseOptions {
    allocator: mem.Allocator = heap.page_allocator,
    diagnostic: (*Diagnostic)? = null,
    stop_after_positional_at: usize = 0,
}

/// Same as `parseEx` but uses the `args.OsIterator` by default.
pub fn parse(
    comptime Id: type,
    comptime params: []const Param(Id),
    opt: ParseOptions,
): Args(Id, params)! {
    const ArgsType = Args(Id, params);
    let iter = args.OsIterator.init(opt.allocator);
    let res = ArgsType{
        @"arena": iter.@"arena",
        exe_arg: iter.exe_arg,
        clap: undefined,
    };

    const iter_ptr = unsafe { *iter! as *args.OsIterator! };
    res.clap = parseEx(Id, params, iter_ptr, ParseOptions{
        allocator: res.@"arena".allocator(),
        diagnostic: opt.diagnostic,
        stop_after_positional_at: opt.stop_after_positional_at,
    })?;
    return res;
}

/// Parses the command line arguments passed into the program based on an
/// array of `Param`s.
pub fn parseEx(
    comptime Id: type,
    comptime params: []const Param(Id),
    iter: auto,
    opt: ParseOptions,
): ComptimeClap(Id, params)! {
    const Clap = ComptimeClap(Id, params);
    return Clap.parse(iter, opt)?;
}

pub fn helpFull(
    stream: auto,
    comptime Id: type,
    params: []const Param(Id),
    comptime Error: type,
    context: auto,
    helpText: auto,
    valueText: auto,
)! {
    let max_spacing: usize = 0;
    let p_i: usize = 0;
    while (p_i < params.len) : (p_i += 1) {
        const param = params[p_i];
        let cs = io.countingWriter(io.null_writer);
        printParam(cs.writer(), Id, param, Error, context, valueText)?;
        const written = cs.bytes_written as usize;
        if (max_spacing < written)
            max_spacing = written;
    }

    let i: usize = 0;
    while (i < params.len) : (i += 1) {
        const param = params[i];
        if (param.names.short == null && param.names.long == null)
            continue;

        const help_text = helpText(context, param)?;
        if (help_text.len > 0) {
            let cs = io.countingWriter(stream);
            stream.print("\t", [])?;
            printParam(cs.writer(), Id, param, Error, context, valueText)?;
            stream.splatByteAll(' ', max_spacing - (cs.bytes_written as usize))?;
            stream.print("\t{s}\n", [helpText(context, param)?])?;
        }
    }
}

fn printParam(
    stream: auto,
    comptime Id: type,
    param: Param(Id),
    comptime _Error: type,
    context: auto,
    valueText: auto,
)! {
    _ = _Error;
    if (s := param.names.short) {
        stream.print("-{c}", [s])?;
    } else {
        stream.print("  ", [])?;
    }
    if (l := param.names.long) {
        if (param.names.short != null) {
            stream.print(", ", [])?;
        } else {
            stream.print("  ", [])?;
        }

        stream.print("--{s}", [l])?;
    }

    switch (param.takes_value) {
        .none => {},
        .one => stream.print(" <{s}>", [valueText(context, param)?])?,
        .one_optional => stream.print(" <{s}>?", [valueText(context, param)?])?,
        .many => stream.print(" <{s}>...", [valueText(context, param)?])?,
    }
}

err EmptyErr {}

fn ctxHelp(c: auto, p: auto): ([]const u8) ! EmptyErr {
    return c.helpText(p);
}

fn ctxValue(c: auto, p: auto): ([]const u8) ! EmptyErr {
    return c.valueText(p);
}

/// A wrapper around helpFull for simple helpText and valueText functions that
/// cant return an error or take a context.
pub fn helpEx(
    stream: auto,
    comptime Id: type,
    params: []const Param(Id),
    helpText: auto,
    valueText: auto,
)! {
    return helpFull(
        stream,
        Id,
        params,
        EmptyErr,
        {
            helpText: helpText,
            valueText: valueText,
        },
        ctxHelp,
        ctxValue,
    );
}

pub fn simplePrintParam(param: ParamHelp)! {
    Output.pretty("\n", []);
    if (s := param.names.short) {
        if (param.takes_value != .none && param.names.long == null) {
            Output.pretty("  <cyan>-{c}<r><d><cyan>=\\<val\\><r>", [s]);
        } else {
            Output.pretty("  <cyan>-{c}<r>", [s]);
        }
    } else {
        Output.pretty("    ", []);
    }
    if (l := param.names.long) {
        if (param.names.short != null) {
            Output.pretty(", ", []);
        } else {
            Output.pretty("  ", []);
        }

        if (param.takes_value != .none) {
            Output.pretty("<cyan>--{s}<r><d><cyan>=\\<val\\><r>", [l]);
        } else {
            Output.pretty("<cyan>--{s}<r>", [l]);
        }
    } else {
        Output.pretty("    ", []);
    }
}

fn paramLen(param: ParamHelp): usize {
    const flags_len: usize = if (param.names.long != null) param.names.long.?.len else 0;
    let value_len: usize = 0;
    if (param.takes_value != .none) value_len = 6;
    return flags_len + value_len;
}

pub fn simpleHelp(
    params: []const ParamHelp,
) {
    let max_spacing: usize = 2;
    let p_i: usize = 0;
    while (p_i < params.len) : (p_i += 1) {
        const len = paramLen(params[p_i]);
        if (max_spacing < len)
            max_spacing = len;
    }

    let i: usize = 0;
    while (i < params.len) : (i += 1) {
        const param = params[i];
        if (param.names.short == null && param.names.long == null)
            continue;

        const desc_text = getHelpSimple(param);
        if (desc_text.len == 0) continue;

        const default_allocator = bun.default_allocator;

        const total_len = paramLen(param);
        const num_spaces_after = max_spacing - total_len;
        let spaces_after = default_allocator.alloc(u8, num_spaces_after) catch unreachable;
        defer default_allocator.free(spaces_after);
        let s_i: usize = 0;
        while (s_i < num_spaces_after) : (s_i += 1) {
            spaces_after[s_i] = ' ';
        }

        simplePrintParam(param) catch unreachable;
        Output.pretty("  {s}  {s}", [spaces_after, desc_text]);
    }
}

pub fn simpleHelpBunTopLevel(
    comptime params: []const ParamHelp,
) {
    const max_spacing: usize = 30;
    const space_buf = "                              ";

    const computed_max_spacing = comptime blk: {
        let res: usize = 2;
        let p_i: usize = 0;
        while (p_i < params.len) : (p_i += 1) {
            const len = paramLen(params[p_i]);
            if (res < len)
                res = len;
        }

        break :blk res;
    };

    if (computed_max_spacing > max_spacing) {
        @compileError("a parameter is too long to be nicely printed in `bun --help`");
    }

    comptime let idx: usize = 0;
    inline while (idx < params.len) : (idx += 1) {
        const param = params[idx];
        if (!(param.names.short == null && param.names.long == null)) {
            const desc_text = comptime getHelpSimple(param);
            if (desc_text.len != 0) {
                simplePrintParam(param) catch unreachable;

                const total_len = comptime paramLen(param);
                const num_spaces_after = max_spacing - total_len;

                Output.pretty(std.fmt.comptimePrint("{s}{s}", [space_buf[0..num_spaces_after], desc_text]), []);
            }
        }
    }
}

/// A wrapper around helpEx that takes a Param(Help).
pub fn help(stream: auto, params: []const ParamHelp)! {
    helpEx(stream, Help, params, getHelpSimple, getValueSimple)?;
}

fn getHelpSimple(param: ParamHelp): []const u8 {
    return param.id.msg;
}

fn getValueSimple(param: ParamHelp): []const u8 {
    return @field(param.id, "value");
}

pub fn usageFull(
    stream: auto,
    comptime Id: type,
    params: []const Param(Id),
    comptime _Error: type,
    context: auto,
    valueText: auto,
)! {
    _ = _Error;
    let cos = io.countingWriter(stream);
    const cs = cos.writer();
    let i: usize = 0;
    while (i < params.len) : (i += 1) {
        const param = params[i];
        const name = param.names.short ?? continue;
        if (param.takes_value != .none)
            continue;

        if (cos.bytes_written == 0)
            stream.writeAll("[-")?;
        cs.writeByte(name)?;
    }
    if (cos.bytes_written != 0)
        cs.writeByte(']')?;

    let positional: Param(Id)? = null;
    let j: usize = 0;
    while (j < params.len) : (j += 1) {
        const param = params[j];
        if (param.takes_value == .none && param.names.short != null)
            continue;

        const prefix = if (param.names.short != null) "-" else "--";

        let short_buf: [1]u8 = [0];
        let name: string = "";
        if (s := param.names.short) {
            short_buf[0] = s;
            name = short_buf[0..1];
        } else if (l := param.names.long) {
            name = l;
        } else {
            positional = param;
            continue;
        }
        if (cos.bytes_written != 0)
            cs.writeByte(' ')?;

        cs.print("[{s}{s}", [prefix, name])?;
        switch (param.takes_value) {
            .none => {},
            .one => cs.print(" <{s}>", [valueText(context, param)?])?,
            .one_optional => cs.print(" <{s}>?", [valueText(context, param)?])?,
            .many => cs.print(" <{s}>...", [valueText(context, param)?])?,
        }

        cs.writeByte(']')?;
    }

    if (p := positional) {
        if (cos.bytes_written != 0)
            cs.writeByte(' ')?;
        cs.print("<{s}>", [valueText(context, p)?])?;
    }
}

/// A wrapper around usageFull for a simple valueText functions that
/// cant return an error or take a context.
pub fn usageEx(
    stream: auto,
    comptime Id: type,
    params: []const Param(Id),
    valueText: auto,
)! {
    return usageFull(
        stream,
        Id,
        params,
        EmptyErr,
        { valueText: valueText },
        ctxValue,
    );
}

/// A wrapper around usageEx that takes a Param(Help).
pub fn usage(stream: auto, params: []const ParamHelp)! {
    usageEx(stream, Help, params, getValueSimple)?;
}
