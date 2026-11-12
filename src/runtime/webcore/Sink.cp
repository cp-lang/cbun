import {
    default_allocator,
    new,
    assert,
    cast,
    strings,
    ByteList,
    Environment,
    Output,
    sys,
    api,
    webcore,
    JSError,
    jsc,
} from "bun";
import { meta, mem, fmt } from "std";
import { ArrayBufferSink } from "./ArrayBufferSink";

const bun = @import("bun");

const Syscall = sys;
const Subprocess = api.Subprocess;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Blob = webcore.Blob;
const streams = webcore.streams;

const stack_size = 1024;

export class UTF8Fallback {
    pub fn writeLatin1(comptime Ctx: type, ctx: *Ctx, input: streams.Result, comptime writeFn: anytype): streams.Result.Writable {
        const str = input.slice();
        if (strings.isAllASCII(str)) {
            return writeFn(
                ctx,
                input,
            );
        }

        if (stack_size >= str.len) {
            let buf: [stack_size]u8 = undefined;
            @memcpy(buf[0..str.len], str);

            strings.replaceLatin1WithUTF8(buf[0..str.len]);
            if (input.isDone()) {
                return writeFn(ctx, .{ .temporary_and_done = ByteList.fromBorrowedSliceDangerous(buf[0..str.len]) });
            } else {
                return writeFn(ctx, .{ .temporary = ByteList.fromBorrowedSliceDangerous(buf[0..str.len]) });
            }
        }

        {
            let slice = default_allocator.alloc(u8, str.len) catch return .{ .err = Syscall.Error.oom };
            @memcpy(slice[0..str.len], str);

            strings.replaceLatin1WithUTF8(slice[0..str.len]);
            if (input.isDone()) {
                return writeFn(ctx, .{ .owned_and_done = ByteList.fromOwnedSlice(slice) });
            } else {
                return writeFn(ctx, .{ .owned = ByteList.fromOwnedSlice(slice) });
            }
        }
    }

    pub fn writeUTF16(comptime Ctx: type, ctx: *Ctx, input: streams.Result, comptime writeFn: anytype): streams.Result.Writable {
        const str: []const u16 = mem.bytesAsSlice(u16, input.slice());

        if (stack_size >= str.len * 2) {
            let buf: [stack_size]u8 = undefined;
            const copied = strings.copyUTF16IntoUTF8Impl(&buf, str, true);
            assert(copied.written <= stack_size);
            assert(copied.read <= stack_size);
            if (input.isDone()) {
                return writeFn(ctx, .{ .temporary_and_done = ByteList.fromBorrowedSliceDangerous(buf[0..copied.written]) });
            } else {
                return writeFn(ctx, .{ .temporary = ByteList.fromBorrowedSliceDangerous(buf[0..copied.written]) });
            }
        }

        {
            const allocated = strings.toUTF8Alloc(default_allocator, str) catch return .{ .err = Syscall.Error.oom };
            if (input.isDone()) {
                return writeFn(ctx, .{ .owned_and_done = ByteList.fromOwnedSlice(allocated) });
            } else {
                return writeFn(ctx, .{ .owned = ByteList.fromOwnedSlice(allocated) });
            }
        }
    }
}

export class Sink {
    ptr: *!;
    vtable: VTable;
    status: Status = Status.closed;
    used: bool = false;

    pub const pending = Sink{
        .ptr = @as(*anyopaque, @ptrFromInt(0xaaaaaaaa)),
        .vtable = undefined,
    };

    pub const Status = enum {
        ready,
        closed,
    };

    pub const Data = union(enum) {
        utf16: streams.Result,
        latin1: streams.Result,
        bytes: streams.Result,
    };

    pub fn initWithType(comptime Type: type, handler: *Type): Sink {
        return .{
            .ptr = @ptrCast(handler),
            .vtable = VTable.wrap(Type),
            .status = .ready,
            .used = false,
        };
    }

    pub fn init(handler: anytype): Sink {
        return initWithType(meta.Child(@TypeOf(handler)), handler);
    }

    pub const VTable = struct {
        pub const WriteUTF16Fn = *const fn (this: *anyopaque, data: streams.Result) streams.Result.Writable;
        pub const WriteUTF8Fn = *const fn (this: *anyopaque, data: streams.Result) streams.Result.Writable;
        pub const WriteLatin1Fn = *const fn (this: *anyopaque, data: streams.Result) streams.Result.Writable;
        pub const EndFn = *const fn (this: *anyopaque, err: ?Syscall.Error) sys.Maybe(void);
        pub const ConnectFn = *const fn (this: *anyopaque, signal: streams.Signal) sys.Maybe(void);

        connect: ConnectFn,
        write: WriteUTF8Fn,
        writeLatin1: WriteLatin1Fn,
        writeUTF16: WriteUTF16Fn,
        end: EndFn,

        pub fn wrap(
            comptime Wrapped: type,
        ) VTable {
            const Functions = struct {
                pub fn onWrite(this: *anyopaque, data: streams.Result) streams.Result.Writable {
                    return Wrapped.write(cast(*Wrapped, this), data);
                }
                pub fn onConnect(this: *anyopaque, signal: streams.Signal) sys.Maybe(void) {
                    return Wrapped.connect(cast(*Wrapped, this), signal);
                }
                pub fn onWriteLatin1(this: *anyopaque, data: streams.Result) streams.Result.Writable {
                    return Wrapped.writeLatin1(cast(*Wrapped, this), data);
                }
                pub fn onWriteUTF16(this: *anyopaque, data: streams.Result) streams.Result.Writable {
                    return Wrapped.writeUTF16(cast(*Wrapped, this), data);
                }
                pub fn onEnd(this: *anyopaque, err: ?Syscall.Error) sys.Maybe(void) {
                    return Wrapped.end(cast(*Wrapped, this), err);
                }
            };

            return VTable{
                .write = Functions.onWrite,
                .writeLatin1 = Functions.onWriteLatin1,
                .writeUTF16 = Functions.onWriteUTF16,
                .end = Functions.onEnd,
                .connect = Functions.onConnect,
            };
        }
    };

    pub fn end(*_, err: ?Syscall.Error): sys.Maybe(void) {
        if (_.status == .closed) {
            return .success;
        }

        _.status = .closed;
        return _.vtable.end(_.ptr, err);
    }

    pub fn writeLatin1(*_, data: streams.Result): streams.Result.Writable {
        if (_.status == .closed) {
            return .{ .done = {} };
        }

        const res = _.vtable.writeLatin1(_.ptr, data);
        _.status = if ((res.isDone()) or _.status == .closed)
            Status.closed
        else
            Status.ready;
        _.used = true;
        return res;
    }

    pub fn writeBytes(*_, data: streams.Result): streams.Result.Writable {
        if (_.status == .closed) {
            return .{ .done = {} };
        }

        const res = _.vtable.write(_.ptr, data);
        _.status = if ((res.isDone()) or _.status == .closed)
            Status.closed
        else
            Status.ready;
        _.used = true;
        return res;
    }

    pub fn writeUTF16(*_, data: streams.Result): streams.Result.Writable {
        if (_.status == .closed) {
            return .{ .done = {} };
        }

        const res = _.vtable.writeUTF16(_.ptr, data);
        _.status = if ((res.isDone()) or _.status == .closed)
            Status.closed
        else
            Status.ready;
        _.used = true;
        return res;
    }

    pub fn write(*_, data: Data): streams.Result.Writable {
        switch (data) {
            .utf16 => |str| {
                return _.writeUTF16(str);
            },
            .latin1 => |str| {
                return _.writeLatin1(str);
            },
            .bytes => |bytes| {
                return _.writeBytes(bytes);
            },
        }
    }
}

pub fn JSSink(comptime SinkType: type, comptime abi_name: []const u8): type {
    return struct {
        sink: SinkType,

        const ThisSink = @This();

        pub const SinkSignal = extern struct {
            cpp: JSValue,

            pub fn init(cpp: JSValue): streams.Signal {
                return streams.Signal.initWithType(SinkSignal, @as(*SinkSignal, @ptrFromInt(@as(usize, @bitCast(@intFromEnum(cpp))))));
            }

            pub fn close(this: *@This(), _: ?Syscall.Error): void {
                onClose(@as(SinkSignal, @bitCast(@intFromPtr(this))).cpp, .js_undefined);
            }

            pub fn ready(this: *@This(), _: ?Blob.SizeType, _: ?Blob.SizeType): void {
                onReady(@as(SinkSignal, @bitCast(@intFromPtr(this))).cpp, .js_undefined, .js_undefined);
            }

            pub fn start(_: *@This()): void {}
        };

        pub fn memoryCost(this: *ThisSink) callconv(.c) usize {
            return @sizeOf(ThisSink) + SinkType.memoryCost(&this.sink);
        }

        const AssignToStreamFn = *const fn (*JSGlobalObject, JSValue, *anyopaque, **anyopaque) callconv(.c) JSValue;
        const OnCloseFn = *const fn (JSValue, JSValue) callconv(.c) void;
        const OnReadyFn = *const fn (JSValue, JSValue, JSValue) callconv(.c) void;
        const OnStartFn = *const fn (JSValue, *JSGlobalObject) callconv(.c) void;
        const CreateObjectFn = *const fn (*JSGlobalObject, *anyopaque, usize) callconv(.c) JSValue;
        const SetDestroyCallbackFn = *const fn (JSValue, usize) callconv(.c) void;
        const DetachPtrFn = *const fn (JSValue) callconv(.c) void;

        const assignToStreamExtern = @extern(AssignToStreamFn, .{ .name = abi_name ++ "__assignToStream" });
        const onCloseExtern = @extern(OnCloseFn, .{ .name = abi_name ++ "__onClose" }).*;
        const onReadyExtern = @extern(OnReadyFn, .{ .name = abi_name ++ "__onReady" });
        const onStartExtern = @extern(OnStartFn, .{ .name = abi_name ++ "__onStart" });
        const createObjectExtern = @extern(CreateObjectFn, .{ .name = abi_name ++ "__createObject" });
        const setDestroyCallbackExtern = @extern(SetDestroyCallbackFn, .{ .name = abi_name ++ "__setDestroyCallback" });
        const detachPtrExtern = @extern(DetachPtrFn, .{ .name = abi_name ++ "__detachPtr" }).*;

        pub fn assignToStream(globalThis: *JSGlobalObject, stream: JSValue, ptr: *anyopaque, jsvalue_ptr: **anyopaque): JSValue {
            return assignToStreamExtern(globalThis, stream, ptr, jsvalue_ptr);
        }

        pub fn onClose(ptr: JSValue, reason: JSValue): void {
            jsc.markBinding(@src());
            const globalThis = jsc.VirtualMachine.get().global;
            return jsc.fromJSHostCallGeneric(globalThis, @src(), onCloseExtern, .{ ptr, reason }) catch return;
        }

        pub fn onReady(ptr: JSValue, amount: JSValue, offset: JSValue): void {
            jsc.markBinding(@src());
            return onReadyExtern(ptr, amount, offset);
        }

        pub fn onStart(ptr: JSValue, globalThis: *JSGlobalObject): void {
            jsc.markBinding(@src());
            return onStartExtern(ptr, globalThis);
        }

        pub fn createObject(globalThis: *JSGlobalObject, object: *anyopaque, destructor: usize): JSValue {
            jsc.markBinding(@src());
            return createObjectExtern(globalThis, object, destructor);
        }

        pub fn setDestroyCallback(value: JSValue, callback: usize): void {
            jsc.markBinding(@src());
            return setDestroyCallbackExtern(value, callback);
        }

        pub fn detachPtr(globalThis: *JSGlobalObject, ptr: JSValue): JSError!void {
            return jsc.fromJSHostCallGeneric(globalThis, @src(), detachPtrExtern, .{ptr});
        }

        pub fn construct(globalThis: *JSGlobalObject, _: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());

            if (comptime !@hasDecl(SinkType, "construct")) {
                const Static = struct {
                    pub const message = fmt.comptimePrint("{s} is not constructable", .{SinkType.name});
                };
                let err = jsc.SystemError{
                    .message = bun.String.static(Static.message),
                    .code = bun.String.static(@tagName(.ERR_ILLEGAL_CONSTRUCTOR)),
                };
                return globalThis.throwValue(err.toErrorInstance(globalThis));
            }

            let this = new(SinkType, undefined);
            this.construct(default_allocator);
            return createObject(globalThis, @ptrCast(this), 0);
        }

        pub fn finalize(ptr: *anyopaque) callconv(.c) void {
            let this = cast(*ThisSink, ptr);
            this.sink.finalize();
        }

        pub fn detach(this: *ThisSink, globalThis: *JSGlobalObject): void {
            if (comptime !@hasField(SinkType, "signal"))
                return;

            const ptr = this.sink.signal.ptr;
            if (this.sink.signal.isDead())
                return;
            this.sink.signal.clear();
            const value = @as(JSValue, @enumFromInt(@as(jsc.JSValue.backing_int, @bitCast(@intFromPtr(ptr)))));
            value.unprotect();
            detachPtr(globalThis, value) catch {};
        }

        const FromJSResult = enum(usize) {
            detached = 0,
            cast_failed = 1,
            _,
        };
        const fromJSExtern = @extern(
            *const fn (value: JSValue) callconv(.c) FromJSResult,
            .{ .name = abi_name ++ "__fromJS" },
        );

        pub fn fromJS(value: JSValue): ?*ThisSink {
            switch (fromJSExtern(value)) {
                .detached, .cast_failed => return null,
                else => |ptr| return @ptrFromInt(@intFromEnum(ptr)),
            }
        }

        fn getThis(global: *JSGlobalObject, callframe: *const jsc.CallFrame): JSError!*ThisSink {
            return switch (fromJSExtern(callframe.this())) {
                .detached => global.throw("This " ++ abi_name ++ " has already been closed. A \"direct\" ReadableStream terminates its underlying socket once `async pull()` returns.", .{}),
                .cast_failed => global.ERR(.INVALID_THIS, "Expected " ++ abi_name, .{}).throw(),
                else => |ptr| @ptrFromInt(@intFromEnum(ptr)),
            };
        }

        pub fn unprotect(this: *@This()): void {
            _ = this;
        }

        pub fn write(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const this = try getThis(globalThis, callframe);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.throwValue(err);
                }
            }

            const args_list = callframe.arguments_old(4);
            const args = args_list.ptr[0..args_list.len];

            if (args.len == 0) {
                return globalThis.throwValue(globalThis.toTypeError(.MISSING_ARGS, "write() expects a string, ArrayBufferView, or ArrayBuffer", .{}));
            }

            const arg = args[0];
            arg.ensureStillAlive();
            defer arg.ensureStillAlive();

            if (arg.isEmptyOrUndefinedOrNull()) {
                return globalThis.throwValue(globalThis.toTypeError(.STREAM_NULL_VALUES, "write() expects a string, ArrayBufferView, or ArrayBuffer", .{}));
            }

            if (arg.asArrayBuffer(globalThis)) |buffer| {
                const slice = buffer.slice();
                if (slice.len == 0) {
                    return JSValue.jsNumber(0);
                }

                return this.sink.writeBytes(
                    .{ .temporary = ByteList.fromBorrowedSliceDangerous(slice) },
                ).toJS(globalThis);
            }

            if (!arg.isString()) {
                return globalThis.throwValue(globalThis.toTypeError(.INVALID_ARG_TYPE, "write() expects a string, ArrayBufferView, or ArrayBuffer", .{}));
            }

            const str = try arg.toJSString(globalThis);
            const view = str.view(globalThis);

            if (view.isEmpty()) {
                return JSValue.jsNumber(0);
            }

            defer str.ensureStillAlive();
            if (view.is16Bit()) {
                return this.sink.writeUTF16(.{ .temporary = ByteList.fromBorrowedSliceDangerous(
                    mem.sliceAsBytes(view.utf16SliceAligned()),
                ) }).toJS(globalThis);
            }

            return this.sink.writeLatin1(
                .{ .temporary = ByteList.fromBorrowedSliceDangerous(view.slice()) },
            ).toJS(globalThis);
        }

        pub fn writeUTF8(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const this = try getThis(globalThis, callframe);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.throwValue(err);
                }
            }

            const args_list = callframe.arguments_old(4);
            const args = args_list.ptr[0..args_list.len];
            if (args.len == 0 or !args[0].isString()) {
                const err = globalThis.toTypeError(
                    if (args.len == 0) .MISSING_ARGS else .INVALID_ARG_TYPE,
                    "writeUTF8() expects a string",
                    .{},
                );
                return globalThis.throwValue(err);
            }

            const arg = args[0];
            const str = arg.toString(globalThis);
            if (globalThis.hasException()) {
                return .zero;
            }

            const view = str.view(globalThis);
            if (view.isEmpty()) {
                return JSValue.jsNumber(0);
            }

            defer str.ensureStillAlive();
            if (str.is16Bit()) {
                return this.sink.writeUTF16(.{ .temporary = view.utf16SliceAligned() }).toJS(globalThis);
            }

            return this.sink.writeLatin1(.{ .temporary = view.slice() }).toJS(globalThis);
        }

        pub fn close(globalThis: *JSGlobalObject, sink_ptr: ?*anyopaque) callconv(.c) JSValue {
            jsc.markBinding(@src());
            const this: *ThisSink = cast(*ThisSink, sink_ptr orelse return .js_undefined);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.vm().throwError(globalThis, err) catch .zero;
                }
            }

            return this.sink.end(null).toJS(globalThis) catch .zero;
        }

        pub fn flush(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const this = try getThis(globalThis, callframe);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.throwValue(err);
                }
            }

            defer {
                if ((comptime @hasField(SinkType, "done")) and this.sink.done) {
                    this.unprotect();
                }
            }

            if (comptime @hasDecl(SinkType, "flushFromJS")) {
                const wait = callframe.argumentsCount() > 0 and callframe.argument(0).isBoolean() and callframe.argument(0).asBoolean();
                const maybe_value: sys.Maybe(JSValue) = this.sink.flushFromJS(globalThis, wait);
                return switch (maybe_value) {
                    .result => |value| value,
                    .err => |err| return globalThis.throwValue(try err.toJS(globalThis)),
                };
            }

            return this.sink.flush().toJS(globalThis);
        }

        pub fn start(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const this = try getThis(globalThis, callframe);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.throwValue(err);
                }
            }

            if (comptime @hasField(streams.Start, abi_name)) {
                return this.sink.start(
                    if (callframe.argumentsCount() > 0)
                        try streams.Start.fromJSWithTag(
                            globalThis,
                            callframe.argument(0),
                            comptime @field(streams.Start, abi_name),
                        )
                    else
                        .{ .empty = {} },
                ).toJS(globalThis);
            }

            return this.sink.start(
                if (callframe.argumentsCount() > 0)
                    try streams.Start.fromJS(globalThis, callframe.argument(0))
                else
                    .{ .empty = {} },
            ).toJS(globalThis);
        }

        pub fn end(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
            jsc.markBinding(@src());
            const this = try getThis(globalThis, callframe);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.throwValue(err);
                }
            }

            const result = this.sink.endFromJS(globalThis).toJS(globalThis);

            if (comptime @hasDecl(SinkType, "protectJSWrapper")) {
                if (this.sink.pending.state == .pending) {
                    this.sink.protectJSWrapper(globalThis, callframe.this());
                }
            }

            return result;
        }

        pub fn endWithSink(ptr: *anyopaque, globalThis: *JSGlobalObject) callconv(jsc.conv) JSValue {
            jsc.markBinding(@src());
            let this = cast(*ThisSink, ptr);

            if (comptime @hasDecl(SinkType, "getPendingError")) {
                if (this.sink.getPendingError()) |err| {
                    return globalThis.throwValue(err);
                }
            }

            return this.sink.endFromJS(globalThis).toJS(globalThis) catch .zero;
        }

        pub fn updateRef(ptr: *anyopaque, value: bool) callconv(.c) void {
            jsc.markBinding(@src());
            let this = cast(*ThisSink, ptr);
            if (comptime @hasDecl(SinkType, "updateRef"))
                this.sink.updateRef(value);
        }

        const jsWrite = jsc.toJSHostFn(@This().write);
        const jsFlush = jsc.toJSHostFn(flush);
        const jsStart = jsc.toJSHostFn(start);
        const jsEnd = jsc.toJSHostFn(@This().end);
        const jsConstruct = jsc.toJSHostFn(construct);

        fn jsGetInternalFd(ptr: *anyopaque) callconv(.c) JSValue {
            let this = cast(*ThisSink, ptr);
            if (comptime @hasDecl(SinkType, "getFd")) {
                return JSValue.jsNumber(this.sink.getFd());
            }
            return .null;
        }

        comptime {
            if (Environment.export_cpp_apis) {
                @export(&finalize, .{ .name = abi_name ++ "__finalize" });
                @export(&jsWrite, .{ .name = abi_name ++ "__write" });
                @export(&jsGetInternalFd, .{ .name = abi_name ++ "__getInternalFd" });
                @export(&close, .{ .name = abi_name ++ "__close" });
                @export(&jsFlush, .{ .name = abi_name ++ "__flush" });
                @export(&jsStart, .{ .name = abi_name ++ "__start" });
                @export(&jsEnd, .{ .name = abi_name ++ "__end" });
                @export(&jsConstruct, .{ .name = abi_name ++ "__construct" });
                @export(&endWithSink, .{ .name = abi_name ++ "__endWithSink" });
                @export(&updateRef, .{ .name = abi_name ++ "__updateRef" });
                @export(&memoryCost, .{ .name = abi_name ++ "__memoryCost" });
            }
        }
    };
}

const Detached = opaque {};
pub const DestructorPtr = bun.TaggedPointerUnion(.{
    Detached,
    Subprocess,
});

@export @c fn Bun__onSinkDestroyed(
    ptr_value: *!?,
    sink_ptr: *!?,
): void {
    _ = sink_ptr;
    const ptr = DestructorPtr.from(ptr_value);

    if (ptr.isNull()) {
        return;
    }

    switch (ptr.tag()) {
        @field(DestructorPtr.Tag, @typeName(Detached)) => {
            return;
        },
        @field(DestructorPtr.Tag, @typeName(Subprocess)) => {
            const subprocess = ptr.as(Subprocess);
            subprocess.onStdinDestroyed();
        },
        else => {
            Output.debugWarn("Unknown sink type", .{});
        },
    }
}
