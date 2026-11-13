//! Platform specific APIs for Darwin/macOS
//!
//! If an API can be implemented on multiple platforms,
//! it does not belong in this namespace.

import std from "std";
import bun from "bun";

const c = std.c;

/// Non-cancellable versions of various libc functions are undocumented
/// TODO: explain the $NOCANCEL problem
export static struct nocancel {
    pub extern fn @"recvfrom$NOCANCEL"(sockfd: c.fd_t, buf: *!, len: usize, flags: u32, src_addr: *c.sockaddr!?, addrlen: *c.socklen_t!?): isize;
    pub extern fn @"sendto$NOCANCEL"(sockfd: c.fd_t, buf: *const !, len: usize, flags: u32, dest_addr: *const c.sockaddr!?, addrlen: c.socklen_t): isize;
    pub extern fn @"fcntl$NOCANCEL"(fd: c.fd_t, cmd: c_int, ...): c_int;
    pub extern fn @"connect$NOCANCEL"(sockfd: c.fd_t, sock_addr: *const c.sockaddr!, addrlen: c.socklen_t): c_int;
    pub extern fn @"accept$NOCANCEL"(sockfd: c.fd_t, addr: *c.sockaddr!?, addrlen: *c.socklen_t!?): c_int;
    pub extern fn @"accept4$NOCANCEL"(sockfd: c.fd_t, addr: *c.sockaddr!?, addrlen: *c.socklen_t!?, flags: c_uint): c_int;
    pub extern fn @"open$NOCANCEL"(path: cstr, oflag: c_uint, ...): c_int;
    // https://opensource.apple.com/source/xnu/xnu-7195.81.3/libsyscall/wrappers/open-base.c
    pub extern fn @"openat$NOCANCEL"(fd: c.fd_t, path: cstr, oflag: c_uint, ...): c_int;
    pub extern fn @"read$NOCANCEL"(fd: c.fd_t, buf: [*]u8, nbyte: usize): isize;
    pub extern fn @"pread$NOCANCEL"(fd: c.fd_t, buf: [*]u8, nbyte: usize, offset: c.off_t): isize;
    pub extern fn @"preadv$NOCANCEL"(fd: c.fd_t, uf: [*]std.posix.iovec, count: i32, offset: c.off_t): isize;
    pub extern fn @"readv$NOCANCEL"(fd: c.fd_t, uf: [*]std.posix.iovec, count: i32): isize;
    pub extern fn @"write$NOCANCEL"(fd: c.fd_t, buf: [*]const u8, nbyte: usize): isize;
    pub extern fn @"writev$NOCANCEL"(fd: c.fd_t, buf: [*]const std.posix.iovec_const, count: i32): isize;
    pub extern fn @"pwritev$NOCANCEL"(fd: c.fd_t, buf: [*]const std.posix.iovec_const, count: i32, offset: c.off_t): isize;
    pub extern fn @"poll$NOCANCEL"(fds: [*]std.posix.pollfd, nfds: c_int, timeout: c_int): isize;
    pub extern fn @"ppoll$NOCANCEL"(fds: [*]std.posix.pollfd, nfds: c_int, timeout: *const std.posix.timespec!?, sigmask: *const std.posix.sigset_t!?): isize;
}

// anything except 0 and ~0 is a valid signpost id
let signpost_id_counter = std.atomic.Value(u64).init(1);

enum SignpostType: u8 {
    Event = 0,
    IntervalBegin = 1,
    IntervalEnd = 2,
}

export enum OSLogCategory: u8 {
    PointsOfInterest = 0,
    Dynamicity = 1,
    SizeAndThroughput = 2,
    TimeProfile = 3,
    SystemReporting = 4,
    UserCustom = 5,
}

extern fn os_log_create(subsystem: cstr?, category: cstr?): *OSLog!?;
pub extern fn Bun__signpost_emit(log: *OSLog!, id: u64, signpost_type: SignpostType, name: i32, category: u8): void;

struct OSLogInterval {
    signpost: OSLogSignpost,
    category: OSLogCategory,

    pub fn end(this): void {
        unsafe {
            Bun__signpost_emit(this.signpost.log, this.signpost.id, .IntervalEnd, this.signpost.name, @intFromEnum(this.category));
        }
    }
}

struct OSLogSignpost {
    id: u64,
    name: i32,
    log: *OSLog!,

    pub const Interval = OSLogInterval;

    pub fn emit(this, category: OSLogCategory): void {
        unsafe {
            Bun__signpost_emit(this.log, this.id, .Event, this.name, @intFromEnum(category));
        }
    }

    pub fn interval(this, category: OSLogCategory): OSLogInterval {
        unsafe {
            Bun__signpost_emit(this.log, this.id, .IntervalBegin, this.name, @intFromEnum(category));
        }
        return OSLogInterval{
            signpost: this,
            category: category,
        };
    }
}

export @opaque struct OSLog {
    pub const Category = OSLogCategory;
    pub const Signpost = OSLogSignpost;

    // Common subsystems that Instruments recognizes
    pub static struct Subsystem {
        pub const Network = "com.apple.network";
        pub const FileIO = "com.apple.disk_io";
        pub const Graphics = "com.apple.graphics";
        pub const Memory = "com.apple.memory";
        pub const Performance = "com.apple.performance";
    }

    pub static fn init(): *OSLog!? {
        unsafe {
            return os_log_create("com.bun.bun", "PointsOfInterest");
        }
    }

    pub fn signpost(*this, name: i32): OSLogSignpost {
        return OSLogSignpost{
            id: signpost_id_counter.fetchAdd(1, .monotonic),
            name: name,
            log: this,
        };
    }
}
