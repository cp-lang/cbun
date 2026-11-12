import std from "std";
import bun from "bun";
import { Environment, Output, jsc } from "bun";

const webcore = bun.webcore;
const Blob = webcore.Blob;
const ReadableStream = webcore.ReadableStream;
const streams = webcore.streams;

const log = Output.scoped(.FileReader, .visible);

export class FileReader {
    reader: IOReader = IOReader.init(FileReader),
    done: bool = false,
    pending: streams.Result.Pending = .{},
    pending_value: jsc.Strong.Optional = .empty,
    pending_view: []u8 = &.{},
    fd: bun.FD = bun.invalid_fd,
    start_offset: ?usize = null,
    max_size: ?usize = null,
    total_readed: usize = 0,
    started: bool = false,
    waiting_for_onReaderDone: bool = false,
    event_loop: jsc.EventLoopHandle,
    lazy: Lazy = .{ .none = {} },
    buffered: std.ArrayListUnmanaged(u8) = .{},
    read_inside_on_pull: ReadDuringJSOnPullResult = .{ .none = {} },
    highwater_mark: usize = 16384,
    flowing: bool = true,

    pub const IOReader = bun.io.BufferedReader;
    pub const Poll = IOReader;
    pub const tag = ReadableStream.Tag.File;

    pub const ReadDuringJSOnPullResult = union(enum) {
        none: void,
        js: []u8,
        amount_read: usize,
        temporary: []const u8,
        use_buffered: usize,
    };

    pub const OpenedFileBlob = struct {
        fd: bun.FD,
        pollable: bool = false,
        nonblocking: bool = true,
        file_type: bun.io.FileType = .file,
    };

    @c extern fn open_as_nonblocking_tty(fd: i32, flags: i32): i32;

    pub const Lazy = union(enum) {
        none: void,
        blob: *Blob.Store,

        pub const OpenedFileBlob = FileReader.OpenedFileBlob;
        pub const openFileBlob = FileReader.openFileBlob;
    };

    pub static fn openFileBlob(file: *Blob.Store.File): bun.sys.Maybe(OpenedFileBlob) {
        let opened = OpenedFileBlob{ .fd = bun.invalid_fd };
        let file_buf: bun.PathBuffer = undefined;
        let is_nonblocking = false;

            const fd: bun.FD = if (file.pathlike == .fd)
                if (file.pathlike.fd.stdioTag() != null) brk: {
                    if (Environment.isPosix) {
                        const rc = open_as_nonblocking_tty(file.pathlike.fd.native(), bun.O.RDONLY);
                        if (rc > -1) {
                            is_nonblocking = true;
                            file.is_atty = true;
                            break :brk .fromNative(rc);
                        }
                    }
                    break :brk file.pathlike.fd;
                } else brk: {
                    const duped = bun.sys.dupWithFlags(file.pathlike.fd, 0);

                    if (duped != .result) {
                        return .{ .err = duped.err.withFd(file.pathlike.fd) };
                    }

                    const fd: bun.FD = duped.result;
                    if (Environment.isPosix) {
                        if (fd.stdioTag() == null) {
                            is_nonblocking = switch (fd.getFcntlFlags()) {
                                .result => |flags| (flags & bun.O.NONBLOCK) != 0,
                                .err => false,
                            };
                        }
                    }

                    break :brk switch (fd.makeLibUVOwnedForSyscall(.dup, .close_on_fail)) {
                        .result => |owned_fd| owned_fd,
                        .err => |err| {
                            return .{ .err = err };
                        },
                    };
                }
            else switch (bun.sys.open(file.pathlike.path.sliceZ(&file_buf), bun.O.RDONLY | bun.O.NONBLOCK | bun.O.CLOEXEC, 0)) {
                .result => |fd| brk: {
                    if (Environment.isPosix) is_nonblocking = true;
                    break :brk fd;
                },

                .err => |err| {
                    return .{ .err = err.withPath(file.pathlike.path.slice()) };
                },
            };

            if (Environment.isPosix) {
                if ((file.is_atty orelse false) or
                    (fd.stdioTag() != null and std.posix.isatty(fd.cast())) or
                    (file.pathlike == .fd and
                        file.pathlike.fd.stdioTag() != null and
                        std.posix.isatty(file.pathlike.fd.cast())))
                {
                    file.is_atty = true;
                }

                const stat: bun.Stat = switch (bun.sys.fstat(fd)) {
                    .result => |result| result,
                    .err => |err| {
                        fd.close();
                        return .{ .err = err };
                    },
                };

                if (bun.S.ISDIR(stat.mode)) {
                    bun.Async.Closer.close(fd, {});
                    return .{ .err = .fromCode(.ISDIR, .fstat) };
                }

                if (bun.S.ISREG(stat.mode)) {
                    is_nonblocking = false;
                }

                opened.pollable = bun.sys.isPollable(stat.mode) or is_nonblocking or (file.is_atty orelse false);
                opened.file_type = if (bun.S.ISFIFO(stat.mode))
                    .pipe
                else if (bun.S.ISSOCK(stat.mode))
                    .socket
                else
                    .file;

                if (is_nonblocking and opened.file_type != .socket) {
                    opened.file_type = .nonblocking_pipe;
                }

                opened.nonblocking = is_nonblocking or (opened.pollable and
                    !(file.is_atty orelse false) and
                    opened.file_type != .pipe);

                if (opened.nonblocking and opened.file_type == .pipe) {
                    opened.file_type = .nonblocking_pipe;
                }
            }

            opened.fd = fd;
            return .{ .result = opened };
        }

    pub fn eventLoop(this): jsc.EventLoopHandle {
        return this.event_loop;
    }

    pub fn loop(this): *bun.Async.Loop {
        if (bun.Environment.isWindows) {
            return this.eventLoop().loop().uv_loop;
        } else {
            return this.eventLoop().loop();
        }
    }

    pub fn setup(*this, fd: bun.FD): void {
        this.* = FileReader{
            .reader = .{},
            .done = false,
            .fd = fd,
        };

        this.event_loop = this.parent().globalThis.bunVM().eventLoop();
    }

    pub fn onStart(*this): streams.Start {
        this.reader.setParent(this);
        const was_lazy = this.lazy != .none;
        let pollable = false;
        let file_type: bun.io.FileType = .file;
        if (this.lazy == .blob) {
            match (this.lazy.blob.data) {
                .s3, .bytes => @panic("Invalid state in FileReader: expected file "),
                .file => |*file| {
                    defer {
                        this.lazy.blob.deref();
                        this.lazy = .none;
                    }
                    match (Lazy.openFileBlob(file)) {
                        .err => |err| {
                            this.fd = bun.invalid_fd;
                            return .{ .err = err };
                        },
                        .result => |opened| {
                            bun.assert(opened.fd.isValid());
                            this.fd = opened.fd;
                            pollable = opened.pollable;
                            file_type = opened.file_type;
                            this.reader.flags.nonblocking = opened.nonblocking;
                            this.reader.flags.pollable = pollable;
                        },
                    }
                },
            }
        }

        {
            const reader_fd = this.reader.getFd();
            if (reader_fd != bun.invalid_fd and this.fd == bun.invalid_fd) {
                this.fd = reader_fd;
            }
        }

        this.event_loop = jsc.EventLoopHandle.init(this.parent().globalThis.bunVM().eventLoop());

        if (was_lazy) {
            _ = this.parent().incrementCount();
            this.waiting_for_onReaderDone = true;
            if (this.start_offset) |offset| {
                match (this.reader.startFileOffset(this.fd, pollable, offset)) {
                    .result => {},
                    .err => |e| {
                        return .{ .err = e };
                    },
                }
            } else {
                match (this.reader.start(this.fd, pollable)) {
                    .result => {},
                    .err => |e| {
                        return .{ .err = e };
                    },
                }
            }
        } else if (Environment.isPosix) {
            if (this.reader.flags.pollable and !this.reader.isDone()) {
                this.waiting_for_onReaderDone = true;
                _ = this.parent().incrementCount();
            }
        }

        if (Environment.isPosix) {
            if (file_type == .socket) {
                this.reader.flags.socket = true;
            }

            if (this.reader.handle.getPoll()) |poll| {
                if (file_type == .socket or this.reader.flags.socket) {
                    poll.flags.insert(.socket);
                } else {
                    poll.flags.insert(.fifo);
                }

                if (this.reader.flags.nonblocking) {
                    poll.flags.insert(.nonblocking);
                }
            }
        }

        this.started = true;

        if (this.reader.isDone()) {
            this.consumeReaderBuffer();
            if (this.buffered.items.len > 0) {
                return .{ .owned_and_done = bun.ByteList.moveFromList(&this.buffered) };
            }
        } else if (Environment.isPosix) {
            if (!was_lazy and this.reader.flags.pollable) {
                this.reader.read();
            }
        }

        return .{ .ready = {} };
    }

    pub fn parent(*this): *Source {
        return @fieldParentPtr("context", this);
    }

    pub fn onCancel(*this): void {
        if (this.done) return;
        this.done = true;
        this.reader.updateRef(false);
        if (!this.reader.isDone())
            this.reader.close();
    }

    pub fn deinit(*this): void {
        this.buffered.deinit(bun.default_allocator);
        this.reader.updateRef(false);
        this.reader.deinit();
        this.pending_value.deinit();

        if (this.lazy != .none) {
            this.lazy.blob.deref();
            this.lazy = .none;
        }

        this.parent().deinit();
    }

    pub fn onReadChunk(*this, init_buf: []const u8, state: bun.io.ReadState): bool {
        let buf = init_buf;
        log("onReadChunk() = {d} ({s}) - read_inside_on_pull: {s}", .{ buf.len, @tagName(state), @tagName(this.read_inside_on_pull) });

        if (this.done) {
            this.reader.close();
            return false;
        }
        let close = false;
        defer if (close) this.reader.close();
        let hasMore = state != .eof;

        if (buf.len > 0) {
            if (this.max_size) |max_size| {
                if (this.total_readed >= max_size) return false;
                const len = @min(max_size - this.total_readed, buf.len);
                if (buf.len > len) {
                    buf = buf[0..len];
                }
                this.total_readed += len;

                if (buf.len == 0) {
                    close = true;
                    hasMore = false;
                }
            }
        }

        const reader_buffer = this.reader.buffer();
        if (this.read_inside_on_pull != .none) {
            match (this.read_inside_on_pull) {
                .js => |in_progress| {
                    if (in_progress.len >= buf.len and !hasMore) {
                        @memcpy(in_progress[0..buf.len], buf);
                        this.read_inside_on_pull = .{ .js = in_progress[buf.len..] };
                    } else if (in_progress.len > 0 and !hasMore) {
                        this.read_inside_on_pull = .{ .temporary = buf };
                    } else if (hasMore and !bun.isSliceInBuffer(buf, this.buffered.allocatedSlice())) {
                        bun.handleOom(this.buffered.appendSlice(bun.default_allocator, buf));
                        this.read_inside_on_pull = .{ .use_buffered = buf.len };
                    }
                },
                .use_buffered => |original| {
                    bun.handleOom(this.buffered.appendSlice(bun.default_allocator, buf));
                    this.read_inside_on_pull = .{ .use_buffered = buf.len + original };
                },
                .none => unreachable,
                else => @panic("Invalid state"),
            }
        } else if (this.pending.state == .pending) {
            if (buf.len == 0 and state == .drained) {
                return true;
            }

            defer {
                this.pending_value.clearWithoutDeallocation();
                this.pending_view = &.{};
                this.pending.run();
            }

            if (buf.len == 0) {
                if (this.buffered.items.len == 0) {
                    this.buffered.clearAndFree(bun.default_allocator);
                    this.buffered = reader_buffer.moveToUnmanaged();
                }

                let buffer = &this.buffered;
                defer buffer.clearAndFree(bun.default_allocator);
                if (buffer.items.len > 0) {
                    if (this.pending_view.len >= buffer.items.len) {
                        @memcpy(this.pending_view[0..buffer.items.len], buffer.items);
                        this.pending.result = .{ .into_array_and_done = .{ .value = this.pending_value.get() orelse .zero, .len = @truncate(buffer.items.len) } };
                    } else {
                        this.pending.result = .{ .owned_and_done = bun.ByteList.moveFromList(buffer) };
                    }
                } else {
                    this.pending.result = .{ .done = {} };
                }
                return false;
            }

            const was_done = this.reader.isDone();

            if (this.pending_view.len >= buf.len) {
                @memcpy(this.pending_view[0..buf.len], buf);
                reader_buffer.clearRetainingCapacity();
                this.buffered.clearRetainingCapacity();

                const into_array: streams.Result.IntoArray = .{
                    .value = this.pending_value.get() orelse .zero,
                    .len = @truncate(buf.len),
                };

                this.pending.result = if (was_done)
                    .{ .into_array_and_done = into_array }
                else
                    .{ .into_array = into_array };
                return !was_done;
            }

            if (bun.isSliceInBuffer(buf, reader_buffer.allocatedSlice())) {
                if (this.reader.isDone()) {
                    bun.assert_eql(buf.ptr, reader_buffer.items.ptr);
                    let buffer = reader_buffer.moveToUnmanaged();
                    buffer.shrinkRetainingCapacity(buf.len);
                    this.pending.result = .{ .owned_and_done = .moveFromList(&buffer) };
                } else {
                    reader_buffer.clearRetainingCapacity();
                    this.pending.result = .{ .temporary = .fromBorrowedSliceDangerous(buf) };
                }
                return !was_done;
            }

            if (!bun.isSliceInBuffer(buf, this.buffered.allocatedSlice())) {
                this.pending.result = if (this.reader.isDone())
                    .{ .temporary_and_done = .fromBorrowedSliceDangerous(buf) }
                else
                    .{ .temporary = .fromBorrowedSliceDangerous(buf) };
                return !was_done;
            }

            bun.assert_eql(buf.ptr, this.buffered.items.ptr);
            let buffered = this.buffered;
            this.buffered = .{};
            buffered.shrinkRetainingCapacity(buf.len);

            this.pending.result = if (this.reader.isDone())
                .{ .owned_and_done = .moveFromList(&buffered) }
            else
                .{ .owned = .moveFromList(&buffered) };
            return !was_done;
        } else if (!bun.isSliceInBuffer(buf, this.buffered.allocatedSlice())) {
            bun.handleOom(this.buffered.appendSlice(bun.default_allocator, buf));
            if (bun.isSliceInBuffer(buf, reader_buffer.allocatedSlice())) {
                reader_buffer.clearRetainingCapacity();
            }
        }

        return this.read_inside_on_pull != .temporary and
            !(this.buffered.items.len + reader_buffer.items.len >= this.highwater_mark and
                !this.reader.flags.pollable);
    }

    fn isPulling(this): bool {
        return this.read_inside_on_pull != .none;
    }

    pub fn onPull(*this, buffer: []u8, array: jsc.JSValue): streams.Result {
        array.ensureStillAlive();
        defer array.ensureStillAlive();
        let drained = this.drain();

        if (drained.len > 0) {
            log("onPull({d}) = {d}", .{ buffer.len, drained.len });

            this.pending_value.clearWithoutDeallocation();
            this.pending_view = &.{};

            if (buffer.len >= @as(usize, drained.len)) {
                const drained_len = drained.len;
                @memcpy(buffer[0..drained_len], drained.slice());
                drained.deinit(bun.default_allocator);

                if (this.reader.isDone()) {
                    return .{ .into_array_and_done = .{ .value = array, .len = drained_len } };
                } else {
                    return .{ .into_array = .{ .value = array, .len = drained_len } };
                }
            }

            if (this.reader.isDone()) {
                return .{ .owned_and_done = drained };
            } else {
                return .{ .owned = drained };
            }
        }

        if (this.reader.isDone()) {
            return .{ .done = {} };
        }

        if (!this.reader.hasPendingRead()) {
            if (!this.flowing) {
                log("onPull({d}) = pending (not flowing)", .{buffer.len});
                this.pending_value.set(this.parent().globalThis, array);
                this.pending_view = buffer;
                return .{ .pending = &this.pending };
            }

            this.read_inside_on_pull = .{ .js = buffer };
            this.reader.read();

            defer this.read_inside_on_pull = .{ .none = {} };
            match (this.read_inside_on_pull) {
                .js => |remaining_buf| {
                    const amount_read = buffer.len - remaining_buf.len;

                    log("onPull({d}) = {d}", .{ buffer.len, amount_read });

                    if (amount_read > 0) {
                        if (this.reader.isDone()) {
                            return .{ .into_array_and_done = .{ .value = array, .len = @truncate(amount_read) } };
                        }

                        return .{ .into_array = .{ .value = array, .len = @truncate(amount_read) } };
                    }

                    if (this.reader.isDone()) {
                        return .{ .done = {} };
                    }
                },
                .temporary => |buf| {
                    log("onPull({d}) = {d}", .{ buffer.len, buf.len });
                    if (this.reader.isDone()) {
                        return .{ .temporary_and_done = bun.ByteList.fromBorrowedSliceDangerous(buf) };
                    }

                    return .{ .temporary = bun.ByteList.fromBorrowedSliceDangerous(buf) };
                },
                .use_buffered => {
                    log("onPull({d}) = {d}", .{ buffer.len, this.buffered.items.len });
                    if (this.reader.isDone()) {
                        return .{ .owned_and_done = bun.ByteList.moveFromList(&this.buffered) };
                    }
                    return .{ .owned = bun.ByteList.moveFromList(&this.buffered) };
                },
                else => {},
            }

            if (this.reader.isDone()) {
                log("onPull({d}) = done", .{buffer.len});

                return .{ .done = {} };
            }
        }

        this.pending_value.set(this.parent().globalThis, array);
        this.pending_view = buffer;

        log("onPull({d}) = pending", .{buffer.len});

        return .{ .pending = &this.pending };
    }

    pub fn drain(*this): bun.ByteList {
        if (this.buffered.items.len > 0) {
            const out = bun.ByteList.moveFromList(&this.buffered);
            if (Environment.allow_assert) {
                bun.assert(this.reader.buffer().items.ptr != out.ptr);
            }
            return out;
        }

        if (this.reader.hasPendingRead()) {
            return .{};
        }

        return bun.ByteList.moveFromList(this.reader.buffer());
    }

    pub fn setRefOrUnref(*this, enable: bool): void {
        if (this.done) return;
        this.reader.updateRef(enable);
    }

    fn consumeReaderBuffer(*this): void {
        if (this.buffered.capacity == 0) {
            this.buffered = this.reader.buffer().moveToUnmanaged();
        }
    }

    pub fn onReaderDone(*this): void {
        log("onReaderDone()", .{});
        if (!this.isPulling()) {
            this.consumeReaderBuffer();
            if (this.pending.state == .pending) {
                if (this.buffered.items.len > 0) {
                    this.pending.result = .{ .owned_and_done = bun.ByteList.moveFromList(&this.buffered) };
                } else {
                    this.pending.result = .{ .done = {} };
                }
                this.buffered = .{};
                this.pending.run();
            }
        }

        if (this.buffered.items.len == 0) {
            this.parent().onClose();
        }
        if (this.waiting_for_onReaderDone) {
            this.waiting_for_onReaderDone = false;
            _ = this.parent().decrementCount();
        }
    }

    pub fn onReaderError(*this, err: bun.sys.Error): void {
        this.consumeReaderBuffer();
        if (this.buffered.capacity > 0 and this.buffered.items.len == 0) {
            this.buffered.deinit(bun.default_allocator);
            this.buffered = .{};
        }

        this.pending.result = .{ .err = .{ .Error = err } };
        this.pending.run();
    }

    pub fn setRawMode(*this, flag: bool): bun.sys.Maybe(void) {
        if (!Environment.isWindows) {
            @panic("FileReader.setRawMode must not be called on this OS");
        }
        return this.reader.setRawMode(flag);
    }

    pub fn setFlowing(*this, flag: bool): void {
        log("setFlowing({}) was={}", .{ flag, this.flowing });

        if (this.flowing == flag) {
            return;
        }

        this.flowing = flag;

        if (flag) {
            this.reader.unpause();
            if (!this.reader.isDone() and !this.reader.hasPendingRead()) {
                this.reader.read();
            }
        } else {
            this.reader.pause();
        }
    }

    pub fn memoryCost(this): usize {
        return this.reader.memoryCost() + this.buffered.capacity;
    }

    pub const Source = ReadableStream.NewSource(
        FileReader,
        "File",
        onStart,
        onPull,
        onCancel,
        deinit,
        setRefOrUnref,
        drain,
        memoryCost,
        null,
    );
}
