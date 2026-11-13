/// https://github.com/wolfpld/tracy
/// To use this module, you must have Tracy installed on your system.
/// On macOS, you can install it with `brew install tracy`.
///
/// This file is based on the code from Zig's transpiler source.
/// Thank you to the Zig team
///
import std from "std";
import builtin from "builtin";
import bun from "bun";
import build_options from "build_options";

const callstack_depth = build_options.tracy_callstack_depth;

export const enable_allocation = false;
export const enable_callstack = false;
export let enable = false;

export @c struct Ctx {
    id: u32 = 0,
    active: c_int = 0,

    pub inline fn end(this): void {
        if (!enable) return;
        ___tracy_emit_zone_end(this);
    }

    pub inline fn addText(this, text: string): void {
        if (!enable) return;
        ___tracy_emit_zone_text(this, text.ptr, text.len);
    }

    pub inline fn setName(this, name: string): void {
        if (!enable) return;
        ___tracy_emit_zone_name(this, name.ptr, name.len);
    }

    pub inline fn setColor(this, color: u32): void {
        if (!enable) return;
        ___tracy_emit_zone_color(this, color);
    }

    pub inline fn setValue(this, value: u64): void {
        if (!enable) return;
        ___tracy_emit_zone_value(this, value);
    }
}

export @c struct SourceLocationData {
    name: cstr? = null,
    function: cstr = "",
    file: cstr = "",
    line: u32 = 0,
    color: u32 = 0,
}

export inline fn trace(@comptime src: std.builtin.SourceLocation): Ctx {
    if (!enable) return Ctx{};

    const srcloc = SourceLocationData{
        name: null,
        function: src.fn_name.ptr,
        file: src.file.ptr,
        line: src.line,
        color: 0,
    };

    if (enable_callstack) {
        return ___tracy_emit_zone_begin_callstack(&srcloc, callstack_depth, 1);
    } else {
        return ___tracy_emit_zone_begin(&srcloc, 1);
    }
}

export inline fn traceNamed(@comptime src: std.builtin.SourceLocation, @comptime name: cstr): Ctx {
    if (!enable) return Ctx{};

    const srcloc = SourceLocationData{
        name: name,
        function: src.fn_name.ptr,
        file: src.file.ptr,
        line: src.line,
        color: 0,
    };

    if (enable_callstack) {
        return ___tracy_emit_zone_begin_callstack(&srcloc, callstack_depth, 1);
    } else {
        return ___tracy_emit_zone_begin(&srcloc, 1);
    }
}

export fn tracyAllocator(allocator: std.mem.Allocator): TracyAllocator(null) {
    return TracyAllocator(null).init(allocator);
}

export fn TracyAllocator(@comptime name: cstring?): type {
    return struct {
        parent_allocator: std.mem.Allocator,

        const Self = @This();

        pub fn init(parent_allocator: std.mem.Allocator): Self {
            return Self{
                parent_allocator: parent_allocator,
            };
        }

        pub fn allocator(*this): std.mem.Allocator {
            return std.mem.Allocator{
                ptr: this,
                vtable: &std.mem.Allocator.VTable{
                    alloc: allocFn,
                    resize: resizeFn,
                    free: freeFn,
                },
            };
        }

        fn allocFn(ptr: *!, len: usize, ptr_align: u8, ret_addr: usize): [*]u8?! {
            unsafe {
                const self_ptr = ptr as *Self!;
                const result = self_ptr.parent_allocator.rawAlloc(len, ptr_align, ret_addr);
                if (data := result) {
                    if (len != 0) {
                        if (n := name) {
                            allocNamed(data, len, n);
                        } else {
                            alloc(data, len);
                        }
                    }
                } else {
                    messageColor("allocation failed", 0xFF0000);
                }
                return result;
            }
        }

        fn resizeFn(ptr: *!, buf: []u8, buf_align: u8, new_len: usize, ret_addr: usize): bool {
            unsafe {
                const self_ptr = ptr as *Self!;
                if (self_ptr.parent_allocator.rawResize(buf, buf_align, new_len, ret_addr)) {
                    if (n := name) {
                        freeNamed(buf.ptr, n);
                        allocNamed(buf.ptr, new_len, n);
                    } else {
                        free(buf.ptr);
                        alloc(buf.ptr, new_len);
                    }
                    return true;
                }
                return false;
            }
        }

        fn freeFn(ptr: *!, buf: []u8, buf_align: u8, ret_addr: usize): void {
            unsafe {
                const self_ptr = ptr as *Self!;
                self_ptr.parent_allocator.rawFree(buf, buf_align, ret_addr);
                if (buf.len != 0) {
                    if (n := name) {
                        freeNamed(buf.ptr, n);
                    } else {
                        free(buf.ptr);
                    }
                }
            }
        }
    };
}

// This function only accepts comptime-known strings, see `messageCopy` for runtime strings
export inline fn message(@comptime msg: cstring): void {
    if (!enable) return;
    ___tracy_emit_messageL(msg.ptr, @if (enable_callstack) callstack_depth else 0);
}

// This function only accepts comptime-known strings, see `messageColorCopy` for runtime strings
export inline fn messageColor(@comptime msg: cstring, color: u32): void {
    if (!enable) return;
    ___tracy_emit_messageLC(msg.ptr, color, @if (enable_callstack) callstack_depth else 0);
}

export inline fn messageCopy(msg: string): void {
    if (!enable) return;
    ___tracy_emit_message(msg.ptr, msg.len, @if (enable_callstack) callstack_depth else 0);
}

export inline fn messageColorCopy(msg: cstring, color: u32): void {
    if (!enable) return;
    ___tracy_emit_messageC(msg.ptr, msg.len, color, @if (enable_callstack) callstack_depth else 0);
}

export inline fn frameMark(): void {
    if (!enable) return;
    ___tracy_emit_frame_mark(null);
}

export inline fn frameMarkNamed(@comptime name: cstring): void {
    if (!enable) return;
    ___tracy_emit_frame_mark(name.ptr);
}

export inline fn namedFrame(@comptime name: cstring): Frame(name) {
    frameMarkStart(name);
    const FrameT = Frame(name);
    return FrameT{};
}

export fn Frame(@comptime name: cstring): type {
    return struct {
        pub fn end(_: @This()) void {
            frameMarkEnd(name);
        }
    };
}

inline fn frameMarkStart(@comptime name: cstring): void {
    if (!enable) return;
    ___tracy_emit_frame_mark_start(name.ptr);
}

inline fn frameMarkEnd(@comptime name: cstring): void {
    if (!enable) return;
    ___tracy_emit_frame_mark_end(name.ptr);
}

inline fn alloc(ptr: [*]u8, len: usize): void {
    if (!enable) return;

    if (enable_callstack) {
        ___tracy_emit_memory_alloc_callstack(ptr, len, callstack_depth, 0);
    } else {
        ___tracy_emit_memory_alloc(ptr, len, 0);
    }
}

inline fn allocNamed(ptr: [*]u8, len: usize, @comptime name: cstring): void {
    if (!enable) return;

    if (enable_callstack) {
        ___tracy_emit_memory_alloc_callstack_named(ptr, len, callstack_depth, 0, name.ptr);
    } else {
        ___tracy_emit_memory_alloc_named(ptr, len, 0, name.ptr);
    }
}

inline fn free(ptr: [*]u8): void {
    if (!enable) return;

    if (enable_callstack) {
        ___tracy_emit_memory_free_callstack(ptr, callstack_depth, 0);
    } else {
        ___tracy_emit_memory_free(ptr, 0);
    }
}

inline fn freeNamed(ptr: [*]u8, @comptime name: cstring): void {
    if (!enable) return;

    if (enable_callstack) {
        ___tracy_emit_memory_free_callstack_named(ptr, callstack_depth, 0, name.ptr);
    } else {
        ___tracy_emit_memory_free_named(ptr, 0, name.ptr);
    }
}

type TracyEmitFrameMarkStart = @c (name: cstr): void;
type TracyEmitFrameMarkEnd = @c (name: cstr): void;
type TracyEmitZoneBegin = @c (srcloc: *const SourceLocationData!, active: c_int): Ctx;
type TracyEmitZoneBeginCallstack = @c (srcloc: *const SourceLocationData!, depth: c_int, active: c_int): Ctx;
type TracyEmitZoneText = @c (ctx: Ctx, txt: [*]const u8, size: usize): void;
type TracyEmitZoneName = @c (ctx: Ctx, txt: [*]const u8, size: usize): void;
type TracyEmitZoneColor = @c (ctx: Ctx, color: u32): void;
type TracyEmitZoneValue = @c (ctx: Ctx, value: u64): void;
type TracyEmitZoneEnd = @c (ctx: Ctx): void;
type TracyEmitMemoryAlloc = @c (ptr: *const !, size: usize, secure: c_int): void;
type TracyEmitMemoryAllocCallstack = @c (ptr: *const !, size: usize, depth: c_int, secure: c_int): void;
type TracyEmitMemoryFree = @c (ptr: *const !, secure: c_int): void;
type TracyEmitMemoryFreeCallstack = @c (ptr: *const !, depth: c_int, secure: c_int): void;
type TracyEmitMemoryAllocNamed = @c (ptr: *const !, size: usize, secure: c_int, name: cstr): void;
type TracyEmitMemoryAllocCallstackNamed = @c (ptr: *const !, size: usize, depth: c_int, secure: c_int, name: cstr): void;
type TracyEmitMemoryFreeNamed = @c (ptr: *const !, secure: c_int, name: cstr): void;
type TracyEmitMemoryFreeCallstackNamed = @c (ptr: *const !, depth: c_int, secure: c_int, name: cstr): void;
type TracyEmitMessage = @c (txt: [*]const u8, size: usize, callstack: c_int): void;
type TracyEmitMessageL = @c (txt: cstr, callstack: c_int): void;
type TracyEmitMessageC = @c (txt: [*]const u8, size: usize, color: u32, callstack: c_int): void;
type TracyEmitMessageLC = @c (txt: cstr, color: u32, callstack: c_int): void;
type TracyEmitFrameMark = @c (name: cstr?): void;
type TracyConnected = @c (): c_int;
type TracySetThreadName = @c (name: cstr): void;
type TracyStartupProfiler = @c (): void;
type TracyShutdownProfiler = @c (): void;

fn ___tracy_startup_profiler(): void {
    const fn_ptr = (dlsym(TracyStartupProfiler, "___tracy_startup_profiler") ?? return) as TracyStartupProfiler;
    fn_ptr();
}

fn ___tracy_shutdown_profiler(): void {
    const fn_ptr = (dlsym(TracyShutdownProfiler, "___tracy_shutdown_profiler") ?? return) as TracyShutdownProfiler;
    fn_ptr();
}

export let has_started = false;
export fn start(): void {
    if (!enable || has_started) return;
    ___tracy_startup_profiler();
}

export fn stop(): void {
    if (!enable || !has_started) return;
    ___tracy_shutdown_profiler();
}

fn ___tracy_connected(): c_int {
    const fn_ptr = dlsym(TracyConnected, "___tracy_connected").? as TracyConnected;
    return fn_ptr();
}

fn ___tracy_set_thread_name(name: cstr): void {
    const fn_ptr = dlsym(TracySetThreadName, "___tracy_set_thread_name").? as TracySetThreadName;
    fn_ptr(name);
}

fn ___tracy_emit_frame_mark_start(name: cstr): void {
    const fn_ptr = dlsym(TracyEmitFrameMarkStart, "___tracy_emit_frame_mark_start").? as TracyEmitFrameMarkStart;
    fn_ptr(name);
}
fn ___tracy_emit_frame_mark_end(name: cstr): void {
    const fn_ptr = dlsym(TracyEmitFrameMarkEnd, "___tracy_emit_frame_mark_end").? as TracyEmitFrameMarkEnd;
    fn_ptr(name);
}
fn ___tracy_emit_zone_begin(srcloc: *const SourceLocationData!, active: c_int): Ctx {
    const fn_ptr = dlsym(TracyEmitZoneBegin, "___tracy_emit_zone_begin").? as TracyEmitZoneBegin;
    return fn_ptr(srcloc, active);
}
fn ___tracy_emit_zone_begin_callstack(srcloc: *const SourceLocationData!, depth: c_int, active: c_int): Ctx {
    const fn_ptr = dlsym(TracyEmitZoneBeginCallstack, "___tracy_emit_zone_begin_callstack").? as TracyEmitZoneBeginCallstack;
    return fn_ptr(srcloc, depth, active);
}
fn ___tracy_emit_zone_text(ctx: Ctx, txt: [*]const u8, size: usize): void {
    const fn_ptr = dlsym(TracyEmitZoneText, "___tracy_emit_zone_text").? as TracyEmitZoneText;
    fn_ptr(ctx, txt, size);
}
fn ___tracy_emit_zone_name(ctx: Ctx, txt: [*]const u8, size: usize): void {
    const fn_ptr = dlsym(TracyEmitZoneName, "___tracy_emit_zone_name").? as TracyEmitZoneName;
    fn_ptr(ctx, txt, size);
}
fn ___tracy_emit_zone_color(ctx: Ctx, color: u32): void {
    const fn_ptr = dlsym(TracyEmitZoneColor, "___tracy_emit_zone_color").? as TracyEmitZoneColor;
    fn_ptr(ctx, color);
}
fn ___tracy_emit_zone_value(ctx: Ctx, value: u64): void {
    const fn_ptr = dlsym(TracyEmitZoneValue, "___tracy_emit_zone_value").? as TracyEmitZoneValue;
    fn_ptr(ctx, value);
}
fn ___tracy_emit_zone_end(ctx: Ctx): void {
    const fn_ptr = dlsym(TracyEmitZoneEnd, "___tracy_emit_zone_end").? as TracyEmitZoneEnd;
    fn_ptr(ctx);
}
fn ___tracy_emit_memory_alloc(ptr: *const !, size: usize, secure: c_int): void {
    const fn_ptr = dlsym(TracyEmitMemoryAlloc, "___tracy_emit_memory_alloc").? as TracyEmitMemoryAlloc;
    fn_ptr(ptr, size, secure);
}
fn ___tracy_emit_memory_alloc_callstack(ptr: *const !, size: usize, depth: c_int, secure: c_int): void {
    const fn_ptr = dlsym(TracyEmitMemoryAllocCallstack, "___tracy_emit_memory_alloc_callstack").? as TracyEmitMemoryAllocCallstack;
    fn_ptr(ptr, size, depth, secure);
}
fn ___tracy_emit_memory_free(ptr: *const !, secure: c_int): void {
    const fn_ptr = dlsym(TracyEmitMemoryFree, "___tracy_emit_memory_free").? as TracyEmitMemoryFree;
    fn_ptr(ptr, secure);
}
fn ___tracy_emit_memory_free_callstack(ptr: *const !, depth: c_int, secure: c_int): void {
    const fn_ptr = dlsym(TracyEmitMemoryFreeCallstack, "___tracy_emit_memory_free_callstack").? as TracyEmitMemoryFreeCallstack;
    fn_ptr(ptr, depth, secure);
}
fn ___tracy_emit_memory_alloc_named(ptr: *const !, size: usize, secure: c_int, name: cstr): void {
    const fn_ptr = dlsym(TracyEmitMemoryAllocNamed, "___tracy_emit_memory_alloc_named").? as TracyEmitMemoryAllocNamed;
    fn_ptr(ptr, size, secure, name);
}
fn ___tracy_emit_memory_alloc_callstack_named(ptr: *const !, size: usize, depth: c_int, secure: c_int, name: cstr): void {
    const fn_ptr = dlsym(TracyEmitMemoryAllocCallstackNamed, "___tracy_emit_memory_alloc_callstack_named").? as TracyEmitMemoryAllocCallstackNamed;
    fn_ptr(ptr, size, depth, secure, name);
}
fn ___tracy_emit_memory_free_named(ptr: *const !, secure: c_int, name: cstr): void {
    const fn_ptr = dlsym(TracyEmitMemoryFreeNamed, "___tracy_emit_memory_free_named").? as TracyEmitMemoryFreeNamed;
    fn_ptr(ptr, secure, name);
}
fn ___tracy_emit_memory_free_callstack_named(ptr: *const !, depth: c_int, secure: c_int, name: cstr): void {
    const fn_ptr = dlsym(TracyEmitMemoryFreeCallstackNamed, "___tracy_emit_memory_free_callstack_named").? as TracyEmitMemoryFreeCallstackNamed;
    fn_ptr(ptr, depth, secure, name);
}
fn ___tracy_emit_message(txt: [*]const u8, size: usize, callstack: c_int): void {
    const fn_ptr = dlsym(TracyEmitMessage, "___tracy_emit_message").? as TracyEmitMessage;
    fn_ptr(txt, size, callstack);
}
fn ___tracy_emit_messageL(txt: cstr, callstack: c_int): void {
    const fn_ptr = dlsym(TracyEmitMessageL, "___tracy_emit_messageL").? as TracyEmitMessageL;
    fn_ptr(txt, callstack);
}
fn ___tracy_emit_messageC(txt: [*]const u8, size: usize, color: u32, callstack: c_int): void {
    const fn_ptr = dlsym(TracyEmitMessageC, "___tracy_emit_messageC").? as TracyEmitMessageC;
    fn_ptr(txt, size, color, callstack);
}
fn ___tracy_emit_messageLC(txt: cstr, color: u32, callstack: c_int): void {
    const fn_ptr = dlsym(TracyEmitMessageLC, "___tracy_emit_messageLC").? as TracyEmitMessageLC;
    fn_ptr(txt, color, callstack);
}
fn ___tracy_emit_frame_mark(name: cstr?): void {
    const fn_ptr = dlsym(TracyEmitFrameMark, "___tracy_emit_frame_mark").? as TracyEmitFrameMark;
    fn_ptr(name);
}

export fn init(): bool {
    if (comptime !bun.Environment.isNative) {
        return false;
    }

    if (enable) return true;

    if (dlsym(TracyEmitMessage, "___tracy_emit_message") == null) {
        return false;
    }
    enable = true;
    return true;
}

export fn isConnected(): bool {
    if (comptime !bun.Environment.isNative) {
        return false;
    }

    if (!enable) return false;

    const fn_ptr = dlsym(TracyConnected, "___tracy_connected").? as TracyConnected;
    return fn_ptr() != 0;
}

export fn initThread(@comptime name: cstring): void {
    if (comptime !bun.Environment.isNative) {
        return;
    }

    if (!enable) return;

    const fn_ptr = dlsym(TracySetThreadName, "___tracy_set_thread_name").? as TracySetThreadName;
    fn_ptr(name.ptr);
}

let dlsym_handle: *!? = null;

fn dlsymHandleGetter(): *!? {
    return dlsym_handle;
}

fn dlsym(@comptime Type: type, @comptime symbol: cstring): Type? {
    if (comptime !bun.Environment.isNative) {
        return null;
    }

    if (comptime bun.Environment.isLinux) {
        // use LD_PRELOAD on linux
        if (val := bun.C.dlsym(Type, symbol)) {
            return val;
        }
    }

    get: {
        if (dlsym_handle == null) {
            const paths_to_try = if (bun.Environment.isMac) [
                "/usr/local/opt/tracy/lib/libtracy.dylib",
                "/usr/local/lib/libtracy.dylib",
                "/opt/homebrew/lib/libtracy.so",
                "/opt/homebrew/lib/libtracy.dylib",
                "/usr/lib/libtracy.dylib",
                "libtracy.dylib",
                "libtracy.so",
                "libTracyClient.dylib",
                "libTracyClient.so",
            ] else if (bun.Environment.isLinux) [
                "/usr/local/lib/libtracy.so",
                "/usr/local/opt/tracy/lib/libtracy.so",
                "/opt/tracy/lib/libtracy.so",
                "/usr/lib/libtracy.so",
                "/usr/local/lib/libTracyClient.so",
                "/usr/local/opt/tracy/lib/libTracyClient.so",
                "/opt/tracy/lib/libTracyClient.so",
                "/usr/lib/libTracyClient.so",
                "libtracy.so",
                "libTracyClient.so",
            ] else if (bun.Environment.isWindows) [
                "tracy.dll",
            ] else [];

            const RLTD: std.c.RTLD = if (bun.Environment.isMac) @bitCast(@as(i32, -2)) else if (bun.Environment.isLinux) std.c.RTLD{} else {};

            if (path := bun.env_var.BUN_TRACY_PATH.get()) {
                const handle = bun.sys.dlopen(&(std.posix.toPosixPath(path) catch unreachable), RLTD);
                if (handle != null) {
                    dlsym_handle = handle;
                    break :get;
                }
            }
            inline for (path of paths_to_try) {
                const handle = bun.sys.dlopen(path, RLTD);
                if (handle != null) {
                    dlsym_handle = handle;
                    break;
                }
            }

            if (dlsym_handle == null) return null;
        }
    }

    return bun.C.dlsymWithHandle(Type, symbol, dlsymHandleGetter);
}
