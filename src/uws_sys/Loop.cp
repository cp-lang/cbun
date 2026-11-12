// src/uws_sys/Loop.cp
// CP port of Loop (PosixLoop and WindowsLoop)

import bun from "bun";
import { InternalLoopData } from "./InternalLoopData.cp";

@c {
    fn us_create_loop(
        hint: *!?,
        wakeup_cb: ?@c fn (*Loop): void,
        pre_cb: ?@c fn (*Loop): void,
        post_cb: ?@c fn (*Loop): void,
        ext_size: u32,
    ): ?*Loop;
    fn us_loop_free(loop: ?*Loop): void;
    fn us_loop_ext(loop: ?*Loop): *!?;
    fn us_quic_loop_flush_if_pending(loop: *Loop): void;
    fn us_loop_run(loop: ?*Loop): void;
    fn us_loop_pump(loop: ?*Loop): void;
    fn us_wakeup_loop(loop: ?*Loop): void;
    fn us_loop_integrate(loop: ?*Loop): void;
    fn us_loop_iteration_number(loop: ?*Loop): i64;
    fn uws_loop_addPostHandler(loop: *Loop, ctx: *!, cb: @c fn (*!, *Loop): void): void;
    fn uws_loop_removePostHandler(loop: *Loop, ctx: *!, cb: @c fn (*!, *Loop): void): void;
    fn uws_loop_addPreHandler(loop: *Loop, ctx: *!, cb: @c fn (*!, *Loop): void): void;
    fn uws_loop_removePreHandler(loop: *Loop, ctx: *!, cb: @c fn (*!, *Loop): void): void;
    fn us_loop_run_bun_tick(loop: ?*Loop, timeout_ms: ?*const bun.timespec): void;
    fn us_internal_free_closed_sockets(loop: *Loop): void;
    fn us_loop_close_all_groups(loop: *Loop): i32;
    fn uws_get_loop(): *Loop;
    fn uws_get_loop_with_native(native: *!): *WindowsLoop;
    fn uws_loop_defer(loop: *Loop, ctx: *!, cb: @c fn (*!): void): void;
    fn uws_res_clear_corked_socket(loop: *Loop): void;
    fn uws_loop_date_header_timer_update(loop: *Loop): void;
}

export class LoopHandler {
    loop: *Loop,
    cb: @c fn (*!, *Loop): void,

    pub fn removePost(this): void {
        unsafe {
            uws_loop_removePostHandler(this.loop, this as *!, this.cb);
        }
    }

    pub fn removePre(this): void {
        unsafe {
            uws_loop_removePreHandler(this.loop, this as *!, this.cb);
        }
    }
}

export @extern class PosixLoop {
    @align(16) internal_loop_data: InternalLoopData,
    num_polls: i32 = 0,
    num_ready_polls: i32 = 0,
    current_ready_poll: i32 = 0,
    fd: i32 = 0,
    active: u32 = 0,
    pending_wakeups: u32 = 0,
    @align(16) ready_polls: bun.EpollEvent[1024],

    pub fn uncork(*this): void {
        uws_res_clear_corked_socket(this as *Loop);
    }

    pub fn updateDate(*this): void {
        uws_loop_date_header_timer_update(this as *Loop);
    }

    pub fn iterationNumber(this): u64 {
        return this.internal_loop_data.iteration_nr;
    }

    pub fn inc(*this): void {
        this.num_polls += 1;
    }

    pub fn dec(*this): void {
        this.num_polls -= 1;
    }

    pub fn ref(*this): void {
        this.num_polls += 1;
        this.active += 1;
    }

    pub fn unref(*this): void {
        this.num_polls -= 1;
        if (this.active > 0) this.active -= 1;
    }

    pub fn isActive(this): bool {
        return this.active > 0;
    }

    pub fn addActive(*this, value: u32): void {
        this.active += value;
    }

    pub fn subActive(*this, value: u32): void {
        if (this.active > value) {
            this.active -= value;
        } else {
            this.active = 0;
        }
    }

    pub fn unrefCount(*this, count: i32): void {
        this.num_polls -= count;
        let c_u32 = count as u32;
        if (this.active > c_u32) {
            this.active -= c_u32;
        } else {
            this.active = 0;
        }
    }

    pub static fn get(): *Loop {
        return uws_get_loop();
    }

    pub fn drainQuicIfNecessary(*this): void {
        if (this.internal_loop_data.quic_head == null) return;
        us_quic_loop_flush_if_pending(this as *Loop);
    }

    pub static fn create(@comptime Handler: type): *Loop {
        let mut_pre_cb = if (@hasDecl(Handler, "pre")) Handler.pre else null;
        let mut_post_cb = if (@hasDecl(Handler, "post")) Handler.post else null;
        return us_create_loop(
            null,
            Handler.wakeup,
            mut_pre_cb,
            mut_post_cb,
            0,
        ).?;
    }

    pub fn wakeup(*this): void {
        us_wakeup_loop(this as *Loop);
    }

    pub fn tick(*this): void {
        us_loop_run_bun_tick(this as *Loop, null);
    }

    pub fn tickWithoutIdle(*this): void {
        const ts: bun.timespec = .{ .sec = 0, .nsec = 0 };
        us_loop_run_bun_tick(this as *Loop, &ts);
    }

    pub fn tickWithTimeout(*this, timespec: *const bun.timespec?): void {
        us_loop_run_bun_tick(this as *Loop, timespec);
    }

    pub fn drainClosedSockets(*this): void {
        us_internal_free_closed_sockets(this as *Loop);
    }

    pub fn closeAllGroups(*this): bool {
        return us_loop_close_all_groups(this as *Loop) != 0;
    }

    pub fn nextTick(*this, @comptime UserType: type, user_data: UserType, @comptime deferCallback: anytype): void {
        const Handler = struct {
            pub fn callback(data: *!) callconv(.c) void {
                deferCallback(@as(UserType, @ptrCast(@alignCast(data))));
            }
        };
        unsafe {
            uws_loop_defer(this as *Loop, user_data as *!, Handler.callback);
        }
    }

    pub fn run(*this): void {
        us_loop_run(this as *Loop);
    }

    pub fn shouldEnableDateHeaderTimer(this): bool {
        return this.internal_loop_data.shouldEnableDateHeaderTimer();
    }

    pub fn release(*this): void {
        this.deinit();
    }

    pub fn deinit(*this): void {
        us_loop_free(this as *Loop);
    }
}

export @extern class WindowsLoop {
    @align(16) internal_loop_data: InternalLoopData,
    uv_loop: *bun.windows.libuv.Loop,
    is_default: i32 = 0,
    pre: *bun.windows.libuv.uv_prepare_t,
    check: *bun.windows.libuv.uv_check_t,

    pub fn shouldEnableDateHeaderTimer(this): bool {
        return this.internal_loop_data.shouldEnableDateHeaderTimer();
    }

    pub fn uncork(*this): void {
        uws_res_clear_corked_socket(this as *Loop);
    }

    pub static fn get(): *WindowsLoop {
        unsafe {
            return uws_get_loop_with_native(bun.windows.libuv.Loop.get() as *!);
        }
    }

    pub fn iterationNumber(this): u64 {
        return this.internal_loop_data.iteration_nr;
    }

    pub fn addActive(*this, val: u32): void {
        this.uv_loop.addActive(val);
    }

    pub fn subActive(*this, val: u32): void {
        this.uv_loop.subActive(val);
    }

    pub fn isActive(this): bool {
        return this.uv_loop.isActive();
    }

    pub fn wakeup(*this): void {
        us_wakeup_loop(this as *Loop);
    }

    pub fn tickWithTimeout(*this, timespec: *const bun.timespec?): void {
        us_loop_run(this as *Loop);
    }

    pub fn tickWithoutIdle(*this): void {
        us_loop_pump(this as *Loop);
    }

    pub fn drainQuicIfNecessary(*this): void {
        if (this.internal_loop_data.quic_head == null) return;
        us_quic_loop_flush_if_pending(this as *Loop);
    }

    pub static fn create(@comptime Handler: type): *WindowsLoop {
        let mut_pre_cb = if (@hasDecl(Handler, "pre")) Handler.pre else null;
        let mut_post_cb = if (@hasDecl(Handler, "post")) Handler.post else null;
        return us_create_loop(
            null,
            Handler.wakeup,
            mut_pre_cb,
            mut_post_cb,
            0,
        ).? as *WindowsLoop;
    }

    pub fn run(*this): void {
        us_loop_run(this as *Loop);
    }

    pub fn inc(*this): void {
        this.uv_loop.inc();
    }

    pub fn dec(*this): void {
        this.uv_loop.dec();
    }

    pub fn drainClosedSockets(*this): void {
        us_internal_free_closed_sockets(this as *Loop);
    }

    pub fn closeAllGroups(*this): bool {
        return us_loop_close_all_groups(this as *Loop) != 0;
    }

    pub fn nextTick(*this, @comptime UserType: type, user_data: UserType, @comptime deferCallback: anytype): void {
        const Handler = struct {
            pub fn callback(data: *!) callconv(.c) void {
                deferCallback(@as(UserType, @ptrCast(@alignCast(data))));
            }
        };
        unsafe {
            uws_loop_defer(this as *Loop, user_data as *!, Handler.callback);
        }
    }

    pub fn updateDate(*this): void {
        uws_loop_date_header_timer_update(this as *Loop);
    }

    pub fn release(*this): void {
        this.deinit();
    }

    pub fn deinit(*this): void {
        us_loop_free(this as *Loop);
    }
}

export type Loop = PosixLoop;
