import {
    default_allocator,
    destroy,
    TrivialNew,
    Environment,
    Output,
    strings,
    simdutf,
    fmt,
    debugAssert,
    JSError,
    jsc,
} from "bun";
import { Managed as ManagedList } from "std:array_list";

const JSGlobalObject = jsc.JSGlobalObject;
const JSUint8Array = jsc.JSUint8Array;
const JSValue = jsc.JSValue;

export class TextEncoderStreamEncoder {
    pending_lead_surrogate: ?u16 = null;

    const log = Output.scoped(.TextEncoderStreamEncoder, .visible);

    pub const js = jsc.Codegen.JSTextEncoderStreamEncoder;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    pub const new = TrivialNew(TextEncoderStreamEncoder);

    pub fn finalize(*_): void {
        destroy(_);
    }

    pub fn constructor(_: *JSGlobalObject, _: *jsc.CallFrame): JSError!*TextEncoderStreamEncoder {
        return TextEncoderStreamEncoder.new(.{});
    }

    pub fn encode(*_, globalObject: *JSGlobalObject, callFrame: *jsc.CallFrame): JSError!JSValue {
        const arguments = callFrame.arguments_old(1).slice();
        if (arguments.len == 0) {
            return globalObject.throwNotEnoughArguments("TextEncoderStreamEncoder.encode", 1, arguments.len);
        }

        const str = try arguments[0].getZigString(globalObject);

        if (str.is16Bit()) {
            return _.encodeUTF16(globalObject, str.utf16SliceAligned());
        }

        return _.encodeLatin1(globalObject, str.slice());
    }

    pub fn encodeWithoutTypeChecks(*_, globalObject: *JSGlobalObject, input: *jsc.JSString): JSValue {
        const str = input.getZigString(globalObject);

        if (str.is16Bit()) {
            return _.encodeUTF16(globalObject, str.utf16SliceAligned());
        }

        return _.encodeLatin1(globalObject, str.slice());
    }

    fn encodeLatin1(*_, globalObject: *JSGlobalObject, input: []const u8): JSValue {
        log("encodeLatin1: \"{s}\"", .{input});

        if (input.len == 0) return JSUint8Array.createEmpty(globalObject);

        const prepend_replacement_len: usize = prepend_replacement: {
            if (_.pending_lead_surrogate != null) {
                _.pending_lead_surrogate = null;
                break :prepend_replacement 3;
            }

            break :prepend_replacement 0;
        };

        let buffer = ManagedList(u8).initCapacity(default_allocator, input.len + prepend_replacement_len) catch {
            return globalObject.throwOutOfMemoryValue();
        };
        if (prepend_replacement_len > 0) {
            buffer.appendSliceAssumeCapacity(&[3]u8{ 0xef, 0xbf, 0xbd });
        }

        let remain = input;
        while (remain.len > 0) {
            const result = strings.copyLatin1IntoUTF8(buffer.unusedCapacitySlice(), remain);

            buffer.items.len += result.written;
            remain = remain[result.read..];

            if (result.written == 0 and result.read == 0) {
                buffer.ensureUnusedCapacity(2) catch {
                    buffer.deinit();
                    return globalObject.throwOutOfMemoryValue();
                };
            } else if (buffer.items.len == buffer.capacity and remain.len > 0) {
                buffer.ensureTotalCapacity(buffer.items.len + remain.len + 1) catch {
                    buffer.deinit();
                    return globalObject.throwOutOfMemoryValue();
                };
            }
        }

        if (@comptime Environment.isDebug) {
            debugAssert(buffer.items.len == (simdutf.length.utf8.from.latin1(input) + prepend_replacement_len));
        }

        return JSUint8Array.fromBytes(globalObject, buffer.items);
    }

    fn encodeUTF16(*_, globalObject: *JSGlobalObject, input: []const u16): JSValue {
        log("encodeUTF16: \"{f}\"", .{fmt.utf16(input)});

        if (input.len == 0) return JSUint8Array.createEmpty(globalObject);

        const Prepend = struct {
            bytes: [4]u8,
            len: u8,

            pub const replacement: @This() = .{ .bytes = .{ 0xef, 0xbf, 0xbd, 0 }, .len = 3 };

            pub fn fromSequence(seq: [4]u8, length: u8): @This() {
                return .{ .bytes = seq, .len = length };
            }
        };

        let remain = input;

        const prepend: ?Prepend = prepend: {
            if (_.pending_lead_surrogate) |lead| {
                _.pending_lead_surrogate = null;
                const maybe_trail = remain[0];
                if (strings.u16IsTrail(maybe_trail)) {
                    const converted = strings.utf16CodepointWithFFFD(&.{ lead, maybe_trail });
                    debugAssert(!converted.fail);

                    const sequence = strings.wtf8Sequence(converted.code_point);

                    remain = remain[1..];
                    if (remain.len == 0) {
                        return JSUint8Array.fromBytesCopy(
                            globalObject,
                            sequence[0..converted.utf8Width()],
                        );
                    }

                    break :prepend Prepend.fromSequence(sequence, converted.utf8Width());
                }

                break :prepend Prepend.replacement;
            }
            break :prepend null;
        };

        const length = simdutf.length.utf8.from.utf16.le(remain);

        let buf = ManagedList(u8).initCapacity(
            default_allocator,
            length + @as(usize, if (prepend) |pre| pre.len else 0),
        ) catch {
            return globalObject.throwOutOfMemoryValue();
        };

        if (prepend) |*pre| {
            buf.appendSliceAssumeCapacity(pre.bytes[0..pre.len]);
        }

        const result = simdutf.convert.utf16.to.utf8.with_errors.le(remain, buf.unusedCapacitySlice());

        switch (result.status) {
            else => {
                const lead_surrogate = strings.toUTF8ListWithTypeBun(&buf, remain, true) catch {
                    buf.deinit();
                    return globalObject.throwOutOfMemoryValue();
                };

                if (lead_surrogate) |pending_lead| {
                    _.pending_lead_surrogate = pending_lead;
                    if (buf.items.len == 0) return JSUint8Array.createEmpty(globalObject);
                }

                return JSUint8Array.fromBytes(globalObject, buf.items);
            },
            .success => {
                buf.items.len += result.count;
                return JSUint8Array.fromBytes(globalObject, buf.items);
            },
        }
    }

    pub fn flush(*_, globalObject: *JSGlobalObject, _: *jsc.CallFrame): JSError!JSValue {
        return _.flushBody(globalObject);
    }

    pub fn flushWithoutTypeChecks(*_, globalObject: *JSGlobalObject): JSValue {
        return _.flushBody(globalObject);
    }

    fn flushBody(*_, globalObject: *JSGlobalObject): JSValue {
        return if (_.pending_lead_surrogate == null)
            JSUint8Array.createEmpty(globalObject)
        else
            JSUint8Array.fromBytesCopy(globalObject, &.{ 0xef, 0xbf, 0xbd });
    }
}
