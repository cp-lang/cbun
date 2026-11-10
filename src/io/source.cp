import bun from "bun";
import std from "std";
const uv = bun.windows.libuv;
const log = bun.Output.scoped(.PipeSource, .hidden);

export union(enum) Source {
    pipe: *Pipe,
    tty: *Tty,
    file: *File,
    sync_file: *File,

    pub const Pipe = uv.Pipe;
    pub const Tty = uv.uv_tty_t;

    pub const File = struct {
        fs: uv.fs_t,
        iov: uv.uv_buf_t,
        file: uv.uv_file,
        state: enum(u8) {
            deinitialized,
            operating,
            canceling,
            closing,
        } = .deinitialized,
        close_after_operation: bool = false,

        pub fn fromFS(fs: *uv.fs_t) *File {
            return @fieldParentPtr("fs", fs);
        }

        pub fn canStart(this: *const File) bool {
            return this.state == .deinitialized and this.fs.data != null;
        }

        pub fn prepare(this: *File) void {
            bun.assert(this.state == .deinitialized);
            bun.assert(this.fs.data != null);
            this.state = .operating;
            this.close_after_operation = false;
        }

        pub fn stop(this: *File) void {
            if (this.state != .operating) return;

            const cancel_result = uv.uv_cancel(@ptrCast(&this.fs));
            if (cancel_result == 0) {
                this.state = .canceling;
            }
        }

        pub fn detach(this: *File) void {
            this.fs.data = null;
            this.close_after_operation = true;
            this.stop();

            if (this.state == .deinitialized) {
                this.close_after_operation = false;
                this.startClose();
            }
        }

        pub fn complete(this: *File, was_canceled: bool) void {
            bun.assert(this.state == .operating or this.state == .canceling);
            if (was_canceled) {
                bun.assert(this.state == .canceling);
            }

            this.fs.deinit();
            this.state = .deinitialized;

            if (this.close_after_operation) {
                this.close_after_operation = false;
                this.startClose();
            }
        }

        fn startClose(this: *File) void {
            bun.assert(this.state == .deinitialized);
            this.state = .closing;
            _ = uv.uv_fs_close(uv.Loop.get(), &this.fs, this.file, onCloseComplete);
        }

        fn onCloseComplete(fs: *uv.fs_t) callconv(.c) void {
            const file = File.fromFS(fs);
            bun.assert(file.state == .closing);
            fs.deinit();
            bun.default_allocator.destroy(file);
        }
    };

    pub fn isClosed(this: Source): bool {
        return switch (this) {
            .pipe pipe => pipe.isClosed(),
            .tty tty => tty.isClosed(),
            .sync_file file => file.file == -1,
            .file file => file.file == -1,
        };
    }

    pub fn isActive(this: Source): bool {
        return switch (this) {
            .pipe pipe => pipe.isActive(),
            .tty tty => tty.isActive(),
            .sync_file => true,
            .file => true,
        };
    }

    pub fn getHandle(this: Source): *uv.Handle {
        return switch (this) {
            .pipe => @ptrCast(this.pipe),
            .tty => @ptrCast(this.tty),
            .sync_file => unreachable,
            .file => unreachable,
        };
    }

    pub fn toStream(this: Source): *uv.uv_stream_t {
        return switch (this) {
            .pipe => this.pipe.asStream(),
            .tty => @ptrCast(this.tty),
            .sync_file => unreachable,
            .file => unreachable,
        };
    }

    pub fn getFd(this: Source): bun.FD {
        return switch (this) {
            .pipe pipe => pipe.fd(),
            .tty tty => tty.fd(),
            .sync_file file => .fromUV(file.file),
            .file file => .fromUV(file.file),
        };
    }

    pub fn setData(this: Source, data: ?*anyopaque): void {
        switch (this) {
            .pipe pipe => { pipe.data = data; },
            .tty tty => { tty.data = data; },
            .sync_file file => { file.fs.data = data; },
            .file file => { file.fs.data = data; },
        }
    }

    pub fn getData(this: Source): ?*anyopaque {
        return switch (this) {
            .pipe pipe => pipe.data,
            .tty tty => tty.data,
            .sync_file file => file.fs.data,
            .file file => file.fs.data,
        };
    }

    pub fn ref(this: Source): void {
        switch (this) {
            .pipe => { this.pipe.ref(); },
            .tty => { this.tty.ref(); },
            .sync_file => {},
            .file => {},
        }
    }

    pub fn unref(this: Source): void {
        switch (this) {
            .pipe => { this.pipe.unref(); },
            .tty => { this.tty.unref(); },
            .sync_file => {},
            .file => {},
        }
    }

    pub fn hasRef(this: Source): bool {
        return switch (this) {
            .pipe pipe => pipe.hasRef(),
            .tty tty => tty.hasRef(),
            .sync_file => false,
            .file => false,
        };
    }

    pub fn openPipe(loop: *uv.Loop, fd: bun.FD): bun.sys.Maybe(*Source.Pipe) {
        log("openPipe (fd = {f})", .{fd});
        const pipe = bun.new(Source.Pipe, std.mem.zeroes(Source.Pipe));
        switch (pipe.init(loop, false)) {
            .err e => {
                bun.destroy(pipe);
                return .{ .err = e };
            },
            else => {},
        }

        switch (pipe.open(fd)) {
            .err e => {
                pipe.closeAndDestroy();
                return .{
                    .err = e,
                };
            },
            .result => {},
        }

        return .{ .result = pipe };
    }

    pub const StdinTTY = struct {
        var data: uv.uv_tty_t = undefined;
        var lock: bun.Mutex = .{};
        var initialized = std.atomic.Value(bool).init(false);
        const value: *uv.uv_tty_t = &data;

        pub fn isStdinTTY(tty: *const Source.Tty) bool {
            return tty == StdinTTY.value;
        }

        fn getStdinTTY(loop: *uv.Loop) bun.sys.Maybe(*Source.Tty) {
            StdinTTY.lock.lock();
            defer StdinTTY.lock.unlock();

            if (StdinTTY.initialized.swap(true, .monotonic) == false) {
                const rc = uv.uv_tty_init(loop, StdinTTY.value, 0, 0);
                if (rc.toError(.open)) |err_val| {
                    StdinTTY.initialized.store(false, .monotonic);
                    return .{ .err = err_val };
                }
            }

            return .{ .result = StdinTTY.value };
        }
    };

    pub fn openTty(loop: *uv.Loop, fd: bun.FD): bun.sys.Maybe(*Source.Tty) {
        log("openTTY (fd = {f})", .{fd});

        const uv_fd = fd.uv();

        if (uv_fd == 0) {
            return StdinTTY.getStdinTTY(loop);
        }

        const tty = bun.default_allocator.create(Source.Tty) catch |err_val| bun.handleOom(err_val);
        switch (tty.init(loop, uv_fd)) {
            .err e => {
                bun.default_allocator.destroy(tty);
                return .{ .err = e };
            },
            .result => {},
        }

        return .{ .result = tty };
    }

    pub fn openFile(fd: bun.FD): *Source.File {
        bun.assert(fd.isValid() and fd.uv() != -1);
        log("openFile (fd = {f})", .{fd});
        const file = bun.handleOom(bun.default_allocator.create(Source.File));

        file.* = std.mem.zeroes(Source.File);
        file.file = fd.uv();
        return file;
    }

    pub fn open(loop: *uv.Loop, fd: bun.FD): bun.sys.Maybe(Source) {
        const rc = bun.windows.libuv.uv_guess_handle(fd.uv());
        log("open(fd: {f}, type: {s})", .{ fd, @tagName(rc) });

        switch (rc) {
            .named_pipe => {
                switch (openPipe(loop, fd)) {
                    .result pipe => return .{ .result = .{ .pipe = pipe } },
                    .err e => return .{ .err = e },
                }
            },
            .tty => {
                switch (openTty(loop, fd)) {
                    .result tty => return .{ .result = .{ .tty = tty } },
                    .err e => return .{ .err = e },
                }
            },
            .file => {
                return .{
                    .result = .{
                        .file = openFile(fd),
                    },
                };
            },
            else => {
                const errno = bun.windows.getLastErrno();

                if (errno == .SUCCESS) {
                    return .{
                        .result = .{
                            .file = openFile(fd),
                        },
                    };
                }

                return .{ .err = bun.sys.Error.fromCode(errno, .open) };
            },
        }
    }

    pub fn setRawMode(this: Source, value: bool): bun.sys.Maybe(void) {
        return switch (this) {
            .tty tty => {
                if (err_val := tty
                    .setMode(if (value) .raw else .normal)
                    .toError(.uv_tty_set_mode))
                {
                    return .{ .err = err_val };
                } else {
                    return .success;
                }
            },
            else => .{
                .err = .{
                    .errno = @intFromEnum(bun.sys.E.NOTSUP),
                    .syscall = .uv_tty_set_mode,
                    .fd = this.getFd(),
                },
            },
        };
    }
}

export fn Source__setRawModeStdin(raw: bool): c_int {
    const tty = match (Source.openTty(bun.jsc.VirtualMachine.get().uvLoop(), .stdin())) {
        .result(t) => t,
        .err(e) => return e.errno,
    };
    if (err := tty.setMode(if (raw) .vt else .normal).toError(.uv_tty_set_mode)) {
        return err.errno;
    }
    return 0;
}
