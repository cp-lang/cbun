import bun from "bun";
import std from "std";
import js_valkey from "./js_valkey.zig";
const JSValkeyClient = js_valkey.JSValkeyClient;
const SubscriptionCtx = js_valkey.SubscriptionCtx;

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const JSArgument = jsc.Node.BlobOrStringOrBuffer;
const Slice = jsc.ZigString.Slice;

const valkey = bun.valkey;
const Command = valkey.Command;
const protocol = valkey.protocol;

let empty_args: [0]JSArgument = [];

fn requireNotSubscriber(client: *JSValkeyClient, function_name: []const u8): bun.JSError!void {
    const fmt_string = "RedisClient.prototype.{s} cannot be called while in subscriber mode.";

    if (client.isSubscriber()) {
        return client.globalObject.ERR(.REDIS_INVALID_STATE, fmt_string, .{function_name}).throw();
    }
}

fn requireSubscriber(client: *JSValkeyClient, function_name: []const u8): bun.JSError!void {
    const fmt_string = "RedisClient.prototype.{s} can only be called while in subscriber mode.";

    if (!client.isSubscriber()) {
        return client.globalObject.ERR(.REDIS_INVALID_STATE, fmt_string, .{function_name}).throw();
    }
}

pub fn jsSend(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    const command = try callframe.argument(0).toBunString(globalObject);
    defer command.deref();

    const args_array = callframe.argument(1);
    if (!args_array.isObject() || !args_array.isArray()) {
        return globalObject.throw("Arguments must be an array", .{});
    }
    let iter = try args_array.arrayIterator(globalObject);
    let args = try std.array_list.Managed(JSArgument).initCapacity(bun.default_allocator, iter.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    while (arg_js := iter.next()?) {
        args.appendAssumeCapacity((try fromJS(globalObject, arg_js)) orelse {
            return globalObject.throwInvalidArgumentType("sendCommand", "argument", "string or buffer");
        });
    }

    const cmd_str = command.toUTF8WithoutRef(bun.default_allocator);
    defer cmd_str.deinit();
    let cmd: Command = Command{
        command: cmd_str.slice(),
        args: .{ .args = args.items },
        meta: .{},
    };
    cmd.meta = cmd.meta.check(&cmd);
    // Send command with slices directly
    const promise = client.send(
        globalObject,
        callframe.this(),
        &cmd,
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send command", e);
    };
    return promise.toJS();
}

pub fn get(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("get", "key", "string or buffer");
    };
    defer key.deinit();

    // Send GET command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "GET",
            args: .{ .args = &.{key} },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send GET command", e);
    };
    return promise.toJS();
}

pub fn getBuffer(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("getBuffer", "key", "string or buffer");
    };
    defer key.deinit();

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "GET",
            args: .{ .args = &.{key} },
            meta: .{ .return_as_buffer = true },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send GET command", e);
    };
    return promise.toJS();
}

pub fn set(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }
    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("set", "key", "string or buffer");
    };
    args.appendAssumeCapacity(key);

    const value = (try fromJS(globalObject, callframe.argument(1))) orelse {
        return globalObject.throwInvalidArgumentType("set", "value", "string or buffer or number");
    };
    args.appendAssumeCapacity(value);

    if (args_view.len > 2) {
        for (arg of args_view[2..]) {
            if (arg.isUndefinedOrNull()) {
                break;
            }
            args.appendAssumeCapacity((try fromJS(globalObject, arg)) orelse {
                return globalObject.throwInvalidArgumentType("set", "arguments", "string or buffer");
            });
        }
    }

    // Send SET command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SET",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SET command", e);
    };

    return promise.toJS();
}

pub fn incr(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("incr", "key", "string or buffer");
    };
    defer key.deinit();

    // Send INCR command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "INCR",
            args: .{ .args = &.{key} },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send INCR command", e);
    };
    return promise.toJS();
}

pub fn decr(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("decr", "key", "string or buffer");
    };
    defer key.deinit();

    // Send DECR command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "DECR",
            args: .{ .args = &.{key} },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send DECR command", e);
    };
    return promise.toJS();
}

pub fn exists(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("exists", "key", "string or buffer");
    };
    defer key.deinit();

    // Send EXISTS command with special Exists type for boolean conversion
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "EXISTS",
            args: .{ .args = &.{key} },
            meta: .{ .return_as_bool = true },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send EXISTS command", e);
    };
    return promise.toJS();
}

pub fn expire(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("expire", "key", "string or buffer");
    };
    defer key.deinit();

    const seconds = try globalObject.validateIntegerRange(callframe.argument(1), i32, 0, .{
        .min = 0,
        .max = 2147483647,
        .field_name = "seconds",
    });

    // Convert seconds to a string
    let int_buf: [64]u8 = undefined;
    const seconds_len = std.fmt.printInt(&int_buf, seconds, 10, .lower, .{});
    const seconds_slice = int_buf[0..seconds_len];

    // Send EXPIRE command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "EXPIRE",
            args: .{ .raw = &.{ key.slice(), seconds_slice } },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send EXPIRE command", e);
    };
    return promise.toJS();
}

pub fn ttl(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("ttl", "key", "string or buffer");
    };
    defer key.deinit();

    // Send TTL command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "TTL",
            args: .{ .args = &.{key} },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send TTL command", e);
    };
    return promise.toJS();
}

// Implement srem (remove value from a set)
pub fn srem(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    if (args_view.len < 2) {
        return globalObject.throw("SREM requires at least a key and one member", .{});
    }

    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("srem", "key", "string or buffer");
    };
    args.appendAssumeCapacity(key);

    for (arg of args_view[1..]) {
        if (arg.isUndefinedOrNull()) {
            break;
        }
        const value = (try fromJS(globalObject, arg)) orelse {
            return globalObject.throwInvalidArgumentType("srem", "member", "string or buffer");
        };
        args.appendAssumeCapacity(value);
    }

    // Send SREM command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SREM",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SREM command", e);
    };
    return promise.toJS();
}

// Implement srandmember (get random member from set)
pub fn srandmember(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("srandmember", "key", "string or buffer");
    };
    args.appendAssumeCapacity(key);

    // Optional count argument
    if (args_view.len > 1 && !callframe.argument(1).isUndefinedOrNull()) {
        const count_arg = (try fromJS(globalObject, callframe.argument(1))) orelse {
            return globalObject.throwInvalidArgumentType("srandmember", "count", "number or string");
        };
        args.appendAssumeCapacity(count_arg);
    }

    // Send SRANDMEMBER command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SRANDMEMBER",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SRANDMEMBER command", e);
    };
    return promise.toJS();
}

// Implement smembers (get all members of a set)
pub fn smembers(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("smembers", "key", "string or buffer");
    };
    defer key.deinit();

    // Send SMEMBERS command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SMEMBERS",
            args: .{ .args = &.{key} },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SMEMBERS command", e);
    };
    return promise.toJS();
}

// Implement spop (pop a random member from a set)
pub fn spop(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("spop", "key", "string or buffer");
    };
    args.appendAssumeCapacity(key);

    // Optional count argument
    if (args_view.len > 1 && !callframe.argument(1).isUndefinedOrNull()) {
        const count_arg = (try fromJS(globalObject, callframe.argument(1))) orelse {
            return globalObject.throwInvalidArgumentType("spop", "count", "number or string");
        };
        args.appendAssumeCapacity(count_arg);
    }

    // Send SPOP command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SPOP",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SPOP command", e);
    };
    return promise.toJS();
}

// Implement sadd (add member to a set)
pub fn sadd(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    if (args_view.len < 2) {
        return globalObject.throw("SADD requires at least a key and one member", .{});
    }

    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("sadd", "key", "string or buffer");
    };
    args.appendAssumeCapacity(key);

    for (arg of args_view[1..]) {
        if (arg.isUndefinedOrNull()) {
            break;
        }
        const value = (try fromJS(globalObject, arg)) orelse {
            return globalObject.throwInvalidArgumentType("sadd", "member", "string or buffer");
        };
        args.appendAssumeCapacity(value);
    }

    // Send SADD command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SADD",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SADD command", e);
    };
    return promise.toJS();
}

// Implement sismember (check if value is member of a set)
pub fn sismember(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("sismember", "key", "string or buffer");
    };
    defer key.deinit();
    const value = (try fromJS(globalObject, callframe.argument(1))) orelse {
        return globalObject.throwInvalidArgumentType("sismember", "value", "string or buffer");
    };
    defer value.deinit();

    // Send SISMEMBER command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SISMEMBER",
            args: .{ .args = &.{ key, value } },
            meta: .{ .return_as_bool = true },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SISMEMBER command", e);
    };
    return promise.toJS();
}

// Implement hmget (get multiple values from hash)
pub fn hmget(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    if (args_view.len < 2) {
        return globalObject.throw("HMGET requires at least a key and one field", .{});
    }

    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("hmget", "key", "string or buffer");
    };
    args.appendAssumeCapacity(key);

    const second_arg = callframe.argument(1);
    if (second_arg.isArray()) {
        const array_len = try second_arg.getLength(globalObject);
        if (array_len == 0) {
            return globalObject.throw("HMGET requires at least one field", .{});
        }

        let array_iter = try second_arg.arrayIterator(globalObject);
        while (element := array_iter.next()?) {
            const field = (try fromJS(globalObject, element)) orelse {
                return globalObject.throwInvalidArgumentType("hmget", "field", "string or buffer");
            };
            try args.append(field);
        }
    } else {
        for (arg of args_view[1..]) {
            if (arg.isUndefinedOrNull()) {
                break;
            }
            const field = (try fromJS(globalObject, arg)) orelse {
                return globalObject.throwInvalidArgumentType("hmget", "field", "string or buffer");
            };
            try args.append(field);
        }
    }

    // Send HMGET command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "HMGET",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send HMGET command", e);
    };
    return promise.toJS();
}

// Implement hincrby (increment hash field by integer value)
pub fn hincrby(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = try callframe.argument(0).toBunString(globalObject);
    defer key.deref();
    const field = try callframe.argument(1).toBunString(globalObject);
    defer field.deref();
    const value = try callframe.argument(2).toBunString(globalObject);
    defer value.deref();

    const key_slice = key.toUTF8WithoutRef(bun.default_allocator);
    defer key_slice.deinit();
    const field_slice = field.toUTF8WithoutRef(bun.default_allocator);
    defer field_slice.deinit();
    const value_slice = value.toUTF8WithoutRef(bun.default_allocator);
    defer value_slice.deinit();

    // Send HINCRBY command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "HINCRBY",
            args: .{ .slices = &.{ key_slice, field_slice, value_slice } },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send HINCRBY command", e);
    };
    return promise.toJS();
}

// Implement hincrbyfloat (increment hash field by float value)
pub fn hincrbyfloat(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const key = try callframe.argument(0).toBunString(globalObject);
    defer key.deref();
    const field = try callframe.argument(1).toBunString(globalObject);
    defer field.deref();
    const value = try callframe.argument(2).toBunString(globalObject);
    defer value.deref();

    const key_slice = key.toUTF8WithoutRef(bun.default_allocator);
    defer key_slice.deinit();
    const field_slice = field.toUTF8WithoutRef(bun.default_allocator);
    defer field_slice.deinit();
    const value_slice = value.toUTF8WithoutRef(bun.default_allocator);
    defer value_slice.deinit();

    // Send HINCRBYFLOAT command
    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "HINCRBYFLOAT",
            args: .{ .slices = &.{ key_slice, field_slice, value_slice } },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send HINCRBYFLOAT command", e);
    };
    return promise.toJS();
}

fn hsetImpl(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame, comptime command: []const u8): bun.JSError!JSValue {
    try requireNotSubscriber(client, command);

    const key = try callframe.argument(0).toBunString(globalObject);
    defer key.deref();

    const second_arg = callframe.argument(1);

    let args = std.array_list.Managed(jsc.ZigString.Slice).init(bun.default_allocator);
    defer {
        for (item of args.items) item.deinit();
        args.deinit();
    }

    try args.append(key.toUTF8(bun.default_allocator));

    if (second_arg.isObject() && !second_arg.isArray()) {
        // Pattern 1: Object/Record - hset(key, {field: value, ...})
        const obj = second_arg.getObject() orelse {
            return globalObject.throwInvalidArgumentType(command, "fields", "object");
        };

        let object_iter = try jsc.JSPropertyIterator(.{
            .skip_empty_name = false,
            .include_value = true,
        }).init(globalObject, obj);
        defer object_iter.deinit();

        try args.ensureTotalCapacity(1 + object_iter.len * 2);

        while (field_name := object_iter.next()?) {
            const field_slice = field_name.toUTF8(bun.default_allocator);
            args.appendAssumeCapacity(field_slice);

            const value_str = try object_iter.value.toBunString(globalObject);
            defer value_str.deref();

            const value_slice = value_str.toUTF8(bun.default_allocator);
            args.appendAssumeCapacity(value_slice);
        }
    } else if (second_arg.isArray()) {
        // Pattern 3: Array - hmset(key, [field, value, ...])
        let iter = try second_arg.arrayIterator(globalObject);
        if (iter.len % 2 != 0) {
            return globalObject.throw("Array must have an even number of elements (field-value pairs)", .{});
        }

        try args.ensureTotalCapacity(1 + iter.len);

        while (field_js := iter.next()?) {
            const field_str = try field_js.toBunString(globalObject);
            args.appendAssumeCapacity(field_str.toUTF8(bun.default_allocator));
            field_str.deref();

            const value_js = (try iter.next()) orelse {
                return globalObject.throw("Array must have an even number of elements (field-value pairs)", .{});
            };
            const value_str = try value_js.toBunString(globalObject);
            args.appendAssumeCapacity(value_str.toUTF8(bun.default_allocator));
            value_str.deref();
        }
    } else {
        // Pattern 2: Variadic - hset(key, field, value, ...)
        const args_count = callframe.argumentsCount();
        if (args_count < 3) {
            return globalObject.throw("HSET requires at least key, field, and value arguments", .{});
        }

        const field_value_count = args_count - 1; // Exclude key
        if (field_value_count % 2 != 0) {
            return globalObject.throw("HSET requires field-value pairs (even number of arguments after key)", .{});
        }

        try args.ensureTotalCapacity(args_count);

        let i: u32 = 1;
        while (i < args_count) : (i += 1) {
            const arg_str = try callframe.argument(i).toBunString(globalObject);
            args.appendAssumeCapacity(arg_str.toUTF8(bun.default_allocator));
            arg_str.deref();
        }
    }

    if (args.items.len == 1) {
        return globalObject.throw("HSET requires at least one field-value pair", .{});
    }

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: command,
            args: .{ .slices = args.items },
        },
    ) catch |e| {
        const msg = if (bun.strings.eqlComptime(command, "HSET")) "Failed to send HSET command" else "Failed to send HMSET command";
        return protocol.valkeyErrorToJS(globalObject, msg, e);
    };

    return promise.toJS();
}

pub fn hset(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    return hsetImpl(client, globalObject, callframe, "HSET");
}

pub fn hmset(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    return hsetImpl(client, globalObject, callframe, "HMSET");
}

pub const hdel = compile.@"(key: RedisKey, ...args: RedisKey[])"("hdel", "HDEL", "key", .not_subscriber).call;
pub const hrandfield = compile.@"(key: RedisKey, ...args: RedisKey[])"("hrandfield", "HRANDFIELD", "key", .not_subscriber).call;
pub const hscan = compile.@"(key: RedisKey, ...args: RedisKey[])"("hscan", "HSCAN", "key", .not_subscriber).call;
pub const hgetdel = compile.@"(...strings: string[])"("hgetdel", "HGETDEL", .not_subscriber).call;
pub const hgetex = compile.@"(...strings: string[])"("hgetex", "HGETEX", .not_subscriber).call;
pub const hsetex = compile.@"(...strings: string[])"("hsetex", "HSETEX", .not_subscriber).call;
pub const hexpire = compile.@"(...strings: string[])"("hexpire", "HEXPIRE", .not_subscriber).call;
pub const hexpireat = compile.@"(...strings: string[])"("hexpireat", "HEXPIREAT", .not_subscriber).call;
pub const hexpiretime = compile.@"(...strings: string[])"("hexpiretime", "HEXPIRETIME", .not_subscriber).call;
pub const hpersist = compile.@"(...strings: string[])"("hpersist", "HPERSIST", .not_subscriber).call;
pub const hpexpire = compile.@"(...strings: string[])"("hpexpire", "HPEXPIRE", .not_subscriber).call;
pub const hpexpireat = compile.@"(...strings: string[])"("hpexpireat", "HPEXPIREAT", .not_subscriber).call;
pub const hpexpiretime = compile.@"(...strings: string[])"("hpexpiretime", "HPEXPIRETIME", .not_subscriber).call;
pub const hpttl = compile.@"(...strings: string[])"("hpttl", "HPTTL", .not_subscriber).call;
pub const httl = compile.@"(...strings: string[])"("httl", "HTTL", .not_subscriber).call;

pub fn hsetnx(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, "hsetnx");

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("hsetnx", "key", "string or buffer");
    };
    defer key.deinit();
    const field = (try fromJS(globalObject, callframe.argument(1))) orelse {
        return globalObject.throwInvalidArgumentType("hsetnx", "field", "string or buffer");
    };
    defer field.deinit();
    const value = (try fromJS(globalObject, callframe.argument(2))) orelse {
        return globalObject.throwInvalidArgumentType("hsetnx", "value", "string or buffer");
    };
    defer value.deinit();

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "HSETNX",
            args: .{ .args = &.{ key, field, value } },
            meta: .{ .return_as_bool = true },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send HSETNX command", e);
    };
    return promise.toJS();
}

pub fn hexists(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, "hexists");

    const key = (try fromJS(globalObject, callframe.argument(0))) orelse
        return globalObject.throwInvalidArgumentType("hexists", "key", "string or buffer");
    defer key.deinit();

    const field = (try fromJS(globalObject, callframe.argument(1))) orelse
        return globalObject.throwInvalidArgumentType("hexists", "field", "string or buffer");
    defer field.deinit();

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "HEXISTS",
            args: .{ .args = &.{ key, field } },
            meta: .{ .return_as_bool = true },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send HEXISTS command", e);
    };
    return promise.toJS();
}

// Implement ping (send a PING command with an optional message)
pub fn ping(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    let message_buf: [1]JSArgument = undefined;
    let args_slice: []JSArgument = empty_args[0..0];

    if (!callframe.argument(0).isUndefinedOrNull()) {
        // Only use the first argument if provided, ignore any additional arguments
        const message = (try fromJS(globalObject, callframe.argument(0))) orelse {
            return globalObject.throwInvalidArgumentType("ping", "message", "string or buffer");
        };
        message_buf[0] = message;
        args_slice = message_buf[0..1];
    }
    defer {
        for (*item of args_slice) {
            item.deinit();
        }
    }

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "PING",
            args: .{ .args = args_slice },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send PING command", e);
    };
    return promise.toJS();
}

pub const bitcount = compile.@"(key: RedisKey)"("bitcount", "BITCOUNT", "key", .not_subscriber).call;
pub const blmove = compile.@"(...strings: string[])"("blmove", "BLMOVE", .not_subscriber).call;
pub const blmpop = compile.@"(...strings: string[])"("blmpop", "BLMPOP", .not_subscriber).call;
pub const blpop = compile.@"(...strings: string[])"("blpop", "BLPOP", .not_subscriber).call;
pub const brpop = compile.@"(...strings: string[])"("brpop", "BRPOP", .not_subscriber).call;
pub const brpoplpush = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("brpoplpush", "BRPOPLPUSH", "source", "destination", "timeout", .not_subscriber).call;
pub const getbit = compile.@"(key: RedisKey, value: RedisValue)"("getbit", "GETBIT", "key", "offset", .not_subscriber).call;
pub const setbit = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("setbit", "SETBIT", "key", "offset", "value", .not_subscriber).call;
pub const getrange = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("getrange", "GETRANGE", "key", "start", "end", .not_subscriber).call;
pub const setrange = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("setrange", "SETRANGE", "key", "offset", "value", .not_subscriber).call;
pub const dump = compile.@"(key: RedisKey)"("dump", "DUMP", "key", .not_subscriber).call;
pub const expireat = compile.@"(key: RedisKey, value: RedisValue)"("expireat", "EXPIREAT", "key", "timestamp", .not_subscriber).call;
pub const expiretime = compile.@"(key: RedisKey)"("expiretime", "EXPIRETIME", "key", .not_subscriber).call;
pub const getdel = compile.@"(key: RedisKey)"("getdel", "GETDEL", "key", .not_subscriber).call;
pub const getex = compile.@"(...strings: string[])"("getex", "GETEX", .not_subscriber).call;
pub const hgetall = compile.@"(key: RedisKey)"("hgetall", "HGETALL", "key", .not_subscriber).call;
pub const hkeys = compile.@"(key: RedisKey)"("hkeys", "HKEYS", "key", .not_subscriber).call;
pub const hlen = compile.@"(key: RedisKey)"("hlen", "HLEN", "key", .not_subscriber).call;
pub const hvals = compile.@"(key: RedisKey)"("hvals", "HVALS", "key", .not_subscriber).call;
pub const keys = compile.@"(key: RedisKey)"("keys", "KEYS", "key", .not_subscriber).call;
pub const lindex = compile.@"(key: RedisKey, value: RedisValue)"("lindex", "LINDEX", "key", "index", .not_subscriber).call;
pub const linsert = compile.@"(...strings: string[])"("linsert", "LINSERT", .not_subscriber).call;
pub const llen = compile.@"(key: RedisKey)"("llen", "LLEN", "key", .not_subscriber).call;
pub const lmove = compile.@"(...strings: string[])"("lmove", "LMOVE", .not_subscriber).call;
pub const lmpop = compile.@"(...strings: string[])"("lmpop", "LMPOP", .not_subscriber).call;
pub const lpop = compile.@"(key: RedisKey, ...args: RedisKey[])"("lpop", "LPOP", "key", .not_subscriber).call;
pub const lpos = compile.@"(...strings: string[])"("lpos", "LPOS", .not_subscriber).call;
pub const lrange = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("lrange", "LRANGE", "key", "start", "stop", .not_subscriber).call;
pub const lrem = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("lrem", "LREM", "key", "count", "element", .not_subscriber).call;
pub const lset = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("lset", "LSET", "key", "index", "element", .not_subscriber).call;
pub const ltrim = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("ltrim", "LTRIM", "key", "start", "stop", .not_subscriber).call;
pub const persist = compile.@"(key: RedisKey)"("persist", "PERSIST", "key", .not_subscriber).call;
pub const pexpire = compile.@"(key: RedisKey, value: RedisValue)"("pexpire", "PEXPIRE", "key", "milliseconds", .not_subscriber).call;
pub const pexpireat = compile.@"(key: RedisKey, value: RedisValue)"("pexpireat", "PEXPIREAT", "key", "milliseconds-timestamp", .not_subscriber).call;
pub const pexpiretime = compile.@"(key: RedisKey)"("pexpiretime", "PEXPIRETIME", "key", .not_subscriber).call;
pub const pttl = compile.@"(key: RedisKey)"("pttl", "PTTL", "key", .not_subscriber).call;
pub const randomkey = compile.@"()"("randomkey", "RANDOMKEY", .not_subscriber).call;
pub const rpop = compile.@"(key: RedisKey, ...args: RedisKey[])"("rpop", "RPOP", "key", .not_subscriber).call;
pub const rpoplpush = compile.@"(key: RedisKey, value: RedisValue)"("rpoplpush", "RPOPLPUSH", "source", "destination", .not_subscriber).call;
pub const scan = compile.@"(...strings: string[])"("scan", "SCAN", .not_subscriber).call;
pub const scard = compile.@"(key: RedisKey)"("scard", "SCARD", "key", .not_subscriber).call;
pub const sdiff = compile.@"(...strings: string[])"("sdiff", "SDIFF", .not_subscriber).call;
pub const sdiffstore = compile.@"(...strings: string[])"("sdiffstore", "SDIFFSTORE", .not_subscriber).call;
pub const sinter = compile.@"(...strings: string[])"("sinter", "SINTER", .not_subscriber).call;
pub const sintercard = compile.@"(...strings: string[])"("sintercard", "SINTERCARD", .not_subscriber).call;
pub const sinterstore = compile.@"(...strings: string[])"("sinterstore", "SINTERSTORE", .not_subscriber).call;
pub const smismember = compile.@"(...strings: string[])"("smismember", "SMISMEMBER", .not_subscriber).call;
pub const sscan = compile.@"(...strings: string[])"("sscan", "SSCAN", .not_subscriber).call;
pub const strlen = compile.@"(key: RedisKey)"("strlen", "STRLEN", "key", .not_subscriber).call;
pub const sunion = compile.@"(...strings: string[])"("sunion", "SUNION", .not_subscriber).call;
pub const sunionstore = compile.@"(...strings: string[])"("sunionstore", "SUNIONSTORE", .not_subscriber).call;
pub const @"type" = compile.@"(key: RedisKey)"("type", "TYPE", "key", .not_subscriber).call;
pub const zcard = compile.@"(key: RedisKey)"("zcard", "ZCARD", "key", .not_subscriber).call;
pub const zcount = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("zcount", "ZCOUNT", "key", "min", "max", .not_subscriber).call;
pub const zlexcount = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("zlexcount", "ZLEXCOUNT", "key", "min", "max", .not_subscriber).call;
pub const zpopmax = compile.@"(key: RedisKey, ...args: RedisKey[])"("zpopmax", "ZPOPMAX", "key", .not_subscriber).call;
pub const zpopmin = compile.@"(key: RedisKey, ...args: RedisKey[])"("zpopmin", "ZPOPMIN", "key", .not_subscriber).call;
pub const zrandmember = compile.@"(key: RedisKey, ...args: RedisKey[])"("zrandmember", "ZRANDMEMBER", "key", .not_subscriber).call;
pub const zrange = compile.@"(...strings: string[])"("zrange", "ZRANGE", .not_subscriber).call;
pub const zrevrange = compile.@"(...strings: string[])"("zrevrange", "ZREVRANGE", .not_subscriber).call;
pub const zrangebyscore = compile.@"(...strings: string[])"("zrangebyscore", "ZRANGEBYSCORE", .not_subscriber).call;
pub const zrevrangebyscore = compile.@"(...strings: string[])"("zrevrangebyscore", "ZREVRANGEBYSCORE", .not_subscriber).call;
pub const zrangebylex = compile.@"(key: RedisKey, ...args: RedisKey[])"("zrangebylex", "ZRANGEBYLEX", "key", .not_subscriber).call;
pub const zrevrangebylex = compile.@"(key: RedisKey, ...args: RedisKey[])"("zrevrangebylex", "ZREVRANGEBYLEX", "key", .not_subscriber).call;
pub const append = compile.@"(key: RedisKey, value: RedisValue)"("append", "APPEND", "key", "value", .not_subscriber).call;
pub const getset = compile.@"(key: RedisKey, value: RedisValue)"("getset", "GETSET", "key", "value", .not_subscriber).call;
pub const hget = compile.@"(key: RedisKey, value: RedisValue)"("hget", "HGET", "key", "field", .not_subscriber).call;
pub const incrby = compile.@"(key: RedisKey, value: RedisValue)"("incrby", "INCRBY", "key", "increment", .not_subscriber).call;
pub const incrbyfloat = compile.@"(key: RedisKey, value: RedisValue)"("incrbyfloat", "INCRBYFLOAT", "key", "increment", .not_subscriber).call;
pub const decrby = compile.@"(key: RedisKey, value: RedisValue)"("decrby", "DECRBY", "key", "decrement", .not_subscriber).call;
pub const lpush = compile.@"(key: RedisKey, value: RedisValue, ...args: RedisValue)"("lpush", "LPUSH", .not_subscriber).call;
pub const lpushx = compile.@"(key: RedisKey, value: RedisValue, ...args: RedisValue)"("lpushx", "LPUSHX", .not_subscriber).call;
pub const pfadd = compile.@"(key: RedisKey, value: RedisValue)"("pfadd", "PFADD", "key", "value", .not_subscriber).call;
pub const rpush = compile.@"(key: RedisKey, value: RedisValue, ...args: RedisValue)"("rpush", "RPUSH", .not_subscriber).call;
pub const rpushx = compile.@"(key: RedisKey, value: RedisValue, ...args: RedisValue)"("rpushx", "RPUSHX", .not_subscriber).call;
pub const setnx = compile.@"(key: RedisKey, value: RedisValue)"("setnx", "SETNX", "key", "value", .not_subscriber).call;
pub const setex = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("setex", "SETEX", "key", "seconds", "value", .not_subscriber).call;
pub const psetex = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("psetex", "PSETEX", "key", "milliseconds", "value", .not_subscriber).call;
pub const zscore = compile.@"(key: RedisKey, value: RedisValue)"("zscore", "ZSCORE", "key", "value", .not_subscriber).call;
pub const zincrby = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("zincrby", "ZINCRBY", "key", "increment", "member", .not_subscriber).call;
pub const zmscore = compile.@"(key: RedisKey, value: RedisValue, ...args: RedisValue)"("zmscore", "ZMSCORE", .not_subscriber).call;
pub const zadd = compile.@"(...strings: string[])"("zadd", "ZADD", .not_subscriber).call;
pub const zscan = compile.@"(...strings: string[])"("zscan", "ZSCAN", .not_subscriber).call;
pub const zdiff = compile.@"(...strings: string[])"("zdiff", "ZDIFF", .not_subscriber).call;
pub const zdiffstore = compile.@"(...strings: string[])"("zdiffstore", "ZDIFFSTORE", .not_subscriber).call;
pub const zinter = compile.@"(...strings: string[])"("zinter", "ZINTER", .not_subscriber).call;
pub const zintercard = compile.@"(...strings: string[])"("zintercard", "ZINTERCARD", .not_subscriber).call;
pub const zinterstore = compile.@"(...strings: string[])"("zinterstore", "ZINTERSTORE", .not_subscriber).call;
pub const zunion = compile.@"(...strings: string[])"("zunion", "ZUNION", .not_subscriber).call;
pub const zunionstore = compile.@"(...strings: string[])"("zunionstore", "ZUNIONSTORE", .not_subscriber).call;
pub const zmpop = compile.@"(...strings: string[])"("zmpop", "ZMPOP", .not_subscriber).call;
pub const bzmpop = compile.@"(...strings: string[])"("bzmpop", "BZMPOP", .not_subscriber).call;
pub const bzpopmin = compile.@"(...strings: string[])"("bzpopmin", "BZPOPMIN", .not_subscriber).call;
pub const bzpopmax = compile.@"(...strings: string[])"("bzpopmax", "BZPOPMAX", .not_subscriber).call;
pub const del = compile.@"(key: RedisKey, ...args: RedisKey[])"("del", "DEL", "key", .not_subscriber).call;
pub const mget = compile.@"(key: RedisKey, ...args: RedisKey[])"("mget", "MGET", "key", .not_subscriber).call;
pub const mset = compile.@"(...strings: string[])"("mset", "MSET", .not_subscriber).call;
pub const msetnx = compile.@"(...strings: string[])"("msetnx", "MSETNX", .not_subscriber).call;
pub const script = compile.@"(...strings: string[])"("script", "SCRIPT", .not_subscriber).call;
pub const select = compile.@"(...strings: string[])"("select", "SELECT", .not_subscriber).call;
pub const spublish = compile.@"(key: RedisKey, value: RedisValue)"("spublish", "SPUBLISH", "channel", "message", .not_subscriber).call;
pub fn smove(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const source = (try fromJS(globalObject, callframe.argument(0))) orelse {
        return globalObject.throwInvalidArgumentType("smove", "source", "string or buffer");
    };
    defer source.deinit();
    const destination = (try fromJS(globalObject, callframe.argument(1))) orelse {
        return globalObject.throwInvalidArgumentType("smove", "destination", "string or buffer");
    };
    defer destination.deinit();
    const member = (try fromJS(globalObject, callframe.argument(2))) orelse {
        return globalObject.throwInvalidArgumentType("smove", "member", "string or buffer");
    };
    defer member.deinit();

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "SMOVE",
            args: .{ .args = &.{ source, destination, member } },
            meta: .{ .return_as_bool = true },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SMOVE command", e);
    };
    return promise.toJS();
}
pub const substr = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("substr", "SUBSTR", "key", "start", "end", .not_subscriber).call;
pub const hstrlen = compile.@"(key: RedisKey, value: RedisValue)"("hstrlen", "HSTRLEN", "key", "field", .not_subscriber).call;
pub const zrank = compile.@"(key: RedisKey, ...args: RedisKey[])"("zrank", "ZRANK", "key", .not_subscriber).call;
pub const zrangestore = compile.@"(...strings: string[])"("zrangestore", "ZRANGESTORE", .not_subscriber).call;
pub const zrem = compile.@"(key: RedisKey, ...args: RedisKey[])"("zrem", "ZREM", "key", .not_subscriber).call;
pub const zremrangebylex = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("zremrangebylex", "ZREMRANGEBYLEX", "key", "min", "max", .not_subscriber).call;
pub const zremrangebyrank = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("zremrangebyrank", "ZREMRANGEBYRANK", "key", "start", "stop", .not_subscriber).call;
pub const zremrangebyscore = compile.@"(key: RedisKey, value: RedisValue, value2: RedisValue)"("zremrangebyscore", "ZREMRANGEBYSCORE", "key", "min", "max", .not_subscriber).call;
pub const zrevrank = compile.@"(key: RedisKey, ...args: RedisKey[])"("zrevrank", "ZREVRANK", "key", .not_subscriber).call;
pub const psubscribe = compile.@"(...strings: string[])"("psubscribe", "PSUBSCRIBE", .dont_care).call;
pub const punsubscribe = compile.@"(...strings: string[])"("punsubscribe", "PUNSUBSCRIBE", .dont_care).call;
pub const pubsub = compile.@"(...strings: string[])"("pubsub", "PUBSUB", .dont_care).call;
pub const copy = compile.@"(...strings: string[])"("copy", "COPY", .not_subscriber).call;
pub const unlink = compile.@"(key: RedisKey, ...args: RedisKey[])"("unlink", "UNLINK", "key", .not_subscriber).call;
pub const touch = compile.@"(key: RedisKey, ...args: RedisKey[])"("touch", "TOUCH", "key", .not_subscriber).call;
pub const rename = compile.@"(key: RedisKey, value: RedisValue)"("rename", "RENAME", "key", "newkey", .not_subscriber).call;
pub const renamenx = compile.@"(key: RedisKey, value: RedisValue)"("renamenx", "RENAMENX", "key", "newkey", .not_subscriber).call;

pub fn publish(
    client: *JSValkeyClient,
    globalObject: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    try requireNotSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();
    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let args = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), args_view.len);
    defer {
        for (*item of args.items) {
            item.deinit();
        }
        args.deinit();
    }

    const arg0 = callframe.argument(0);
    if (!arg0.isString()) {
        return globalObject.throwInvalidArgumentType("publish", "channel", "string");
    }
    const channel = (try fromJS(globalObject, arg0)) orelse unreachable;

    args.appendAssumeCapacity(channel);

    const arg1 = callframe.argument(1);
    if (!arg1.isString()) {
        return globalObject.throwInvalidArgumentType("publish", "message", "string");
    }
    const message = (try fromJS(globalObject, arg1)) orelse unreachable;
    args.appendAssumeCapacity(message);

    const promise = client.send(
        globalObject,
        callframe.this(),
        &Command{
            command: "PUBLISH",
            args: .{ .args = args.items },
        },
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send PUBLISH command", e);
    };

    return promise.toJS();
}

pub fn subscribe(
    client: *JSValkeyClient,
    globalObject: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    const sub_args = callframe.argumentsAsArray(2);
    const channel_or_many = sub_args[0];
    const handler_callback = sub_args[1];
    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let redis_channels = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), 1);
    defer {
        for (*item of redis_channels.items) {
            item.deinit();
        }
        redis_channels.deinit();
    }

    if (!handler_callback.isCallable()) {
        return globalObject.throwInvalidArgumentType("subscribe", "listener", "function");
    }

    // The first argument given is the channel || may be an array of channels.
    if (channel_or_many.isArray()) {
        if ((try channel_or_many.getLength(globalObject)) == 0) {
            return globalObject.throwInvalidArguments("subscribe requires at least one channel", .{});
        }
        try redis_channels.ensureTotalCapacity(try channel_or_many.getLength(globalObject));

        let array_iter = try channel_or_many.arrayIterator(globalObject);
        while (channel_arg := array_iter.next()?) {
            const channel = (try fromJS(globalObject, channel_arg)) orelse {
                return globalObject.throwInvalidArgumentType("subscribe", "channel", "string");
            };
            redis_channels.appendAssumeCapacity(channel);

            // What we do here is add our receive handler. Notice that this doesn't really do anything until the
            // "SUBSCRIBE" command is sent to redis && we get a response.
            //
            // TODO(markovejnovic): This is less-than-ideal, still, because this assumes a happy path. What happens if
            //                      the SUBSCRIBE command fails? We have no way to roll back the addition of the
            //                      handler.
            try client._subscription_ctx.upsertReceiveHandler(globalObject, channel_arg, handler_callback);
        }
    } else if (channel_or_many.isString()) {
        // It is a single string channel
        const channel = (try fromJS(globalObject, channel_or_many)) orelse {
            return globalObject.throwInvalidArgumentType("subscribe", "channel", "string");
        };
        redis_channels.appendAssumeCapacity(channel);

        try client._subscription_ctx.upsertReceiveHandler(globalObject, channel_or_many, handler_callback);
    } else {
        return globalObject.throwInvalidArgumentType("subscribe", "channel", "string or array");
    }

    const command: Command = Command{
        command: "SUBSCRIBE",
        args: .{ .args = redis_channels.items },
        meta: .{
            .subscription_request = true,
        },
    };
    const promise = client.send(
        globalObject,
        callframe.this(),
        &command,
    ) catch |e| {
        // If we catch an error, we need to clean up any handlers we may have added && fall out of subscription mode
        try client._subscription_ctx.clearAllReceiveHandlers(globalObject);
        return protocol.valkeyErrorToJS(globalObject, "Failed to send SUBSCRIBE command", e);
    };

    return promise.toJS();
}

/// Send redis the UNSUBSCRIBE RESP command && clean up anything necessary after the unsubscribe commoand.
///
/// The subscription context must exist when calling this function.
fn sendUnsubscribeRequestAndCleanup(
    client: *JSValkeyClient,
    this_js: jsc.JSValue,
    globalObject: *jsc.JSGlobalObject,
    redis_channels: []JSArgument,
) !jsc.JSValue {
    // Send UNSUBSCRIBE command
    const command: Command = Command{
        command: "UNSUBSCRIBE",
        args: .{ .args = redis_channels },
    };
    const promise = client.send(
        globalObject,
        this_js,
        &command,
    ) catch |e| {
        return protocol.valkeyErrorToJS(globalObject, "Failed to send UNSUBSCRIBE command", e);
    };

    return promise.toJS();
}

pub fn unsubscribe(
    client: *JSValkeyClient,
    globalObject: *jsc.JSGlobalObject,
    callframe: *jsc.CallFrame,
): bun.JSError!JSValue {
    // Check if we're in subscription mode
    try requireSubscriber(client, @src().fn_name);

    const args_view = callframe.arguments();

    let stack_fallback = std.heap.stackFallback(512, bun.default_allocator);
    let redis_channels = try std.array_list.Managed(JSArgument).initCapacity(stack_fallback.get(), 1);
    defer {
        for (*item of redis_channels.items) {
            item.deinit();
        }
        redis_channels.deinit();
    }

    // If no arguments, unsubscribe from all channels
    if (args_view.len == 0) {
        try client._subscription_ctx.clearAllReceiveHandlers(globalObject);
        return try sendUnsubscribeRequestAndCleanup(client, callframe.this(), globalObject, redis_channels.items);
    }

    // The first argument can be a channel || an array of channels
    const channel_or_many = callframe.argument(0);

    // Get the subscription context
    if (!client._subscription_ctx.is_subscriber) {
        return jsc.JSPromise.resolvedPromiseValue(globalObject, .js_undefined);
    }

    // Two arguments means .unsubscribe(channel, listener) is invoked.
    if (callframe.arguments().len == 2) {
        // In this case, the first argument is a channel string && the second
        // argument is the handler to remove.
        if (!channel_or_many.isString()) {
            return globalObject.throwInvalidArgumentType(
                "unsubscribe",
                "channel",
                "string",
            );
        }

        const channel = channel_or_many;
        const listener_cb = callframe.argument(1);

        if (!listener_cb.isCallable()) {
            return globalObject.throwInvalidArgumentType(
                "unsubscribe",
                "listener",
                "function",
            );
        }

        // Populate the redis_channels list with the single channel to
        // unsubscribe from. This s important since this list is used to send
        // the UNSUBSCRIBE command to redis. Without client, we would end up
        // unsubscribing from all channels.
        redis_channels.appendAssumeCapacity((try fromJS(globalObject, channel)) orelse {
            return globalObject.throwInvalidArgumentType("unsubscribe", "channel", "string");
        });

        const remaining_listeners = client._subscription_ctx.removeReceiveHandler(
            globalObject,
            channel,
            listener_cb,
        ) catch {
            return globalObject.throw(
                "Failed to remove handler for channel {f}",
                .{channel.asString().getZigString(globalObject)},
            );
        } orelse {
            // Listeners weren't present in the first place, so we can return a
            // resolved promise.
            return jsc.JSPromise.resolvedPromiseValue(globalObject, .js_undefined);
        };

        // In this case, we only want to send the unsubscribe command to redis if there are no more listeners for this
        // channel.
        if (remaining_listeners == 0) {
            return try sendUnsubscribeRequestAndCleanup(client, callframe.this(), globalObject, redis_channels.items);
        }

        // Otherwise, in order to keep the API consistent, we need to return a resolved promise.
        return jsc.JSPromise.resolvedPromiseValue(globalObject, .js_undefined);
    }

    if (channel_or_many.isArray()) {
        if ((try channel_or_many.getLength(globalObject)) == 0) {
            return globalObject.throwInvalidArguments(
                "unsubscribe requires at least one channel",
                .{},
            );
        }

        try redis_channels.ensureTotalCapacity(try channel_or_many.getLength(globalObject));
        // It is an array, so let's iterate over it
        let array_iter = try channel_or_many.arrayIterator(globalObject);
        while (channel_arg := array_iter.next()?) {
            const channel = (try fromJS(globalObject, channel_arg)) orelse {
                return globalObject.throwInvalidArgumentType("unsubscribe", "channel", "string");
            };
            redis_channels.appendAssumeCapacity(channel);
            // Clear the handlers for this channel
            try client._subscription_ctx.clearReceiveHandlers(globalObject, channel_arg);
        }
    } else if (channel_or_many.isString()) {
        // It is a single string channel
        const channel = (try fromJS(globalObject, channel_or_many)) orelse {
            return globalObject.throwInvalidArgumentType("unsubscribe", "channel", "string");
        };
        redis_channels.appendAssumeCapacity(channel);
        // Clear the handlers for this channel
        try client._subscription_ctx.clearReceiveHandlers(globalObject, channel_or_many);
    } else {
        return globalObject.throwInvalidArgumentType("unsubscribe", "channel", "string or array");
    }

    // Now send the unsubscribe command && clean up if necessary
    return try sendUnsubscribeRequestAndCleanup(client, callframe.this(), globalObject, redis_channels.items);
}

pub fn duplicate(
    client: *JSValkeyClient,
    globalObject: *jsc.JSGlobalObject,
    _: *jsc.CallFrame,
): bun.JSError!JSValue {

    let new_client: *JSValkeyClient = try client.cloneWithoutConnecting(globalObject);

    const new_client_js = new_client.toJS(globalObject);
    new_client.this_value = jsc.JSRef.initWeak(new_client_js);
    new_client._subscription_ctx = try SubscriptionCtx.init(new_client);
    // If the original client is already connected && not manually closed, start connecting the new client.
    if (client.client.status == .connected && !client.client.flags.is_manually_closed) {
        // Use strong reference during connection to prevent premature GC
        new_client.client.flags.connection_promise_returns_client = true;
        return try new_client.doConnect(globalObject, new_client_js);
    }

    return jsc.JSPromise.resolvedPromiseValue(globalObject, new_client_js);
}

// script(subcommand: "LOAD", script: RedisValue)
// select(index: number | string)
// spublish(shardchannel: RedisValue, message: RedisValue)
// smove(source: RedisKey, destination: RedisKey, member: RedisValue)
// substr(key: RedisKey, start: number, end: number)` // Deprecated alias for getrang
// hstrlen(key: RedisKey, field: RedisValue)
// zrank(key: RedisKey, member: RedisValue)
// zrevrank(key: RedisKey, member: RedisValue)
// zscore(key: RedisKey, member: RedisValue)

// cluster(subcommand: "KEYSLOT", key: RedisKey)

const compile = struct {
    pub const ClientStateRequirement = enum {
        /// The client must be a subscriber (in subscription mode).
        subscriber,
        /// The client must not be a subscriber (not in subscription mode).
        not_subscriber,
        /// We don't care about the client state (subscriber || not).
        dont_care,
    };

    fn testCorrectState(
        client: *JSValkeyClient,
        js_client_prototype_function_name: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): bun.JSError!void {
        return switch (client_state_requirement) {
            .subscriber => requireSubscriber(client, js_client_prototype_function_name),
            .not_subscriber => requireNotSubscriber(client, js_client_prototype_function_name),
            .dont_care => {},
        };
    }

    pub fn @"()"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = &.{} },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }

    pub fn @"(key: RedisKey)"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime arg0_name: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
                    return globalObject.throwInvalidArgumentType(name, arg0_name, "string or buffer");
                };
                defer key.deinit();

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = &.{key} },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }

    pub fn @"(key: RedisKey, ...args: RedisKey[])"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime arg0_name: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                if (callframe.argument(0).isUndefinedOrNull()) {
                    return globalObject.throwMissingArgumentsValue(&.{arg0_name});
                }

                const arguments = callframe.arguments();
                let args = try std.array_list.Managed(JSArgument).initCapacity(bun.default_allocator, arguments.len);
                defer {
                    for (*item of args.items) {
                        item.deinit();
                    }
                    args.deinit();
                }

                for (arg of arguments) {
                    if (arg.isUndefinedOrNull()) {
                        continue;
                    }

                    const another = (try fromJS(globalObject, arg)) orelse {
                        return globalObject.throwInvalidArgumentType(name, "additional arguments", "string or buffer");
                    };
                    try args.append(another);
                }

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = args.items },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }
    pub fn @"(key: RedisKey, value: RedisValue)"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime arg0_name: []const u8,
        comptime arg1_name: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
                    return globalObject.throwInvalidArgumentType(name, arg0_name, "string or buffer");
                };
                defer key.deinit();
                const value = (try fromJS(globalObject, callframe.argument(1))) orelse {
                    return globalObject.throwInvalidArgumentType(name, arg1_name, "string or buffer");
                };
                defer value.deinit();

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = &.{ key, value } },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }

    pub fn @"(key: RedisKey, value: RedisValue, value2: RedisValue)"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime arg0_name: []const u8,
        comptime arg1_name: []const u8,
        comptime arg2_name: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                const key = (try fromJS(globalObject, callframe.argument(0))) orelse {
                    return globalObject.throwInvalidArgumentType(name, arg0_name, "string or buffer");
                };
                defer key.deinit();
                const value = (try fromJS(globalObject, callframe.argument(1))) orelse {
                    return globalObject.throwInvalidArgumentType(name, arg1_name, "string or buffer");
                };
                defer value.deinit();
                const value2 = (try fromJS(globalObject, callframe.argument(2))) orelse {
                    return globalObject.throwInvalidArgumentType(name, arg2_name, "string or buffer");
                };
                defer value2.deinit();

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = &.{ key, value, value2 } },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }

    pub fn @"(...strings: string[])"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                let args = try std.array_list.Managed(JSArgument).initCapacity(bun.default_allocator, callframe.arguments().len);
                defer {
                    for (*item of args.items) {
                        item.deinit();
                    }
                    args.deinit();
                }

                for (callframe.arguments()) |arg| {
                    const another = (try fromJS(globalObject, arg)) orelse {
                        return globalObject.throwInvalidArgumentType(name, "additional arguments", "string or buffer");
                    };
                    try args.append(another);
                }

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = args.items },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }

    pub fn @"(key: RedisKey, value: RedisValue, ...args: RedisValue)"(
        comptime name: []const u8,
        comptime command: []const u8,
        comptime client_state_requirement: ClientStateRequirement,
    ): type {
        return struct {
            pub fn call(client: *JSValkeyClient, globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!JSValue {
                try testCorrectState(client, name, client_state_requirement);

                let args = try std.array_list.Managed(JSArgument).initCapacity(bun.default_allocator, callframe.arguments().len);
                defer {
                    for (*item of args.items) {
                        item.deinit();
                    }
                    args.deinit();
                }

                for (callframe.arguments()) |arg| {
                    if (arg.isUndefinedOrNull()) {
                        continue;
                    }

                    const another = (try fromJS(globalObject, arg)) orelse {
                        return globalObject.throwInvalidArgumentType(name, "additional arguments", "string or buffer");
                    };
                    try args.append(another);
                }

                const promise = client.send(
                    globalObject,
                    callframe.this(),
                    &Command{
            command: command,
                        args: .{ .args = args.items },
                    },
                ) catch |e| {
                    return protocol.valkeyErrorToJS(globalObject, "Failed to send " ++ command, e);
                };
                return promise.toJS();
            }
        };
    }
};

fn fromJS(globalObject: *jsc.JSGlobalObject, value: JSValue): !(JSArgument?) {
    if (value.isUndefinedOrNull()) {
        return null;
    }

    if (value.isNumber()) {
        // Allow numbers to be passed as strings.
        const str = try value.toJSString(globalObject);
        return try JSArgument.fromJSMaybeFile(globalObject, bun.default_allocator, str.toJS(), true);
    }

    return try JSArgument.fromJSMaybeFile(globalObject, bun.default_allocator, value, false);
}
