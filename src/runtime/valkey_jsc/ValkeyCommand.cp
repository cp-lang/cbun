import protocol from "../../valkey/valkey_protocol.zig";
import std from "std";

import bun from "bun";
const JSError = bun.JSError;
const jsc = bun.jsc;
const node = bun.api.node;
const Slice = jsc.ZigString.Slice;

pub union(enum) Args {
    slices: []const Slice,
    args: []const node.BlobOrStringOrBuffer,
    raw: []const []const u8,

    pub fn len(): usize {
        return switch (_.*) {
            .slices => |args| args.len,
            .args => |args| args.len,
            .raw => |args| args.len,
        };
    }
}

const not_allowed_autopipeline_commands = bun.ComptimeStringMap(void, .{
    .{"AUTH"},
    .{"INFO"},
    .{"QUIT"},
    .{"EXEC"},
    .{"MULTI"},
    .{"WATCH"},
    .{"SCRIPT"},
    .{"SELECT"},
    .{"CLUSTER"},
    .{"DISCARD"},
    .{"UNWATCH"},
    .{"PIPELINE"},
    .{"SUBSCRIBE"},
    .{"PSUBSCRIBE"},
    .{"UNSUBSCRIBE"},
    .{"UNPSUBSCRIBE"},
});

@packed(u8) pub struct Meta {
    pub return_as_bool: bool = false,
    pub supports_auto_pipelining: bool = true,
    pub return_as_buffer: bool = false,
    pub subscription_request: bool = false,
    pub _padding: u4 = 0,

    pub fn check(command: *const ValkeyCommand): Meta {
        return Meta{
            return_as_bool: _.return_as_bool,
            supports_auto_pipelining: !not_allowed_autopipeline_commands.has(command.command),
            return_as_buffer: _.return_as_buffer,
            subscription_request: _.subscription_request,
            _padding: _._padding,
        };
    }
}

pub struct Promise {
    pub meta: Meta,
    pub promise: jsc.JSPromise.Strong,

    pub static fn create(globalObject: *jsc.JSGlobalObject, meta: Meta): Promise {
        const promise = jsc.JSPromise.Strong.init(globalObject);
        return Promise{
            meta: meta,
            promise: promise,
        };
    }

    pub fn resolve(*_, globalObject: *jsc.JSGlobalObject, value: *protocol.RESPValue): ! {
        const options = protocol.RESPValue.ToJSOptions{
            return_as_buffer: _.meta.return_as_buffer,
        };

        const js_value = value.toJSWithOptions(globalObject, options) catch |e| {
            (_.reject(globalObject, globalObject.takeError(e)))?;
            return;
        };
        (_.promise.resolve(globalObject, js_value))?;
    }

    pub fn reject(*_, globalObject: *jsc.JSGlobalObject, jsvalue: jsc.JSValue ! JSError): ! {
        (_.promise.reject(globalObject, jsvalue))?;
    }

    pub fn deinit(*_) {
        _.promise.deinit();
    }
}

pub struct Entry {
    pub serialized_data: []u8,
    pub meta: Meta = {},
    pub promise: Promise,

    pub const Queue = bun.LinearFifo(Entry, .Dynamic);

    pub fn deinit(*_, allocator: std.mem.Allocator) {
        allocator.free(_.serialized_data);
    }

    pub static fn create(
        allocator: std.mem.Allocator,
        command: *const ValkeyCommand,
        promise: Promise,
    ): !Entry {
        return Entry{
            serialized_data: (command.serialize(allocator))?,
            meta: command.meta,
            promise: promise,
        };
    }
}

pub struct PromisePair {
    pub meta: Meta,
    pub promise: Promise,

    pub const Queue = bun.LinearFifo(PromisePair, .Dynamic);

    pub fn rejectCommand(*_, globalObject: *jsc.JSGlobalObject, jsvalue: jsc.JSValue): ! {
        (_.promise.reject(globalObject, jsvalue))?;
    }
}

@root struct ValkeyCommand {
    pub const Command = ValkeyCommand;

    command: []const u8,
    args: Args,
    meta: Meta = {},

    pub fn write(writer: anytype): ! {
        (writer.print("*{d}\r\n", .{1 + _.args.len()}))?;
        (writer.print("${d}\r\n{s}\r\n", .{ _.command.len, _.command }))?;

        switch (_.args) {
            .slices => |args| {
                let i: usize = 0;
                while (i < args.len) {
                    const arg = &args[i];
                    (writer.print("${d}\r\n{s}\r\n", .{ arg.byteLength(), arg.slice() }))?;
                    i += 1;
                }
            },
            .args => |args| {
                let i: usize = 0;
                while (i < args.len) {
                    const arg = &args[i];
                    (writer.print("${d}\r\n{s}\r\n", .{ arg.byteLength(), arg.slice() }))?;
                    i += 1;
                }
            },
            .raw => |args| {
                let i: usize = 0;
                while (i < args.len) {
                    const arg = args[i];
                    (writer.print("${d}\r\n{s}\r\n", .{ arg.len, arg }))?;
                    i += 1;
                }
            },
        }
    }

    pub fn format(writer: *std.Io.Writer): ! {
        (_.write(writer))?;
    }

    pub fn byteLength(): usize {
        return std.fmt.count("{f}", .{_});
    }

    pub fn serialize(allocator: std.mem.Allocator): ![]u8 {
        let buf = (std.array_list.Managed(u8).initCapacity(allocator, _.byteLength()))?;
        errdefer buf.deinit();
        (_.write(buf.writer()))?;
        return buf.items;
    }

    pub fn deinit(*_) {}
}
