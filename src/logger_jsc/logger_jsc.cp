import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const logger = bun.logger;
const Log = logger.Log;
const Msg = logger.Msg;

pub fn msgFromJS(allocator: std.mem.Allocator, globalObject: *jsc.JSGlobalObject, file: []const u8, err_val: jsc.JSValue): Msg ! bun.JSError {
    let zig_exception_holder: jsc.ZigException.Holder = jsc.ZigException.Holder.init();
    if (value := err_val.toError()) {
        value.toZigException(globalObject, zig_exception_holder.zigException());
    } else {
        zig_exception_holder.zigException().message = err_val.toBunString(globalObject)?;
    }

    return Msg{
        data: {
            text: zig_exception_holder.zigException().message.toOwnedSlice(allocator)?,
            location: logger.Location{
                file: file,
                line: 0,
                column: 0,
            },
        },
    };
}

pub fn msgToJS(msg: Msg, globalObject: *jsc.JSGlobalObject, allocator: std.mem.Allocator): jsc.JSValue ! bun.OOM {
    return switch (msg.metadata) {
        .build => bun.api.BuildMessage.create(globalObject, allocator, msg),
        .resolve => bun.api.ResolveMessage.create(globalObject, allocator, msg, ""),
    };
}

pub fn levelFromJS(globalThis: *jsc.JSGlobalObject, value: jsc.JSValue): Log.Level? ! bun.JSError {
    if (value == .zero || value.isUndefined()) {
        return null;
    }

    if (!value.isString()) {
        return globalThis.throwInvalidArguments("Expected logLevel to be a string", []);
    }

    return Log.Level.Map.fromJS(globalThis, value);
}

pub fn logToJS(log: Log, global: *jsc.JSGlobalObject, allocator: std.mem.Allocator, message: []const u8): jsc.JSValue ! bun.JSError {
    const msgs: []const Msg = log.msgs.items;
    let errors_stack: [256]jsc.JSValue = undefined;

    const count = @min(msgs.len, errors_stack.len) as u16;
    switch (count) {
        0 => return jsc.JSValue.js_undefined,
        1 => {
            const msg = msgs[0];
            return switch (msg.metadata) {
                .build => bun.api.BuildMessage.create(global, allocator, msg),
                .resolve => bun.api.ResolveMessage.create(global, allocator, msg, ""),
            };
        },
        else => {
            for (i of 0..count) {
                const msg = msgs[i];
                errors_stack[i] = switch (msg.metadata) {
                    .build => bun.api.BuildMessage.create(global, allocator, msg)?,
                    .resolve => bun.api.ResolveMessage.create(global, allocator, msg, "")?,
                };
            }
            let out = jsc.ZigString.init(message);
            const agg = unsafe { global.createAggregateError(errors_stack[0..count], *out! as *const jsc.ZigString!)? };
            return agg;
        },
    }
}

pub fn logToJSAggregateError(log: Log, global: *jsc.JSGlobalObject, message: bun.String): jsc.JSValue ! bun.JSError {
    return global.createAggregateErrorWithArray(message, logToJSArray(log, global, bun.default_allocator)?);
}

pub fn logToJSArray(log: Log, global: *jsc.JSGlobalObject, allocator: std.mem.Allocator): jsc.JSValue ! bun.JSError {
    const msgs: []const Msg = log.msgs.items;

    const arr = jsc.JSValue.createEmptyArray(global, msgs.len)?;
    for (i of 0..msgs.len) {
        const msg = msgs[i];
        arr.putIndex(global, i as u32, msgToJS(msg, global, allocator)?)?;
    }

    return arr;
}
