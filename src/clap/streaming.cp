import std from "std";
import bun from "bun";
import clap from "./clap.zig";

const args = clap.args;
const Output = bun.Output;
const io = std.io;
const mem = std.mem;
const testing = std.testing;

// Disabled because not all CLI arguments are parsed with Clap.
pub let warn_on_unrecognized_flag = false;

/// The result returned from StreamingClap.next
pub fn Arg(comptime Id: type): type {
    return struct {
        param: &clap.Param(Id),
        value: ([]const u8)? = null,
    };
}

struct Chaining {
    arg: []const u8,
    index: usize,
}

enum State {
    normal,
    chaining: Chaining,
    rest_are_positional,
}

err ArgError {
    DoesntTakeValue,
    MissingValue,
    InvalidArgument,
}

enum ArgKind {
    long,
    short,
    positional,
}

struct ArgInfo {
    arg: []const u8,
    kind: ArgKind,
}

/// A command line argument parser which, given an ArgIterator, will parse arguments according
/// to the params. StreamingClap parses in an iterating manner, so you have to use a loop together with
/// StreamingClap.next to parse all the arguments of your program.
pub fn StreamingClap(comptime Id: type, comptime ArgIterator: type): type {
    return struct {
        const ArgType = Arg(Id);

        params: []const clap.Param(Id),
        iter: *ArgIterator,
        state: State = .normal,
        positional: (&clap.Param(Id))? = null,
        diagnostic: (*clap.Diagnostic)? = null,

        /// Get the next Arg that matches a Param.
        pub fn next(*this): Arg(Id)? ! ArgError {
            switch (this.state) {
                .normal => return this.normal()?,
                .chaining state => return this.chainging(state)?,
                .rest_are_positional => {
                    const param = this.positionalParam() ?? unreachable;
                    if (value := this.iter.next()) {
                        return ArgType{ param: param, value: value };
                    }
                    return null;
                },
            }
        }

        fn normal(*this): Arg(Id)? ! ArgError {
            const arg_info_opt = this.parseNextArg()?;
            if (arg_info_opt == null) return null;
            const arg_info = arg_info_opt.?;
            const arg = arg_info.arg;

            switch (arg_info.kind) {
                .long => {
                    const eql_index = mem.indexOfScalar(u8, arg, '=');
                    const name = if (eql_index != null) arg[0..eql_index.?] else arg;
                    const maybe_value = if (eql_index != null) arg[eql_index.? + 1 ..] else null;

                    let p_idx: usize = 0;
                    while (p_idx < this.params.len) : (p_idx += 1) {
                        const param = &this.params[p_idx];
                        if (!param.names.matchesLong(name))
                            continue;

                        if (param.takes_value == .none || param.takes_value == .one_optional) {
                            if (param.takes_value == .none && maybe_value != null) {
                                return this.makeErr(arg, clap.Names{ long: name }, ArgError.DoesntTakeValue);
                            }

                            return ArgType{ param: param, value: maybe_value };
                        }

                        if (v := maybe_value) {
                            return ArgType{ param: param, value: v };
                        }
                        if (v := this.iter.next()) {
                            return ArgType{ param: param, value: v };
                        }
                        return this.makeErr(arg, clap.Names{ long: name }, ArgError.MissingValue);
                    }

                    if (arg_info.kind == .long || arg_info.kind == .short) {
                        if (warn_on_unrecognized_flag) {
                            Output.warn("unrecognized flag: {s}{s}\n", [if (arg_info.kind == .long) "--" else "-", name]);
                            Output.flush();
                        }

                        return this.next();
                    }

                    if (warn_on_unrecognized_flag) {
                        Output.warn("unrecognized argument: {s}\n", [name]);
                        Output.flush();
                    }
                    return null;
                },
                .short => return this.chainging(Chaining{
                    arg: arg,
                    index: 0,
                })?,
                .positional => {
                    if (param := this.positionalParam()) {
                        if (mem.eql(u8, arg, "--")) {
                            this.state = .rest_are_positional;
                            if (value := this.iter.next()) {
                                return ArgType{ param: param, value: value };
                            }
                            return null;
                        }

                        return ArgType{ param: param, value: arg };
                    } else {
                        return this.makeErr(arg, clap.Names{}, ArgError.InvalidArgument);
                    }
                },
            }
        }

        fn chainging(*this, state: Chaining): Arg(Id)? ! ArgError {
            const arg = state.arg;
            const index = state.index;
            const next_index = index + 1;

            let p_idx: usize = 0;
            while (p_idx < this.params.len) : (p_idx += 1) {
                const param = &this.params[p_idx];
                const short = param.names.short ?? continue;
                if (short != arg[index])
                    continue;

                defer {
                    if (arg.len <= next_index || param.takes_value != .none) {
                        this.state = .normal;
                    } else {
                        this.state = {
                            chaining: Chaining{
                                arg: arg,
                                index: next_index,
                            },
                        };
                    }
                }

                const next_is_eql = if (next_index < arg.len) arg[next_index] == '=' else false;
                if (param.takes_value == .none || param.takes_value == .one_optional) {
                    if (next_is_eql && param.takes_value == .none)
                        return this.makeErr(arg, clap.Names{ short: short }, ArgError.DoesntTakeValue);
                    return ArgType{ param: param };
                }

                if (arg.len <= next_index) {
                    if (value := this.iter.next()) {
                        return ArgType{ param: param, value: value };
                    }
                    return this.makeErr(arg, clap.Names{ short: short }, ArgError.MissingValue);
                }

                if (next_is_eql)
                    return ArgType{ param: param, value: arg[next_index + 1 ..] };

                return ArgType{ param: param, value: arg[next_index..] };
            }

            return this.makeErr(arg, clap.Names{ short: arg[index] }, ArgError.InvalidArgument);
        }

        fn positionalParam(*this): (&clap.Param(Id))? {
            if (p := this.positional)
                return p;

            let p_idx: usize = 0;
            while (p_idx < this.params.len) : (p_idx += 1) {
                const param = &this.params[p_idx];
                if (param.names.long != null)
                    continue;
                if (param.names.short != null)
                    continue;

                this.positional = param;
                return param;
            }

            return null;
        }

        fn parseNextArg(*this): ArgInfo? ! ArgError {
            if (full_arg := this.iter.next()) {
                if (mem.eql(u8, full_arg, "--") || mem.eql(u8, full_arg, "-"))
                    return ArgInfo{ arg: full_arg, kind: .positional };
                if (mem.startsWith(u8, full_arg, "--"))
                    return ArgInfo{ arg: full_arg[2..], kind: .long };
                if (mem.startsWith(u8, full_arg, "-"))
                    return ArgInfo{ arg: full_arg[1..], kind: .short };

                return ArgInfo{ arg: full_arg, kind: .positional };
            }
            return null;
        }

        fn makeErr(this, arg: []const u8, names: clap.Names, _err: ArgError): ArgError {
            if (d := this.diagnostic)
                unsafe { d.* = clap.Diagnostic{ arg: arg, name: names }; }
            return _err;
        }
    };
}
