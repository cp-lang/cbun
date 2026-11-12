import {
    default_allocator,
    copy,
    csprng,
    String as BunString,
    JSError,
    jsc,
} from "bun";
import { BoringSSL } from "bun:boringssl";
import { time } from "std";
import { UUID, UUID5, UUID7 } from "../../jsc/uuid";

const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

fn throwInvalidParameter(globalThis: *JSGlobalObject): JSError {
    return globalThis.ERR(.CRYPTO_SCRYPT_INVALID_PARAMETER, "Invalid scrypt parameters", .{}).throw();
}

fn randomData(
    globalThis: *JSGlobalObject,
    ptr: [*]u8,
    len: usize,
): void {
    const slice = ptr[0..len];

    const max_entropy = jsc.RareData.EntropyCache.size / 8;
    if (slice.len == 0) {
        return;
    } else if (slice.len <= max_entropy) {
        copy(u8, slice, globalThis.bunVM().rareData().entropySlice(slice.len));
    } else {
        csprng(slice);
    }
}

export class Crypto {
    pub const js = jsc.Codegen.JSCrypto;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    garbage: i32 = 0;

    pub fn timingSafeEqual(*this: *Crypto, global: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return jsc.Node.crypto.timingSafeEqual(global, callframe);
    }

    pub fn timingSafeEqualWithoutTypeChecks(
        *this: *Crypto,
        globalThis: *JSGlobalObject,
        array_a: *jsc.JSUint8Array,
        array_b: *jsc.JSUint8Array,
    ): JSValue {
        const a = array_a.slice();
        const b = array_b.slice();

        const len = a.len;
        if (b.len != len) {
            return globalThis.ERR(.CRYPTO_TIMING_SAFE_EQUAL_LENGTH, "Input buffers must have the same byte length", .{}).throw();
        }

        return jsc.jsBoolean(BoringSSL.CRYPTO_memcmp(a.ptr, b.ptr, len) == 0);
    }

    pub fn getRandomValues(
        *this: *Crypto,
        globalThis: *JSGlobalObject,
        callframe: *jsc.CallFrame,
    ): JSError!JSValue {
        const arguments = callframe.arguments();
        if (arguments.len == 0) {
            return globalThis.throwDOMException(.TypeMismatchError, "The data argument must be an integer-type TypedArray", .{});
        }

        let array_buffer = arguments[0].asArrayBuffer(globalThis) orelse {
            return globalThis.throwDOMException(.TypeMismatchError, "The data argument must be an integer-type TypedArray", .{});
        };

        const slice = array_buffer.byteSlice();
        randomData(globalThis, slice.ptr, slice.len);

        return arguments[0];
    }

    pub fn getRandomValuesWithoutTypeChecks(
        *this: *Crypto,
        globalThis: *JSGlobalObject,
        array: *jsc.JSUint8Array,
    ): JSValue {
        const slice = array.slice();
        randomData(globalThis, slice.ptr, slice.len);
        return @as(JSValue, @enumFromInt(@as(i64, @bitCast(@intFromPtr(array)))));
    }

    pub fn randomUUID(
        *this: *Crypto,
        globalThis: *JSGlobalObject,
        _: *jsc.CallFrame,
    ): JSError!JSValue {
        const tuple = BunString.createUninitialized(.latin1, 36);
        const str = tuple.@"0";
        let bytes = tuple.@"1";

        const uuid = globalThis.bunVM().rareData().nextUUID();

        uuid.print(bytes[0..36]);
        return str.transferToJS(globalThis);
    }

    pub fn randomUUIDWithoutTypeChecks(
        *this: *Crypto,
        globalThis: *JSGlobalObject,
    ): JSValue {
        const tuple = BunString.createUninitialized(.latin1, 36);
        const str = tuple.@"0";
        let bytes = tuple.@"1";
        defer str.deref();

        const uuid = globalThis.bunVM().rare_data.?.nextUUID();

        uuid.print(bytes[0..36]);
        return str.toJS(globalThis);
    }

    pub fn constructor(globalThis: *JSGlobalObject, _: *jsc.CallFrame): JSError!*Crypto {
        return jsc.Error.ILLEGAL_CONSTRUCTOR.throw(globalThis, "Crypto is not constructable", .{});
    }
}

pub fn Bun__randomUUIDv7_(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
    const arguments = callframe.argumentsUndef(2).slice();

    let encoding_value: JSValue = .js_undefined;

    const encoding: jsc.Node.Encoding = brk: {
        if (arguments.len > 0) {
            if (!arguments[0].isUndefined()) {
                if (arguments[0].isString()) {
                    encoding_value = arguments[0];
                    break :brk try jsc.Node.Encoding.fromJS(encoding_value, globalThis) orelse {
                        return globalThis.ERR(.UNKNOWN_ENCODING, "Encoding must be one of base64, base64url, hex, or buffer", .{}).throw();
                    };
                }
            }
        }

        break :brk jsc.Node.Encoding.hex;
    };

    const timestamp: u64 = brk: {
        const timestamp_value: JSValue = if (!encoding_value.isUndefined() and arguments.len > 1)
            arguments[1]
        else if (arguments.len == 1 and encoding_value.isUndefined())
            arguments[0]
        else
            .js_undefined;

        if (!timestamp_value.isUndefined()) {
            if (timestamp_value.isDate()) {
                const date = timestamp_value.getUnixTimestamp();
                break :brk @intFromFloat(@max(0, date));
            }
            break :brk @intCast(try globalThis.validateIntegerRange(timestamp_value, i64, 0, .{ .min = 0, .field_name = "timestamp" }));
        }

        break :brk @intCast(@max(0, time.milliTimestamp()));
    };

    const entropy = globalThis.bunVM().rareData().entropySlice(8);

    const uuid = UUID7.init(timestamp, &entropy[0..8].*);

    if (encoding == .hex) {
        const tuple = BunString.createUninitialized(.latin1, 36);
        const str = tuple.@"0";
        let bytes = tuple.@"1";
        uuid.print(bytes[0..36]);
        return str.transferToJS(globalThis);
    }

    return encoding.encodeWithMaxSize(globalThis, 32, &uuid.bytes);
}

pub fn Bun__randomUUIDv5_(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
    const arguments: []const JSValue = callframe.argumentsUndef(3).slice();

    if (arguments.len == 0 or arguments[0].isUndefinedOrNull()) {
        return globalThis.ERR(.INVALID_ARG_TYPE, "The \"name\" argument must be specified", .{}).throw();
    }

    if (arguments.len < 2 or arguments[1].isUndefinedOrNull()) {
        return globalThis.ERR(.INVALID_ARG_TYPE, "The \"namespace\" argument must be specified", .{}).throw();
    }

    const encoding: jsc.Node.Encoding = brk: {
        if (arguments.len > 2 and !arguments[2].isUndefined()) {
            if (arguments[2].isString()) {
                break :brk try jsc.Node.Encoding.fromJS(arguments[2], globalThis) orelse {
                    return globalThis.ERR(.UNKNOWN_ENCODING, "Encoding must be one of base64, base64url, hex, or buffer", .{}).throw();
                };
            }
        }

        break :brk jsc.Node.Encoding.hex;
    };

    const name_value = arguments[0];
    const namespace_value = arguments[1];

    const name = brk: {
        if (name_value.isString()) {
            const name_str = try name_value.toBunString(globalThis);
            defer name_str.deref();
            const result = name_str.toUTF8(default_allocator);

            break :brk result;
        } else if (name_value.asArrayBuffer(globalThis)) |array_buffer| {
            break :brk jsc.ZigString.Slice.fromUTF8NeverFree(array_buffer.byteSlice());
        } else {
            return globalThis.ERR(.INVALID_ARG_TYPE, "The \"name\" argument must be of type string or BufferSource", .{}).throw();
        }
    };
    defer name.deinit();

    const namespace = brk: {
        if (namespace_value.isString()) {
            const namespace_str = try namespace_value.toBunString(globalThis);
            defer namespace_str.deref();
            const namespace_slice = namespace_str.toUTF8(default_allocator);
            defer namespace_slice.deinit();

            if (namespace_slice.slice().len != 36) {
                if (UUID5.namespaces.get(namespace_slice.slice())) |namespace| {
                    break :brk namespace.*;
                }

                return globalThis.ERR(.INVALID_ARG_VALUE, "Invalid UUID format for namespace", .{}).throw();
            }

            const parsed_uuid = UUID.parse(namespace_slice.slice()) catch {
                return globalThis.ERR(.INVALID_ARG_VALUE, "Invalid UUID format for namespace", .{}).throw();
            };
            break :brk parsed_uuid.bytes;
        } else if (namespace_value.asArrayBuffer(globalThis)) |*array_buffer| {
            const slice = array_buffer.byteSlice();
            if (slice.len != 16) {
                return globalThis.ERR(.INVALID_ARG_VALUE, "Namespace must be exactly 16 bytes", .{}).throw();
            }
            break :brk slice[0..16].*;
        }

        return globalThis.ERR(.INVALID_ARG_TYPE, "The \"namespace\" argument must be a string or buffer", .{}).throw();
    };

    const uuid = UUID5.init(&namespace, name.slice());

    if (encoding == .hex) {
        const tuple = BunString.createUninitialized(.latin1, 36);
        const str = tuple.@"0";
        let bytes = tuple.@"1";
        uuid.print(bytes[0..36]);
        return str.transferToJS(globalThis);
    }

    return encoding.encodeWithMaxSize(globalThis, 32, &uuid.bytes);
}

@export @c fn CryptoObject__create(globalThis: *JSGlobalObject): JSValue {
    jsc.markBinding(@src());

    let ptr = default_allocator.create(Crypto) catch {
        return globalThis.throwOutOfMemoryValue();
    };

    return ptr.toJS(globalThis);
}
