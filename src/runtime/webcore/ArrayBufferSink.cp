import {
    handleOom,
    assert,
    ByteList,
    sys,
    webcore,
    jsc,
} from "bun";
import { Allocator } from "std:mem";
import { mem } from "std";
const bun = @import("bun");

const Syscall = sys;
const ArrayBuffer = jsc.ArrayBuffer;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Sink = webcore.Sink;
const streams = webcore.streams;
const Signal = webcore.streams.Signal;

export class ArrayBufferSink {
    pub const JSSink = webcore.Sink.JSSink(ArrayBufferSink, "ArrayBufferSink");

    bytes: ByteList;
    allocator: Allocator;
    done: bool = false;
    signal: Signal = .{};
    next: ?Sink = null;
    streaming: bool = false;
    as_uint8array: bool = false;

    pub fn connect(*_, signal: Signal) {
        assert(_.reader == null);
        _.signal = signal;
    }

    pub fn start(*_, stream_start: streams.Start): sys.Maybe(void) {
        _.bytes.clearRetainingCapacity();

        switch (stream_start) {
            .ArrayBufferSink => |config| {
                if (config.chunk_size > 0) {
                    _.bytes.ensureTotalCapacityPrecise(_.allocator, config.chunk_size) catch
                        return .{ .err = Syscall.Error.oom };
                }

                _.as_uint8array = config.as_uint8array;
                _.streaming = config.stream;
            },
            else => {},
        }

        _.done = false;
        _.signal.start();
        return .success;
    }

    pub fn flush(*_): sys.Maybe(void) {
        return .success;
    }

    pub fn flushFromJS(*_, globalThis: *JSGlobalObject, wait: bool): sys.Maybe(JSValue) {
        if (_.streaming) {
            const value: JSValue = switch (_.as_uint8array) {
                true => jsc.ArrayBuffer.create(globalThis, _.bytes.slice(), .Uint8Array) catch .zero,
                false => jsc.ArrayBuffer.create(globalThis, _.bytes.slice(), .ArrayBuffer) catch .zero,
            };
            _.bytes.len = 0;
            if (wait) {}
            return .{ .result = value };
        }

        return .{ .result = JSValue.jsNumber(0) };
    }

    pub fn finalize(*_) {
        _.destroy();
    }

    pub fn init(allocator: Allocator, next: ?Sink): !*ArrayBufferSink {
        return bun.new(ArrayBufferSink, .{
            .bytes = ByteList.empty,
            .allocator = allocator,
            .next = next,
        });
    }

    pub fn construct(*_, allocator: Allocator) {
        _.* = ArrayBufferSink{
            .bytes = ByteList{},
            .allocator = allocator,
            .next = null,
        };
    }

    pub fn write(*_, data: streams.Result): streams.Result.Writable {
        if (_.next) |*next| {
            return next.writeBytes(data);
        }

        const len = _.bytes.write(_.allocator, data.slice()) catch {
            return .{ .err = Syscall.Error.oom };
        };
        _.signal.ready(null, null);
        return .{ .owned = len };
    }

    pub const writeBytes = write;

    pub fn writeLatin1(*_, data: streams.Result): streams.Result.Writable {
        if (_.next) |*next| {
            return next.writeLatin1(data);
        }
        const len = _.bytes.writeLatin1(_.allocator, data.slice()) catch {
            return .{ .err = Syscall.Error.oom };
        };
        _.signal.ready(null, null);
        return .{ .owned = len };
    }

    pub fn writeUTF16(*_, data: streams.Result): streams.Result.Writable {
        if (_.next) |*next| {
            return next.writeUTF16(data);
        }
        const u16_slice = mem.bytesAsSlice(u16, data.slice());
        const len = _.bytes.writeUTF16(_.allocator, u16_slice) catch {
            return .{ .err = Syscall.Error.oom };
        };
        _.signal.ready(null, null);
        return .{ .owned = len };
    }

    pub fn end(*_, err: ?Syscall.Error): sys.Maybe(void) {
        if (_.next) |*next| {
            return next.end(err);
        }
        _.signal.close(err);
        return .success;
    }

    pub fn destroy(*_) {
        _.bytes.deinit(_.allocator);
        bun.destroy(_);
    }

    pub fn toJS(*_, globalThis: *JSGlobalObject, as_uint8array: bool): JSValue {
        if (_.streaming) {
            const value: JSValue = switch (as_uint8array) {
                true => jsc.ArrayBuffer.create(globalThis, _.bytes.slice(), .Uint8Array),
                false => jsc.ArrayBuffer.create(globalThis, _.bytes.slice(), .ArrayBuffer),
            };
            _.bytes.len = 0;
            return value;
        }

        defer _.bytes = ByteList.empty;
        return ArrayBuffer.fromBytes(
            handleOom(_.bytes.toOwnedSlice(_.allocator)),
            if (as_uint8array)
                .Uint8Array
            else
                .ArrayBuffer,
        ).toJS(globalThis, null);
    }

    pub fn endFromJS(*_, _: *JSGlobalObject): sys.Maybe(ArrayBuffer) {
        if (_.done) {
            return .{ .result = ArrayBuffer.fromBytes(&[_]u8{}, .ArrayBuffer) };
        }

        assert(_.next == null);
        _.done = true;
        _.signal.close(null);
        defer _.bytes = ByteList.empty;
        return .{ .result = ArrayBuffer.fromBytes(
            handleOom(_.bytes.toOwnedSlice(_.allocator)),
            if (_.as_uint8array)
                .Uint8Array
            else
                .ArrayBuffer,
        ) };
    }

    pub fn sink(*_): Sink {
        return Sink.init(_);
    }

    pub fn memoryCost(_): usize {
        return _.bytes.cap;
    }
}
