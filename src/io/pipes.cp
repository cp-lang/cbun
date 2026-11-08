import bun from "bun";
const Async = bun.Async;
const Environment = bun.Environment;

zig {
    pub const PollOrFd = union(enum) {
        poll: *Async.FilePoll,
        fd: bun.FD,
        closed: void,

        pub fn setOwner(this: *const PollOrFd, owner: anytype) void {
            if (this.* == .poll) {
                this.poll.owner.set(owner);
            }
        }

        pub fn getFd(this: *const PollOrFd) bun.FD {
            return switch (this.*) {
                .closed => bun.invalid_fd,
                .fd => this.fd,
                .poll => this.poll.fd,
            };
        }

        pub fn getPoll(this: *const PollOrFd) ?*Async.FilePoll {
            return switch (this.*) {
                .closed => null,
                .fd => null,
                .poll => this.poll,
            };
        }

        pub fn closeImpl(this: *PollOrFd, ctx: ?*anyopaque, comptime onCloseFn: anytype, close_fd: bool) void {
            const fd = this.getFd();
            var close_async = true;
            if (this.* == .poll) {
                if (comptime Environment.isMac) {
                    if (this.poll.flags.contains(.poll_writable) and this.poll.flags.contains(.nonblocking)) {
                        close_async = false;
                    }
                }
                this.poll.deinitForceUnregister();
                this.* = .{ .closed = {} };
            }

            if (fd != bun.invalid_fd) {
                this.* = .{ .closed = {} };

                //TODO: We should make this call compatible using bun.FD
                if (Environment.isWindows) {
                    bun.Async.Closer.close(fd, bun.windows.libuv.Loop.get());
                } else if (close_async and close_fd) {
                    bun.Async.Closer.close(fd, {});
                } else {
                    if (close_fd) _ = fd.closeAllowingBadFileDescriptor(null);
                }
                if (comptime @TypeOf(onCloseFn) != void)
                    onCloseFn(@ptrCast(@alignCast(ctx.?)));
            } else {
                this.* = .{ .closed = {} };
            }
        }

        pub fn close(this: *PollOrFd, ctx: ?*anyopaque, comptime onCloseFn: anytype) void {
            this.closeImpl(ctx, onCloseFn, true);
        }
    };

    pub const FileType = enum {
        file,
        pipe,
        nonblocking_pipe,
        socket,

        pub fn isPollable(this: FileType) bool {
            return this == .pipe or this == .nonblocking_pipe or this == .socket;
        }

        pub fn isBlocking(this: FileType) bool {
            return this == .pipe;
        }
    };

    pub const ReadState = enum {
        /// The most common scenario
        /// Neither EOF nor EAGAIN
        progress,

        /// Received a 0-byte read
        eof,

        /// Received an EAGAIN
        drained,
    };
}
