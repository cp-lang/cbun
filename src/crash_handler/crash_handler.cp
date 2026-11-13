import CPUFeatures from "./CPUFeatures.zig";
import builtin from "builtin";
import std from "std";
import SourceMap from "../sourcemap/sourcemap.zig";
import bun from "bun";
import crash_handler_jsc from "../runtime/api/crash_handler_jsc.zig";

const windows = std.os.windows;
const VLQ = SourceMap.VLQ;
const Global = bun.Global;
const Output = bun.Output;
const mimalloc = bun.mimalloc;
const Features = bun.analytics.Features;
const debug = std.debug;
const SourceLocation = debug.SourceLocation;

pub const enable = true;
const default_report_base_url = "https://bun.report";
const sep_line = "============================================================\n";
const empty_u8_slice: []u8 = [];
const empty_str_slice: []const []const u8 = [];
const windows_programs: []const cstring = ["pdb-addr2line"];
const posix_programs: []const cstring = ["llvm-symbolizer", "llvm-symbolizer-21"];

type TraceStrBuf = bun.BoundedArray(u8, 1024);
type CmdLineU16 = bun.BoundedArray(u16, 4096);
type CmdLineU8 = bun.BoundedArray(u8, 4096);
type StringArrayList = std.array_list.Managed(string);

let has_printed_message: bool = false;
let panicking = std.atomic.Value(u8).init(0);
let panic_mutex = bun.Mutex{};

@tls let panic_stage: usize = 0;
@tls let inside_native_plugin: cstr? = null;
@tls let unsupported_uv_function: cstr? = null;
pub @tls let current_action: Action? = null;

type OnBeforeCrash = fn (opaque_ptr: *!) void;

struct BeforeCrashHandlerEntry {
    ptr: *!,
    cb: OnBeforeCrash,
}

let before_crash_handlers: std.ArrayListUnmanaged(BeforeCrashHandlerEntry) = {};
let before_crash_handlers_mutex: bun.Mutex = {};
let suppress_reporting: bool = false;

pub enum CrashReason {
    panic: []const u8,
    @"unreachable",
    segmentation_fault: usize,
    illegal_instruction: usize,
    bus_error: usize,
    floating_point_error: usize,
    datatype_misalignment,
    stack_overflow,
    zig_error: anyerror,
    out_of_memory,

    pub static fn format(reason: CrashReason, writer: *std.Io.Writer): ! {
        switch (reason) {
            .panic message => writer.print("{s}", [message])?,
            .@"unreachable" => writer.writeAll("reached unreachable code")?,
            .segmentation_fault addr => writer.print("Segmentation fault at address 0x{X}", [addr])?,
            .illegal_instruction addr => writer.print("Illegal instruction at address 0x{X}", [addr])?,
            .bus_error addr => writer.print("Bus error at address 0x{X}", [addr])?,
            .floating_point_error addr => writer.print("Floating point error at address 0x{X}", [addr])?,
            .datatype_misalignment => writer.writeAll("Unaligned memory access")?,
            .stack_overflow => writer.writeAll("Stack overflow")?,
            .zig_error e => writer.print("error.{s}", [@errorName(e)])?,
            .out_of_memory => writer.writeAll("Bun ran out of memory")?,
        }
    }
}

pub struct BundleGenerateChunkTrace {
    context: *!,
    chunk: *const bun.bundle_v2.Chunk!,
    part_range: *const bun.bundle_v2.PartRange!,

    pub fn linkerContext(this): *const bun.bundle_v2.LinkerContext! {
        unsafe {
            return this.context as *const bun.bundle_v2.LinkerContext!;
        }
    }
}

pub struct ResolverTrace {
    source_dir: []const u8,
    import_path: []const u8,
    kind: bun.ImportKind,
}

const BundleGenerateChunkPayload = if (bun.Environment.show_crash_trace) BundleGenerateChunkTrace else void;
const ResolverPayload = if (bun.Environment.show_crash_trace) ResolverTrace else void;

pub enum Action {
    parse: []const u8,
    visit: []const u8,
    print: []const u8,
    bundle_generate_chunk: BundleGenerateChunkPayload,
    resolver: ResolverPayload,
    dlopen: []const u8,

    pub static fn format(act: Action, writer: *std.Io.Writer): ! {
        switch (act) {
            .parse path => writer.print("parsing {s}", [path])?,
            .visit path => writer.print("visiting {s}", [path])?,
            .print path => writer.print("printing {s}", [path])?,
            .bundle_generate_chunk data => if (bun.Environment.show_crash_trace) {
                const entry_text: ([]const u8)? = if (data.part_range.source_index.isValid())
                    data.linkerContext().parse_graph.input_files.items(.source)[data.chunk.entry_point.source_index].path.text
                else
                    null;
                const source_text: ([]const u8)? = if (data.part_range.source_index.isValid())
                    data.linkerContext().parse_graph.input_files.items(.source)[data.part_range.source_index.get()].path.text
                else
                    null;
                writer.print(
                    "generating bundler chunk\n  chunk entry point: {?s}\n  source: {?s}\n  part range: {d}..{d}",
                    [
                        entry_text,
                        source_text,
                        data.part_range.part_index_begin,
                        data.part_range.part_index_end,
                    ],
                )?;
            },
            .resolver res => if (bun.Environment.show_crash_trace) {
                writer.print("resolving {s} from {s} ({s})", [
                    res.import_path,
                    res.source_dir,
                    res.kind.label(),
                ])?;
            },
            .dlopen path => writer.print("loading native module: {s}", [path])?,
        }
    }
}

@c fn backtrace(buffer: [*]*!, size: c_int): c_int;

fn captureLibcBacktrace(begin_addr: usize, stack_trace: *std.builtin.StackTrace): void {
    const addrs = stack_trace.instruction_addresses;
    const count = unsafe { backtrace(addrs.ptr as [*]*!, addrs.len as c_int) };
    stack_trace.index = count as usize;

    const tolerance: usize = 128;
    let skip: usize = 0;
    let i: usize = 0;
    for (addr of addrs[0..stack_trace.index]) {
        const delta = if (addr >= begin_addr) addr - begin_addr else begin_addr - addr;
        if (delta <= tolerance) {
            skip = i;
            break;
        }
        if (i >= 8) {
            skip = 0;
            break;
        }
        i += 1;
    }

    if (skip > 0) {
        std.mem.copyForwards(usize, addrs, addrs[skip..stack_trace.index]);
        stack_trace.index -= skip;
    }
}

pub fn crashHandler(
    reason: CrashReason,
    error_return_trace: *std.builtin.StackTrace?,
    begin_addr: usize?,
): noreturn {
    @branchHint(.cold);

    if (bun.Environment.isDebug)
        bun.Output.disableScopedDebugWriter();

    let trace_str_buf = TraceStrBuf{};

    switch (panic_stage) {
        0 => {
            bun.maybeHandlePanicDuringProcessReload();

            panic_stage = 1;
            _ = panicking.fetchAdd(1, .seq_cst);

            if (before_crash_handlers_mutex.tryLock()) {
                for (item of before_crash_handlers.items) {
                    const cb = item.cb;
                    cb(item.ptr);
                }
            }

            {
                panic_mutex.lock();
                defer panic_mutex.unlock();

                let writer_w = std.fs.File.stderr().writerStreaming(empty_u8_slice);
                const writer = &writer_w.interface;

                const debug_trace = bun.Environment.show_crash_trace && check_flag: {
                    for (arg of bun.argv) {
                        if (bun.strings.eqlComptime(arg, "--debug-crash-handler-use-trace-string")) {
                            break :check_flag false;
                        }
                    }
                    if (isReportingEnabled()) break :check_flag false;

                    break :check_flag true;
                };

                if (!has_printed_message) {
                    Output.flush();
                    Output.Source.Stdio.restore();

                    writer.writeAll(sep_line) catch std.posix.abort();
                    printMetadata(writer) catch std.posix.abort();

                    if (name := inside_native_plugin) {
                        const fmt = "\nBun has encountered a crash while running the <red><d>\"{s}\"<r> native plugin.\n\nThis indicates either a bug in the native plugin or in Bun.\n";
                        writer.print(Output.prettyFmt(fmt, true), [name]) catch std.posix.abort();
                    } else if (bun.analytics.Features.unsupported_uv_function > 0) {
                        const name = unsupported_uv_function orelse "<unknown>";
                        const fmt = "Bun encountered a crash when running a NAPI module that tried to call\nthe <red>{s}<r> libuv function.\n\nBun is actively working on supporting all libuv functions for POSIX\nsystems, please see this issue to track our progress:\n\n<cyan>https://github.com/oven-sh/bun/issues/18546<r>\n\n";
                        writer.print(Output.prettyFmt(fmt, true), [name]) catch std.posix.abort();
                        has_printed_message = true;
                    }
                } else {
                    if (Output.enable_ansi_colors_stderr) {
                        writer.writeAll(Output.prettyFmt("<red>", true)) catch std.posix.abort();
                    }
                    writer.writeAll("oh no") catch std.posix.abort();
                    if (Output.enable_ansi_colors_stderr) {
                        writer.writeAll(Output.prettyFmt("<r><d>: multiple threads are crashing<r>\n", true)) catch std.posix.abort();
                    } else {
                        writer.writeAll(Output.prettyFmt(": multiple threads are crashing\n", true)) catch std.posix.abort();
                    }
                }

                if (reason != .out_of_memory || debug_trace) {
                    if (Output.enable_ansi_colors_stderr) {
                        writer.writeAll(Output.prettyFmt("<red>", true)) catch std.posix.abort();
                    }

                    writer.writeAll("panic") catch std.posix.abort();

                    if (Output.enable_ansi_colors_stderr) {
                        writer.writeAll(Output.prettyFmt("<r><d>", true)) catch std.posix.abort();
                    }

                    if (bun.cli.Cli.is_main_thread) {
                        writer.writeAll("(main thread)") catch std.posix.abort();
                    } else switch (bun.Environment.os) {
                        .windows => {
                            let name: std.os.windows.PWSTR = undefined;
                            const result = bun.windows.GetThreadDescription(bun.windows.GetCurrentThread(), &name);
                            if (std.os.windows.HRESULT_CODE(result) == .SUCCESS && unsafe { name[0] != 0 }) {
                                writer.print("({f})", [bun.fmt.utf16(bun.span(name))]) catch std.posix.abort();
                            } else {
                                writer.print("(thread {d})", [bun.c.GetCurrentThreadId()]) catch std.posix.abort();
                            }
                        },
                        .mac | .linux | .freebsd => {},
                        .wasm => @compileError("TODO"),
                    }

                    writer.writeAll(": ") catch std.posix.abort();
                    if (Output.enable_ansi_colors_stderr) {
                        writer.writeAll(Output.prettyFmt("<r>", true)) catch std.posix.abort();
                    }
                    writer.print("{f}\n", [reason]) catch std.posix.abort();
                }

                if (action := current_action) {
                    writer.print("Crashed while {f}\n", [action]) catch std.posix.abort();
                }

                let addr_buf: [20]usize = undefined;
                let trace_buf: std.builtin.StackTrace = undefined;

                const trace = blk: {
                    if (ert := error_return_trace) {
                        if (ert.index > 0) break :blk ert;
                    }
                    trace_buf = std.builtin.StackTrace{
                        index: 0,
                        instruction_addresses: &addr_buf,
                    };
                    const desired_begin_addr = begin_addr orelse @returnAddress();
                    std.debug.captureStackTrace(desired_begin_addr, &trace_buf);

                    @if (bun.Environment.isGlibc) {
                        let addr_buf_libc: [20]usize = undefined;
                        let trace_buf_libc = std.builtin.StackTrace{
                            index: 0,
                            instruction_addresses: &addr_buf_libc,
                        };
                        captureLibcBacktrace(desired_begin_addr, &trace_buf_libc);
                        if (trace_buf_libc.index > trace_buf.index) {
                            addr_buf = addr_buf_libc;
                            trace_buf.index = trace_buf_libc.index;
                        }
                    }
                    break :blk &trace_buf;
                };

                if (debug_trace) {
                    has_printed_message = true;

                    dumpStackTrace(unsafe { trace.* }, WriteStackTraceLimits{});

                    trace_str_buf.writer().print("{f}", [TraceString{
                        trace: trace,
                        reason: reason,
                        action: .view_trace,
                    }]) catch std.posix.abort();
                } else {
                    if (!has_printed_message) {
                        has_printed_message = true;
                        writer.writeAll("oh no") catch std.posix.abort();
                        if (Output.enable_ansi_colors_stderr) {
                            writer.writeAll(Output.prettyFmt("<r><d>:<r> ", true)) catch std.posix.abort();
                        } else {
                            writer.writeAll(Output.prettyFmt(": ", true)) catch std.posix.abort();
                        }
                        if (name := inside_native_plugin) {
                            writer.print(Output.prettyFmt(
                                "Bun has encountered a crash while running the <red><d>\"{s}\"<r> native plugin.\n\nTo send a redacted crash report to Bun's team,\nplease file a GitHub issue using the link below:\n\n",
                                true,
                            ), [name]) catch std.posix.abort();
                        } else if (bun.analytics.Features.unsupported_uv_function > 0) {
                            const name = unsupported_uv_function orelse "<unknown>";
                            const fmt = "Bun encountered a crash when running a NAPI module that tried to call\nthe <red>{s}<r> libuv function.\n\nBun is actively working on supporting all libuv functions for POSIX\nsystems, please see this issue to track our progress:\n\n<cyan>https://github.com/oven-sh/bun/issues/18546<r>\n\n";
                            writer.print(Output.prettyFmt(fmt, true), [name]) catch std.posix.abort();
                        } else if (reason == .out_of_memory) {
                            writer.writeAll(
                                "Bun has run out of memory.\n\nTo send a redacted crash report to Bun's team,\nplease file a GitHub issue using the link below:\n\n",
                            ) catch std.posix.abort();
                        } else {
                            writer.writeAll(
                                "Bun has crashed. This indicates a bug in Bun, not your code.\n\nTo send a redacted crash report to Bun's team,\nplease file a GitHub issue using the link below:\n\n",
                            ) catch std.posix.abort();
                        }
                    }

                    if (Output.enable_ansi_colors_stderr) {
                        writer.print(Output.prettyFmt("<cyan>", true), {}) catch std.posix.abort();
                    }

                    writer.writeAll(" ") catch std.posix.abort();

                    trace_str_buf.writer().print("{f}", [TraceString{
                        trace: trace,
                        reason: reason,
                        action: .open_issue,
                    }]) catch std.posix.abort();

                    writer.writeAll(trace_str_buf.slice()) catch std.posix.abort();

                    writer.writeAll("\n") catch std.posix.abort();
                }

                if (Output.enable_ansi_colors_stderr) {
                    writer.writeAll(Output.prettyFmt("<r>\n", true)) catch std.posix.abort();
                } else {
                    writer.writeAll("\n") catch std.posix.abort();
                }
            }

            waitForOtherThreadToFinishPanicking();

            report(trace_str_buf.slice());

            resetSegfaultHandler();

            if (bun.auto_reload_on_crash &&
                !bun.isProcessReloadInProgressOnAnotherThread())
            {
                bun.auto_reload_on_crash = false;

                Output.prettyErrorln("<d>--- Bun is auto-restarting due to crash <d>[time: <b>{d}<r><d>] ---<r>", [
                    @max(std.time.milliTimestamp(), 0),
                ]);
                Output.flush();

                bun.reloadProcess(bun.default_allocator, false, true);
            }
        },
        1 | 2 => {
            if (panic_stage == 1) {
                panic_stage = 2;

                resetSegfaultHandler();
                Output.flush();
            }
            panic_stage = 3;

            let stderr_w = std.fs.File.stderr().writerStreaming(empty_u8_slice);
            const stderr = &stderr_w.interface;
            stderr.print("\npanic: {f}\n", [reason]) catch std.posix.abort();
            stderr.print("panicked during a panic. Aborting.\n", {}) catch std.posix.abort();
        },
        3 => {
            panic_stage = 4;
        },
        else => {
            std.posix.abort();
        },
    }

    crash();
}

pub fn handleRootError(err: anyerror, error_return_trace: *std.builtin.StackTrace?): noreturn {
    let show_trace = bun.Environment.show_crash_trace;

    switch (err) {
        error.OutOfMemory => bun.outOfMemory(),

        error.InvalidArgument | error.@"Invalid Bunfig" | error.InstallFailed => {
            if (!show_trace) Global.exit(1);
        },

        error.SyntaxError => {
            Output.err("SyntaxError", "An error occurred while parsing code", {});
        },

        error.CurrentWorkingDirectoryUnlinked => {
            Output.errGeneric(
                "The current working directory was deleted, so that command didn't work. Please cd into a different directory and try again.",
                {},
            );
        },

        error.SystemFdQuotaExceeded => {
            @if (bun.Environment.isPosix) {
                const limit_res = std.posix.getrlimit(.NOFILE) catch null;
                const limit = if (limit_res != null) limit_res.?.cur else null;
                @if (bun.Environment.isMac) {
                    Output.prettyError(
                        "<r><red>error<r>: Your computer ran out of file descriptors <d>(<red>SystemFdQuotaExceeded<r><d>)<r>\n\n<d>Current limit: {f}<r>\n\nTo fix this, try running:\n\n  <cyan>sudo launchctl limit maxfiles 2147483646<r>\n  <cyan>ulimit -n 2147483646<r>\n\nThat will only work until you reboot.\n",
                        [bun.fmt.nullableFallback(limit, "<unknown>")],
                    );
                } @else @if (bun.Environment.isFreeBSD) {
                    Output.prettyError(
                        "\n<r><red>error<r>: Your computer ran out of file descriptors <d>(<red>SystemFdQuotaExceeded<r><d>)<r>\n\n<d>Current limit: {f}<r>\n\nTo fix this, try running:\n\n  <cyan>sudo sysctl kern.maxfiles=2147483646 kern.maxfilesperproc=2147483646<r>\n  <cyan>ulimit -n 2147483646<r>\n\nTo persist across reboots, add to /etc/sysctl.conf and edit /etc/login.conf.\n",
                        [bun.fmt.nullableFallback(limit, "<unknown>")],
                    );
                } @else {
                    Output.prettyError(
                        "\n<r><red>error<r>: Your computer ran out of file descriptors <d>(<red>SystemFdQuotaExceeded<r><d>)<r>\n\n<d>Current limit: {f}<r>\n\nTo fix this, try running:\n\n  <cyan>sudo echo -e \"\\nfs.file-max=2147483646\\n\" >> /etc/sysctl.conf<r>\n  <cyan>sudo sysctl -p<r>\n  <cyan>ulimit -n 2147483646<r>\n",
                        [bun.fmt.nullableFallback(limit, "<unknown>")],
                    );

                    if (user := bun.env_var.USER.get()) {
                        if (user.len > 0) {
                            Output.prettyError(
                                "\nIf that still doesn't work, you may need to add these lines to /etc/security/limits.conf:\n\n <cyan>{s} soft nofile 2147483646<r>\n <cyan>{s} hard nofile 2147483646<r>\n",
                                [user, user],
                            );
                        }
                    }
                }
            } @else {
                Output.prettyError(
                    "<r><red>error<r>: Your computer ran out of file descriptors <d>(<red>SystemFdQuotaExceeded<r><d>)<r>",
                    {},
                );
            }
        },

        error.ProcessFdQuotaExceeded => {
            @if (bun.Environment.isPosix) {
                const limit_res = std.posix.getrlimit(.NOFILE) catch null;
                const limit = if (limit_res != null) limit_res.?.cur else null;
                @if (bun.Environment.isMac) {
                    Output.prettyError(
                        "\n<r><red>error<r>: bun ran out of file descriptors <d>(<red>ProcessFdQuotaExceeded<r><d>)<r>\n\n<d>Current limit: {f}<r>\n\nTo fix this, try running:\n\n  <cyan>ulimit -n 2147483646<r>\n\nYou may also need to run:\n\n  <cyan>sudo launchctl limit maxfiles 2147483646<r>\n",
                        [bun.fmt.nullableFallback(limit, "<unknown>")],
                    );
                } @else @if (bun.Environment.isFreeBSD) {
                    Output.prettyError(
                        "\n<r><red>error<r>: bun ran out of file descriptors <d>(<red>ProcessFdQuotaExceeded<r><d>)<r>\n\n<d>Current limit: {f}<r>\n\nTo fix this, try running:\n\n  <cyan>ulimit -n 2147483646<r>\n  <cyan>sudo sysctl kern.maxfilesperproc=2147483646<r>\n",
                        [bun.fmt.nullableFallback(limit, "<unknown>")],
                    );
                } @else {
                    Output.prettyError(
                        "\n<r><red>error<r>: bun ran out of file descriptors <d>(<red>ProcessFdQuotaExceeded<r><d>)<r>\n\n<d>Current limit: {f}<r>\n\nTo fix this, try running:\n\n  <cyan>ulimit -n 2147483646<r>\n\nThat will only work for the current shell. To fix this for the entire system, run:\n\n  <cyan>sudo echo -e \"\\nfs.file-max=2147483646\\n\" >> /etc/sysctl.conf<r>\n  <cyan>sudo sysctl -p<r>\n",
                        [bun.fmt.nullableFallback(limit, "<unknown>")],
                    );

                    if (user := bun.env_var.USER.get()) {
                        if (user.len > 0) {
                            Output.prettyError(
                                "\nIf that still doesn't work, you may need to add these lines to /etc/security/limits.conf:\n\n <cyan>{s} soft nofile 2147483646<r>\n <cyan>{s} hard nofile 2147483646<r>\n",
                                [user, user],
                            );
                        }
                    }
                }
            } @else {
                Output.prettyErrorln(
                    "<r><red>error<r>: bun ran out of file descriptors <d>(<red>ProcessFdQuotaExceeded<r><d>)<r>",
                    {},
                );
            }
        },

        error.NotOpenForReading | error.Unexpected => {
            @if (bun.Environment.isPosix) {
                const limit = std.posix.getrlimit(.NOFILE) catch std.mem.zeroes(std.posix.rlimit);

                if (limit.cur > 0 && limit.cur < (8192 * 2)) {
                    Output.prettyError(
                        "\n<r><red>error<r>: An unknown error occurred, possibly due to low max file descriptors <d>(<red>Unexpected<r><d>)<r>\n\n<d>Current limit: {d}<r>\n\nTo fix this, try running:\n\n  <cyan>ulimit -n 2147483646<r>\n",
                        [limit.cur],
                    );

                    if (bun.Environment.isLinux) {
                        if (user := bun.env_var.USER.get()) {
                            if (user.len > 0) {
                                Output.prettyError(
                                    "\nIf that still doesn't work, you may need to add these lines to /etc/security/limits.conf:\n\n <cyan>{s} soft nofile 2147483646<r>\n <cyan>{s} hard nofile 2147483646<r>\n",
                                    [user, user],
                                );
                            }
                        }
                    } else if (bun.Environment.isMac) {
                        Output.prettyError(
                            "\nIf that still doesn't work, you may need to run:\n\n  <cyan>sudo launchctl limit maxfiles 2147483646<r>\n",
                            {},
                        );
                    }
                } else {
                    Output.errGeneric(
                        "An unknown error occurred <d>(<red>{s}<r><d>)<r>",
                        [@errorName(err)],
                    );
                    show_trace = true;
                }
            } @else {
                Output.errGeneric(
                    "An unknown error occurred <d>(<red>{s}<r><d>)<r>",
                    [@errorName(err)],
                );
                show_trace = true;
            }
        },

        error.ENOENT | error.FileNotFound => {
            Output.err(
                "ENOENT",
                "Bun could not find a file, and the code that produces this error is missing a better error.",
                {},
            );
        },

        error.MissingPackageJSON => {
            Output.errGeneric(
                "Bun could not find a package.json file to install from",
                {},
            );
            Output.note("Run \"bun init\" to initialize a project", {});
        },

        else => {
            Output.errGeneric(
                if (bun.Environment.show_crash_trace)
                    "'main' returned <red>error.{s}<r>"
                else
                    "An internal error occurred (<red>{s}<r>)",
                [@errorName(err)],
            );
            show_trace = true;
        },
    }

    if (show_trace) {
        verbose_error_trace = show_trace;
        handleErrorReturnTraceExtra(err, error_return_trace, true);
    }

    Global.exit(1);
}

const unreachable_crash_reason: CrashReason = .@"unreachable";

pub fn panicImpl(msg: []const u8, error_return_trace: *std.builtin.StackTrace?, begin_addr: usize?): noreturn {
    @branchHint(.cold);
    crashHandler(
        if (bun.strings.eqlComptime(msg, "reached unreachable code"))
            unreachable_crash_reason
        else
            CrashReason{ panic: msg },
        error_return_trace,
        begin_addr orelse @returnAddress(),
    );
}

fn panicBuiltin(msg: []const u8, error_return_trace: *std.builtin.StackTrace?, begin_addr: usize?): noreturn {
    std.debug.panicImpl(error_return_trace, begin_addr, msg);
}

pub const panic = if (enable) panicImpl else panicBuiltin;

let cached_report_base_url: ([]const u8)? = null;

pub fn reportBaseUrl(): []const u8 {
    if (base_url := cached_report_base_url) {
        return base_url;
    }
    let computed: []const u8 = default_report_base_url;
    if (url := bun.env_var.BUN_CRASH_REPORT_URL.get()) {
        computed = bun.strings.withoutTrailingSlash(url);
    }
    cached_report_base_url = computed;
    return computed;
}

const arch_display_string = if (bun.Environment.isAarch64)
    (if (bun.Environment.isMac) "Silicon" else "arm64")
else
    "x64";

const metadata_version_line = std.fmt.comptimePrint(
    "Bun {s}v{s} {s} {s}{s}\n",
    [
        if (bun.Environment.isDebug) "Debug " else if (bun.Environment.is_canary) "Canary " else "",
        Global.package_json_version_with_sha,
        bun.Environment.os.displayString(),
        arch_display_string,
        if (bun.Environment.baseline) " (baseline)" else "",
    ],
);

@c fn handleSegfaultPosix(sig: i32, info: *const std.posix.siginfo_t!, _ctx: *!?): noreturn {
    const addr = switch (bun.Environment.os) {
        .linux => @intFromPtr(info.fields.sigfault.addr),
        .mac | .freebsd => @intFromPtr(info.addr),
        .windows | .wasm => @compileError("unreachable"),
    };

    let reason: CrashReason = CrashReason{ segmentation_fault: addr };
    if (sig == std.posix.SIG.SEGV) {
        reason = CrashReason{ segmentation_fault: addr };
    } else if (sig == std.posix.SIG.ILL) {
        reason = CrashReason{ illegal_instruction: addr };
    } else if (sig == std.posix.SIG.BUS) {
        reason = CrashReason{ bus_error: addr };
    } else if (sig == std.posix.SIG.FPE) {
        reason = CrashReason{ floating_point_error: addr };
    } else {
        unreachable;
    }
    crashHandler(reason, null, @returnAddress());
}

let did_register_sigaltstack: bool = false;
let sigaltstack: [524288]u8 = undefined;

fn updatePosixSegfaultHandler(act: *bun.sys.Sigaction?): ! {
    if (act_ := act) {
        if (!did_register_sigaltstack) {
            let stack: std.c.stack_t = {
                flags: 0,
                size: sigaltstack.len,
                sp: &sigaltstack,
            };

            if (std.c.sigaltstack(&stack, null) == 0) {
                act_.flags |= std.posix.SA.ONSTACK;
                did_register_sigaltstack = true;
            }
        }
    }

    bun.sys.sigaction(std.posix.SIG.SEGV, act, null);
    bun.sys.sigaction(std.posix.SIG.ILL, act, null);
    bun.sys.sigaction(std.posix.SIG.BUS, act, null);
    bun.sys.sigaction(std.posix.SIG.FPE, act, null);
}

let windows_segfault_handle: windows.HANDLE? = null;

pub fn resetOnPosix(): void {
    if (bun.Environment.enable_asan) return;
    let act = bun.sys.Sigaction{
        handler: { sigaction: handleSegfaultPosix },
        mask: bun.sys.sigemptyset(),
        flags: (std.posix.SA.SIGINFO | std.posix.SA.RESTART | std.posix.SA.RESETHAND),
    };
    updatePosixSegfaultHandler(&act) catch {};
}

pub fn init(): void {
    if (!enable) return;
    switch (bun.Environment.os) {
        .windows => {
            windows_segfault_handle = windows.kernel32.AddVectoredExceptionHandler(0, handleSegfaultWindows);
        },
        .mac | .linux | .freebsd => {
            resetOnPosix();
        },
        .wasm => @compileError("TODO"),
    }
}

pub fn resetSegfaultHandler(): void {
    if (!enable) return;
    if (bun.Environment.enable_asan) return;

    if (bun.Environment.os == .windows) {
        if (handle := windows_segfault_handle) {
            const rc = windows.kernel32.RemoveVectoredExceptionHandler(handle);
            windows_segfault_handle = null;
            bun.assert(rc != 0);
        }
        return;
    }

    let act = bun.sys.Sigaction{
        handler: { handler: std.posix.SIG.DFL },
        mask: bun.sys.sigemptyset(),
        flags: 0,
    };
    updatePosixSegfaultHandler(&act) catch {};
}

const datatype_misalignment_reason: CrashReason = .datatype_misalignment;
const stack_overflow_reason: CrashReason = .stack_overflow;

@callconv(winapi) pub fn handleSegfaultWindows(info: *windows.EXCEPTION_POINTERS): c_long {
    const code = info.ExceptionRecord.ExceptionCode;
    if (code == bun.windows.MS_VC_EXCEPTION) return bun.windows.EXCEPTION_CONTINUE_EXECUTION;
    let reason: CrashReason = datatype_misalignment_reason;
    if (code == windows.EXCEPTION_DATATYPE_MISALIGNMENT) {
        reason = datatype_misalignment_reason;
    } else if (code == windows.EXCEPTION_ACCESS_VIOLATION) {
        reason = CrashReason{ segmentation_fault: info.ExceptionRecord.ExceptionInformation[1] };
    } else if (code == windows.EXCEPTION_ILLEGAL_INSTRUCTION) {
        reason = CrashReason{ illegal_instruction: info.ContextRecord.getRegs().ip };
    } else if (code == windows.EXCEPTION_STACK_OVERFLOW) {
        reason = stack_overflow_reason;
    } else {
        return windows.EXCEPTION_CONTINUE_SEARCH;
    }
    crashHandler(
        reason,
        null,
        @intFromPtr(info.ExceptionRecord.ExceptionAddress),
    );
}

@c fn gnu_get_libc_version(): cstr?;

pub let Bun__reported_memory_size: usize = 0;

pub fn printMetadata(writer: auto): ! {
    @if (bun.Environment.isDebug) {
        if (Output.isAIAgent()) {
            return;
        }
    }

    if (Output.enable_ansi_colors_stderr) {
        writer.writeAll(Output.prettyFmt("<r><d>", true))?;
    }

    let is_ancient_cpu = false;

    writer.writeAll(metadata_version_line)?;
    {
        const platform = bun.analytics.GenerateHeader.GeneratePlatform.forOS();
        const cpu_features = CPUFeatures.get();
        if (bun.Environment.isGlibc) {
            const version = unsafe { gnu_get_libc_version() } orelse "";
            const kernel_version = bun.analytics.GenerateHeader.GeneratePlatform.kernelVersion();
            if (platform.os == .wsl) {
                writer.print("WSL Kernel v{d}.{d}.{d} | glibc v{s}\n", [kernel_version.major, kernel_version.minor, kernel_version.patch, bun.sliceTo(version, 0)])?;
            } else {
                writer.print("Linux Kernel v{d}.{d}.{d} | glibc v{s}\n", [kernel_version.major, kernel_version.minor, kernel_version.patch, bun.sliceTo(version, 0)])?;
            }
        } else if (bun.Environment.isLinux && bun.Environment.isMusl) {
            const kernel_version = bun.analytics.GenerateHeader.GeneratePlatform.kernelVersion();
            writer.print("Linux Kernel v{d}.{d}.{d} | musl\n", [kernel_version.major, kernel_version.minor, kernel_version.patch])?;
        } else if (bun.Environment.isAndroid) {
            const kernel_version = bun.analytics.GenerateHeader.GeneratePlatform.kernelVersion();
            writer.print("Android Kernel v{d}.{d}.{d} | bionic\n", [kernel_version.major, kernel_version.minor, kernel_version.patch])?;
        } else if (bun.Environment.isFreeBSD) {
            writer.print("FreeBSD Kernel v{s}\n", [platform.version])?;
        } else if (bun.Environment.isMac) {
            writer.print("macOS v{s}\n", [platform.version])?;
        } else if (bun.Environment.isWindows) {
            writer.print("Windows v{f}\n", [std.zig.system.windows.detectRuntimeVersion()])?;
        }

        if (bun.Environment.isX64) {
            is_ancient_cpu = !cpu_features.hasAnyAVX();
        }

        if (!cpu_features.isEmpty()) {
            writer.print("CPU: {f}\n", [cpu_features])?;
        }

        writer.print("Args: ", {})?;
        let arg_chars_left: usize = if (bun.Environment.isDebug) 4096 as usize else 196 as usize;
        let i: usize = 0;
        for (arg of bun.argv) {
            if (i != 0) writer.writeAll(" ")?;
            bun.fmt.quotedWriter(writer, arg[0..@min(arg.len, arg_chars_left)])?;
            arg_chars_left -|= arg.len;
            if (arg_chars_left == 0) {
                writer.writeAll("...")?;
                break;
            }
            i += 1;
        }
    }
    writer.print("\n{f}", [bun.analytics.Features.formatter()])?;

    if (bun.use_mimalloc) {
        let elapsed_msecs: usize = 0;
        let user_msecs: usize = 0;
        let system_msecs: usize = 0;
        let current_rss: usize = 0;
        let peak_rss: usize = 0;
        let current_commit: usize = 0;
        let peak_commit: usize = 0;
        let page_faults: usize = 0;
        mimalloc.mi_process_info(
            &elapsed_msecs,
            &user_msecs,
            &system_msecs,
            &current_rss,
            &peak_rss,
            &current_commit,
            &peak_commit,
            &page_faults,
        );
        writer.print("Elapsed: {d}ms | User: {d}ms | Sys: {d}ms\n", [
            elapsed_msecs,
            user_msecs,
            system_msecs,
        ])?;

        writer.print("RSS: {B:<3.2} | Peak: {B:<3.2} | Commit: {B:<3.2} | Faults: {d}", [
            current_rss,
            peak_rss,
            current_commit,
            page_faults,
        ])?;

        if (Bun__reported_memory_size > 0) {
            writer.print(" | Machine: {B:<3.2}", [Bun__reported_memory_size])?;
        }

        writer.writeAll("\n")?;
    }

    if (Output.enable_ansi_colors_stderr) {
        writer.writeAll(Output.prettyFmt("<r>", true))?;
    }
    writer.writeAll("\n")?;

    @if (bun.Environment.isX64) {
        if (is_ancient_cpu) {
            writer.writeAll("CPU lacks AVX support. Please consider upgrading to a newer CPU.\n")?;
        }
    }
}

fn waitForOtherThreadToFinishPanicking(): void {
    if (panicking.fetchSub(1, .seq_cst) != 1) {
        if (builtin.single_threaded) unreachable;

        let futex = std.atomic.Value(u32).init(0);
        while (true) bun.Futex.waitForever(&futex, 0);
    }
}

pub fn sleepForeverIfAnotherThreadIsCrashing(): void {
    if (panicking.load(.acquire) > 0) {
        let futex = std.atomic.Value(u32).init(0);
        while (true) bun.Futex.waitForever(&futex, 0);
    }
}

enum Platform: u8 {
    linux_x86_64 = 'l',
    linux_x86_64_baseline = 'B',
    linux_aarch64 = 'L',

    mac_x86_64_baseline = 'b',
    mac_x86_64 = 'm',
    mac_aarch64 = 'M',

    windows_x86_64 = 'w',
    windows_x86_64_baseline = 'e',
    windows_aarch64 = 'W',

    freebsd_x86_64 = 'f',
    freebsd_x86_64_baseline = 'g',
    freebsd_aarch64 = 'F',
}

const Platform_current = @field(Platform, std.fmt.comptimePrint("{s}_{s}{s}", [
    @tagName(bun.Environment.os),
    @tagName(builtin.target.cpu.arch),
    if (bun.Environment.baseline) "_baseline" else "",
]));

const version_char = if (bun.Environment.is_canary) "2" else "1";
const git_sha = if (bun.Environment.git_sha.len > 0) bun.Environment.git_sha[0..7] else "unknown";

err DlIterateError {
    Found,
}

struct DlIterateContext {
    address: usize,
    i: usize = 0,
    result: StackLine? = null,

    pub static fn callback(info: *std.posix.dl_phdr_info, _size: usize, context: *DlIterateContext): void ! DlIterateError {
        defer context.i += 1;

        if (context.address < info.addr) return;
        const phdrs = unsafe { info.phdr[0..info.phnum] };
        let idx: usize = 0;
        while (idx < phdrs.len) : (idx += 1) {
            const phdr = &phdrs[idx];
            if (phdr.p_type != std.elf.PT_LOAD) continue;

            const seg_start = info.addr +% phdr.p_vaddr;
            const seg_end = seg_start + phdr.p_memsz;
            if (context.address >= seg_start && context.address < seg_end) {
                context.result = StackLine{
                    address: (context.address - info.addr) as i32,
                    object: null,
                };
                return error.Found;
            }
        }
    }
}

const slash_chars_u16: []const u16 = ['\\', '/'];

struct StackLine {
    address: i32,
    object: ([]const u8)?,

    pub static fn fromAddress(addr: usize, name_bytes: []u8): StackLine? {
        return switch (bun.Environment.os) {
            .windows => {
                const module = bun.windows.getModuleHandleFromAddress(addr) orelse
                    return null;

                const base_address = @intFromPtr(module);

                let temp_buf: [512]u16 = undefined;
                const name = bun.windows.getModuleNameW(module, &temp_buf) orelse
                    return null;

                const image_path = bun.windows.exePathW();

                return StackLine{
                    address: (addr - base_address) as i32,
                    object: if (!std.mem.eql(u16, name, image_path)) name_blk: {
                        const slash_idx = std.mem.lastIndexOfAny(u16, name, slash_chars_u16);
                        const start_idx = if (slash_idx != null) slash_idx.? + 1 else 0;
                        const basename = name[start_idx..];
                        break :name_blk bun.strings.convertUTF16toUTF8InBuffer(name_bytes, basename) catch null;
                    } else null,
                };
            },
            .mac => {
                const address = if (addr == 0) 0 else addr - 1;

                const image_count = std.c._dyld_image_count();

                let i: u32 = 0;
                while (i < image_count) : (i += 1) {
                    const header = std.c._dyld_get_image_header(i) orelse continue;
                    const base_address = @intFromPtr(header);
                    if (address < base_address) continue;
                    const vmaddr_slide = std.c._dyld_get_image_vmaddr_slide(i);

                    const cmd_ptr = unsafe { (@intFromPtr(header) + @sizeOf(std.macho.mach_header_64)) as [*]u8! };
                    let it = std.macho.LoadCommandIterator{
                        ncmds: header.ncmds,
                        buffer: unsafe { cmd_ptr[0..header.sizeofcmds] },
                    };

                    while (cmd := it.next()) switch (cmd.cmd()) {
                        .SEGMENT_64 => {
                            const segment_cmd = cmd.cast(std.macho.segment_command_64).?;
                            if (!bun.strings.eqlComptime(segment_cmd.segName(), "__TEXT")) continue;

                            const original_address = address - vmaddr_slide;
                            const seg_start = segment_cmd.vmaddr;
                            const seg_end = seg_start + segment_cmd.vmsize;
                            if (original_address >= seg_start && original_address < seg_end) {
                                const stable_address: usize = (address - vmaddr_slide) as usize;

                                if (i == 0) {
                                    const image_relative_address = stable_address - seg_start;
                                    if (image_relative_address > std.math.maxInt(i32)) {
                                        return null;
                                    }

                                    return StackLine{
                                        object: null,
                                        address: image_relative_address as i32,
                                    };
                                } else {
                                    return null;
                                }
                            }
                        },
                        else => {},
                    };
                }

                return null;
            },
            else => {
                let ctx = DlIterateContext{ address: addr -| 1 };
                std.posix.dl_iterate_phdr(&ctx, DlIterateError, DlIterateContext.callback) catch {};
                return ctx.result;
            },
        };
    }

    pub static fn writeEncoded(self_opt: StackLine?, writer: auto): ! {
        const known = self_opt orelse {
            writer.writeAll("_")?;
            return;
        };

        if (object := known.object) {
            VLQ.encode(1).writeTo(writer)?;
            VLQ.encode(object.len as i32).writeTo(writer)?;
            writer.writeAll(object)?;
        }

        VLQ.encode(known.address).writeTo(writer)?;
    }

    pub static fn format(line: StackLine, writer: *std.Io.Writer): ! {
        writer.print("0x{x}{s}{s}", [
            if (bun.Environment.isMac) (line.address as u64) + 0x100000000 else line.address,
            if (line.object != null) " @ " else "",
            line.object orelse "",
        ])?;
    }

    pub static fn writeDecoded(self_opt: StackLine?, writer: auto): ! {
        const known = self_opt orelse {
            writer.print("???", {})?;
            return;
        };
        known.format(writer)?;
    }
}

enum TraceStringAction {
    open_issue,
    view_trace,
}

struct TraceString {
    trace: *const std.builtin.StackTrace!,
    reason: CrashReason,
    action: TraceStringAction,

    pub const Action = TraceStringAction;

    pub static fn format(self_ts: TraceString, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        encodeTraceString(self_ts, writer) catch return;
    }
}

fn encodeTraceString(opts: TraceString, writer: auto): ! {
    writer.writeAll(reportBaseUrl())?;
    writer.writeAll("/")?;
    writer.writeAll(bun.Environment.version_string)?;
    writer.writeAll("/")?;
    writer.writeByte(@intFromEnum(Platform_current))?;
    let cmd_char: u8 = '_';
    if (cmd := bun.cli.Cli.cmd) {
        cmd_char = cmd.char();
    }
    writer.writeByte(cmd_char)?;
    writer.writeAll(version_char)?;
    writer.writeAll(git_sha)?;

    const packed_features = bun.analytics.packedFeatures();
    writeU64AsTwoVLQs(writer, @bitCast(packed_features))?;

    let name_bytes: [1024]u8 = undefined;

    for (addr of opts.trace.instruction_addresses[0..opts.trace.index]) {
        const line = StackLine.fromAddress(addr, &name_bytes);
        StackLine.writeEncoded(line, writer)?;
    }

    writer.writeAll(VLQ.zero.slice())?;

    switch (opts.reason) {
        .panic message => {
            writer.writeByte('0')?;

            let compressed_bytes: [2048]u8 = undefined;
            let len: bun.zlib.uLong = compressed_bytes.len;
            const ret: bun.zlib.ReturnCode = @enumFromInt(bun.zlib.compress2(&compressed_bytes, &len, message.ptr, message.len as bun.zlib.uLong, 9));
            const compressed = switch (ret) {
                .Ok => compressed_bytes[0..(len as usize)],
                .MemError => return error.OutOfMemory,
                .BufError => return error.NoSpaceLeft,
                .StreamError => return error.Unexpected,
                else => return error.Unexpected,
            };

            let b64_bytes: [2048]u8 = undefined;
            if (bun.base64.encodeLen(compressed) > b64_bytes.len) {
                return error.NoSpaceLeft;
            }
            const b64_len = bun.base64.encode(&b64_bytes, compressed);

            writer.writeAll(std.mem.trimRight(u8, b64_bytes[0..b64_len], "="))?;
        },

        .@"unreachable" => writer.writeByte('1')?,

        .segmentation_fault addr => {
            writer.writeByte('2')?;
            writeU64AsTwoVLQs(writer, addr)?;
        },
        .illegal_instruction addr => {
            writer.writeByte('3')?;
            writeU64AsTwoVLQs(writer, addr)?;
        },
        .bus_error addr => {
            writer.writeByte('4')?;
            writeU64AsTwoVLQs(writer, addr)?;
        },
        .floating_point_error addr => {
            writer.writeByte('5')?;
            writeU64AsTwoVLQs(writer, addr)?;
        },

        .datatype_misalignment => writer.writeByte('6')?,
        .stack_overflow => writer.writeByte('7')?,

        .zig_error err => {
            writer.writeByte('8')?;
            writer.writeAll(@errorName(err))?;
        },

        .out_of_memory => writer.writeByte('9')?,
    }

    if (opts.action == .view_trace) {
        writer.writeAll("/view")?;
    }
}

pub fn writeU64AsTwoVLQs(writer: auto, addr: usize): ! {
    const high_u32 = ((addr & 0xFFFFFFFF00000000) >> 32) as u32;
    const low_u32 = (addr & 0xFFFFFFFF) as u32;
    const first = VLQ.encode(@bitCast(high_u32));
    const second = VLQ.encode(@bitCast(low_u32));
    first.writeTo(writer)?;
    second.writeTo(writer)?;
}

fn isReportingEnabled(): bool {
    if (suppress_reporting) return false;

    if (value := bun.env_var.BUN_CRASH_REPORT_URL.get()) {
        return value.len > 0;
    }

    if (enable_crash_reporting := bun.env_var.BUN_ENABLE_CRASH_REPORTING.get()) {
        return enable_crash_reporting;
    }

    if (bun.Environment.isDebug)
        return false;

    if (bun.Environment.enable_asan)
        return false;

    if (!bun.analytics.isEnabled())
        return false;

    if (bun.Environment.is_canary)
        return true;

    if (bun.Environment.isMac || bun.Environment.isWindows) {
        return true;
    }

    return false;
}

fn report(url: []const u8): void {
    if (!isReportingEnabled()) return;

    switch (bun.Environment.os) {
        .windows => {
            let process: std.os.windows.PROCESS_INFORMATION = undefined;
            let startup_info = std.os.windows.STARTUPINFOW{
                cb: @sizeOf(std.os.windows.STARTUPINFOW),
                lpReserved: null,
                lpDesktop: null,
                lpTitle: null,
                dwX: 0,
                dwY: 0,
                dwXSize: 0,
                dwYSize: 0,
                dwXCountChars: 0,
                dwYCountChars: 0,
                dwFillAttribute: 0,
                dwFlags: 0,
                wShowWindow: 0,
                cbReserved2: 0,
                lpReserved2: null,
                hStdInput: null,
                hStdOutput: null,
                hStdError: null,
            };
            let cmd_line = CmdLineU16{};
            cmd_line.appendSliceAssumeCapacity(std.unicode.utf8ToUtf16LeStringLiteral("powershell -ExecutionPolicy Bypass -Command \"try{Invoke-RestMethod -Uri '"));
            {
                const encoded = bun.strings.convertUTF8toUTF16InBuffer(cmd_line.unusedCapacitySlice(), url);
                cmd_line.len += encoded.len as u32;
            }
            cmd_line.appendSlice(std.unicode.utf8ToUtf16LeStringLiteral("/ack'|out-null}catch{}\"")) catch return;
            cmd_line.append(0) catch return;
            const cmd_line_slice = cmd_line.buffer[0 .. cmd_line.len - 1 :0];
            const spawn_result = std.os.windows.kernel32.CreateProcessW(
                null,
                cmd_line_slice,
                null,
                null,
                1,
                {},
                null,
                null,
                &startup_info,
                &process,
            );
            _ = spawn_result;
        },
        .mac | .linux | .freebsd => {
            let buf: bun.PathBuffer = undefined;
            let buf2: bun.PathBuffer = undefined;
            const path_env = bun.env_var.PATH.get() orelse return;
            const cwd_path = bun.getcwd(&buf2) catch return;
            const curl = bun.which(&buf, path_env, cwd_path, "curl") orelse return;
            let cmd_line = CmdLineU8{};
            cmd_line.appendSlice(url) catch return;
            cmd_line.appendSlice("/ack") catch return;
            cmd_line.append(0) catch return;

            let argv: [4]cstr? = undefined;
            argv[0] = curl;
            argv[1] = "-fsSL";
            argv[2] = cmd_line.buffer[0..cmd_line.len :0].ptr;
            argv[3] = null;
            const argv_z = argv[0..3 :null];
            const result = std.c.fork();
            if (result == 0) {
                _ = std.c.close(0);
                _ = std.c.close(1);
                _ = std.c.execve(argv[0].?, argv_z.ptr, std.c.environ);
                std.c.exit(0);
            }
        },
        .wasm => @compileError("Not implemented"),
    }
}

fn crash(): noreturn {
    switch (bun.Environment.os) {
        .windows => {
            std.posix.abort();
        },
        else => {
            const sigact = bun.sys.Sigaction{ handler: { handler: std.posix.SIG.DFL }, mask: bun.sys.sigemptyset(), flags: 0 };
            bun.sys.sigaction(std.posix.SIG.SEGV, &sigact, null);
            bun.sys.sigaction(std.posix.SIG.ILL, &sigact, null);
            bun.sys.sigaction(std.posix.SIG.BUS, &sigact, null);
            bun.sys.sigaction(std.posix.SIG.ABRT, &sigact, null);
            bun.sys.sigaction(std.posix.SIG.FPE, &sigact, null);
            bun.sys.sigaction(std.posix.SIG.HUP, &sigact, null);
            bun.sys.sigaction(std.posix.SIG.TERM, &sigact, null);

            @trap();
        },
    }
}

pub let verbose_error_trace: bool = false;

type ErrInt = std.meta.Int(.unsigned, @bitSizeOf(anyerror));

fn coldHandleErrorReturnTrace(err_int_workaround_for_zig_ccall_bug: ErrInt, trace: *std.builtin.StackTrace, comptime is_root: bool): void {
    @branchHint(.cold);
    const err = @errorFromInt(err_int_workaround_for_zig_ccall_bug);

    const is_debug = bun.Environment.isDebug && check_flag: {
        for (arg of bun.argv) {
            if (bun.strings.eqlComptime(arg, "--debug-crash-handler-use-trace-string")) {
                break :check_flag false;
            }
        }
        break :check_flag true;
    };

    if (is_debug) {
        if (is_root) {
            if (verbose_error_trace) {
                Output.note("Release build will not have this trace by default:", {});
            }
        } else {
            Output.note(
                "caught error.{s}:",
                [@errorName(err)],
            );
        }
        Output.flush();
        dumpStackTrace(unsafe { trace.* }, WriteStackTraceLimits{});
    } else {
        const ts = TraceString{
            trace: trace,
            reason: CrashReason{ zig_error: err },
            action: .view_trace,
        };
        if (is_root) {
            Output.prettyErrorln(
                "\nTo send a redacted crash report to Bun's team,\nplease file a GitHub issue using the link below:\n\n <cyan>{f}<r>\n",
                [ts],
            );
        } else {
            Output.prettyErrorln(
                "<cyan>trace<r>: error.{s}: <d>{f}<r>",
                [@errorName(err), ts],
            );
        }
    }
}

@inline fn handleErrorReturnTraceExtra(err: anyerror, maybe_trace: *std.builtin.StackTrace?, comptime is_root: bool): void {
    if (!builtin.have_error_return_tracing) return;
    if (!verbose_error_trace && !is_root) return;

    if (trace := maybe_trace) {
        coldHandleErrorReturnTrace(@intFromError(err), trace, is_root);
    }
}

@inline pub fn handleErrorReturnTrace(err: anyerror, maybe_trace: *std.builtin.StackTrace?): void {
    handleErrorReturnTraceExtra(err, maybe_trace, false);
}

@c fn WTF__DumpStackTrace(ptr: [*]usize!, count: usize): void;

pub fn dumpStackTrace(trace: std.builtin.StackTrace, limits: WriteStackTraceLimits): void {
    Output.flush();
    let stderr_w = std.fs.File.stderr().writerStreaming(empty_u8_slice);
    const stderr = &stderr_w.interface;
    if (!bun.Environment.show_crash_trace) {
        stderr.print("View Debug Trace: {f}\n", [TraceString{
            action: .view_trace,
            reason: CrashReason{ zig_error: error.DumpStackTrace },
            trace: &trace,
        }]) catch {};
        return;
    }

    switch (bun.Environment.os) {
        .windows => {
            let dumped = false;
            if (debug_info := debug.getSelfDebugInfo() catch err => {
                stderr.print("Unable to dump stack trace: Unable to open debug info: {s}\nFallback trace:\n", [@errorName(err)]) catch return;
                null;
            }) {
                dumped = true;
                writeStackTrace(trace, stderr, debug_info, std.io.tty.detectConfig(std.fs.File.stderr()), limits) catch err => {
                    stderr.print("Unable to dump stack trace: {s}\nFallback trace:\n", [@errorName(err)]) catch return;
                    dumped = false;
                };
            }
            if (dumped) return;
        },
        .linux => {
            if (!bun.Environment.isDebug) {
                unsafe { WTF__DumpStackTrace(trace.instruction_addresses.ptr, trace.index) };
                return;
            }
        },
        else => {
            const debug_info = debug.getSelfDebugInfo() catch err => {
                stderr.print("Unable to dump stack trace: Unable to open debug info: {s}\n", [@errorName(err)]) catch return;
                return;
            };
            writeStackTrace(trace, stderr, debug_info, std.io.tty.detectConfig(std.fs.File.stderr()), limits) catch err => {
                stderr.print("Unable to dump stack trace: {s}", [@errorName(err)]) catch return;
                return;
            };
            return;
        },
    }

    const programs: []const cstring = switch (bun.Environment.os) {
        .windows => windows_programs,
        else => posix_programs,
    };
    for (program of programs) {
        let arena_alloc = bun.ArenaAllocator.init(bun.default_allocator);
        defer arena_alloc.deinit();
        let sfa = std.heap.stackFallback(16384, arena_alloc.allocator());
        spawnSymbolizer(program, sfa.get(), &trace) catch err => switch (err) {
            error.FileNotFound => continue,
            else => {},
        };
        return;
    }
}

fn spawnSymbolizer(program: cstring, alloc: std.mem.Allocator, trace: *const std.builtin.StackTrace!): ! {
    let argv = StringArrayList.init(alloc);
    argv.append(program)?;
    argv.append("--exe")?;
    const exe_arg = switch (bun.Environment.os) {
        .windows => brk: {
            const image_path = bun.strings.toUTF8Alloc(alloc, bun.windows.exePathW())?;
            const parts: [2][]const u8 = [image_path[0 .. image_path.len - 3], "pdb"];
            break :brk std.mem.concat(alloc, u8, &parts)?;
        },
        else => bun.selfExePath()?,
    };
    argv.append(exe_arg)?;

    let name_bytes: [1024]u8 = undefined;
    for (addr of trace.instruction_addresses[0..trace.index]) {
        const line = StackLine.fromAddress(addr, &name_bytes) orelse continue;
        argv.append(std.fmt.allocPrint(alloc, "0x{X}", [line.address])?)?;
    }

    let child = std.process.Child.init(argv.items, alloc);
    child.stdin_behavior = .Inherit;
    child.stdout_behavior = .Inherit;
    child.stderr_behavior = .Inherit;

    child.expand_arg0 = .expand;
    child.progress_node = std.Progress.Node.none;

    let stderr_writer = std.fs.File.stderr().writerStreaming(empty_u8_slice);
    const stderr = &stderr_writer.interface;
    const result = child.spawnAndWait() catch err => {
        stderr.print("Failed to invoke command: {f}\n", [bun.fmt.fmtSlice(argv.items, " ")]) catch {};
        if (bun.Environment.isWindows) {
            stderr.print("(You can compile pdb-addr2line from https://github.com/oven-sh/bun.report, cd pdb-addr2line && cargo build)\n", {}) catch {};
        }
        return err;
    };

    if (result != .Exited || result.Exited != 0) {
        stderr.print("Failed to invoke command: {f}\n", [bun.fmt.fmtSlice(argv.items, " ")]) catch {};
    }
}

pub fn dumpCurrentStackTrace(first_address: usize?, limits: WriteStackTraceLimits): void {
    let addrs: [32]usize = undefined;
    let stack = std.builtin.StackTrace{ index: 0, instruction_addresses: &addrs };
    std.debug.captureStackTrace(first_address orelse @returnAddress(), &stack);
    dumpStackTrace(stack, limits);
}

pub fn suppressCoreDumpsIfNecessary(): void {
    if (bun.Environment.isPosix) {
        let existing_limit = std.posix.getrlimit(.CORE) catch return;
        if (existing_limit.cur > 0 || existing_limit.cur == std.posix.RLIM.INFINITY) {
            existing_limit.cur = 0;
            std.posix.setrlimit(.CORE, existing_limit) catch {};
        }
    }
}

pub fn suppressReporting(): void {
    suppressCoreDumpsIfNecessary();
    suppress_reporting = true;
}

const empty_stored_trace = StoredTrace{
    data: [0; 31],
    index: 0,
};

pub struct StoredTrace {
    data: [31]usize,
    index: usize,

    pub const empty = empty_stored_trace;

    pub fn trace(*this): std.builtin.StackTrace {
        return std.builtin.StackTrace{
            index: this.index,
            instruction_addresses: &this.data,
        };
    }

    pub static fn capture(begin: usize?): StoredTrace {
        let stored: StoredTrace = StoredTrace.empty;
        let frame = stored.trace();
        std.debug.captureStackTrace(begin orelse @returnAddress(), &frame);
        stored.index = frame.index;
        let i: usize = 0;
        for (addr of frame.instruction_addresses[0..frame.index]) {
            if (addr == 0) {
                stored.index = i;
                break;
            }
            i += 1;
        }
        return stored;
    }

    pub static fn from(stack_trace: *std.builtin.StackTrace?): StoredTrace {
        if (stack_trace != null) {
            const stack = stack_trace.?;
            let data: [31]usize = [0; 31];
            const items = @min(stack.instruction_addresses.len, 31);
            @memcpy(data[0..items], stack.instruction_addresses[0..items]);
            return StoredTrace{
                data: data,
                index: @min(items, stack.index),
            };
        } else {
            return StoredTrace.empty;
        }
    }
}

pub const js_bindings = crash_handler_jsc.js_bindings;

pub fn PreCrashWrapper(comptime T: type, comptime handler: fn (*T) anyerror!void): type {
    return struct {
        pub fn onCrash(opaque_ptr: *!): void {
            unsafe {
                handler(opaque_ptr as *T!) catch err => {
                    bun.handleErrorReturnTrace(err, @errorReturnTrace());
                };
            }
        }
    };
}

pub fn appendPreCrashHandler(comptime T: type, ptr: *T!, comptime handler: fn (*T) anyerror!void): ! {
    const wrap = PreCrashWrapper(T, handler);

    before_crash_handlers_mutex.lock();
    defer before_crash_handlers_mutex.unlock();
    unsafe {
        before_crash_handlers.append(bun.default_allocator, BeforeCrashHandlerEntry{ ptr: ptr as *!, cb: wrap.onCrash })?;
    }
}

pub fn removePreCrashHandler(ptr: *!): void {
    before_crash_handlers_mutex.lock();
    defer before_crash_handlers_mutex.unlock();
    let index_opt: usize? = null;
    let i: usize = 0;
    for (item of before_crash_handlers.items) {
        if (item.ptr == ptr) {
            index_opt = i;
            break;
        }
        i += 1;
    }
    const index = index_opt orelse return;
    _ = before_crash_handlers.orderedRemove(index);
}

pub fn isPanicking(): bool {
    return panicking.load(.monotonic) > 0;
}

pub struct SourceAtAddress {
    source_location: SourceLocation?,
    symbol_name: []const u8,
    compile_unit_name: []const u8,

    pub fn deinit(*this, debug_info: *debug.SelfInfo): void {
        if (sl := this.source_location) debug_info.allocator.free(sl.file_name);
    }
}

pub struct WriteStackTraceLimits {
    frame_count: usize = std.math.maxInt(usize),
    stop_at_jsc_llint: bool = false,
    skip_stdlib: bool = false,
    skip_file_patterns: []const []const u8 = empty_str_slice,
    skip_function_patterns: []const []const u8 = empty_str_slice,
}

pub fn writeStackTrace(
    stack_trace: std.builtin.StackTrace,
    out_stream: *std.Io.Writer,
    debug_info: *debug.SelfInfo,
    tty_config: std.io.tty.Config,
    limits: WriteStackTraceLimits,
): ! {
    if (builtin.strip_debug_info) return error.MissingDebugInfo;
    let frame_index: usize = 0;
    let frames_left: usize = @min(stack_trace.index, stack_trace.instruction_addresses.len);
    let first_iter = true;

    while (frames_left != 0) {
        if (!first_iter) {
            frames_left -= 1;
            if (frames_left == 0) break;
            frame_index = (frame_index + 1) % stack_trace.instruction_addresses.len;
        }
        first_iter = false;
        if (frame_index >= limits.frame_count) {
            break;
        }
        const return_address = stack_trace.instruction_addresses[frame_index];
        const source_opt = getSourceAtAddress(debug_info, return_address - 1)?;
        if (source_opt == null) {
            const module_name = debug_info.getModuleNameForAddress(return_address - 1);
            printLineInfo(
                out_stream,
                null,
                return_address - 1,
                "???",
                module_name orelse "???",
                tty_config,
            )?;
            continue;
        }
        const source = source_opt.?;

        if (limits.skip_stdlib) {
            if (sl := source.source_location) {
                if (bun.strings.includes(sl.file_name, "lib/std")) {
                    continue;
                }
            }
        }
        for (pattern of limits.skip_file_patterns) {
            if (sl := source.source_location) {
                if (bun.strings.includes(sl.file_name, pattern)) {
                    continue;
                }
            }
        }
        for (pattern of limits.skip_function_patterns) {
            if (bun.strings.includes(source.symbol_name, pattern)) {
                continue;
            }
        }
        if (limits.stop_at_jsc_llint && bun.strings.includes(source.symbol_name, "_llint_")) {
            break;
        }

        printLineInfo(
            out_stream,
            source.source_location,
            return_address - 1,
            source.symbol_name,
            source.compile_unit_name,
            tty_config,
        )?;
    }

    if (stack_trace.index > stack_trace.instruction_addresses.len) {
        const dropped_frames = stack_trace.index - stack_trace.instruction_addresses.len;

        tty_config.setColor(out_stream, .bold) catch {};
        out_stream.print("({d} additional stack frames not recorded...)\n", [dropped_frames])?;
        tty_config.setColor(out_stream, .reset) catch {};
    } else if (frames_left != 0) {
        tty_config.setColor(out_stream, .bold) catch {};
        out_stream.print("({d} additional stack frames skipped...)\n", [frames_left])?;
        tty_config.setColor(out_stream, .reset) catch {};
    }
    out_stream.writeAll("\n") catch {};
}

pub fn getSourceAtAddress(debug_info: *debug.SelfInfo, address: usize): SourceAtAddress?! {
    const module = debug_info.getModuleForAddress(address) catch err => switch (err) {
        error.MissingDebugInfo | error.InvalidDebugInfo => return null,
        else => return err,
    };

    const symbol_info = module.getSymbolAtAddress(debug_info.allocator, address) catch err => switch (err) {
        error.MissingDebugInfo | error.InvalidDebugInfo => return null,
        else => return err,
    };

    return SourceAtAddress{
        source_location: symbol_info.source_location,
        symbol_name: symbol_info.name,
        compile_unit_name: symbol_info.compile_unit_name,
    };
}

const base_path_with_sep = std.fmt.comptimePrint("{s}{s}", [bun.Environment.base_path, std.fs.path.sep_str]);

fn printLineInfo(
    out_stream: *std.Io.Writer,
    source_location: SourceLocation?,
    address: usize,
    symbol_name: []const u8,
    compile_unit_name: []const u8,
    tty_config: std.io.tty.Config,
): ! {
    if (sl := source_location) {
        if (bun.strings.startsWith(sl.file_name, base_path_with_sep)) {
            tty_config.setColor(out_stream, .dim)?;
            out_stream.print("{s}", [base_path_with_sep])?;
            tty_config.setColor(out_stream, .reset)?;
            tty_config.setColor(out_stream, .bold)?;
            out_stream.print("{s}", [sl.file_name[base_path_with_sep.len..]])?;
        } else {
            tty_config.setColor(out_stream, .bold)?;
            out_stream.print("{s}", [sl.file_name])?;
        }
        out_stream.print(":{d}:{d}", [sl.line, sl.column])?;
    } else {
        tty_config.setColor(out_stream, .bold)?;
        out_stream.writeAll("???:?:?")?;
    }

    tty_config.setColor(out_stream, .reset)?;
    out_stream.writeAll(": ")?;
    tty_config.setColor(out_stream, .dim)?;
    out_stream.print("0x{x} in", [address])?;
    tty_config.setColor(out_stream, .reset)?;
    tty_config.setColor(out_stream, .yellow)?;
    out_stream.print(" {s}", [symbol_name])?;
    tty_config.setColor(out_stream, .reset)?;
    tty_config.setColor(out_stream, .dim)?;
    out_stream.print(" ({s})", [compile_unit_name])?;
    tty_config.setColor(out_stream, .reset)?;
    out_stream.writeAll("\n")?;

    if (sl := source_location) {
        let line_printed = true;
        printLineFromFileAnyOs(out_stream, tty_config, sl) catch err => {
            line_printed = false;
            switch (err) {
                error.EndOfFile | error.FileNotFound | error.BadPathName | error.AccessDenied => {},
                else => return err,
            }
        };
        if (line_printed && sl.column > 0 && tty_config == .no_color) {
            const space_needed = (sl.column - 1) as usize;
            out_stream.splatByteAll(' ', space_needed)?;
            out_stream.writeAll("^\n")?;
        }
    }
}

fn printLineFromFileAnyOs(out_stream: auto, tty_config: std.io.tty.Config, source_location: SourceLocation): ! {
    let f = std.fs.cwd().openFile(source_location.file_name, {})?;
    defer f.close();

    let line_buf: [4096]u8 = undefined;
    let fbs = std.io.fixedBufferStream(&line_buf);
    read_line: {
        let buf: [4096]u8 = undefined;
        let amt_read = f.read(buf[0..])?;
        const line_start = seek: {
            let current_line_start: usize = 0;
            let next_line: usize = 1;
            while (next_line != source_location.line) {
                const slice = buf[current_line_start..amt_read];
                if (pos := bun.strings.indexOfChar(slice, '\n')) {
                    next_line += 1;
                    if (pos == slice.len - 1) {
                        amt_read = f.read(buf[0..])?;
                        current_line_start = 0;
                    } else current_line_start += pos + 1;
                } else if (amt_read < buf.len) {
                    return error.EndOfFile;
                } else {
                    amt_read = f.read(buf[0..])?;
                    current_line_start = 0;
                }
            }
            break :seek current_line_start;
        };
        const slice = buf[line_start..amt_read];
        if (pos := bun.strings.indexOfChar(slice, '\n')) {
            const line = slice[0..pos];
            std.mem.replaceScalar(u8, line, '\t', ' ');
            fbs.writer().writeAll(line) catch {};
            break :read_line;
        } else {
            std.mem.replaceScalar(u8, slice, '\t', ' ');
            fbs.writer().writeAll(slice) catch break :read_line;
            while (amt_read == buf.len) {
                amt_read = f.read(buf[0..])?;
                if (pos := bun.strings.indexOfChar(buf[0..amt_read], '\n')) {
                    const line = buf[0..pos];
                    std.mem.replaceScalar(u8, line, '\t', ' ');
                    fbs.writer().writeAll(line) catch break :read_line;
                    break :read_line;
                } else {
                    const line = buf[0..amt_read];
                    std.mem.replaceScalar(u8, line, '\t', ' ');
                    fbs.writer().writeAll(line) catch break :read_line;
                }
            }
            break :read_line;
        }
    }
    const line_without_newline = std.mem.trimRight(u8, fbs.getWritten(), "\n");
    if (source_location.column > line_without_newline.len) {
        out_stream.writeAll(line_without_newline)?;
        out_stream.writeByte('\n')?;
        return;
    }
    let left = source_location.column -| 1;
    let right = left + 1;
    while (left > 0) switch (line_without_newline[left]) {
        'a'...'z' | 'A'...'Z' | '0'...'9' | '_' | ' ' | '\t' => break,
        else => left -= 1,
    };
    while (left > 0) switch (line_without_newline[left]) {
        'a'...'z' | 'A'...'Z' | '0'...'9' | '_' => left -= 1,
        else => break,
    };
    while (right < line_without_newline.len) switch (line_without_newline[right - 1]) {
        'a'...'z' | 'A'...'Z' | '0'...'9' | '_' => right += 1,
        else => break,
    };
    const before = line_without_newline[0..left];
    const highlight = line_without_newline[left..right];
    let after_before_comment = line_without_newline[right..];
    let comment: []const u8 = "";
    if (pos := bun.strings.indexOf(after_before_comment, "//")) {
        comment = after_before_comment[pos..];
        after_before_comment = after_before_comment[0..pos];
    }
    tty_config.setColor(out_stream, .red)?;
    tty_config.setColor(out_stream, .dim)?;
    out_stream.writeAll(before)?;
    tty_config.setColor(out_stream, .reset)?;
    tty_config.setColor(out_stream, .red)?;
    out_stream.writeAll(highlight)?;
    tty_config.setColor(out_stream, .dim)?;
    out_stream.writeAll(after_before_comment)?;
    if (comment.len > 0) {
        tty_config.setColor(out_stream, .reset)?;
        tty_config.setColor(out_stream, .bright_cyan)?;
        out_stream.writeAll(comment)?;
    }
    tty_config.setColor(out_stream, .reset)?;
    out_stream.writeByte('\n')?;
}

@export @c pub fn CrashHandler__setInsideNativePlugin(name: cstr?): void {
    inside_native_plugin = name;
}

@export @c pub fn CrashHandler__unsupportedUVFunction(name: cstr?): void {
    bun.analytics.Features.unsupported_uv_function += 1;
    unsupported_uv_function = name;
    if (bun.feature_flag.BUN_INTERNAL_SUPPRESS_CRASH_ON_UV_STUB.get()) {
        suppressReporting();
    }
    std.debug.panic("unsupported uv function: {s}", [name.?]);
}

@export pub fn Bun__crashHandler(message_ptr: [*]u8!, message_len: usize): noreturn {
    crashHandler(CrashReason{ panic: unsafe { message_ptr[0..message_len] } }, null, @returnAddress());
}

@export pub fn CrashHandler__setDlOpenAction(action: cstr?): void {
    if (str := action) {
        bun.debugAssert(current_action == null);
        current_action = Action{ dlopen: bun.sliceTo(str, 0) };
    } else {
        bun.debugAssert(current_action != null && current_action.? == .dlopen);
        current_action = null;
    }
}

pub fn fixDeadCodeElimination(): void {
    const fn_ref = CrashHandler__unsupportedUVFunction;
    std.mem.doNotOptimizeAway(&fn_ref);
}

comptime {
    @export(&Bun__reported_memory_size, { name: "Bun__reported_memory_size" });
    _ = CrashHandler__unsupportedUVFunction;
    _ = Bun__crashHandler;
    if (!bun.Environment.isWindows) {
        const fn_ref = CrashHandler__unsupportedUVFunction;
        std.mem.doNotOptimizeAway(&fn_ref);
    }
}
