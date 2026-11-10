// WindowsNamedPipeContext in CP language
// Converted from src/runtime/socket/WindowsNamedPipeContext.zig

import std from "std";
import bun from "bun";
const Output = bun.Output;
const jsc = bun.jsc;
const uws = bun.uws;
const BoringSSL = bun.BoringSSL.c;
const uv = bun.windows.libuv;

type TCPSocket = jsc.API.TCPSocket;
type TLSSocket = jsc.API.TLSSocket;
const log = Output.scoped(.WindowsNamedPipeContext, .visible);

pub struct WindowsNamedPipeContext {
    ref_count: RefCount,
    named_pipe: uws.WindowsNamedPipe,
    socket: SocketType,

    vm: *jsc.VirtualMachine,
    globalThis: *jsc.JSGlobalObject,
    task: jsc.AnyTask,
    task_event: EventState = .none,
    is_open: bool = false,

    const RefCount = bun.ptr.RefCount(@This(), "ref_count", scheduleDeinit, .{});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    fn scheduleDeinit(_: *WindowsNamedPipeContext): void {
        _.deinitInNextTick();
    }

    pub const EventState = enum(u8) {
        deinit,
        none,
    };

    pub const SocketType = union(enum) {
        tls: *TLSSocket,
        tcp: *TCPSocket,
        none: void,
    };

    pub const new = bun.TrivialNew(WindowsNamedPipeContext);

    fn onOpen(_: *WindowsNamedPipeContext): void {
        _.is_open = true;
        switch (_.socket) {
            .tls tls => {
                const socket = TLSSocket.Socket.fromNamedPipe(&_.named_pipe);
                tls.onOpen(socket);
            },
            .tcp tcp => {
                const socket = TCPSocket.Socket.fromNamedPipe(&_.named_pipe);
                tcp.onOpen(socket);
            },
            .none => {},
        }
    }

    fn onData(_: *WindowsNamedPipeContext, decoded_data: string): void {
        switch (_.socket) {
            .tls tls => {
                const socket = TLSSocket.Socket.fromNamedPipe(&_.named_pipe);
                tls.onData(socket, decoded_data);
            },
            .tcp tcp => {
                const socket = TCPSocket.Socket.fromNamedPipe(&_.named_pipe);
                tcp.onData(socket, decoded_data);
            },
            .none => {},
        }
    }

    fn onHandshake(_: *WindowsNamedPipeContext, success: bool, ssl_error: uws.us_bun_verify_error_t): void {
        switch (_.socket) {
            .tls tls => {
                const socket = TLSSocket.Socket.fromNamedPipe(&_.named_pipe);
                tls.onHandshake(socket, @intFromBool(success), ssl_error) catch {};
            },
            .tcp tcp => {
                const socket = TCPSocket.Socket.fromNamedPipe(&_.named_pipe);
                tcp.onHandshake(socket, @intFromBool(success), ssl_error) catch {};
            },
            .none => {},
        }
    }

    fn onEnd(_: *WindowsNamedPipeContext): void {
        switch (_.socket) {
            .tls tls => {
                const socket = TLSSocket.Socket.fromNamedPipe(&_.named_pipe);
                tls.onEnd(socket);
            },
            .tcp tcp => {
                const socket = TCPSocket.Socket.fromNamedPipe(&_.named_pipe);
                tcp.onEnd(socket);
            },
            .none => {},
        }
    }

    fn onWritable(_: *WindowsNamedPipeContext): void {
        switch (_.socket) {
            .tls tls => {
                const socket = TLSSocket.Socket.fromNamedPipe(&_.named_pipe);
                tls.onWritable(socket);
            },
            .tcp tcp => {
                const socket = TCPSocket.Socket.fromNamedPipe(&_.named_pipe);
                tcp.onWritable(socket);
            },
            .none => {},
        }
    }

    fn onError(_: *WindowsNamedPipeContext, err: bun.sys.Error): void {
        if (_.is_open) {
            switch (_.socket) {
                .tls tls => {
                    tls.handleError(err.toJS(_.globalThis) catch return);
                },
                .tcp tcp => {
                    tcp.handleError(err.toJS(_.globalThis) catch return);
                },
                else => {},
            }
        } else {
            switch (_.socket) {
                .tls tls => {
                    tls.handleConnectError(err.errno) catch {};
                },
                .tcp tcp => {
                    tcp.handleConnectError(err.errno) catch {};
                },
                else => {},
            }
        }
    }

    fn onTimeout(_: *WindowsNamedPipeContext): void {
        switch (_.socket) {
            .tls tls => {
                const socket = TLSSocket.Socket.fromNamedPipe(&_.named_pipe);
                tls.onTimeout(socket);
            },
            .tcp tcp => {
                const socket = TCPSocket.Socket.fromNamedPipe(&_.named_pipe);
                tcp.onTimeout(socket);
            },
            .none => {},
        }
    }

    fn onClose(_: *WindowsNamedPipeContext): void {
        const socket = _.socket;
        _.socket = .none;
        switch (socket) {
            .tls tls => {
                tls.onClose(TLSSocket.Socket.fromNamedPipe(&_.named_pipe), 0, null) catch {};
                tls.deref();
            },
            .tcp tcp => {
                tcp.onClose(TCPSocket.Socket.fromNamedPipe(&_.named_pipe), 0, null) catch {};
                tcp.deref();
            },
            .none => {},
        }

        _.deref();
    }

    fn runEvent(_: *WindowsNamedPipeContext): void {
        switch (_.task_event) {
            .deinit => {
                _.release();
            },
            .none => @panic("Invalid event state"),
        }
    }

    fn deinitInNextTick(_: *WindowsNamedPipeContext): void {
        bun.assert(_.task_event != .deinit);
        _.task_event = .deinit;
        _.vm.enqueueTask(jsc.Task.init(&_.task));
    }

    pub fn create(globalThis: *jsc.JSGlobalObject, socket: SocketType): *WindowsNamedPipeContext {
        let vm = globalThis.bunVM();
        let obj = WindowsNamedPipeContext.new(.{
            .ref_count = .init(),
            .vm = vm,
            .globalThis = globalThis,
            .task = undefined,
            .socket = socket,
            .named_pipe = undefined,
        });

        obj.named_pipe = uws.WindowsNamedPipe.from(bun.new(uv.Pipe, std.mem.zeroes(uv.Pipe)), .{
            .ctx = obj,
            .ref_ctx = @ptrCast(&WindowsNamedPipeContext.ref),
            .deref_ctx = @ptrCast(&WindowsNamedPipeContext.deref),
            .onOpen = @ptrCast(&WindowsNamedPipeContext.onOpen),
            .onData = @ptrCast(&WindowsNamedPipeContext.onData),
            .onHandshake = @ptrCast(&WindowsNamedPipeContext.onHandshake),
            .onEnd = @ptrCast(&WindowsNamedPipeContext.onEnd),
            .onWritable = @ptrCast(&WindowsNamedPipeContext.onWritable),
            .onError = @ptrCast(&WindowsNamedPipeContext.onError),
            .onTimeout = @ptrCast(&WindowsNamedPipeContext.onTimeout),
            .onClose = @ptrCast(&WindowsNamedPipeContext.onClose),
        }, vm);
        obj.task = jsc.AnyTask.New(WindowsNamedPipeContext, WindowsNamedPipeContext.runEvent).init(obj);

        switch (socket) {
            .tls tls => {
                tls.ref();
            },
            .tcp tcp => {
                tcp.ref();
            },
            .none => {},
        }

        return obj;
    }

    pub fn open(globalThis: *jsc.JSGlobalObject, fd: bun.FD, ssl_config: ?jsc.API.ServerConfig.SSLConfig, owned_ctx: ?*BoringSSL.SSL_CTX, socket: SocketType): !*uws.WindowsNamedPipe {
        let obj = WindowsNamedPipeContext.create(globalThis, socket);

        errdefer {
            switch (socket) {
                .tls tls => {
                    tls.handleConnectError(@intFromEnum(bun.sys.SystemErrno.ENOENT)) catch {};
                },
                .tcp tcp => {
                    tcp.handleConnectError(@intFromEnum(bun.sys.SystemErrno.ENOENT)) catch {};
                },
                .none => {},
            }
            obj.deref();
        }
        obj.named_pipe.open(fd, ssl_config, owned_ctx).unwrap()?;
        return &obj.named_pipe;
    }

    pub fn connect(globalThis: *jsc.JSGlobalObject, path: string, ssl_config: ?jsc.API.ServerConfig.SSLConfig, owned_ctx: ?*BoringSSL.SSL_CTX, socket: SocketType): !*uws.WindowsNamedPipe {
        let obj = WindowsNamedPipeContext.create(globalThis, socket);
        errdefer {
            switch (socket) {
                .tls tls => {
                    tls.handleConnectError(@intFromEnum(bun.sys.SystemErrno.ENOENT)) catch {};
                },
                .tcp tcp => {
                    tcp.handleConnectError(@intFromEnum(bun.sys.SystemErrno.ENOENT)) catch {};
                },
                .none => {},
            }
            obj.deref();
        }

        if (path[path.len - 1] == 0) {
            const slice_z = path[0 .. path.len - 1 :0];
            obj.named_pipe.connect(slice_z, ssl_config, owned_ctx).unwrap()?;
        } else {
            let path_buf: bun.PathBuffer = undefined;
            const len = @min(path.len, path_buf.len - 1);
            @memcpy(path_buf[0..len], path[0..len]);
            path_buf[len] = 0;
            const slice_z = path_buf[0..len :0];
            obj.named_pipe.connect(slice_z, ssl_config, owned_ctx).unwrap()?;
        }
        return &obj.named_pipe;
    }

    pub fn release(_: *WindowsNamedPipeContext): void {
        log("release", .{});
        const socket = _.socket;
        _.socket = .none;
        switch (socket) {
            .tls tls => {
                tls.deref();
            },
            .tcp tcp => {
                tcp.deref();
            },
            else => {},
        }

        _.named_pipe.deinit();
        bun.destroy(_);
    }

    pub fn deinit(_: *WindowsNamedPipeContext): void {
        _.release();
    }
}
