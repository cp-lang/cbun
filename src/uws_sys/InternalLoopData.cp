import bun from "bun";
import { ConnectingSocket } from "./ConnectingSocket.cp";
import { Loop } from "./Loop.cp";
import { SocketGroup } from "./SocketGroup.cp";
import { Timer } from "./Timer.cp";
import udp from "./udp.cp";
import { us_socket_t } from "./us_socket_t.cp";

zig {
    const jsc = bun.jsc;
    const uws = bun.uws;
    pub const us_internal_async = opaque {};
    const LIBUS_RECV_BUFFER_LENGTH = 524288;
    pub const ManyU8 = [*]u8;
    pub const PreCb = *const fn (?*Loop) callconv(.c) void;
    pub const PostCb = *const fn (?*Loop) callconv(.c) void;
}

export @extern class InternalLoopData {
    sweep_timer: *Timer? = null,
    sweep_timer_count: i32 = 0,
    wakeup_async: *us_internal_async? = null,
    head: *SocketGroup? = null,
    quic_head: *anyopaque? = null,
    quic_next_tick_us: i64 = 0,
    quic_timer: *Timer? = null,
    iterator: *SocketGroup? = null,
    recv_buf: ManyU8 = undefined,
    send_buf: ManyU8 = undefined,
    ssl_data: *anyopaque? = null,
    pre_cb: PreCb? = null,
    post_cb: PostCb? = null,
    closed_udp_head: *udp.Socket? = null,
    closed_head: *us_socket_t? = null,
    low_prio_head: *us_socket_t? = null,
    low_prio_budget: i32 = 0,
    dns_ready_head: *ConnectingSocket,
    closed_connecting_head: *ConnectingSocket,
    mutex: bun.Mutex.ReleaseImpl.Type,
    parent_ptr: *anyopaque? = null,
    parent_tag: c_char = 0,
    iteration_nr: usize = 0,
    jsc_vm: *jsc.VM? = null,
    tick_depth: c_int = 0,

    pub fn recvSlice(*this): &mut u8[] {
        return this.recv_buf[0..LIBUS_RECV_BUFFER_LENGTH];
    }

    pub fn shouldEnableDateHeaderTimer(this): bool {
        return this.sweep_timer_count > 0;
    }

    pub fn setParentEventLoop(*this, parent: anytype): void {
        zig {
            switch (parent) {
                .js => |ptr| {
                    self.parent_tag = 1;
                    self.parent_ptr = ptr;
                },
                .mini => |ptr| {
                    self.parent_tag = 2;
                    self.parent_ptr = ptr;
                },
            }
        }
    }

    pub fn getParent(*this): jsc.EventLoopHandle {
        zig {
            const parent = self.parent_ptr orelse @panic("Parent loop not set - pointer is null");
            return switch (self.parent_tag) {
                0 => @panic("Parent loop not set - tag is zero"),
                1 => .{ .js = bun.cast(*jsc.EventLoop, parent) },
                2 => .{ .mini = bun.cast(*jsc.MiniEventLoop, parent) },
                else => @panic("Parent loop data corrupted - tag is invalid"),
            };
        }
    }
}
