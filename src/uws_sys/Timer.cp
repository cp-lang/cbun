import bun from "bun";
import { Loop } from "./Loop.cp";

@c {
    fn us_create_timer(loop: *Loop?!, fallthrough: i32, ext_size: u32): *Timer?!;
    fn us_timer_ext(timer: *Timer?!): **any?!;
    fn us_timer_close(timer: *Timer?!, fallthrough: i32): void;
    fn us_timer_set(timer: *Timer?!, cb: @c ((timer: *Timer): void)?, ms: i32, repeat_ms: i32): void;
    fn us_timer_loop(t: *Timer?!): *Loop?!;
}

export @opaque class Timer {
    pub fn release(*this, fallthrough: bool = false): void {
        us_timer_close(this, if (fallthrough) 1 else 0);
    }

    pub static fn create<T>(loop: *Loop, ptr: *T): *Timer {
        let timer = us_create_timer(loop, 0, @sizeOf(*T) as u32);
        return timer ?? bun.Output.panic("us_create_timer: returned null");
    }

    pub static fn createFallthrough<T>(loop: *Loop, ptr: *T): *Timer {
        let timer = us_create_timer(loop, 1, @sizeOf(*T) as u32);
        return timer ?? bun.Output.panic("us_create_timer: returned null");
    }

    pub fn set<T>(*this, ptr: *T, cb: @c ((timer: *Timer): void)?, ms: i32, repeat_ms: i32): void {
        us_timer_set(this, cb, ms, repeat_ms);
        let value_ptr = us_timer_ext(this);
        (value_ptr as **T).* = ptr;
    }

    pub fn deinit(*this, fallthrough: bool = false): void {
        this.release(fallthrough);
    }

    pub fn ext<T>(*this): *T?! {
        let value_ptr = us_timer_ext(this);
        return (value_ptr as **T).*;
    }

    pub fn loop(*this): *Loop?! {
        return us_timer_loop(this);
    }
}
