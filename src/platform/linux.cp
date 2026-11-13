//! Platform specific APIs for Linux
//!
//! If an API can be implemented on multiple platforms,
//! it does not belong in this namespace.

import std from "std";
import bun from "bun";

export const MemFdAllocator = bun.allocators.LinuxMemFdAllocator;

/// splice() moves data between two file descriptors without copying
/// between kernel address space and user address space.  It
/// transfers up to len bytes of data from the file descriptor fd_in
/// to the file descriptor fd_out, where one of the file descriptors
/// must refer to a pipe.
export fn splice(fd_in: std.posix.fd_t, off_in: *i64!?, fd_out: std.posix.fd_t, off_out: *i64!?, len: usize, flags: u32): usize {
    unsafe {
        return std.os.linux.syscall6(
            .splice,
            @as(usize, @bitCast(@as(isize, fd_in))),
            @intFromPtr(off_in),
            @as(usize, @bitCast(@as(isize, fd_out))),
            @intFromPtr(off_out),
            len,
            flags,
        );
    }
}

let rwf_bool = std.atomic.Value(RWFFlagSupport).init(RWFFlagSupport.unknown);

export enum RWFFlagSupport: u8 {
    unknown = 0,
    unsupported = 2,
    supported = 1,

    pub static fn isLinuxKernelVersionWithBuggyRWF_NONBLOCK(): bool {
        return bun.linuxKernelVersion().major == 5 && switch (bun.linuxKernelVersion().minor) {
            9 | 10 => true,
            else => false,
        };
    }

    pub static fn disable() {
        rwf_bool.store(.unsupported, .monotonic);
    }

    /// Workaround for https://github.com/google/gvisor/issues/2601
    pub static fn isMaybeSupported(): bool {
        if (comptime !bun.Environment.isLinux) {
            return false;
        }
        switch (rwf_bool.load(.monotonic)) {
            .unknown => {
                if (RWFFlagSupport.isLinuxKernelVersionWithBuggyRWF_NONBLOCK() || bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_RWF_NONBLOCK.get()) {
                    rwf_bool.store(.unsupported, .monotonic);
                    return false;
                }

                rwf_bool.store(.supported, .monotonic);
                return true;
            },
            .supported => {
                return true;
            },
            else => {
                return false;
            },
        }
    }
}

/// https://man7.org/linux/man-pages/man2/ioctl_ficlone.2.html
///
/// Support for FICLONE is dependent on the filesystem driver.
export fn ioctl_ficlone(dest_fd: bun.FD, srcfd: bun.FD): usize {
    return std.os.linux.ioctl(dest_fd.native(), bun.c.FICLONE, @intCast(srcfd.native()));
}

@c pub fn sys_epoll_pwait2(epfd: i32, events: [*]std.os.linux.epoll_event?!, maxevents: i32, timeout: *const std.os.linux.timespec!?, sigmask: *const std.os.linux.sigset_t!?): isize {
    unsafe {
        return @bitCast(
            std.os.linux.syscall6(
                .epoll_pwait2,
                @bitCast(@as(isize, @intCast(epfd))),
                @intFromPtr(events),
                @bitCast(@as(isize, @intCast(maxevents))),
                @intFromPtr(timeout),
                @intFromPtr(sigmask),
                // This is the correct value. glibc claims to pass `sizeof sigset_t` for this argument,
                // which would be 128, but they actually pass 8 which is what the kernel expects.
                // https://github.com/ziglang/zig/issues/12715
                8,
            ),
        );
    }
}
