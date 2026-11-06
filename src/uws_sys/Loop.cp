// src/uws_sys/Loop.cp
// CP port of Loop (PosixLoop and WindowsLoop)

import bun from "bun";
import { InternalLoopData } from "./InternalLoopData.cp";

zig {
    const std = @import("std");
    const Environment = bun.Environment;
    const log = bun.Output.scoped(.Loop, .visible);

    const c = struct {
        pub extern fn us_create_loop(
            hint: ?*anyopaque,
            wakeup_cb: ?*const fn (*Loop) callconv(.c) void,
            pre_cb: ?*const fn (*Loop) callconv(.c) void,
            post_cb: ?*const fn (*Loop) callconv(.c) void,
            ext_size: c_uint,
        ) ?*Loop;
        pub extern fn us_loop_free(loop: ?*Loop) void;
        pub extern fn us_loop_ext(loop: ?*Loop) ?*anyopaque;
        pub extern fn us_quic_loop_flush_if_pending(loop: *Loop) void;
        pub extern fn us_loop_run(loop: ?*Loop) void;
        pub extern fn us_loop_pump(loop: ?*Loop) void;
        pub extern fn us_wakeup_loop(loop: ?*Loop) void;
        pub extern fn us_loop_integrate(loop: ?*Loop) void;
        pub extern fn us_loop_iteration_number(loop: ?*Loop) c_longlong;
        pub extern fn uws_loop_addPostHandler(loop: *Loop, ctx: *anyopaque, cb: *const (fn (ctx: *anyopaque, loop: *Loop) callconv(.c) void)) void;
        pub extern fn uws_loop_removePostHandler(loop: *Loop, ctx: *anyopaque, cb: *const (fn (ctx: *anyopaque, loop: *Loop) callconv(.c) void)) void;
        pub extern fn uws_loop_addPreHandler(loop: *Loop, ctx: *anyopaque, cb: *const (fn (ctx: *anyopaque, loop: *Loop) callconv(.c) void)) void;
        pub extern fn uws_loop_removePreHandler(loop: *Loop, ctx: *anyopaque, cb: *const (fn (ctx: *anyopaque, loop: *Loop) callconv(.c) void)) void;
        pub extern fn us_loop_run_bun_tick(loop: ?*Loop, timouetMs: ?*const bun.timespec) void;
        pub extern fn us_internal_free_closed_sockets(loop: *Loop) void;
        pub extern fn us_loop_close_all_groups(loop: *Loop) c_int;
        pub extern fn uws_get_loop() *Loop;
        pub extern fn uws_get_loop_with_native(*anyopaque) *WindowsLoop;
        pub extern fn uws_loop_defer(loop: *Loop, ctx: *anyopaque, cb: *const (fn (ctx: *anyopaque) callconv(.c) void)) void;
        pub extern fn uws_res_clear_corked_socket(loop: *Loop) void;
        pub extern fn uws_loop_date_header_timer_update(loop: *Loop) void;
    };
}

export @extern class PosixLoop {
    zig {
        internal_loop_data: InternalLoopData align(16),
        num_polls: i32,
        num_ready_polls: i32,
        current_ready_poll: i32,
        fd: i32,
        active: u32 = 0,
        pending_wakeups: u32 = 0,
        ready_polls: [1024]EventType align(16),

        const EventType = switch (Environment.os) {
            .linux => std.os.linux.epoll_event,
            .mac => std.posix.system.kevent64_s,
            .freebsd => std.c.Kevent,
            .windows => *anyopaque,
            .wasm => @compileError("Unsupported OS"),
        };

        pub const wake = wakeup;

        fn NewHandler(comptime UserType: type, comptime callback_fn: fn (UserType) void) type {
            return struct {
                loop: *Loop,
                pub fn removePost(handler: @This()) void {
                    return c.uws_loop_removePostHandler(handler.loop, callback);
                }
                pub fn removePre(handler: @This()) void {
                    return c.uws_loop_removePostHandler(handler.loop, callback);
                }
                pub fn callback(data: *anyopaque, _: *Loop) callconv(.c) void {
                    callback_fn(@as(UserType, @ptrCast(@alignCast(data))));
                }
            };
        }
    }

    pub fn uncork(*this): void {
        zig {
            c.uws_res_clear_corked_socket(self);
        }
    }

    pub fn updateDate(*this): void {
        zig {
            c.uws_loop_date_header_timer_update(self);
        }
    }

    pub fn iterationNumber(this): u64 {
        return this.internal_loop_data.iteration_nr;
    }

    pub fn inc(*this): void {
        zig {
            log("inc {d} + 1 = {d}", .{ self.num_polls, self.num_polls + 1 });
            self.num_polls += 1;
        }
    }

    pub fn dec(*this): void {
        zig {
            log("dec {d} - 1 = {d}", .{ self.num_polls, self.num_polls - 1 });
            self.num_polls -= 1;
        }
    }

    pub fn ref(*this): void {
        zig {
            log("ref {d} + 1 = {d} | {d} + 1 = {d}", .{ self.num_polls, self.num_polls + 1, self.active, self.active + 1 });
            self.num_polls += 1;
            self.active += 1;
        }
    }

    pub fn unref(*this): void {
        zig {
            log("unref {d} - 1 = {d} | {d} - 1 = {d}", .{ self.num_polls, self.num_polls - 1, self.active, self.active -| 1 });
            self.num_polls -= 1;
            self.active -|= 1;
        }
    }

    pub fn isActive(this): bool {
        return this.active > 0;
    }

    pub fn addActive(*this, value: u32): void {
        zig {
            log("add {d} + {d} = {d}", .{ self.active, value, self.active +| value });
            self.active +|= value;
        }
    }

    pub fn subActive(*this, value: u32): void {
        zig {
            log("sub {d} - {d} = {d}", .{ self.active, value, self.active -| value });
            self.active -|= value;
        }
    }

    pub fn unrefCount(*this, count: i32): void {
        zig {
            log("unref x {d}", .{count});
            self.num_polls -= count;
            self.active -|= @as(u32, @intCast(count));
        }
    }

    pub static fn get(): *Loop {
        zig {
            return c.uws_get_loop();
        }
    }

    pub fn drainQuicIfNecessary(*this): void {
        if (this.internal_loop_data.quic_head == null) return;
        zig {
            c.us_quic_loop_flush_if_pending(self);
        }
    }

    pub static fn create(@comptime Handler: anytype): *Loop {
        zig {
            return c.us_create_loop(
                null,
                Handler.wakeup,
                if (@hasDecl(Handler, "pre")) Handler.pre else null,
                if (@hasDecl(Handler, "post")) Handler.post else null,
                0,
            ).?;
        }
    }

    pub fn wakeup(*this): void {
        zig {
            return c.us_wakeup_loop(self);
        }
    }

    pub fn tick(*this): void {
        zig {
            c.us_loop_run_bun_tick(self, null);
        }
    }

    pub fn tickWithoutIdle(*this): void {
        zig {
            const timespec = bun.timespec{ .sec = 0, .nsec = 0 };
            c.us_loop_run_bun_tick(self, &timespec);
        }
    }

    pub fn tickWithTimeout(*this, timespec: *const bun.timespec?): void {
        zig {
            c.us_loop_run_bun_tick(self, timespec);
        }
    }

    pub fn drainClosedSockets(*this): void {
        zig {
            c.us_internal_free_closed_sockets(self);
        }
    }

    pub fn closeAllGroups(*this): bool {
        zig {
            return c.us_loop_close_all_groups(self) != 0;
        }
    }

    pub fn nextTick(*this, @comptime UserType: type, user_data: UserType, @comptime deferCallback: anytype): void {
        zig {
            const Handler = struct {
                pub fn callback(data: *anyopaque) callconv(.c) void {
                    deferCallback(@as(UserType, @ptrCast(@alignCast(data))));
                }
            };
            c.uws_loop_defer(self, user_data, Handler.callback);
        }
    }

    zig {
        pub fn addPostHandler(this: *PosixLoop, comptime UserType: type, ctx: UserType, comptime callback: fn (UserType) void) NewHandler(UserType, callback) {
            const Handler = NewHandler(UserType, callback);
            c.uws_loop_addPostHandler(this, ctx, Handler.callback);
            return Handler{
                .loop = this,
            };
        }

        pub fn addPreHandler(this: *PosixLoop, comptime UserType: type, ctx: UserType, comptime callback: fn (UserType) void) NewHandler(UserType, callback) {
            const Handler = NewHandler(UserType, callback);
            c.uws_loop_addPreHandler(this, ctx, Handler.callback);
            return Handler{
                .loop = this,
            };
        }
    }

    pub fn run(*this): void {
        zig {
            c.us_loop_run(self);
        }
    }

    pub fn shouldEnableDateHeaderTimer(this): bool {
        return this.internal_loop_data.shouldEnableDateHeaderTimer();
    }

    pub fn release(*this): void {
        this.deinit();
    }

    pub fn deinit(*this): void {
        zig {
            c.us_loop_free(self);
        }
    }
}

export @extern class WindowsLoop {
    zig {
        const uv = bun.windows.libuv;
        internal_loop_data: InternalLoopData align(16),
        uv_loop: *uv.Loop,
        is_default: c_int,
        pre: *uv.uv_prepare_t,
        check: *uv.uv_check_t,

        pub const wake = wakeup;
        pub const tick = run;
        pub const wait = run;
        pub const ref = inc;
        pub const unref = dec;

        fn NewHandler(comptime UserType: type, comptime callback_fn: fn (UserType) void) type {
            return struct {
                loop: *Loop,
                pub fn removePost(handler: @This()) void {
                    return c.uws_loop_removePostHandler(handler.loop, callback);
                }
                pub fn removePre(handler: @This()) void {
                    return c.uws_loop_removePostHandler(handler.loop, callback);
                }
                pub fn callback(data: *anyopaque, _: *Loop) callconv(.c) void {
                    callback_fn(@as(UserType, @ptrCast(@alignCast(data))));
                }
            };
        }
    }

    pub fn shouldEnableDateHeaderTimer(this): bool {
        return this.internal_loop_data.shouldEnableDateHeaderTimer();
    }

    pub fn uncork(*this): void {
        zig {
            c.uws_res_clear_corked_socket(self);
        }
    }

    pub static fn get(): *WindowsLoop {
        zig {
            return c.uws_get_loop_with_native(bun.windows.libuv.Loop.get());
        }
    }

    pub fn iterationNumber(this): u64 {
        return this.internal_loop_data.iteration_nr;
    }

    pub fn addActive(this, val: u32): void {
        zig {
            self.uv_loop.addActive(val);
        }
    }

    pub fn subActive(this, val: u32): void {
        zig {
            self.uv_loop.subActive(val);
        }
    }

    pub fn isActive(this): bool {
        zig {
            return self.uv_loop.isActive();
        }
    }

    pub fn wakeup(*this): void {
        zig {
            c.us_wakeup_loop(self);
        }
    }

    pub fn tickWithTimeout(*this, timespec: *const bun.timespec?): void {
        zig {
            _ = timespec;
            c.us_loop_run(self);
        }
    }

    pub fn tickWithoutIdle(*this): void {
        zig {
            c.us_loop_pump(self);
        }
    }

    pub fn drainQuicIfNecessary(*this): void {
        if (this.internal_loop_data.quic_head == null) return;
        zig {
            c.us_quic_loop_flush_if_pending(self);
        }
    }

    pub static fn create(@comptime Handler: anytype): *WindowsLoop {
        zig {
            return c.us_create_loop(
                null,
                Handler.wakeup,
                if (@hasDecl(Handler, "pre")) Handler.pre else null,
                if (@hasDecl(Handler, "post")) Handler.post else null,
                0,
            ).?;
        }
    }

    pub fn run(*this): void {
        zig {
            c.us_loop_run(self);
        }
    }

    pub fn inc(*this): void {
        zig {
            self.uv_loop.inc();
        }
    }

    pub fn dec(*this): void {
        zig {
            self.uv_loop.dec();
        }
    }

    pub fn drainClosedSockets(*this): void {
        zig {
            c.us_internal_free_closed_sockets(self);
        }
    }

    pub fn closeAllGroups(*this): bool {
        zig {
            return c.us_loop_close_all_groups(self) != 0;
        }
    }

    pub fn nextTick(*this, @comptime UserType: type, user_data: UserType, @comptime deferCallback: anytype): void {
        zig {
            const Handler = struct {
                pub fn callback(data: *anyopaque) callconv(.c) void {
                    deferCallback(@as(UserType, @ptrCast(@alignCast(data))));
                }
            };
            c.uws_loop_defer(self, user_data, Handler.callback);
        }
    }

    pub fn updateDate(*this): void {
        zig {
            c.uws_loop_date_header_timer_update(self);
        }
    }

    pub fn release(*this): void {
        this.deinit();
    }

    pub fn deinit(*this): void {
        zig {
            c.us_loop_free(self);
        }
    }
}

zig {
    pub const Loop = if (bun.Environment.isWindows) WindowsLoop else PosixLoop;
}
