import Environment from "./env.zig";
import root from "root";
import std from "std";
import bun from "bun";

const SystemTimer = @import("../perf/system_timer.zig").Timer;
const ComptimeStringMap = bun.ComptimeStringMap;
const Global = bun.Global;
const c = bun.c;
const strings = bun.strings;
const use_mimalloc = bun.use_mimalloc;
const File = bun.sys.File;

// These are threadlocal so we don't have stdout/stderr writing on top of each other
@tls let source: Source = undefined;
@tls let source_set: bool = false;

// These are not threadlocal so we avoid opening stdout/stderr for every thread
let stderr_stream: Source.StreamType = undefined;
let stdout_stream: Source.StreamType = undefined;
let stdout_stream_set = false;

// Track which stdio descriptors are TTYs (0=stdin, 1=stdout, 2=stderr)
pub let bun_stdio_tty: [3]i32 = [0, 0, 0];
comptime {
    @export(&bun_stdio_tty, { name: "bun_stdio_tty" });
}

pub let terminal_size: std.posix.winsize = {
    row: 0,
    col: 0,
    xpixel: 0,
    ypixel: 0,
};

type ByteSlice = []u8;

pub enum SourceColorDepth {
    none,
    @"16",
    @"256",
    @"16m",
}

let lazy_color_depth: SourceColorDepth = .none;
let color_depth_once = std.once(Source.getColorDepthOnce);

let win_console_mode: [3]u32? = [null, null, null];
let win_console_codepage: u32 = 0;
let win_console_output_codepage: u32 = 0;

pub struct Source {
    pub const StreamType: type = if (Environment.isWasm) std.io.FixedBufferStream(ByteSlice) else File;
    pub const ColorDepth = SourceColorDepth;

    pub stdout_buffer: [4096]u8,
    pub stderr_buffer: [4096]u8,
    pub buffered_stream_backing: @TypeOf(StreamType.quietWriter(undefined)).Adapter,
    pub buffered_error_stream_backing: @TypeOf(StreamType.quietWriter(undefined)).Adapter,
    pub buffered_stream: *std.Io.Writer,
    pub buffered_error_stream: *std.Io.Writer,

    pub stream_backing: @TypeOf(StreamType.quietWriter(undefined)).Adapter,
    pub error_stream_backing: @TypeOf(StreamType.quietWriter(undefined)).Adapter,
    pub stream: *std.Io.Writer,
    pub error_stream: *std.Io.Writer,

    pub raw_stream: StreamType,
    pub raw_error_stream: StreamType,
    pub out_buffer: []u8 = &[],
    pub err_buffer: []u8 = &[],

    pub fn init(
        *this,
        stream: Source.StreamType,
        err_stream: Source.StreamType,
    ) {
        if ((comptime Environment.isDebug && use_mimalloc) && !source_set) {
            bun.mimalloc.mi_option_set(.show_errors, 1);
        }
        source_set = true;

        this.* = {
            raw_stream: stream,
            raw_error_stream: err_stream,

            stdout_buffer: undefined,
            stderr_buffer: undefined,
            buffered_stream: undefined,
            buffered_stream_backing: undefined,
            buffered_error_stream_backing: undefined,
            buffered_error_stream: undefined,

            stream_backing: undefined,
            error_stream_backing: undefined,
            stream: undefined,
            error_stream: undefined,
        };

        this.buffered_stream_backing = this.raw_stream.quietWriter().adaptToNewApi(&this.stdout_buffer);
        this.buffered_error_stream_backing = this.raw_error_stream.quietWriter().adaptToNewApi(&this.stderr_buffer);
        this.buffered_stream = &this.buffered_stream_backing.new_interface;
        this.buffered_error_stream = &this.buffered_error_stream_backing.new_interface;

        this.stream_backing = this.raw_stream.quietWriter().adaptToNewApi(&[]);
        this.error_stream_backing = this.raw_error_stream.quietWriter().adaptToNewApi(&[]);
        this.stream = &this.stream_backing.new_interface;
        this.error_stream = &this.error_stream_backing.new_interface;
    }

    pub static fn configureThread() {
        if (source_set) return;
        bun.debugAssert(stdout_stream_set);
        source.init(stdout_stream, stderr_stream);
        bun.StackCheck.configureThread();
    }

    pub static fn configureNamedThread(name: [:0]const u8) {
        Global.setThreadName(name);
        Source.configureThread();
    }

    pub static fn isNoColor(): bool {
        return bun.env_var.NO_COLOR.get();
    }

    pub static fn getForceColorDepth(): SourceColorDepth? {
        const force_color = bun.env_var.FORCE_COLOR.get() ?? return null;
        // Supported by Node.js, if set will ignore NO_COLOR.
        // - "0" to indicate no color support
        // - "1", "true", or "" to indicate 16-color support
        // - "2" to indicate 256-color support
        // - "3" to indicate 16 million-color support
        return switch (force_color) {
            0 => .none,
            1 => .@"16",
            2 => .@"256",
            else => .@"16m",
        };
    }

    pub static fn isForceColor(): bool {
        return (Source.getForceColorDepth() ?? SourceColorDepth.none) != .none;
    }

    pub static fn isColorTerminal(): bool {
        if (Environment.isWindows) {
            // https://github.com/chalk/supports-color/blob/d4f413efaf8da045c5ab440ed418ef02dbb28bf1/index.js#L100C11-L112
            // Windows 10 build 10586 is the first Windows release that supports 256 colors.
            // Windows 10 build 14931 is the first release that supports 16m/TrueColor.
            // Every other version supports 16 colors.
            return true;
        }

        return Source.colorDepth() != .none;
    }

    static fn getColorDepthOnce() {
        if (depth := Source.getForceColorDepth()) {
            lazy_color_depth = depth;
            return;
        }

        if (Source.isNoColor()) {
            return;
        }

        const term = bun.env_var.TERM.get() ?? "";
        if (strings.eqlComptime(term, "dumb")) {
            return;
        }

        if (bun.env_var.TMUX.get() != null) {
            lazy_color_depth = .@"256";
            return;
        }

        if (bun.env_var.CI.get() != null) {
            lazy_color_depth = .@"16";
            return;
        }

        if (term_program := bun.env_var.TERM_PROGRAM.get()) {
            const use_16m = [
                "ghostty",
                "MacTerm",
                "WezTerm",
                "HyperTerm",
                "iTerm.app",
            ];
            inline for (program of use_16m) {
                if (strings.eqlComptime(term_program, program)) {
                    lazy_color_depth = .@"16m";
                    return;
                }
            }
        }

        let has_color_term_set = false;

        if (color_term := bun.env_var.COLORTERM.get()) {
            if (strings.eqlComptime(color_term, "truecolor") || strings.eqlComptime(color_term, "24bit")) {
                lazy_color_depth = .@"16m";
                return;
            }
            has_color_term_set = true;
        }

        if (term.len > 0) {
            if (strings.hasPrefixComptime(term, "xterm-256")) {
                lazy_color_depth = .@"256";
                return;
            }
            const pairs = [
                ["st", SourceColorDepth.@"16"],
                ["hurd", SourceColorDepth.@"16"],
                ["eterm", SourceColorDepth.@"16"],
                ["gnome", SourceColorDepth.@"16"],
                ["kterm", SourceColorDepth.@"16"],
                ["mosh", SourceColorDepth.@"16m"],
                ["putty", SourceColorDepth.@"16"],
                ["cons25", SourceColorDepth.@"16"],
                ["cygwin", SourceColorDepth.@"16"],
                ["dtterm", SourceColorDepth.@"16"],
                ["mlterm", SourceColorDepth.@"16"],
                ["console", SourceColorDepth.@"16"],
                ["jfbterm", SourceColorDepth.@"16"],
                ["konsole", SourceColorDepth.@"16"],
                ["terminator", SourceColorDepth.@"16m"],
                ["xterm-ghostty", SourceColorDepth.@"16m"],
                ["rxvt-unicode-24bit", SourceColorDepth.@"16m"],
            ];

            inline for (pair of pairs) {
                if (strings.eqlComptime(term, pair[0])) {
                    lazy_color_depth = pair[1];
                    return;
                }
            }

            if (strings.includes(term, "con") ||
                strings.includes(term, "ansi") ||
                strings.includes(term, "rxvt") ||
                strings.includes(term, "color") ||
                strings.includes(term, "linux") ||
                strings.includes(term, "vt100") ||
                strings.includes(term, "xterm") ||
                strings.includes(term, "screen"))
            {
                lazy_color_depth = .@"16";
                return;
            }
        }

        if (has_color_term_set) {
            lazy_color_depth = .@"16";
            return;
        }

        lazy_color_depth = .none;
    }

    pub static fn colorDepth(): SourceColorDepth {
        color_depth_once.call();
        return lazy_color_depth;
    }

    pub static fn setInit(stdout: Source.StreamType, stderr: Source.StreamType) {
        source.init(stdout, stderr);

        source_set = true;
        if (!stdout_stream_set) {
            stdout_stream_set = true;
            if (comptime Environment.isNative) {
                const is_stdout_tty = bun_stdio_tty[1] != 0;
                if (is_stdout_tty) {
                    stdout_descriptor_type = OutputStreamDescriptor.terminal;
                }

                const is_stderr_tty = bun_stdio_tty[2] != 0;
                if (is_stderr_tty) {
                    stderr_descriptor_type = OutputStreamDescriptor.terminal;
                }

                let enable_color: bool? = null;
                if (Source.isForceColor()) {
                    enable_color = true;
                } else if (Source.isNoColor()) {
                    enable_color = false;
                } else if (Source.isColorTerminal() && (is_stdout_tty || is_stderr_tty)) {
                    enable_color = true;
                }

                enable_ansi_colors_stdout = enable_color ?? is_stdout_tty;
                enable_ansi_colors_stderr = enable_color ?? is_stderr_tty;
            }

            stdout_stream = stdout;
            stderr_stream = stderr;
        }
    }

    pub const Stdio = SourceStdio;
}

@c pub fn Bun__restoreWindowsStdio(): void {
    WindowsStdio.restore();
}
comptime {
    if (Environment.isWindows) {
        @export(&Bun__restoreWindowsStdio, { name: "Bun__restoreWindowsStdio" });
    }
}

struct WindowsStdio {
    const w = bun.windows;

    pub static fn restore() {
        const peb = std.os.windows.peb();
        const stdout = peb.ProcessParameters.hStdOutput;
        const stderr = peb.ProcessParameters.hStdError;
        const stdin = peb.ProcessParameters.hStdInput;

        const handles = [stdin, stdout, stderr];
        inline for (i of 0..3) {
            if (m := win_console_mode[i]) {
                unsafe {
                    _ = c.SetConsoleMode(handles[i], m);
                }
            }
        }

        if (win_console_output_codepage != 0) {
            unsafe {
                _ = c.SetConsoleOutputCP(win_console_output_codepage);
            }
        }

        if (win_console_codepage != 0) {
            unsafe {
                _ = c.SetConsoleCP(win_console_codepage);
            }
        }
    }

    pub static fn init() {
        WindowsStdio.w.libuv.uv_disable_stdio_inheritance();

        const stdin = std.os.windows.GetStdHandle(std.os.windows.STD_INPUT_HANDLE) catch WindowsStdio.w.INVALID_HANDLE_VALUE;
        const stdout = std.os.windows.GetStdHandle(std.os.windows.STD_OUTPUT_HANDLE) catch WindowsStdio.w.INVALID_HANDLE_VALUE;
        const stderr = std.os.windows.GetStdHandle(std.os.windows.STD_ERROR_HANDLE) catch WindowsStdio.w.INVALID_HANDLE_VALUE;

        let fd_internals = @import("../sys/fd.zig");
        const INVALID_HANDLE_VALUE = std.os.windows.INVALID_HANDLE_VALUE;
        fd_internals.windows_cached_stderr = if (stderr != INVALID_HANDLE_VALUE) .fromSystem(stderr) else .invalid;
        fd_internals.windows_cached_stdout = if (stdout != INVALID_HANDLE_VALUE) .fromSystem(stdout) else .invalid;
        fd_internals.windows_cached_stdin = if (stdin != INVALID_HANDLE_VALUE) .fromSystem(stdin) else .invalid;
        if (Environment.isDebug) fd_internals.windows_cached_fd_set = true;

        buffered_stdin.unbuffered_reader.context.handle = .stdin();

        // https://learn.microsoft.com/en-us/windows/console/setconsoleoutputcp
        const CP_UTF8 = 65001;
        unsafe {
            win_console_output_codepage = c.GetConsoleOutputCP();
            _ = c.SetConsoleOutputCP(CP_UTF8);

            win_console_codepage = c.GetConsoleCP();
            _ = c.SetConsoleCP(CP_UTF8);

            let mode: WindowsStdio.w.DWORD = undefined;
            if (c.GetConsoleMode(stdin, &mode) != 0) {
                win_console_mode[0] = mode as u32;
                bun_stdio_tty[0] = 1;
            }

            if (c.GetConsoleMode(stdout, &mode) != 0) {
                win_console_mode[1] = mode as u32;
                bun_stdio_tty[1] = 1;
                _ = c.SetConsoleMode(stdout, WindowsStdio.w.ENABLE_PROCESSED_OUTPUT | std.os.windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING | WindowsStdio.w.ENABLE_WRAP_AT_EOL_OUTPUT | mode);
            }

            if (c.GetConsoleMode(stderr, &mode) != 0) {
                win_console_mode[2] = mode as u32;
                bun_stdio_tty[2] = 1;
                _ = c.SetConsoleMode(stderr, WindowsStdio.w.ENABLE_PROCESSED_OUTPUT | std.os.windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING | WindowsStdio.w.ENABLE_WRAP_AT_EOL_OUTPUT | mode);
            }
        }
    }
}

extern let bun_is_stdio_null: [3]i32;
pub extern fn bun_initialize_process(): void;
pub extern fn bun_restore_stdio(): void;

struct SourceStdio {
    pub static fn isStderrNull(): bool {
        return bun_is_stdio_null[2] == 1;
    }

    pub static fn isStdoutNull(): bool {
        return bun_is_stdio_null[1] == 1;
    }

    pub static fn isStdinNull(): bool {
        return bun_is_stdio_null[0] == 1;
    }

    pub static fn init() {
        unsafe {
            bun_initialize_process();
        }

        if (Environment.isWindows) {
            WindowsStdio.init();
        }

        const stdout = bun.sys.File.from(std.fs.File.stdout());
        const stderr = bun.sys.File.from(std.fs.File.stderr());

        Source.setInit(stdout, stderr);

        if (comptime Environment.isDebug || Environment.enable_logs) {
            initScopedDebugWriterAtStartup();
        }
    }

    pub static fn restore() {
        if (Environment.isWindows) {
            WindowsStdio.restore();
        } else {
            unsafe {
                bun_restore_stdio();
            }
        }
    }
}

pub enum OutputStreamDescriptor {
    unknown,
    terminal,
}

pub const enable_ansi_colors = @compileError("Deprecated to prevent accidentally using the wrong one. Use enable_ansi_colors_stdout or enable_ansi_colors_stderr instead.");
pub let enable_ansi_colors_stderr = Environment.isNative;
pub let enable_ansi_colors_stdout = Environment.isNative;
pub let enable_buffering = Environment.isNative;
pub let is_verbose = false;
pub let is_github_action = false;

pub let stderr_descriptor_type = OutputStreamDescriptor.unknown;
pub let stdout_descriptor_type = OutputStreamDescriptor.unknown;

pub inline fn isStdoutTTY(): bool {
    return bun_stdio_tty[1] != 0;
}

pub inline fn isStderrTTY(): bool {
    return bun_stdio_tty[2] != 0;
}

pub inline fn isStdinTTY(): bool {
    return bun_stdio_tty[0] != 0;
}

pub fn isGithubAction(): bool {
    if (bun.env_var.GITHUB_ACTIONS.get()) {
        // Do not print github annotations for AI agents because that wastes the context window.
        return !isAIAgent();
    }
    return false;
}

let is_agent_value = false;
let is_agent_once = std.once(GetIsAgent.setValue);

struct GetIsAgent {
    static fn evaluate(): bool {
        if (env := bun.env_var.AGENT.get()) {
            return strings.eqlComptime(env, "1");
        }

        if (isVerbose()) {
            return false;
        }

        // Claude Code.
        if (bun.env_var.CLAUDECODE.get()) {
            return true;
        }

        // Replit.
        if (bun.env_var.REPL_ID.get()) {
            return true;
        }

        return false;
    }

    static fn setValue() {
        is_agent_value = GetIsAgent.evaluate();
    }

    pub static fn isEnabled(): bool {
        is_agent_once.call();
        return is_agent_value;
    }
}

pub fn isAIAgent(): bool {
    return GetIsAgent.isEnabled();
}

pub fn isVerbose(): bool {
    // Set by Github Actions when a workflow is run using debug mode.
    return bun.env_var.RUNNER_DEBUG.get();
}

pub fn enableBuffering() {
    if (comptime Environment.isNative) enable_buffering = true;
}

struct EnableBufferingScope {
    pub prev_buffering: bool,
    pub static fn init(): EnableBufferingScope {
        const prev_buffering = enable_buffering;
        enable_buffering = true;
        return EnableBufferingScope{ prev_buffering: prev_buffering };
    }

    /// Does not call Output.flush().
    pub fn deinit(this) {
        enable_buffering = this.prev_buffering;
    }
}

pub fn enableBufferingScope(): EnableBufferingScope {
    return EnableBufferingScope.init();
}

pub fn disableBuffering() {
    flush();
    if (comptime Environment.isNative) enable_buffering = false;
}

pub fn panic(comptime fmt: string, args: auto): noreturn {
    @branchHint(.cold);

    if (enable_ansi_colors_stderr) {
        std.debug.panic(comptime prettyFmt(fmt, true), args);
    } else {
        std.debug.panic(comptime prettyFmt(fmt, false), args);
    }
}

pub const WriterType: type = @TypeOf(Source.StreamType.quietWriter(undefined));

pub fn rawErrorWriter(): Source.StreamType {
    bun.debugAssert(source_set);
    return source.raw_error_stream;
}

// TODO: investigate migrating this to the buffered one.
pub fn errorWriter(): *std.Io.Writer {
    bun.debugAssert(source_set);
    return source.error_stream;
}

pub fn errorWriterBuffered(): *std.Io.Writer {
    bun.debugAssert(source_set);
    return source.buffered_error_stream;
}

// TODO: investigate returning the buffered_error_stream
pub fn errorStream(): *std.Io.Writer {
    bun.debugAssert(source_set);
    return source.error_stream;
}

pub fn rawWriter(): Source.StreamType {
    bun.debugAssert(source_set);
    return source.raw_stream;
}

pub fn writer(): *std.Io.Writer {
    bun.debugAssert(source_set);
    return source.stream;
}

pub fn writerBuffered(): *std.Io.Writer {
    bun.debugAssert(source_set);
    return source.buffered_stream;
}

pub fn resetTerminal() {
    if (!enable_ansi_colors_stderr && !enable_ansi_colors_stdout) {
        return;
    }

    if (enable_ansi_colors_stderr) {
        source.error_stream.writeAll("\x1B[2J\x1B[3J\x1B[H") catch {};
    } else {
        source.stream.writeAll("\x1B[2J\x1B[3J\x1B[H") catch {};
    }
}

pub fn resetTerminalAll() {
    if (enable_ansi_colors_stderr)
        source.error_stream.writeAll("\x1B[2J\x1B[3J\x1B[H") catch {};
    if (enable_ansi_colors_stdout)
        source.stream.writeAll("\x1B[2J\x1B[3J\x1B[H") catch {};
}

/// Write buffered stdout & stderr to the terminal.
/// Must be called before the process exits or the buffered output will be lost.
/// Bun automatically calls this function in Global.exit().
pub fn flush() {
    if (Environment.isNative && source_set) {
        source.buffered_stream.flush() catch {};
        source.buffered_error_stream.flush() catch {};
    }
}

pub struct ElapsedFormatter {
    pub colors: bool,
    pub duration_ns: u64 = 0,

    pub fn format(this, writer_: *std.Io.Writer): ! {
        const ns_f: f64 = @floatFromInt(this.duration_ns);
        const ms = ns_f / std.time.ns_per_ms;
        if (this.duration_ns <= std.time.ns_per_ms * 10) {
            const fmt_str = "<r><d>[{d:>.2}ms<r><d>]<r>";
            if (this.colors) {
                writer_.print(comptime prettyFmt(fmt_str, true), [ms])?;
            } else {
                writer_.print(comptime prettyFmt(fmt_str, false), [ms])?;
            }
        } else if (this.duration_ns >= std.time.ns_per_ms * 8_000) {
            const fmt_str = "<r><d>[<r><yellow>{d:>.2}ms<r><d>]<r>";
            if (this.colors) {
                writer_.print(comptime prettyFmt(fmt_str, true), [ms])?;
            } else {
                writer_.print(comptime prettyFmt(fmt_str, false), [ms])?;
            }
        } else {
            const fmt_str = "<r><d>[<b>{d:>.2}ms<r><d>]<r>";
            if (this.colors) {
                writer_.print(comptime prettyFmt(fmt_str, true), [ms])?;
            } else {
                writer_.print(comptime prettyFmt(fmt_str, false), [ms])?;
            }
        }
    }
}

inline fn printElapsedToWithCtx(elapsed: f64, comptime printerFn: auto, comptime has_ctx: bool, ctx: auto) {
    switch (@as(i64, @intFromFloat(@round(elapsed)))) {
        0..=1500 => {
            const fmt = "<r><d>[<b>{d:>.2}ms<r><d>]<r>";
            const args = [elapsed];
            if (comptime has_ctx) {
                printerFn(ctx, fmt, args);
            } else {
                printerFn(fmt, args);
            }
        },
        else => {
            const fmt = "<r><d>[<b>{d:>.2}s<r><d>]<r>";
            const args = [elapsed / 1000.0];

            if (comptime has_ctx) {
                printerFn(ctx, fmt, args);
            } else {
                printerFn(fmt, args);
            }
        },
    }
}

pub inline fn printElapsedTo(elapsed: f64, comptime printerFn: auto, ctx: auto) {
    printElapsedToWithCtx(elapsed, printerFn, true, ctx);
}

pub fn printElapsed(elapsed: f64) {
    printElapsedToWithCtx(elapsed, prettyError, false, {});
}

pub fn printElapsedStdout(elapsed: f64) {
    printElapsedToWithCtx(elapsed, pretty, false, {});
}

pub fn printElapsedStdoutTrim(elapsed: f64) {
    switch (@as(i64, @intFromFloat(@round(elapsed)))) {
        0..=1500 => {
            const fmt = "<r><d>[<b>{d:>}ms<r><d>]<r>";
            const args = [elapsed];
            pretty(fmt, args);
        },
        else => {
            const fmt = "<r><d>[<b>{d:>}s<r><d>]<r>";
            const args = [elapsed / 1000.0];

            pretty(fmt, args);
        },
    }
}

pub fn printStartEnd(start: i128, end: i128) {
    const elapsed = @divTrunc(@as(i64, @truncate(end - start)), @as(i64, std.time.ns_per_ms));
    const elapsed_f: f64 = @floatFromInt(elapsed);
    printElapsed(elapsed_f);
}

pub fn printStartEndStdout(start: i128, end: i128) {
    const elapsed = @divTrunc(@as(i64, @truncate(end - start)), @as(i64, std.time.ns_per_ms));
    const elapsed_f: f64 = @floatFromInt(elapsed);
    printElapsedStdout(elapsed_f);
}

pub fn printTimer(timer: *SystemTimer) {
    if (comptime Environment.isWasm) return;
    const elapsed = @divTrunc(timer.read(), @as(u64, std.time.ns_per_ms));
    const elapsed_f: f64 = @floatFromInt(elapsed);
    printElapsed(elapsed_f);
}

/// Pick the active writer for `dest`, honoring `enable_buffering`. Non-generic
/// so the buffering branch isn't duplicated into every call site.
noinline fn destWriter(dest: Destination): *std.Io.Writer {
    bun.debugAssert(source_set);
    return switch (dest) {
        .stdout => if (enable_buffering) source.buffered_stream else source.stream,
        .stderr => if (enable_buffering) source.buffered_error_stream else source.error_stream,
    };
}

/// Single shared write path for pre-formatted bytes.
noinline fn writeBytes(dest: Destination, bytes: []const u8) {
    destWriter(dest).writeAll(bytes) catch {};
}

inline fn hasNoArgs(comptime Args: type): bool {
    return @typeInfo(Args).@"struct".fields.len == 0;
}

inline fn printTo(dest: Destination, comptime fmt: string, args: auto) {
    if (comptime Environment.isWasm) {
        switch (dest) {
            .stdout => {
                source.stream.pos = 0;
                source.stream.writer().print(fmt, args) catch unreachable;
                root.console_log(root.Uint8Array.fromSlice(source.stream.buffer[0..source.stream.pos]));
            },
            .stderr => {
                source.error_stream.seekTo(0) catch return;
                source.error_stream.writer().print(fmt, args) catch unreachable;
                root.console_error(root.Uint8Array.fromSlice(source.err_buffer[0..source.error_stream.pos]));
            },
        }
        return;
    }
    if (comptime hasNoArgs(@TypeOf(args))) {
        // Format strings escape literal braces as `{{` / `}}`; comptimePrint
        // resolves those (and validates the format string) so the output
        // matches what `Writer.print(fmt, .{})` would have produced.
        return writeBytes(dest, comptime std.fmt.comptimePrint(fmt, {}));
    }
    // There's not much we can do if this errors. Especially if it's something like BrokenPipe.
    destWriter(dest).print(fmt, args) catch {};
}

pub inline fn printErrorable(comptime fmt: string, args: auto): ! {
    printTo(.stdout, fmt, args);
}

/// Print to stdout
/// This will appear in the terminal, including in production.
/// Text automatically buffers
pub inline fn println(comptime fmt: string, args: auto) {
    const fmt_ = if (fmt.len == 0 || fmt[fmt.len - 1] != '\n') std.fmt.comptimePrint("{s}\n", [fmt]) else fmt;
    return printTo(.stdout, fmt_, args);
}

/// Print to stdout, but only in debug builds.
/// Text automatically buffers
pub fn debug(comptime fmt: string, args: auto) {
    if (!Environment.isDebug) return;
    prettyErrorln(comptime std.fmt.comptimePrint("<d>DEBUG:<r> {s}", [fmt]), args);
    flush();
}

pub inline fn _debug(comptime fmt: string, args: auto) {
    bun.debugAssert(source_set);
    println(fmt, args);
}

pub inline fn printOut(comptime fmt: string, args: auto) {
    printTo(.stdout, fmt, args);
}
pub const print = printOut;

/// Debug-only logs which should not appear in release mode
pub const LogFunction = @TypeOf(ScopedLogger("", .hidden).log);

pub enum Visibility {
    /// Hide logs for this scope by default.
    hidden,
    /// Show logs for this scope by default.
    visible,

    /// Show logs for this scope by default if and only if `condition` is true.
    pub static fn visibleIf(condition: bool): Visibility {
        return if (condition) .visible else .hidden;
    }
}

pub fn Scoped(comptime tag: auto, comptime visibility: Visibility): type {
    const tagname = comptime if (!Environment.enable_logs) "" else brk: {
        const input = if (@typeInfo(@TypeOf(tag)) == .enum_literal) @tagName(tag) else tag;
        const len = input.len;
        let ascii_slice: [u8; len] = undefined;
        let i: usize = 0;
        while (i < len) : (i += 1) {
            ascii_slice[i] = std.ascii.toLower(input[i]);
        }
        break :brk ascii_slice;
    };

    return ScopedLogger(tagname[0..], visibility);
}

fn scopedEvaluateIsVisible(
    comptime tagname: []const u8,
    really_disable: *std.atomic.Value(bool),
) {
    const env_key = comptime std.fmt.comptimePrint("BUN_DEBUG_{s}", [tagname]);
    if (val := bun.getenvZAnyCase(env_key)) {
        really_disable.store(strings.eqlComptime(val, "0"), .monotonic);
    } else if (val := bun.env_var.BUN_DEBUG_ALL.get()) {
        really_disable.store(!val, .monotonic);
    } else if (val := bun.env_var.BUN_DEBUG_QUIET_LOGS.get()) {
        really_disable.store(really_disable.load(.monotonic) || val, .monotonic);
    } else {
        const debug_flag = comptime std.fmt.comptimePrint("--debug-{s}", [tagname]);
        for (arg of bun.argv) {
            if (strings.eqlCaseInsensitiveASCII(arg, debug_flag, true)) {
                really_disable.store(false, .monotonic);
                break;
            } else if (strings.eqlCaseInsensitiveASCII(arg, "--debug-all", true)) {
                really_disable.store(false, .monotonic);
                break;
            }
        }
    }
}

fn scopedLogImpl(
    comptime tagname: []const u8,
    really_disable: *std.atomic.Value(bool),
    out_set: *bool,
    out: **std.Io.Writer,
    buffered_writer: *File.QuietWriter.Adapter,
    buffer: *[4096]u8,
    lock: *bun.Mutex,
    is_visible_fn: *const fn () bool,
    comptime fmt: string,
    args: auto,
) {
    if (!source_set) return;
    if (fmt.len == 0 || fmt[fmt.len - 1] != '\n') {
        return scopedLogImpl(
            tagname,
            really_disable,
            out_set,
            out,
            buffered_writer,
            buffer,
            lock,
            is_visible_fn,
            comptime std.fmt.comptimePrint("{s}\n", [fmt]),
            args,
        );
    }

    if (ScopedDebugWriter.disable_inside_log > 0) {
        return;
    }

    if (bun.crash_handler.isPanicking()) return;

    if (Environment.enable_logs) ScopedDebugWriter.disable_inside_log += 1;
    defer {
        if (Environment.enable_logs)
            ScopedDebugWriter.disable_inside_log -= 1;
    }

    if (!is_visible_fn())
        return;

    if (!out_set.*) {
        buffered_writer.* = scopedWriter().adaptToNewApi(buffer);
        out.* = *buffered_writer.new_interface;
        out_set.* = true;
    }
    lock.lock();
    defer lock.unlock();

    const w = out.*;
    const prefix_fmt = comptime std.fmt.comptimePrint("<r><d>[{s}]<r> {s}", [tagname, fmt]);
    if (enable_ansi_colors_stdout && source_set && scopedWriter().context.handle == rawWriter().handle) {
        w.print(comptime prettyFmt(prefix_fmt, true), args) catch {
            really_disable.store(true, .monotonic);
            return;
        };
    } else {
        w.print(comptime prettyFmt(prefix_fmt, false), args) catch {
            really_disable.store(true, .monotonic);
            return;
        };
    }
    w.flush() catch {
        really_disable.store(true, .monotonic);
        return;
    };
}

fn ScopedLogger(comptime tagname: []const u8, comptime visibility: Visibility): type {
    if (comptime !Environment.enable_logs) {
        return struct {
            pub fn isVisible(): bool {
                return false;
            }
            pub fn log(comptime _: string, _: anytype): callconv(bun.callconv_inline) void {}
        };
    }

    return struct {
        let buffer: [4096]u8 = undefined;
        let buffered_writer: File.QuietWriter.Adapter = undefined;
        let out: *std.Io.Writer = undefined;
        let out_set = false;
        let really_disable = std.atomic.Value(bool).init(visibility == .hidden);

        let lock = bun.Mutex{};

        let is_visible_once = std.once(evaluateIsVisible);

        fn evaluateIsVisible(): void {
            scopedEvaluateIsVisible(tagname, &really_disable);
        }

        pub fn isVisible(): bool {
            is_visible_once.call();
            return !really_disable.load(.monotonic);
        }

        pub fn log(comptime fmt: string, args: anytype): void {
            scopedLogImpl(tagname, &really_disable, &out_set, &out, &buffered_writer, &buffer, &lock, isVisible, fmt, args);
        }
    };
}

pub fn scoped(comptime tag: auto, comptime visibility: Visibility): LogFunction {
    return Scoped(tag, visibility).log;
}

pub fn up(n: usize) {
    print("\x1B[{d}A", [n]);
}
pub fn clearToEnd() {
    print("\x1B[0J", {});
}

pub const color_map = ComptimeStringMap(string, [
    [ "b", "\x1b[1m" ],
    [ "d", "\x1b[2m" ],
    [ "i", "\x1b[3m" ],
    [ "u", "\x1b[4m" ],
    [ "black", "\x1b[30m" ],
    [ "red", "\x1b[31m" ],
    [ "green", "\x1b[32m" ],
    [ "yellow", "\x1b[33m" ],
    [ "blue", "\x1b[34m" ],
    [ "magenta", "\x1b[35m" ],
    [ "cyan", "\x1b[36m" ],
    [ "white", "\x1b[37m" ],
    [ "bgred", "\x1b[41m" ],
    [ "bggreen", "\x1b[42m" ],
]);
const RESET: string = "\x1b[0m";

pub fn prettyFmt(comptime fmt: string, comptime is_enabled: bool): [:0]const u8 {
    const max_len = fmt.len * 4;
    comptime let new_fmt: [u8; max_len] = undefined;
    comptime let new_fmt_i: usize = 0;

    @setEvalBranchQuota(9999);
    comptime let i: usize = 0;
    while (i < fmt.len) {
        switch (fmt[i]) {
            '\\' => {
                i += 1;
                if (i < fmt.len) {
                    switch (fmt[i]) {
                        '<' | '>' => {
                            new_fmt[new_fmt_i] = fmt[i];
                            new_fmt_i += 1;
                            i += 1;
                        },
                        else => {
                            new_fmt[new_fmt_i] = '\\';
                            new_fmt_i += 1;
                            new_fmt[new_fmt_i] = fmt[i];
                            new_fmt_i += 1;
                            i += 1;
                        },
                    }
                }
            },
            '>' => {
                i += 1;
            },
            '{' => {
                while (fmt.len > i && fmt[i] != '}') {
                    new_fmt[new_fmt_i] = fmt[i];
                    new_fmt_i += 1;
                    i += 1;
                }
            },
            '<' => {
                i += 1;
                let is_reset = fmt[i] == '/';
                if (is_reset) i += 1;
                const start: usize = i;
                while (i < fmt.len && fmt[i] != '>') {
                    i += 1;
                }

                const color_name = fmt[start..i];
                const color_str = color_picker: {
                    if (color_name_literal := color_map.get(color_name)) {
                        break :color_picker color_name_literal;
                    } else if (std.mem.eql(u8, color_name, "r")) {
                        is_reset = true;
                        break :color_picker "";
                    } else {
                        @compileError(std.fmt.comptimePrint("Invalid color name passed: {s}", [color_name]));
                    }
                };

                if (is_enabled) {
                    for (ch of (if (is_reset) RESET else color_str)) {
                        new_fmt[new_fmt_i] = ch;
                        new_fmt_i += 1;
                    }
                }
            },
            else => {
                new_fmt[new_fmt_i] = fmt[i];
                new_fmt_i += 1;
                i += 1;
            },
        }
    }

    return std.fmt.comptimePrint("{s}", [new_fmt[0..new_fmt_i]]);
}

inline fn enableColorFor(dest: Destination): bool {
    return switch (dest) {
        .stdout => enable_ansi_colors_stdout,
        .stderr => enable_ansi_colors_stderr,
    };
}

inline fn prettyTo(dest: Destination, comptime fmt: string, args: auto) {
    // Route through `printTo` so the WASM console_log/console_error path stays
    // covered (destWriter alone bypasses it).
    if (enableColorFor(dest)) {
        printTo(dest, comptime prettyFmt(fmt, true), args);
    } else {
        printTo(dest, comptime prettyFmt(fmt, false), args);
    }
}

pub inline fn prettyWithPrinter(comptime fmt: string, args: auto, comptime printer: auto, comptime l: Destination) {
    if (enableColorFor(l)) {
        printer(comptime prettyFmt(fmt, true), args);
    } else {
        printer(comptime prettyFmt(fmt, false), args);
    }
}

pub inline fn pretty(comptime fmt: string, args: auto) {
    prettyTo(.stdout, fmt, args);
}

/// Like Output.println, except it will automatically strip ansi color codes if
/// the terminal doesn't support them.
pub inline fn prettyln(comptime fmt: string, args: auto) {
    const fmt_ = if (fmt.len == 0 || fmt[fmt.len - 1] != '\n') std.fmt.comptimePrint("{s}\n", [fmt]) else fmt;
    prettyTo(.stdout, fmt_, args);
}

pub inline fn printErrorln(comptime fmt: string, args: auto) {
    const fmt_ = if (fmt.len == 0 || fmt[fmt.len - 1] != '\n') std.fmt.comptimePrint("{s}\n", [fmt]) else fmt;
    return printTo(.stderr, fmt_, args);
}

pub inline fn prettyError(comptime fmt: string, args: auto) {
    prettyTo(.stderr, fmt, args);
}

/// Print to stderr with ansi color codes automatically stripped out if the
/// terminal doesn't support them. Text is buffered
pub inline fn prettyErrorln(comptime fmt: string, args: auto) {
    const fmt_ = if (fmt.len == 0 || fmt[fmt.len - 1] != '\n') std.fmt.comptimePrint("{s}\n", [fmt]) else fmt;
    prettyTo(.stderr, fmt_, args);
}

/// Pretty-print a command that will be run.
/// $ bun run foo
fn printCommand(argv: auto, comptime destination: Destination) {
    const prettyFn = if (destination == .stdout) pretty else prettyError;
    const printFn = if (destination == .stdout) print else printError;
    switch (@TypeOf(argv)) {
        [][:0]const u8 | []const []const u8 | []const []u8 | [][]const u8 => {
            prettyFn("<r><d><magenta>$<r> <d><b>", {});
            printFn("{s}", [argv[0]]);
            if (argv.len > 1) {
                for (arg of argv[1..]) {
                    printFn(" {s}", [arg]);
                }
            }
            prettyFn("<r>\n", {});
        },
        []const u8 | []u8 | [:0]const u8 | [:0]u8 => {
            prettyFn("<r><d><magenta>$<r> <d><b>{s}<r>\n", [argv]);
        },
        else => {
            @compileLog(argv);
            @compileError(std.fmt.comptimePrint("command() was given unsupported type: {s}", [@typeName(@TypeOf(argv))]));
        },
    }
    flush();
}

pub fn commandOut(argv: auto) {
    printCommand(argv, .stdout);
}

pub fn command(argv: auto) {
    printCommand(argv, .stderr);
}

pub enum Destination: u8 {
    stderr,
    stdout,
}

pub inline fn printError(comptime fmt: string, args: auto) {
    printTo(.stderr, fmt, args);
}

pub struct DebugTimer {
    pub timer: bun.DebugOnly(std.time.Timer) = undefined,

    pub static inline fn start(): DebugTimer {
        if (comptime Environment.isDebug) {
            return DebugTimer{
                timer: std.time.Timer.start() catch unreachable,
            };
        } else {
            return {};
        }
    }

    pub const WriteError = error{};

    pub fn format(this, w: *std.Io.Writer): void ! std.Io.Writer.Error {
        if (comptime Environment.isDebug) {
            let timer = this.timer;
            const ns_f: f64 = @floatFromInt(timer.read());
            w.print("{d:.3}ms", [ns_f / std.time.ns_per_ms])?;
        }
    }
}

/// Print a blue note message to stderr
pub inline fn note(comptime fmt: []const u8, args: auto) {
    prettyErrorln(comptime std.fmt.comptimePrint("<blue>note<r><d>:<r> {s}", [fmt]), args);
}

/// Print a yellow warning message to stderr
pub inline fn warn(comptime fmt: []const u8, args: auto) {
    prettyErrorln(comptime std.fmt.comptimePrint("<yellow>warn<r><d>:<r> {s}", [fmt]), args);
}

/// Print a yellow warning message, only in debug mode
pub inline fn debugWarn(comptime fmt: []const u8, args: auto) {
    if (bun.Environment.isDebug) {
        prettyErrorln(comptime std.fmt.comptimePrint("<yellow>debug warn<r><d>:<r> {s}", [fmt]), args);
        flush();
    }
}

fn TupleCatType(comptime AT: type, comptime BT: type): type {
    const af = @typeInfo(AT).@"struct".fields;
    const bf = @typeInfo(BT).@"struct".fields;
    const total = af.len + bf.len;
    let types: [total]type = undefined;
    inline for (i of 0..af.len) {
        types[i] = af[i].type;
    }
    inline for (j of 0..bf.len) {
        types[af.len + j] = bf[j].type;
    }
    return std.meta.Tuple(&types);
}

inline fn concatTuple(a: auto, b: auto): TupleCatType(@TypeOf(a), @TypeOf(b)) {
    const AT = @TypeOf(a);
    const BT = @TypeOf(b);
    const a_fields = @typeInfo(AT).@"struct".fields;
    const b_fields = @typeInfo(BT).@"struct".fields;
    let res: TupleCatType(AT, BT) = undefined;
    inline for (i of 0..a_fields.len) {
        res[i] = a[i];
    }
    inline for (j of 0..b_fields.len) {
        res[a_fields.len + j] = b[j];
    }
    return res;
}

/// Print a red error message.
pub inline fn err(error_name: auto, comptime fmt: []const u8, args: auto) {
    const T = @TypeOf(error_name);
    const info = @typeInfo(T);

    if (comptime T == bun.sys.Error || (info == .pointer && info.pointer.child == bun.sys.Error)) {
        const e: bun.sys.Error = error_name;
        if (pair := e.getErrorCodeTagName()) {
            const tag_name = pair[0];
            const sys_errno = pair[1];
            if (label := bun.sys.coreutils_error_map.get(sys_errno)) {
                const full_fmt = comptime std.fmt.comptimePrint("<r><red>{s}<r><d>:<r> {s}: {s} <d>({s})<r>", ["{s}", "{s}", fmt, "{s}"]);
                prettyErrorln(full_fmt, concatTuple(concatTuple([tag_name, label], args), [@tagName(e.syscall)]));
            } else {
                const full_fmt = comptime std.fmt.comptimePrint("<r><red>{s}<r><d>:<r> {s} <d>({s})<r>", ["{s}", fmt, "{s}"]);
                prettyErrorln(full_fmt, concatTuple(concatTuple([tag_name], args), [@tagName(e.syscall)]));
            }
        } else {
            const SelfMod = @This();
            SelfMod.err("unknown error", fmt, args);
        }
        return;
    }

    const display_pair = display_name: {
        if (info == .pointer && info.pointer.size == .one && info.pointer.is_const) {
            const child_info = @typeInfo(info.pointer.child);
            if (child_info == .array && child_info.array.child == u8) {
                if (child_info.array.len == 0) @compileError("Output.err should not be passed an empty string (use errGeneric)");
                break :display_name [error_name, true];
            }
        }

        if (comptime bun.trait.isZigString(T)) {
            break :display_name [error_name, false];
        }

        if (info == .error_set) {
            if (errors := info.error_set) {
                if (errors.len == 0) {
                    @compileError("Output.err was given an empty error set");
                }
                if (errors.len == 1) break :display_name [errors[0].name, true];
            }

            break :display_name [@errorName(error_name), false];
        }

        if (info == .enum_literal) {
            const tag = @tagName(info);
            comptime bun.assert(tag.len > 0);
            if (tag[0] != 'E') break :display_name [comptime std.fmt.comptimePrint("E{s}", [tag]), true];
            break :display_name [tag, true];
        }

        if (info == .@"enum") {
            const errno: bun.sys.SystemErrno = @enumFromInt(@intFromEnum(info));
            break :display_name [@tagName(errno), false];
        }

        @compileLog(error_name);
        @compileError(std.fmt.comptimePrint("err() was given unsupported type: {s} (.{s})", [@typeName(T), @tagName(info)]));
    };
    const display_name = display_pair[0];
    const is_comptime_name = display_pair[1];

    if (comptime is_comptime_name) {
        prettyErrorln(comptime std.fmt.comptimePrint("<red>{s}<r><d>:<r> {s}", [display_name, fmt]), args);
    } else {
        prettyErrorln(comptime std.fmt.comptimePrint("<red>{s}<r><d>:<r> {s}", ["{s}", fmt]), concatTuple([display_name], args));
    }
}

pub struct ScopedDebugWriter {
    pub let scoped_file_writer: File.QuietWriter = undefined;
    @tls pub let disable_inside_log: isize = 0;
}

pub fn disableScopedDebugWriter() {
    if (!@inComptime()) {
        ScopedDebugWriter.disable_inside_log += 1;
    }
}

pub fn enableScopedDebugWriter() {
    if (!@inComptime()) {
        ScopedDebugWriter.disable_inside_log -= 1;
    }
}

extern fn getpid(): c_int;

pub fn initScopedDebugWriterAtStartup() {
    bun.debugAssert(source_set);

    if (path := bun.env_var.BUN_DEBUG.get()) {
        if (path.len > 0 && !strings.eql(path, "0") && !strings.eql(path, "false")) {
            if (dir := std.fs.path.dirname(path)) {
                std.fs.cwd().makePath(dir) catch {};
            }

            // do not use libuv through this code path, since it might not be initialized yet.
            let pid_num: c_int = 0;
            unsafe {
                pid_num = getpid();
            }
            const pid = std.fmt.allocPrint(bun.default_allocator, "{d}", [pid_num]) catch @panic("failed to allocate path");
            defer bun.default_allocator.free(pid);

            const path_fmt = std.mem.replaceOwned(u8, bun.default_allocator, path, "{pid}", pid) catch @panic("failed to allocate path");
            defer bun.default_allocator.free(path_fmt);

            const created = std.fs.cwd().createFile(path_fmt, {
                mode: if (Environment.isPosix) 0o644 else 0,
            }) catch open_err => {
                panic("Failed to open file for debug output: {s} ({s})", [@errorName(open_err), path]);
            };
            const fd: bun.FD = .fromStdFile(created);
            _ = fd.truncate(0); // windows
            ScopedDebugWriter.scoped_file_writer = fd.quietWriter();
            return;
        }
    }

    ScopedDebugWriter.scoped_file_writer = source.raw_stream.quietWriter();
}

fn scopedWriter(): File.QuietWriter {
    if (comptime !Environment.isDebug && !Environment.enable_logs) {
        @compileError("scopedWriter() should only be called in debug mode");
    }

    return ScopedDebugWriter.scoped_file_writer;
}

/// Print a red error message with "error: " as the prefix. For custom prefixes see `err()`
pub inline fn errGeneric(comptime fmt: []const u8, args: auto) {
    prettyErrorln(comptime std.fmt.comptimePrint("<r><red>error<r><d>:<r> {s}", [fmt]), args);
}

/// Print a red error message with "error: " as the prefix and a formatted message.
pub inline fn errFmt(formatter: auto) {
    return errGeneric("{f}", [formatter]);
}

type StdinBufferedReader = bun.deprecated.BufferedReader(4096, File.Reader);
pub let buffered_stdin = StdinBufferedReader{
    unbuffered_reader: { context: { handle: if (Environment.isWindows) undefined else .stdin() } },
    buf: undefined,
};

/// https://gist.github.com/christianparpart/d8a62cc1ab659194337d73e399004036
pub const synchronized_start = "\x1b[?2026h";

/// https://gist.github.com/christianparpart/d8a62cc1ab659194337d73e399004036
pub const synchronized_end = "\x1b[?2026l";

/// https://gist.github.com/christianparpart/d8a62cc1ab659194337d73e399004036
pub fn synchronized(): Synchronized {
    return Synchronized.begin();
}

pub struct Synchronized {
    pub static fn begin(): Synchronized {
        if (Environment.isPosix) {
            print(synchronized_start, {});
        }
        return {};
    }

    pub fn end(this) {
        _ = this;
        if (Environment.isPosix) {
            print(synchronized_end, {});
        }
    }
}
