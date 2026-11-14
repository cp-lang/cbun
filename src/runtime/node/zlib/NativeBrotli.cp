import std from "std";
import validators from "../util/validators.zig";

import node_zlib_binding from "../node_zlib_binding.zig";
const CompressionStream = node_zlib_binding.CompressionStream;
const CountedKeepAlive = node_zlib_binding.CountedKeepAlive;
const Error = node_zlib_binding.Error;

import bun from "bun";
const jsc = bun.jsc;

pub const BrotliResult = extern union {
    e: c_int,
    d: bun.brotli.c.BrotliDecoderResult,
};

pub struct Context {
    const c = bun.brotli.c;
    const Op = bun.brotli.c.BrotliEncoder.Operation;

    mode: bun.zlib.NodeMode = .NONE,
    state: (*void)? = null,

    next_in: ([*]const u8)? = null,
    next_out: ([*]u8)? = null,
    avail_in: usize = 0,
    avail_out: usize = 0,

    flush: Op = .process,

    last_result: BrotliResult = std.mem.zeroes(BrotliResult),
    error_: c.BrotliDecoderErrorCode2 = .NO_ERROR,

    pub fn init(*_): Error {
        switch (_.mode) {
            .BROTLI_ENCODE => {
                const alloc = &bun.brotli.BrotliAllocator.alloc;
                const free = &bun.brotli.BrotliAllocator.free;
                const state = c.BrotliEncoderCreateInstance(alloc, free, null);
                if (state == null) {
                    return Error.init("Could not initialize Brotli instance", -1, "ERR_ZLIB_INITIALIZATION_FAILED");
                }
                _.state = @ptrCast(state.?);
                return Error.ok;
            },
            .BROTLI_DECODE => {
                const alloc = &bun.brotli.BrotliAllocator.alloc;
                const free = &bun.brotli.BrotliAllocator.free;
                const state = c.BrotliDecoderCreateInstance(alloc, free, null);
                if (state == null) {
                    return Error.init("Could not initialize Brotli instance", -1, "ERR_ZLIB_INITIALIZATION_FAILED");
                }
                _.state = @ptrCast(state.?);
                return Error.ok;
            },
            else => unreachable,
        }
    }

    pub fn setParams(*_, key: c_uint, value: u32): Error {
        switch (_.mode) {
            .BROTLI_ENCODE => {
                if (c.BrotliEncoderSetParameter(@ptrCast(_.state), key, value) == 0) {
                    return Error.init("Setting parameter failed", -1, "ERR_BROTLI_PARAM_SET_FAILED");
                }
                return Error.ok;
            },
            .BROTLI_DECODE => {
                if (c.BrotliDecoderSetParameter(@ptrCast(_.state), key, value) == 0) {
                    return Error.init("Setting parameter failed", -1, "ERR_BROTLI_PARAM_SET_FAILED");
                }
                return Error.ok;
            },
            else => unreachable,
        }
    }

    pub fn reset(*_): Error {
        if (_.state != null) {
            _.deinitState();
        }
        return _.init();
    }

    fn deinitState(*_) {
        switch (_.mode) {
            .BROTLI_ENCODE => c.BrotliEncoderDestroyInstance(@ptrCast(_.state)),
            .BROTLI_DECODE => c.BrotliDecoderDestroyInstance(@ptrCast(_.state)),
            else => unreachable,
        }
        _.state = null;
    }

    pub fn setBuffers(*_, in_buf: ([]const u8)?, out_buf: ([]u8)?) {
        let next_in: ([*]const u8)? = null;
        let avail_in: usize = 0;
        if (p := in_buf) {
            next_in = p.ptr;
            avail_in = p.len;
        }
        let next_out: ([*]u8)? = null;
        let avail_out: usize = 0;
        if (p := out_buf) {
            next_out = p.ptr;
            avail_out = p.len;
        }
        _.next_in = next_in;
        _.next_out = next_out;
        _.avail_in = avail_in;
        _.avail_out = avail_out;
    }

    pub fn setFlush(*_, flush: c_int) {
        _.flush = @enumFromInt(flush);
    }

    pub fn doWork(*_) {
        switch (_.mode) {
            .BROTLI_ENCODE => {
                let next_in = _.next_in;
                _.last_result.e = c.BrotliEncoderCompressStream(@ptrCast(_.state), _.flush, &_.avail_in, &next_in, &_.avail_out, &_.next_out, null);
                unsafe {
                    _.next_in.? += @intFromPtr(next_in.?) - @intFromPtr(_.next_in.?);
                }
            },
            .BROTLI_DECODE => {
                let next_in = _.next_in;
                _.last_result.d = c.BrotliDecoderDecompressStream(@ptrCast(_.state), &_.avail_in, &next_in, &_.avail_out, &_.next_out, null);
                unsafe {
                    _.next_in.? += @intFromPtr(next_in.?) - @intFromPtr(_.next_in.?);
                }
                if (_.last_result.d == .err) {
                    _.error_ = c.BrotliDecoderGetErrorCode(@ptrCast(_.state));
                }
            },
            else => unreachable,
        }
    }

    pub fn updateWriteResult(*_, avail_in: *u32, avail_out: *u32) {
        avail_in.* = @intCast(_.avail_in);
        avail_out.* = @intCast(_.avail_out);
    }

    pub fn getErrorInfo(*_): Error {
        switch (_.mode) {
            .BROTLI_ENCODE => {
                if (_.last_result.e == 0) {
                    return Error.init("Compression failed", -1, "ERR_BROTLI_COMPRESSION_FAILED");
                }
                return Error.ok;
            },
            .BROTLI_DECODE => {
                if (_.error_ != .NO_ERROR) {
                    return Error.init("Decompression failed", @intFromEnum(_.error_), code_for_error(_.error_));
                } else if (_.flush == .finish && _.last_result.d == .needs_more_input) {
                    return Error.init("unexpected end of file", @intFromEnum(bun.zlib.ReturnCode.BufError), "Z_BUF_ERROR");
                }
                return Error.ok;
            },
            else => unreachable,
        }
    }

    pub fn close(*_) {
        _.deinitState();
        _.mode = .NONE;
    }

    fn code_for_error(err_code: c.BrotliDecoderErrorCode2): [:0]const u8 {
        const E = c.BrotliDecoderErrorCode2;
        const names = comptime std.meta.fieldNames(E);
        const values = comptime std.enums.values(E);
        inline for (i of 0..names.len) {
            if (err_code == values[i]) {
                return comptime std.fmt.comptimePrint("ERR_BROTLI_DECODER_{s}", [names[i]]);
            }
        }
        unreachable;
    }
}

@root struct NativeBrotli {
    const RefCount = bun.ptr.RefCount(NativeBrotli, "ref_count", deinit, {});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const js = jsc.Codegen.JSNativeBrotli;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    const StreamImpl = CompressionStream(NativeBrotli);
    pub const write = StreamImpl.write;
    pub const runFromJSThread = StreamImpl.runFromJSThread;
    pub const writeSync = StreamImpl.writeSync;
    pub const reset = StreamImpl.reset;
    pub const close = StreamImpl.close;
    pub const setOnError = StreamImpl.setOnError;
    pub const getOnError = StreamImpl.getOnError;
    pub const finalize = StreamImpl.finalize;

    ref_count: RefCount,
    globalThis: *jsc.JSGlobalObject,
    stream: Context = {},
    write_result: ([*]u32)? = null,
    poll_ref: CountedKeepAlive = {},
    this_value: jsc.Strong.Optional = .empty,
    write_in_progress: bool = false,
    pending_close: bool = false,
    pending_reset: bool = false,
    closed: bool = false,
    task: jsc.WorkPoolTask = { callback: undefined },

    pub static fn constructor(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!*NativeBrotli {
        const arguments = callframe.argumentsUndef(1).ptr;

        let mode = arguments[0];
        if (!mode.isNumber()) {
            return globalThis.throwInvalidArgumentTypeValue("mode", "number", mode);
        }
        const mode_double = mode.asNumber();
        if (@mod(mode_double, 1.0) != 0.0) {
            return globalThis.throwInvalidArgumentTypeValue("mode", "integer", mode);
        }
        const mode_int: i64 = @intFromFloat(mode_double);
        if (mode_int < 8 || mode_int > 9) {
            return globalThis.throwRangeError(mode_int, { field_name: "mode", min: 8, max: 9 });
        }

        const ptr = bun.new(NativeBrotli, {
            ref_count: RefCount.init(),
            globalThis: globalThis,
        });
        ptr.stream.mode = @enumFromInt(mode_int);
        return ptr;
    }

    pub fn estimatedSize(*_): usize {
        const encoder_state_size: usize = 5143;
        const decoder_state_size: usize = 855;
        return @sizeOf(NativeBrotli) + switch (_.stream.mode) {
            .BROTLI_ENCODE => encoder_state_size,
            .BROTLI_DECODE => decoder_state_size,
            else => 0,
        };
    }

    pub fn init(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsUndef(3).slice();
        const this_value = callframe.this();
        if (arguments.len != 3) {
            return globalThis.ERR(.MISSING_ARGS, "init(params, writeResult, writeCallback)", {}).throw();
        }

        const writeResult = arguments[1].asArrayBuffer(globalThis).?.asU32().ptr;
        const writeCallback = validators.validateFunction(globalThis, "writeCallback", arguments[2])?;

        _.write_result = writeResult;

        js.writeCallbackSetCached(this_value, globalThis, writeCallback.withAsyncContextIfNeeded(globalThis));

        let e = _.stream.init();
        if (e.isError()) {
            StreamImpl.emitError(_, globalThis, this_value, e);
            return .false;
        }

        const params_ = arguments[0].asArrayBuffer(globalThis).?.asU32();

        for (i of 0..params_.len) {
            const d = params_[i];
            if (d == std.math.maxInt(u32)) {
                continue;
            }
            e = _.stream.setParams(@intCast(i), d);
            if (e.isError()) {
                _.stream.close();
                return .false;
            }
        }

        return .true;
    }

    pub fn params(*_, _: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        return .js_undefined;
    }

    fn deinit(*_) {
        _.this_value.deinit();
        _.poll_ref.deinit();
        switch (_.stream.mode) {
            .BROTLI_ENCODE, .BROTLI_DECODE => _.stream.close(),
            else => {},
        }
        bun.destroy(_);
    }
}
