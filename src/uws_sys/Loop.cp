zig {
    const std = @import("std");
    const bun = @import("bun");
    const Environment = bun.Environment;
    const uws = bun.uws;
    const InternalLoopData = uws.InternalLoopData;

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

export @extern @value class PosixLoop {
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

        pub fn uncork(this: *PosixLoop) void {
            c.uws_res_clear_corked_socket(this);
        }

        pub fn updateDate(this: *PosixLoop) void {
            c.uws_loop_date_header_timer_update(this);
        }

        pub fn iterationNumber(this: *const PosixLoop) u64 {
            return this.internal_loop_data.iteration_nr;
        }

        pub fn isActive(this: *const Loop) bool {
            return this.active > 0;
        }

        pub fn get() *Loop {
            return c.uws_get_loop();
        }

        pub fn drainQuicIfNecessary(this: *Loop) void {
            if (this.internal_loop_data.quic_head == null) return;
            c.us_quic_loop_flush_if_pending(this);
        }

        pub fn create(comptime Handler: anytype) *Loop {
            return c.us_create_loop(
                null,
                Handler.wakeup,
                if (@hasDecl(Handler, "pre")) Handler.pre else null,
                if (@hasDecl(Handler, "post")) Handler.post else null,
                0,
            ).?;
        }

        pub const wake = wakeup;

        pub fn tick(this: *PosixLoop) void {
            c.us_loop_run_bun_tick(this, null);
        }

        pub fn tickWithoutIdle(this: *PosixLoop) void {
            const timespec = bun.timespec{ .sec = 0, .nsec = 0 };
            c.us_loop_run_bun_tick(this, &timespec);
        }

        pub fn tickWithTimeout(this: *PosixLoop, timespec: ?*const bun.timespec) void {
            c.us_loop_run_bun_tick(this, timespec);
        }

        pub fn drainClosedSockets(this: *PosixLoop) void {
            c.us_internal_free_closed_sockets(this);
        }

        pub fn closeAllGroups(this: *PosixLoop) bool {
            return c.us_loop_close_all_groups(this) != 0;
        }

        pub fn nextTick(this: *PosixLoop, comptime UserType: type, user_data: UserType, comptime deferCallback: fn (ctx: UserType) void) void {
            const Handler = struct {
                pub fn callback(data: *anyopaque) callconv(.c) void {
                    deferCallback(@as(UserType, @ptrCast(@alignCast(data))));
                }
            };
            c.uws_loop_defer(this, user_data, Handler.callback);
        }

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

        pub fn shouldEnableDateHeaderTimer(this: *const PosixLoop) bool {
            return this.internal_loop_data.shouldEnableDateHeaderTimer();
        }
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

    pub fn wakeup(*this): void {
        zig {
            c.us_wakeup_loop(self);
        }
    }

    pub fn run(*this): void {
        zig {
            c.us_loop_run(self);
        }
    }

    pub fn deinit(*this): void {
        zig {
            c.us_loop_free(self);
        }
    }

    pub fn release(*this): void {
        this.deinit();
    }
}

export @extern @value class WindowsLoop {
    zig {
        const uv = bun.windows.libuv;

        internal_loop_data: InternalLoopData align(16),
        uv_loop: *uv.Loop,
        is_default: c_int,
        pre: *uv.uv_prepare_t,
        check: *uv.uv_check_t,

        pub fn shouldEnableDateHeaderTimer(this: *const WindowsLoop) bool {
            return this.internal_loop_data.shouldEnableDateHeaderTimer();
        }

        pub fn uncork(this: *PosixLoop) void {
            c.uws_res_clear_corked_socket(this);
        }

        pub fn get() *WindowsLoop {
            return c.uws_get_loop_with_native(bun.windows.libuv.Loop.get());
        }

        pub fn iterationNumber(this: *const WindowsLoop) u64 {
            return this.internal_loop_data.iteration_nr;
        }

        pub fn addActive(this: *const WindowsLoop, val: u32) void {
            this.uv_loop.addActive(val);
        }

        pub fn subActive(this: *const WindowsLoop, val: u32) void {
            this.uv_loop.subActive(val);
        }

        pub fn isActive(this: *const WindowsLoop) bool {
            return this.uv_loop.isActive();
        }

        pub const wake = wakeup;

        pub fn tickWithTimeout(this: *WindowsLoop, _: ?*const bun.timespec) void {
            c.us_loop_run(this);
        }

        pub fn tickWithoutIdle(this: *WindowsLoop) void {
            c.us_loop_pump(this);
        }

        pub fn drainQuicIfNecessary(this: *WindowsLoop) void {
            if (this.internal_loop_data.quic_head == null) return;
            c.us_quic_loop_flush_if_pending(this);
        }

        pub fn create(comptime Handler: anytype) *WindowsLoop {
            return c.us_create_loop(
                null,
                Handler.wakeup,
                if (@hasDecl(Handler, "pre")) Handler.pre else null,
                if (@hasDecl(Handler, "post")) Handler.post else null,
                0,
            ).?;
        }

        pub const tick = run;
        pub const wait = run;

        pub fn inc(this: *WindowsLoop) void {
            this.uv_loop.inc();
        }

        pub fn dec(this: *WindowsLoop) void {
            this.uv_loop.dec();
        }

        pub const ref = inc;
        pub const unref = dec;

        pub fn drainClosedSockets(this: *WindowsLoop) void {
            c.us_internal_free_closed_sockets(this);
        }

        pub fn closeAllGroups(this: *WindowsLoop) bool {
            return c.us_loop_close_all_groups(this) != 0;
        }

        pub fn nextTick(this: *Loop, comptime UserType: type, user_data: UserType, comptime deferCallback: fn (ctx: UserType) void) void {
            const Handler = struct {
                pub fn callback(data: *anyopaque) callconv(.c) void {
                    deferCallback(@as(UserType, @ptrCast(@alignCast(data))));
                }
            };
            c.uws_loop_defer(this, user_data, Handler.callback);
        }

        pub fn updateDate(this: *Loop) void {
            c.uws_loop_date_header_timer_update(this);
        }

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

    pub fn wakeup(*this): void {
        zig {
            c.us_wakeup_loop(self);
        }
    }

    pub fn run(*this): void {
        zig {
            c.us_loop_run(self);
        }
    }

    pub fn deinit(*this): void {
        zig {
            c.us_loop_free(self);
        }
    }

    pub fn release(*this): void {
        this.deinit();
    }
}

zig {
    pub const Loop = if (bun.Environment.isWindows) WindowsLoop else PosixLoop;
}
