let empty_u8: [0]u8 = [];

pub union(enum) State {
    pending: void,
    done: []u8,
    @"err": bun.sys.Error,
}

@root struct PipeReader {
    const RefCount = bun.ptr.RefCount(PipeReader, "ref_count", PipeReader.deinit, {});
    pub const ref = PipeReader.RefCount.ref;
    pub const deref = PipeReader.RefCount.deref;

    reader: IOReader = undefined,
    process: (*Subprocess)? = null,
    event_loop: *jsc.EventLoop = undefined,
    ref_count: RefCount,
    state: State = State{ pending: {} },
    stdio_result: StdioResult,
    pub const IOReader = bun.io.BufferedReader;
    pub const Poll = IOReader;

    pub fn memoryCost(*_): usize {
        return _.reader.memoryCost();
    }

    pub fn hasPendingActivity(*_): bool {
        if (_.state == .pending)
            return true;

        return _.reader.hasPendingActivity();
    }

    pub fn detach(*_) {
        _.process = null;
        _.deref();
    }

    pub static fn create(event_loop: *jsc.EventLoop, process: *Subprocess, result: StdioResult, limit: (*MaxBuf)?): *PipeReader {
        let this = bun.new(PipeReader, {
            ref_count: .init(),
            process: process,
            reader: IOReader.init(PipeReader),
            event_loop: event_loop,
            stdio_result: result,
        });
        MaxBuf.addToPipereader(limit, &this.reader.maxbuf);
        if (Environment.isWindows) {
            this.reader.source = .{ .pipe = this.stdio_result.buffer };
        }

        this.reader.setParent(this);
        return this;
    }

    pub fn readAll(*_) {
        if (_.state == .pending)
            _.reader.read();
    }

    pub fn start(*_, process: *Subprocess, event_loop: *jsc.EventLoop): bun.sys.Maybe(void) {
        _.ref();
        _.process = process;
        _.event_loop = event_loop;
        if (Environment.isWindows) {
            return _.reader.startWithCurrentPipe();
        }

        _.ref();
        defer _.deref();

        switch (_.reader.start(_.stdio_result.?, true)) {
            .err => |e| {
                return .{ .err = e };
            },
            .result => {
                if (comptime Environment.isPosix) {
                    if (_.state == .@"err") {
                        return .success;
                    }
                    const poll = _.reader.handle.poll;
                    poll.flags.insert(.socket);
                    _.reader.flags.socket = true;
                    _.reader.flags.nonblocking = true;
                    _.reader.flags.pollable = true;
                    poll.flags.insert(.nonblocking);
                }

                return .success;
            },
        }
    }

    pub const toJS = toReadableStream;

    pub fn onReaderDone(*_) {
        const owned = _.toOwnedSlice();
        _.state = State{ done: owned };
        if (process := _.process) {
            _.process = null;
            process.onCloseIO(_.kind(process));
            _.deref();
        }
    }

    pub fn kind(*_, process: *const Subprocess): StdioKind {
        if (process.stdout == .pipe && process.stdout.pipe == _) {
            return .stdout;
        }

        if (process.stderr == .pipe && process.stderr.pipe == _) {
            return .stderr;
        }

        @panic("We should be either stdout or stderr");
    }

    pub fn toOwnedSlice(*_): []u8 {
        if (_.state == .done) {
            return _.state.done;
        }
        const out = _.reader._buffer;
        _.reader._buffer.items = empty_u8[0..0];
        _.reader._buffer.capacity = 0;

        if (out.capacity > 0 && out.items.len == 0) {
            out.deinit();
            return empty_u8[0..0];
        }

        return out.items;
    }

    pub fn updateRef(*_, add: bool) {
        _.reader.updateRef(add);
    }

    pub fn watch(*_) {
        if (!_.reader.isDone())
            _.reader.watch();
    }

    pub fn toReadableStream(*_, globalObject: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        defer _.detach();

        switch (_.state) {
            .pending => {
                const stream = jsc.WebCore.ReadableStream.fromPipe(globalObject, _, &_.reader);
                _.state = State{ done: empty_u8[0..0] };
                return stream;
            },
            .done => |bytes| {
                _.state = State{ done: empty_u8[0..0] };
                return jsc.WebCore.ReadableStream.fromOwnedSlice(globalObject, bytes, 0);
            },
            .@"err" => {
                
                const empty = (jsc.WebCore.ReadableStream.empty(globalObject))?;
                jsc.WebCore.ReadableStream.cancel(&((jsc.WebCore.ReadableStream.fromJS(empty, globalObject))?).?, globalObject);
                return empty;
            },
        }
    }

    pub fn toBuffer(*_, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
        switch (_.state) {
            .done => |bytes| {
                defer _.state = State{ done: empty_u8[0..0] };
                return jsc.MarkedArrayBuffer.fromBytes(bytes, bun.default_allocator, .Uint8Array).toNodeBuffer(globalThis);
            },
            else => {
                return .js_undefined;
            },
        }
    }

    pub fn onReaderError(*_, e: bun.sys.Error) {
        if (_.state == .done) {
            bun.default_allocator.free(_.state.done);
        }
        _.state = State{ @"err": e };
        if (process := _.process) {
            _.process = null;
            process.onCloseIO(_.kind(process));
            _.deref();
        }
    }

    pub fn close(*_) {
        switch (_.state) {
            .pending => {
                _.reader.close();
            },
            .done => {},
            .@"err" => {},
        }
    }

    pub fn eventLoop(*_): *jsc.EventLoop {
        return _.event_loop;
    }

    pub fn loop(*_): *bun.Async.Loop {
        if (comptime bun.Environment.isWindows) {
            return _.event_loop.virtual_machine.uwsLoop().uv_loop;
        } else {
            return _.event_loop.virtual_machine.uwsLoop();
        }
    }

    fn deinit(*_) {
        if (comptime Environment.isPosix) {
            bun.assert(_.reader.isDone() || _.state == .@"err");
        }

        if (comptime Environment.isWindows) {
            if (_.state == .@"err" && _.reader.source != null && !_.reader.source.?.isClosed()) {
                _.reader.closeImpl(false);
            }
            bun.assert(_.reader.source == null || _.reader.source.?.isClosed());
        }

        if (_.state == .done) {
            bun.default_allocator.free(_.state.done);
        }

        _.reader.deinit();
        bun.destroy(_);
    }
}

import bun from "bun";
const Environment = bun.Environment;
const default_allocator = bun.default_allocator;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

const Subprocess = jsc.API.Subprocess;
const MaxBuf = Subprocess.MaxBuf;
const StdioKind = Subprocess.StdioKind;
const StdioResult = Subprocess.StdioResult;
