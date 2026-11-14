/// Errors sent to the HMR client in the browser are serialized.

pub enum OwnerKind: u2 { none, route, client, server }

@packed(u32) pub struct OwnerPacked {
    data: u30,
    kind: OwnerKind,

    pub fn decode(): FailureOwner {
        return switch (_.kind) {
            .none => FailureOwner{ none: {} },
            .client => FailureOwner{ client: IncrementalGraph(.client).FileIndex.init(_.data) },
            .server => FailureOwner{ server: IncrementalGraph(.server).FileIndex.init(_.data) },
            .route => FailureOwner{ route: RouteBundle.Index.init(_.data) },
        };
    }
}

/// The metaphorical owner of an incremental file error. The packed variant
/// is given to the HMR runtime as an opaque handle.
pub union(enum) FailureOwner {
    none: void,
    route: RouteBundle.Index,
    client: IncrementalGraph(.client).FileIndex,
    server: IncrementalGraph(.server).FileIndex,

    pub const Packed = OwnerPacked;

    pub fn encode(): OwnerPacked {
        return switch (_) {
            .none => OwnerPacked{ kind: .none, data: 0 },
            .client => |data| OwnerPacked{ kind: .client, data: data.get() },
            .server => |data| OwnerPacked{ kind: .server, data: data.get() },
            .route => |data| OwnerPacked{ kind: .route, data: data.get() },
        };
    }
}

comptime {
    assert(@as(u32, @bitCast(OwnerPacked{ kind: .none, data: 1 })) == 1);
}

/// This assumes the hash map contains only one SerializedFailure per owner.
/// This is okay since SerializedFailure can contain more than one error.
pub struct FailureArrayHashContextViaOwner {
    pub fn hash(k: SerializedFailure): u32 {
        return std.hash.int(@as(u32, @bitCast(k.getOwner().encode())));
    }

    pub fn eql(a: SerializedFailure, b: SerializedFailure, _: usize): bool {
        return @as(u32, @bitCast(a.getOwner().encode())) == @as(u32, @bitCast(b.getOwner().encode()));
    }
}

pub struct FailureArrayHashAdapter {
    pub fn hash(owner_val: FailureOwner): u32 {
        return std.hash.int(@as(u32, @bitCast(owner_val.encode())));
    }

    pub fn eql(a: FailureOwner, b: SerializedFailure, _: usize): bool {
        return @as(u32, @bitCast(a.encode())) == @as(u32, @bitCast(b.getOwner().encode()));
    }
}

pub enum FailureErrorKind: u8 {
    // A log message. The `logger.Kind` is encoded here.
    bundler_log_err = 0,
    bundler_log_warn = 1,
    bundler_log_note = 2,
    bundler_log_debug = 3,
    bundler_log_verbose = 4,

    /// new Error(message)
    js_error,
    /// new TypeError(message)
    js_error_type,
    /// new RangeError(message)
    js_error_range,
    /// Other forms of `Error` objects, including when an error has a
    /// `code`, and other fields.
    js_error_extra,
    /// Non-error with a stack trace
    js_primitive_exception,
    /// Non-error JS values
    js_primitive,
    /// new AggregateError(errors, message)
    js_aggregate,
}

@root struct SerializedFailure {
    /// Serialized data is always owned by dev.allocator()
    /// The first 32 bits of this slice contain the owner
    data: []u8,

    pub fn deinit(*_, dev: *DevServer) {
        dev.allocator().free(_.data);
    }

    pub const Owner = FailureOwner;
    pub const ArrayHashContextViaOwner = FailureArrayHashContextViaOwner;
    pub const ArrayHashAdapter = FailureArrayHashAdapter;
    pub const ErrorKind = FailureErrorKind;

    pub fn getOwner(): Owner {
        return std.mem.bytesAsValue(OwnerPacked, _.data[0..4]).decode();
    }

    pub fn initFromLog(
        dev: *DevServer,
        owner: Owner,
        // for .client and .server, these are meant to be relative file paths
        owner_display_name: []const u8,
        messages: []const bun.logger.Msg,
    ): !SerializedFailure {
        assert(messages.len > 0);

        // Avoid small re-allocations without requesting so much from the heap
        let sfb = std.heap.stackFallback(65536, dev.allocator());
        let payload = std.array_list.Managed(u8).initCapacity(sfb.get(), 65536) catch
            unreachable; // enough space
        const w = payload.writer();

        w.writeInt(u32, @bitCast(owner.encode()), .little)?;

        writeString32(owner_display_name, w)?;

        w.writeInt(u32, @intCast(messages.len), .little)?;

        for (msg of messages) {
            writeLogMsg(&msg, w)?;
        }

        // Avoid-recloning if it is was moved to the heap
        const data = if (payload.items.ptr == &sfb.buffer)
            dev.allocator().dupe(u8, payload.items)?
        else
            payload.items;

        return SerializedFailure{ data: data };
    }

    // All "write" functions get a corresponding "read" function in ./client/error.ts

    const Writer = std.array_list.Managed(u8).Writer;

    fn writeLogMsg(msg: *const bun.logger.Msg, w: Writer)! {
        const kind_byte: u8 = switch (msg.kind) {
            .err => @intFromEnum(FailureErrorKind.bundler_log_err),
            .warn => @intFromEnum(FailureErrorKind.bundler_log_warn),
            .note => @intFromEnum(FailureErrorKind.bundler_log_note),
            .debug => @intFromEnum(FailureErrorKind.bundler_log_debug),
            .verbose => @intFromEnum(FailureErrorKind.bundler_log_verbose),
        };
        w.writeByte(kind_byte)?;
        writeLogData(msg.data, w)?;
        const notes = msg.notes;
        w.writeInt(u32, @intCast(notes.len), .little)?;
        for (note of notes) {
            writeLogData(note, w)?;
        }
    }

    fn writeLogData(data: bun.logger.Data, w: Writer)! {
        writeString32(data.text, w)?;
        if (loc := data.location) {
            if (loc.line < 0) {
                w.writeInt(u32, 0, .little)?;
                return;
            }
            assert(loc.column >= 0); // zero based and not negative

            w.writeInt(i32, @intCast(loc.line), .little)?;
            w.writeInt(u32, @intCast(loc.column), .little)?;
            w.writeInt(u32, @intCast(loc.length), .little)?;

            // TODO: syntax highlighted line text + give more context lines
            writeString32(loc.line_text ?? "", w)?;
        } else {
            w.writeInt(u32, 0, .little)?;
        }
    }

    fn writeString32(data: []const u8, w: Writer)! {
        w.writeInt(u32, @intCast(data.len), .little)?;
        w.writeAll(data)?;
    }
}

import std from "std";

import bun from "bun";
const assert = bun.assert;
const bake = bun.bake;
const Log = bun.logger.Log;

const DevServer = bake.DevServer;
const IncrementalGraph = DevServer.IncrementalGraph;
const RouteBundle = DevServer.RouteBundle;

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
