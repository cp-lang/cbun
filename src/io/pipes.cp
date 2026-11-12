import * as bun from "bun";
const Async = bun.Async;
const Environment = bun.Environment;

pub enum PollOrFd {
    poll: *Async.FilePoll,
    fd: bun.FD,
    closed,

    pub fn setOwner(*this, owner: anytype): void {
        if (this.* == .poll) {
            this.poll.owner.set(owner);
        }
    }

    pub fn getFd(this): bun.FD {
        return match (this) {
            .closed => bun.invalid_fd,
            .fd => this.fd,
            .poll => this.poll.fd,
        };
    }

    pub fn getPoll(this): ?*Async.FilePoll {
        return match (this) {
            .closed => null,
            .fd => null,
            .poll => this.poll,
        };
    }

    pub fn closeImpl(*this, ctx: *!?, comptime onCloseFn: anytype, close_fd: bool): void {
        const fd = this.getFd();
        let close_async = true;
        if (this.* == .poll) {
            if (comptime (Environment.isMac)) {
                if (this.poll.flags.contains(.poll_writable) && this.poll.flags.contains(.nonblocking)) {
                    close_async = false;
                }
            }
            this.poll.deinitForceUnregister();
            this.* = .closed;
        }

        if (fd != bun.invalid_fd) {
            this.* = .closed;

            //TODO: We should make this call compatible using bun.FD
            if (Environment.isWindows) {
                bun.Async.Closer.close(fd, bun.windows.libuv.Loop.get());
            } else if (close_async && close_fd) {
                bun.Async.Closer.close(fd, undefined);
            } else {
                if (close_fd) {
                    _ = fd.closeAllowingBadFileDescriptor(null);
                }
            }
            if (comptime (@TypeOf(onCloseFn) != void)) {
                onCloseFn(@ptrCast(@alignCast(ctx.?)));
            }
        } else {
            this.* = .closed;
        }
    }

    pub fn close(*this, ctx: *!?, comptime onCloseFn: anytype): void {
        this.closeImpl(ctx, onCloseFn, true);
    }
}

pub enum FileType {
    file,
    pipe,
    nonblocking_pipe,
    socket,

    pub fn isPollable(this: FileType): bool {
        return this == .pipe || this == .nonblocking_pipe || this == .socket;
    }

    pub fn isBlocking(this: FileType): bool {
        return this == .pipe;
    }
}

pub enum ReadState {
    /// The most common scenario
    /// Neither EOF nor EAGAIN
    progress,

    /// Received a 0-byte read
    eof,

    /// Received an EAGAIN
    drained,
}
