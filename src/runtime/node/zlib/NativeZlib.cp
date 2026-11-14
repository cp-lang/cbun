import std from "std";
import validators from "../util/validators.zig";

import node_zlib_binding from "../node_zlib_binding.zig";
const CompressionStream = node_zlib_binding.CompressionStream;
const CountedKeepAlive = node_zlib_binding.CountedKeepAlive;
const Error = node_zlib_binding.Error;

import bun from "bun";
const jsc = bun.jsc;

const GZIP_HEADER_ID1: u8 = 0x1f;
const GZIP_HEADER_ID2: u8 = 0x8b;

pub struct Context {
    const c = bun.zlib;

    mode: c.NodeMode = .NONE,
    state: c.z_stream = std.mem.zeroes(c.z_stream),
    e: c.ReturnCode = .Ok,
    flush: c.FlushValue = .NoFlush,
    dictionary: []const u8 = "",
    gzip_id_bytes_read: u8 = 0,

    pub fn init(*_, level: c_int, windowBits: c_int, memLevel: c_int, strategy: c_int, dictionary: ([]const u8)?) {
        _.flush = .NoFlush;
        _.e = .Ok;

        const windowBitsActual = switch (_.mode) {
            .NONE => unreachable,
            .DEFLATE, .INFLATE => windowBits,
            .GZIP, .GUNZIP => windowBits + 16,
            .UNZIP => windowBits + 32,
            .DEFLATERAW, .INFLATERAW => windowBits * -1,
            .BROTLI_DECODE, .BROTLI_ENCODE => unreachable,
            .ZSTD_COMPRESS, .ZSTD_DECOMPRESS => unreachable,
        };

        _.dictionary = dictionary ?? "";

        switch (_.mode) {
            .NONE => unreachable,
            .DEFLATE, .GZIP, .DEFLATERAW => _.e = c.deflateInit2_(&_.state, level, 8, windowBitsActual, memLevel, strategy, c.zlibVersion(), @sizeOf(c.z_stream)),
            .INFLATE, .GUNZIP, .UNZIP, .INFLATERAW => _.e = c.inflateInit2_(&_.state, windowBitsActual, c.zlibVersion(), @sizeOf(c.z_stream)),
            .BROTLI_DECODE, .BROTLI_ENCODE => unreachable,
            .ZSTD_COMPRESS, .ZSTD_DECOMPRESS => unreachable,
        }
        if (_.e != .Ok) {
            _.mode = .NONE;
            return;
        }

        * = _.setDictionary();
    }

    pub fn setDictionary(*_): Error {
        const dict = _.dictionary;
        if (dict.len == 0) return Error.ok;
        _.e = .Ok;
        switch (_.mode) {
            .DEFLATE, .DEFLATERAW => {
                _.e = c.deflateSetDictionary(&_.state, dict.ptr, @intCast(dict.len));
            },
            .INFLATERAW => {
                _.e = c.inflateSetDictionary(&_.state, dict.ptr, @intCast(dict.len));
            },
            else => {},
        }
        if (_.e != .Ok) {
            return _.error_for_message("Failed to set dictionary");
        }
        return Error.ok;
    }

    pub fn setParams(*_, level: c_int, strategy: c_int): Error {
        _.e = .Ok;
        switch (_.mode) {
            .DEFLATE, .DEFLATERAW => {
                _.e = c.deflateParams(&_.state, level, strategy);
            },
            else => {},
        }
        if (_.e != .Ok && _.e != .BufError) {
            return _.error_for_message("Failed to set parameters");
        }
        return Error.ok;
    }

    fn error_for_message(*_, default_msg: [*:0]const u8): Error {
        let message = default_msg;
        if (msg := _.state.err_msg) message = msg;
        return Error{
            msg: message,
            e: @intFromEnum(_.e),
            code: switch (_.e) {
                .Ok => "Z_OK",
                .StreamEnd => "Z_STREAM_END",
                .NeedDict => "Z_NEED_DICT",
                .ErrNo => "Z_ERRNO",
                .StreamError => "Z_STREAM_ERROR",
                .DataError => "Z_DATA_ERROR",
                .MemError => "Z_MEM_ERROR",
                .BufError => "Z_BUF_ERROR",
                .VersionError => "Z_VERSION_ERROR",
            },
        };
    }

    pub fn reset(*_): Error {
        _.e = .Ok;
        switch (_.mode) {
            .DEFLATE, .DEFLATERAW, .GZIP => {
                _.e = c.deflateReset(&_.state);
            },
            .INFLATE, .INFLATERAW, .GUNZIP => {
                _.e = c.inflateReset(&_.state);
            },
            else => {},
        }
        if (_.e != .Ok) {
            return _.error_for_message("Failed to reset stream");
        }
        return _.setDictionary();
    }

    pub fn setBuffers(*_, in_buf: ([]const u8)?, out_buf: ([]u8)?) {
        let avail_in: u32 = 0;
        let next_in: ([*]const u8)? = null;
        if (p := in_buf) {
            avail_in = @intCast(p.len);
            next_in = p.ptr;
        }
        let avail_out: u32 = 0;
        let next_out: ([*]u8)? = null;
        if (p := out_buf) {
            avail_out = @intCast(p.len);
            next_out = p.ptr;
        }
        _.state.avail_in = avail_in;
        _.state.next_in = next_in;
        _.state.avail_out = avail_out;
        _.state.next_out = next_out;
    }

    pub fn setFlush(*_, flush: c_int) {
        _.flush = @enumFromInt(flush);
    }

    pub fn doWork(*_) {
        let next_expected_header_byte: ([*]const u8)? = null;

        switch (_.mode) {
            .DEFLATE, .GZIP, .DEFLATERAW => {
                return _.doWorkDeflate();
            },
            .UNZIP => {
                if (_.state.avail_in > 0) {
                    next_expected_header_byte = _.state.next_in.?;
                }
                if (_.gzip_id_bytes_read == 0) {
                    if (next_expected_header_byte == null) {
                        return _.doWorkInflate();
                    }
                    if (next_expected_header_byte.?[0] == GZIP_HEADER_ID1) {
                        _.gzip_id_bytes_read = 1;
                        unsafe {
                            next_expected_header_byte.? += 1;
                        }
                        if (_.state.avail_in == 1) {
                            return _.doWorkInflate();
                        }
                    } else {
                        _.mode = .INFLATE;
                        return _.doWorkInflate();
                    }
                }
                if (_.gzip_id_bytes_read == 1) {
                    if (next_expected_header_byte == null) {
                        return _.doWorkInflate();
                    }
                    if (next_expected_header_byte.?[0] == GZIP_HEADER_ID2) {
                        _.gzip_id_bytes_read = 2;
                        _.mode = .GUNZIP;
                    } else {
                        _.mode = .INFLATE;
                    }
                    return _.doWorkInflate();
                }
                bun.assert(false);
            },
            .INFLATE, .GUNZIP, .INFLATERAW => {
                return _.doWorkInflate();
            },
            .NONE => {},
            .BROTLI_ENCODE, .BROTLI_DECODE => {},
            .ZSTD_COMPRESS, .ZSTD_DECOMPRESS => {},
        }
    }

    fn doWorkDeflate(*_) {
        _.e = c.deflate(&_.state, _.flush);
    }

    fn doWorkInflate(*_) {
        _.e = c.inflate(&_.state, _.flush);

        if (_.mode != .INFLATERAW && _.e == .NeedDict && _.dictionary.len > 0) {
            _.e = c.inflateSetDictionary(&_.state, _.dictionary.ptr, @intCast(_.dictionary.len));

            if (_.e == .Ok) {
                _.e = c.inflate(&_.state, _.flush);
            } else if (_.e == .DataError) {
                _.e = .NeedDict;
            }
        }
        while (_.state.avail_in > 0 && _.mode == .GUNZIP && _.e == .StreamEnd && _.state.next_in.?[0] != 0) {
            * = _.reset();
            _.e = c.inflate(&_.state, _.flush);
        }
    }

    pub fn updateWriteResult(*_, avail_in: *u32, avail_out: *u32) {
        avail_in.* = _.state.avail_in;
        avail_out.* = _.state.avail_out;
    }

    pub fn getErrorInfo(*_): Error {
        switch (_.e) {
            .Ok, .BufError => {
                if (_.state.avail_out != 0 && _.flush == .Finish) {
                    return _.error_for_message("unexpected end of file");
                }
            },
            .StreamEnd => {},
            .NeedDict => {
                if (_.dictionary.len == 0) {
                    return _.error_for_message("Missing dictionary");
                } else {
                    return _.error_for_message("Bad dictionary");
                }
            },
            else => {
                return _.error_for_message("Zlib error");
            },
        }
        return Error.ok;
    }

    pub fn close(*_) {
        let status = c.ReturnCode.Ok;
        switch (_.mode) {
            .DEFLATE, .DEFLATERAW, .GZIP => {
                status = c.deflateEnd(&_.state);
            },
            .INFLATE, .INFLATERAW, .GUNZIP, .UNZIP => {
                status = c.inflateEnd(&_.state);
            },
            .NONE => {},
            .BROTLI_ENCODE, .BROTLI_DECODE => {},
            .ZSTD_COMPRESS, .ZSTD_DECOMPRESS => {},
        }
        bun.assert(status == .Ok || status == .DataError);
        _.mode = .NONE;
    }
}

@root struct NativeZlib {
    const RefCount = bun.ptr.RefCount(NativeZlib, "ref_count", deinit, {});
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub const js = jsc.Codegen.JSNativeZlib;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    const StreamImpl = CompressionStream(NativeZlib);
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

    pub static fn constructor(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!*NativeZlib {
        const arguments = callframe.argumentsUndef(4).ptr;

        let mode = arguments[0];
        if (!mode.isNumber()) {
            return globalThis.throwInvalidArgumentTypeValue("mode", "number", mode);
        }
        const mode_double = mode.asNumber();
        if (@mod(mode_double, 1.0) != 0.0) {
            return globalThis.throwInvalidArgumentTypeValue("mode", "integer", mode);
        }
        const mode_int: i64 = @intFromFloat(mode_double);
        if (mode_int < 1 || mode_int > 7) {
            return globalThis.throwRangeError(mode_int, { field_name: "mode", min: 1, max: 7 });
        }

        const ptr = bun.new(NativeZlib, {
            ref_count: RefCount.init(),
            globalThis: globalThis,
        });
        ptr.stream.mode = @enumFromInt(mode_int);
        return ptr;
    }

    pub fn estimatedSize(*_): usize {
        const internal_state_size = 3309;
        return @sizeOf(NativeZlib) + internal_state_size;
    }

    pub fn init(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsUndef(7).slice();
        const this_value = callframe.this();

        if (arguments.len != 7) {
            return globalThis.ERR(.MISSING_ARGS, "init(windowBits, level, memLevel, strategy, writeResult, writeCallback, dictionary)", {}).throw();
        }

        const windowBits = validators.validateInt32(globalThis, arguments[0], "windowBits", {}, null, null)?;
        const level = validators.validateInt32(globalThis, arguments[1], "level", {}, null, null)?;
        const memLevel = validators.validateInt32(globalThis, arguments[2], "memLevel", {}, null, null)?;
        const strategy = validators.validateInt32(globalThis, arguments[3], "strategy", {}, null, null)?;
        const writeResult = arguments[4].asArrayBuffer(globalThis).?.asU32().ptr;
        const writeCallback = validators.validateFunction(globalThis, "writeCallback", arguments[5])?;
        const dictionary = if (arguments[6].isUndefined()) null else arguments[6].asArrayBuffer(globalThis).?.byteSlice();

        _.write_result = writeResult;
        js.writeCallbackSetCached(this_value, globalThis, writeCallback.withAsyncContextIfNeeded(globalThis));

        if (dictionary != null) {
            js.dictionarySetCached(this_value, globalThis, arguments[6]);
        }

        _.stream.init(level, windowBits, memLevel, strategy, dictionary);

        return .js_undefined;
    }

    pub fn params(*_, globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        const arguments = callframe.argumentsUndef(2).slice();

        if (arguments.len != 2) {
            return globalThis.ERR(.MISSING_ARGS, "params(level, strategy)", {}).throw();
        }

        const level = validators.validateInt32(globalThis, arguments[0], "level", {}, null, null)?;
        const strategy = validators.validateInt32(globalThis, arguments[1], "strategy", {}, null, null)?;

        const e = _.stream.setParams(level, strategy);
        if (e.isError()) {
            StreamImpl.emitError(_, globalThis, callframe.this(), e);
        }
        return .js_undefined;
    }

    fn deinit(*_) {
        _.this_value.deinit();
        _.poll_ref.deinit();
        _.stream.close();
        bun.destroy(_);
    }
}
