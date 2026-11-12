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

const log = Output.scoped(.TextEncoderStreamEncoder, .visible);

export struct TextEncoderStreamEncoderPrepend {
    bytes: [4]u8;
    len: u8;

    pub const replacement: TextEncoderStreamEncoderPrepend = .{ .bytes = .{ 0xef, 0xbf, 0xbd, 0 }, .len = 3 };

    pub fn fromSequence(seq: [4]u8, length: u8): TextEncoderStreamEncoderPrepend {
        return .{ .bytes = seq, .len = length };
    }
}

export class TextEncoderStreamEncoder {
    pending_lead_surrogate: ?u16 = null;

    pub const js = jsc.Codegen.JSTextEncoderStreamEncoder;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    pub const new = TrivialNew(TextEncoderStreamEncoder);

    pub fn finalize(*this: *TextEncoderStreamEncoder): void {
        destroy(this);
    }

    pub fn constructor(_: *JSGlobalObject, _: *jsc.CallFrame): JSError!*TextEncoderStreamEncoder {
        return TextEncoderStreamEncoder.new(.{});
    }

    pub fn encode(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject, callFrame: *jsc.CallFrame): JSError!JSValue {
        const arguments = callFrame.arguments_old(1).slice();
        if (arguments.len == 0) {
            return globalObject.throwNotEnoughArguments("TextEncoderStreamEncoder.encode", 1, arguments.len);
        }

        const str = try arguments[0].getZigString(globalObject);

        if (str.is16Bit()) {
            return this.encodeUTF16(globalObject, str.utf16SliceAligned());
        }

        return this.encodeLatin1(globalObject, str.slice());
    }

    pub fn encodeWithoutTypeChecks(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject, input: *jsc.JSString): JSValue {
        const str = input.getZigString(globalObject);

        if (str.is16Bit()) {
            return this.encodeUTF16(globalObject, str.utf16SliceAligned());
        }

        return this.encodeLatin1(globalObject, str.slice());
    }

    fn encodeLatin1(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject, input: []const u8): JSValue {
        log("encodeLatin1: \"{s}\"", .{ input });

        if (input.len == 0) return JSUint8Array.createEmpty(globalObject);

        const prepend_replacement_len: usize = prepend_replacement: {
            if (this.pending_lead_surrogate != null) {
                this.pending_lead_surrogate = null;
                break :prepend_replacement @as(usize, 3);
            }

            break :prepend_replacement @as(usize, 0);
        };

        let buffer = ManagedList(u8).initCapacity(default_allocator, input.len + prepend_replacement_len) catch {
            return globalObject.throwOutOfMemoryValue();
        };
        if (prepend_replacement_len > 0) {
            buffer.appendSliceAssumeCapacity(&.{ 0xef, 0xbf, 0xbd });
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

        if (Environment.isDebug) {
            debugAssert(buffer.items.len == (simdutf.length.utf8.from.latin1(input) + prepend_replacement_len));
        }

        return JSUint8Array.fromBytes(globalObject, buffer.items);
    }

    fn encodeUTF16(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject, input: []const u16): JSValue {
        log("encodeUTF16: \"{f}\"", .{ fmt.utf16(input) });

        if (input.len == 0) return JSUint8Array.createEmpty(globalObject);

        let remain = input;

        const prepend: ?TextEncoderStreamEncoderPrepend = prepend: {
            if (this.pending_lead_surrogate) |lead| {
                this.pending_lead_surrogate = null;
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

                    break :prepend TextEncoderStreamEncoderPrepend.fromSequence(sequence, converted.utf8Width());
                }

                break :prepend TextEncoderStreamEncoderPrepend.replacement;
            }
            break :prepend null;
        };

        const length = simdutf.length.utf8.from.utf16.le(remain);
        let prepend_len: usize = if (prepend != null) prepend.?.len else 0;

        let buf = ManagedList(u8).initCapacity(
            default_allocator,
            length + prepend_len,
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
                    this.pending_lead_surrogate = pending_lead;
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

    pub fn flush(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject, _: *jsc.CallFrame): JSError!JSValue {
        return this.flushBody(globalObject);
    }

    pub fn flushWithoutTypeChecks(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject): JSValue {
        return this.flushBody(globalObject);
    }

    fn flushBody(*this: *TextEncoderStreamEncoder, globalObject: *JSGlobalObject): JSValue {
        return if (this.pending_lead_surrogate == null)
            JSUint8Array.createEmpty(globalObject)
        else
            JSUint8Array.fromBytesCopy(globalObject, &.{ 0xef, 0xbf, 0xbd });
    }
}
