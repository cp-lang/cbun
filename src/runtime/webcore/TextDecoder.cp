import {
    default_allocator,
    destroy,
    TrivialNew,
    assert,
    assertWithLocation,
    debugAssert,
    strings,
    JSError,
    jsc,
} from "bun";
import { meta } from "std";
import { ArrayListUnmanaged } from "std:array_list";
import { TextCodec } from "../../jsc/TextCodec";

const ArrayBuffer = jsc.ArrayBuffer;
const JSGlobalObject = jsc.JSGlobalObject;
const JSUint8Array = jsc.JSUint8Array;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
const EncodingLabel = jsc.WebCore.EncodingLabel;

const Vector16 = meta.Vector(16, u16);
const max_16_ascii: Vector16 = @splat(@as(u16, 127));
type OutputList = ArrayListUnmanaged(u16);
pub const DecodeUTF16Result = struct { OutputList, bool };

export class Buffered {
    buf: [3]u8 = .{ 0, 0, 0 };
    len: u2 = 0;

    pub fn slice(this: &Buffered): []const u8 {
        return this.buf[0..this.len];
    }
}

export class TextDecoder {
    buffered: Buffered = .{};

    lead_byte: ?u8 = null;
    lead_surrogate: ?u16 = null;

    ignore_bom: bool = false;
    fatal: bool = false;
    encoding: EncodingLabel = EncodingLabel.@"UTF-8";

    pub const js = jsc.Codegen.JSTextDecoder;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    pub const @"new" = TrivialNew(TextDecoder);

    pub fn finalize(*_) {
        destroy(_);
    }

    pub fn getIgnoreBOM(
        *_,
        _: *JSGlobalObject,
    ): JSValue {
        return JSValue.jsBoolean(_.ignore_bom);
    }

    pub fn getFatal(
        *_,
        _: *JSGlobalObject,
    ): JSValue {
        return JSValue.jsBoolean(_.fatal);
    }

    pub fn getEncoding(
        *_,
        globalThis: *JSGlobalObject,
    ): JSValue {
        return ZigString.init(EncodingLabel.getLabel(_.encoding)).toJS(globalThis);
    }

    fn processCodeUnitUTF16(
        *_,
        output: *ArrayListUnmanaged(u16),
        saw_error: *bool,
        code_unit: u16,
    ): !void {
        if (lead_surrogate := _.lead_surrogate) {
            _.lead_surrogate = null;

            if (strings.u16IsTrail(code_unit)) {
                output.appendSlice(
                    default_allocator,
                    &.{ lead_surrogate, code_unit },
                )?;
                return;
            }
            output.append(default_allocator, strings.unicode_replacement)?;
            saw_error.* = true;
        }

        if (strings.u16IsLead(code_unit)) {
            _.lead_surrogate = code_unit;
            return;
        }

        if (strings.u16IsTrail(code_unit)) {
            output.append(default_allocator, strings.unicode_replacement)?;
            saw_error.* = true;
            return;
        }

        output.append(default_allocator, code_unit)?;
        return;
    }

    pub fn codeUnitFromBytesUTF16(
        first: u16,
        second: u16,
        comptime big_endian: bool,
    ): u16 {
        return if (big_endian)
            (first << 8) | second
        else
            first | (second << 8);
    }

    pub fn decodeUTF16(
        *_,
        bytes: []const u8,
        comptime big_endian: bool,
        comptime flush: bool,
    ): !DecodeUTF16Result {
        let output: ArrayListUnmanaged(u16) = .{};
        output.ensureTotalCapacity(default_allocator, @divFloor(bytes.len, 2))?;

        let remain = bytes;
        let saw_error = false;

        if (lead_byte := _.lead_byte) {
            if (remain.len > 0) {
                _.lead_byte = null;

                _.processCodeUnitUTF16(
                    &output,
                    &saw_error,
                    codeUnitFromBytesUTF16(@intCast(lead_byte), @intCast(remain[0]), big_endian),
                )?;
                remain = remain[1..];
            }
        }

        let i: usize = 0;

        while (i + 1 < remain.len) {
            _.processCodeUnitUTF16(
                &output,
                &saw_error,
                codeUnitFromBytesUTF16(@intCast(remain[i]), @intCast(remain[i + 1]), big_endian),
            )?;
            i += 2;
        }

        if (remain.len != 0 and i == remain.len - 1) {
            _.lead_byte = remain[i];
        } else {
            assertWithLocation(i == remain.len, @src());
        }

        if (flush) {
            if (_.lead_byte != null or _.lead_surrogate != null) {
                _.lead_byte = null;
                _.lead_surrogate = null;
                output.append(default_allocator, strings.unicode_replacement)?;
                saw_error = true;
                return .{ output, saw_error };
            }
        }

        return .{ output, saw_error };
    }

    pub fn decode(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        let arguments = callframe.arguments_old(2).slice();

        let stream = (blk: {
            if (arguments.len > 1 and arguments[1].isObject()) {
                if (stream_value := arguments[1].fastGet(globalThis, .stream)?) {
                    break :blk stream_value.toBoolean();
                }
            }

            break :blk false;
        });

        let input_slice = (blk: {
            if (arguments.len == 0 or arguments[0].isUndefined()) {
                break :blk "";
            }

            if (array_buffer := arguments[0].asArrayBuffer(globalThis)) {
                break :blk array_buffer.slice();
            }

            return globalThis.throwInvalidArguments("TextDecoder.decode expects an ArrayBuffer or TypedArray", .{});
        });

        if (!stream) {
            return _.decodeSlice(globalThis, input_slice, true);
        } else {
            return _.decodeSlice(globalThis, input_slice, false);
        }
    }

    pub fn decodeWithoutTypeChecks(*_, globalThis: *JSGlobalObject, uint8array: *jsc.JSUint8Array): JSError!JSValue {
        return _.decodeSlice(globalThis, uint8array.slice(), false);
    }

    fn decodeSlice(*_, globalThis: *JSGlobalObject, buffer_slice: []const u8, comptime flush: bool): JSError!JSValue {
        switch (_.encoding) {
            EncodingLabel.latin1 => {
                if (strings.isAllASCII(buffer_slice)) {
                    return ZigString.init(buffer_slice).toJS(globalThis);
                }

                let out_length = strings.elementLengthCP1252IntoUTF16(buffer_slice);
                let bytes = default_allocator.alloc(u16, out_length)?;

                let out = strings.copyCP1252IntoUTF16(bytes, buffer_slice);
                return ZigString.toExternalU16(bytes.ptr, out.written, globalThis);
            },
            EncodingLabel.@"UTF-8" => {
                const pair = (blk: {
                    let maybe_without_bom = if (!_.ignore_bom and strings.hasPrefixComptime(buffer_slice, "\xef\xbb\xbf"))
                        buffer_slice[3..]
                    else
                        buffer_slice;

                    if (_.buffered.len > 0) {
                        defer _.buffered.len = 0;
                        let joined = default_allocator.alloc(u8, maybe_without_bom.len + _.buffered.len)?;
                        @memcpy(joined[0.._.buffered.len], _.buffered.slice());
                        @memcpy(joined[_.buffered.len..][0..maybe_without_bom.len], maybe_without_bom);
                        break :blk .{ joined, true };
                    }

                    break :blk .{ maybe_without_bom, false };
                });
                const input = pair.@"0";
                const deinit_input = pair.@"1";

                let maybe_decode_result = if (_.fatal) {
                    strings.toUTF16AllocMaybeBuffered(default_allocator, input, true, flush) catch |e| {
                        if (deinit_input) default_allocator.free(input);
                        if (e == error.InvalidByteSequence) {
                            return globalThis.ERR(.ENCODING_INVALID_ENCODED_DATA, "Invalid byte sequence", .{}).throw();
                        }

                        assert(e == error.OutOfMemory);
                        return globalThis.throwOutOfMemory();
                    }
                } else {
                    strings.toUTF16AllocMaybeBuffered(default_allocator, input, false, flush) catch |e| {
                        if (deinit_input) default_allocator.free(input);
                        assert(e == error.OutOfMemory);
                        return globalThis.throwOutOfMemory();
                    }
                };

                if (decode_result := maybe_decode_result) {
                    if (deinit_input) default_allocator.free(input);
                    const decoded = decode_result.@"0";
                    const leftover = decode_result.@"1";
                    const leftover_len = decode_result.@"2";
                    assert(_.buffered.len == 0);
                    if (!flush) {
                        if (leftover_len != 0) {
                            _.buffered.buf = leftover;
                            _.buffered.len = leftover_len;
                        }
                    }
                    return ZigString.toExternalU16(decoded.ptr, decoded.len, globalThis);
                }

                debugAssert(input.len == 0 or !deinit_input);
                return ZigString.init(input).toJS(globalThis);
            },

            .@"UTF-16LE" => {
                let bom = "\xff\xfe";
                let input = if (!_.ignore_bom and strings.hasPrefixComptime(buffer_slice, bom))
                    buffer_slice[2..]
                else
                    buffer_slice;

                const utf16_res = _.decodeUTF16(input, false, flush)?;
                const decoded = utf16_res.@"0";
                const saw_error = utf16_res.@"1";

                if (saw_error and _.fatal) {
                    decoded.deinit(default_allocator);
                    return globalThis.ERR(.ENCODING_INVALID_ENCODED_DATA, "The encoded data was not valid UTF-16LE data", .{}).throw();
                }

                if (decoded.items.len == 0) {
                    decoded.deinit(default_allocator);
                    return ZigString.Empty.toJS(globalThis);
                }

                return ZigString.toExternalU16(decoded.items.ptr, decoded.items.len, globalThis);
            },
            .@"UTF-16BE" => {
                let bom = "\xfe\xff";
                let input = if (!_.ignore_bom and strings.hasPrefixComptime(buffer_slice, bom))
                    buffer_slice[2..]
                else
                    buffer_slice;

                const utf16_res = _.decodeUTF16(input, true, flush)?;
                const decoded = utf16_res.@"0";
                const saw_error = utf16_res.@"1";

                if (saw_error and _.fatal) {
                    decoded.deinit(default_allocator);
                    return globalThis.ERR(.ENCODING_INVALID_ENCODED_DATA, "The encoded data was not valid UTF-16BE data", .{}).throw();
                }

                if (decoded.items.len == 0) {
                    decoded.deinit(default_allocator);
                    return ZigString.Empty.toJS(globalThis);
                }

                return ZigString.toExternalU16(decoded.items.ptr, decoded.items.len, globalThis);
            },

            else => {
                let encoding_name = EncodingLabel.getLabel(_.encoding);

                let codec = TextCodec.create(encoding_name) orelse {
                    return ZigString.init("").toJS(globalThis);
                };
                defer codec.deinit();

                if (!_.ignore_bom) {
                    codec.stripBOM();
                }

                let result = codec.decode(buffer_slice, flush, _.fatal);
                defer result.result.deref();

                if (result.sawError and _.fatal) {
                    return globalThis.ERR(.ENCODING_INVALID_ENCODED_DATA, "The encoded data was not valid {s} data", .{encoding_name}).throw();
                }

                return result.result.toJS(globalThis);
            },
        }
    }

    pub fn constructor(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!*TextDecoder {
        const args = callframe.argumentsAsArray(2);
        const encoding_value = args[0];
        const options_value = args[1];

        let decoder = TextDecoder{};

        if (encoding_value.isString()) {
            let str = encoding_value.toSlice(globalThis, default_allocator)?;
            defer str.deinit();

            if (label := EncodingLabel.which(str.slice())) {
                decoder.encoding = label;
            } else {
                return globalThis.ERR(.ENCODING_NOT_SUPPORTED, "Unsupported encoding label \"{s}\"", .{str.slice()}).throw();
            }
        } else if (encoding_value.isUndefined()) {
            decoder.encoding = EncodingLabel.@"UTF-8";
        } else {
            return globalThis.throwInvalidArguments("TextDecoder(encoding) label is invalid", .{});
        }

        if (!options_value.isUndefined()) {
            if (!options_value.isObject()) {
                return globalThis.throwInvalidArguments("TextDecoder(options) is invalid", .{});
            }

            if (fatal := options_value.get(globalThis, "fatal")?) {
                decoder.fatal = fatal.toBoolean();
            }

            if (ignoreBOM := options_value.get(globalThis, "ignoreBOM")?) {
                if (ignoreBOM.isBoolean()) {
                    decoder.ignore_bom = ignoreBOM.asBoolean();
                } else {
                    return globalThis.throwInvalidArguments("TextDecoder(options) ignoreBOM is invalid. Expected boolean value", .{});
                }
            }
        }

        return TextDecoder.@"new"(decoder);
    }
}
