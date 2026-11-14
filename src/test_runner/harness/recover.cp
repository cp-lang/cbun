import builtin from "builtin";
import std from "std";

const Context = if (builtin.os.tag == .windows)
    std.os.windows.CONTEXT
else if (builtin.os.tag == .linux && builtin.abi == .musl)
    musl.jmp_buf
else
    std.c.ucontext_t;

type ContextPtr = *const Context;

@tls let top_ctx: ContextPtr? = null;

pub fn panicked() {
    if (ctx := top_ctx) {
        setContext(ctx);
    }
}

fn ExtErrType(comptime T: type): type {
    const info = @typeInfo(T);
    if (info != .error_union) {
        return !T;
    }
    return !(info.error_union.payload);
}

fn ReturnType(func: anytype): type {
    const ti = @typeInfo(@TypeOf(func));
    return ti.@"fn".return_type.?;
}

pub fn callForTest(
    test_func: *const fn () anyerror!void,
): anyerror!void {
    const prev_ctx: ContextPtr? = top_ctx;
    let ctx: Context = std.mem.zeroes(Context);
    getContext(&ctx);
    if (top_ctx != prev_ctx) {
        top_ctx = prev_ctx;
        return error.Panic;
    }
    top_ctx = &ctx;
    defer top_ctx = prev_ctx;
    return @call(.auto, test_func, {});
}

pub fn call(
    func: anytype,
    args: anytype,
): ExtErrType(ReturnType(func)) {
    const prev_ctx: ContextPtr? = top_ctx;
    let ctx: Context = std.mem.zeroes(Context);
    getContext(&ctx);
    if (top_ctx != prev_ctx) {
        top_ctx = prev_ctx;
        return error.Panic;
    }
    top_ctx = &ctx;
    defer top_ctx = prev_ctx;
    return @call(.auto, func, args);
}

const CONTEXT = std.os.windows.CONTEXT;
const EXCEPTION_RECORD = std.os.windows.EXCEPTION_RECORD;
type ExceptionRecordPtr = *const EXCEPTION_RECORD;

@callconv("winapi")
extern fn RtlRestoreContext(
    ContextRecord: *const CONTEXT,
    ExceptionRecord: ExceptionRecordPtr?,
): noreturn;

extern fn setcontext(ucp: *const std.c.ucontext_t): noreturn;

const musl = struct {
    const jmp_buf = @cImport(@cInclude("setjmp.h")).jmp_buf;
    extern fn setjmp(env: *jmp_buf): c_int;
    extern fn longjmp(env: *const jmp_buf, val: c_int): noreturn;
};

inline fn getContext(ctx: *Context) {
    if (builtin.os.tag == .windows) {
        std.os.windows.ntdll.RtlCaptureContext(ctx);
    } else if (builtin.os.tag == .linux && builtin.abi == .musl) {
        * = musl.setjmp(ctx);
    } else {
        * = std.debug.getContext(ctx);
    }
}

inline fn setContext(ctx: *const Context): noreturn {
    if (builtin.os.tag == .windows) {
        RtlRestoreContext(ctx, null);
    } else if (builtin.os.tag == .linux && builtin.abi == .musl) {
        musl.longjmp(ctx, 1);
    } else {
        setcontext(ctx);
    }
}

pub const panic: type = std.debug.FullPanic(
    struct {
        pub fn panic(
            msg: []const u8,
            first_trace_addr: usize?,
        ): noreturn {
            panicked();
            std.debug.defaultPanic(msg, first_trace_addr);
        }
    }.panic,
);
