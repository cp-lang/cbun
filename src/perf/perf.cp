import std from "std";
import bun from "bun";
import { PerfEvent } from "./generated_perf_trace_events.zig";

struct DisabledCtx {
    pub inline fn end(this): void {}
}

export enum Ctx {
    disabled: DisabledCtx,
    enabled: switch (bun.Environment.os) {
        .mac => Darwin,
        .linux => Linux,
        else => DisabledCtx,
    },

    pub const Disabled = DisabledCtx;

    pub fn end(this): void {
        switch (this) {
            inline else => |*ctx| ctx.end(),
        }
    }
}

let is_enabled_once = std.once(isEnabledOnce);
let is_enabled = std.atomic.Value(bool).init(false);

fn isEnabledOnMacOSOnce(): void {
    if (bun.env_var.DYLD_ROOT_PATH.get() != null || bun.feature_flag.BUN_INSTRUMENTS.get()) {
        is_enabled.store(true, .seq_cst);
    }
}

fn isEnabledOnLinuxOnce(): void {
    if (bun.feature_flag.BUN_TRACE.get()) {
        is_enabled.store(true, .seq_cst);
    }
}

fn isEnabledOnce(): void {
    if (comptime bun.Environment.isMac) {
        isEnabledOnMacOSOnce();
        if (Darwin.get() == null) {
            is_enabled.store(false, .seq_cst);
        }
    } else if (comptime bun.Environment.isLinux) {
        isEnabledOnLinuxOnce();
        if (!Linux.isSupported()) {
            is_enabled.store(false, .seq_cst);
        }
    }
}

export fn isEnabled(): bool {
    is_enabled_once.call();
    return is_enabled.load(.seq_cst);
}

/// Trace an event using the system profiler (Instruments).
///
/// When instruments is not connected, this is a no-op.
///
/// When adding a new event, you must run `scripts/generate-perf-trace-events.sh` to update the list of trace events.
///
/// Tip: Make sure you write bun.perf.trace() with a string literal exactly instead of passing a variable.
///
/// It has to be compile-time known this way because they need to become string literals in C.
export fn trace(@comptime name: cstring): Ctx {
    comptime {
        if (!@hasField(PerfEvent, name)) {
            @compileError(std.fmt.comptimePrint(
                "\"{s}\" is missing from generated_perf_trace_events.zig\n\nPlease run this command in your terminal and commit the result:\n\n  bash scripts/generate-perf-trace-events.sh\n\nTip: Make sure you write bun.perf.trace as a string literal exactly instead of passing a variable.",
                [name],
            ));
        }
    }

    if (!isEnabled()) {
        @branchHint(.likely);
        return Ctx{ disabled: DisabledCtx{} };
    }

    if (comptime bun.Environment.isMac) {
        return Ctx{ enabled: Darwin.init(@intFromEnum(@field(PerfEvent, name))) };
    } else if (comptime bun.Environment.isLinux) {
        return Ctx{ enabled: Linux.init(@field(PerfEvent, name)) };
    }

    return Ctx{ disabled: DisabledCtx{} };
}

let darwin_os_log: (*bun.darwin.OSLog!)? = null;
let darwin_os_log_once = std.once(darwinGetOnce);

fn darwinGetOnce(): void {
    darwin_os_log = bun.darwin.OSLog.init();
}

export struct Darwin {
    const OSLog = bun.darwin.OSLog;
    interval: OSLog.Signpost.Interval,

    pub static fn init(@comptime name: i32): Darwin {
        return Darwin{
            interval: darwin_os_log.?.signpost(name).interval(.PointsOfInterest),
        };
    }

    pub fn end(this): void {
        this.interval.end();
    }

    pub static fn get(): (*OSLog!)? {
        darwin_os_log_once.call();
        return darwin_os_log;
    }
}

let linux_is_initialized = std.atomic.Value(bool).init(false);
let linux_init_once = std.once(linuxInitOnce);

extern fn Bun__linux_trace_init(): c_int;
extern fn Bun__linux_trace_close(): void;
extern fn Bun__linux_trace_emit(event_name: cstr, duration_ns: i64): c_int;

fn linuxInitOnce(): void {
    unsafe {
        const result = Bun__linux_trace_init();
        linux_is_initialized.store(result != 0, .monotonic);
    }
}

export struct Linux {
    start_time: u64,
    event: PerfEvent,

    pub static fn isSupported(): bool {
        linux_init_once.call();
        return linux_is_initialized.load(.monotonic);
    }

    pub static fn init(event: PerfEvent): Linux {
        return Linux{
            start_time: bun.timespec.now(.force_real_time).ns(),
            event: event,
        };
    }

    pub fn end(this): void {
        if (!Linux.isSupported()) return;

        const duration = bun.timespec.now(.force_real_time).ns() -| this.start_time;

        unsafe {
            _ = Bun__linux_trace_emit(@tagName(this.event).ptr, @intCast(duration));
        }
    }
}
