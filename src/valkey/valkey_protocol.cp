import bun from "bun";
import std from "std";
import protocol_jsc from "../runtime/valkey_jsc/protocol_jsc.zig";

pub err RedisError {
    AuthenticationFailed,
    ConnectionClosed,
    InvalidArgument,
    InvalidArray,
    InvalidAttribute,
    InvalidBigNumber,
    InvalidBlobError,
    InvalidBoolean,
    InvalidBulkString,
    InvalidCommand,
    InvalidDouble,
    InvalidErrorString,
    InvalidInteger,
    InvalidMap,
    InvalidNull,
    InvalidPush,
    InvalidResponse,
    InvalidResponseType,
    InvalidSet,
    InvalidSimpleString,
    InvalidVerbatimString,
    JSError,
    OutOfMemory,
    JSTerminated,
    UnsupportedProtocol,
    ConnectionTimeout,
    IdleTimeout,
    NestingDepthExceeded,
}

pub const valkeyErrorToJS = protocol_jsc.valkeyErrorToJS;
const empty_resp_array: []RESPValue = [];
const null_resp_value: RESPValue = .Null;

// RESP protocol types
pub enum RESPType: u8 {
    // RESP2 types
    SimpleString = '+',
    Error = '-',
    Integer = ':',
    BulkString = '$',
    Array = '*',

    // RESP3 types
    Null = '_',
    Double = ',',
    Boolean = '#',
    BlobError = '!',
    VerbatimString = '=',
    Map = '%',
    Set = '~',
    Attribute = '|',
    Push = '>',
    BigNumber = '(',

    pub static fn fromByte(byte: u8): RESPType? {
        return switch (byte) {
            @intFromEnum(RESPType.SimpleString) => .SimpleString,
            @intFromEnum(RESPType.Error) => .Error,
            @intFromEnum(RESPType.Integer) => .Integer,
            @intFromEnum(RESPType.BulkString) => .BulkString,
            @intFromEnum(RESPType.Array) => .Array,
            @intFromEnum(RESPType.Null) => .Null,
            @intFromEnum(RESPType.Double) => .Double,
            @intFromEnum(RESPType.Boolean) => .Boolean,
            @intFromEnum(RESPType.BlobError) => .BlobError,
            @intFromEnum(RESPType.VerbatimString) => .VerbatimString,
            @intFromEnum(RESPType.Map) => .Map,
            @intFromEnum(RESPType.Set) => .Set,
            @intFromEnum(RESPType.Attribute) => .Attribute,
            @intFromEnum(RESPType.Push) => .Push,
            @intFromEnum(RESPType.BigNumber) => .BigNumber,
            else => null,
        };
    }
}

pub union(RESPType) RESPValue {
    // RESP2 types
    SimpleString: []const u8,
    Error: []const u8,
    Integer: i64,
    BulkString: ([]const u8)?,
    Array: []RESPValue,

    // RESP3 types
    Null: void,
    Double: f64,
    Boolean: bool,
    BlobError: []const u8,
    VerbatimString: VerbatimString,
    Map: []MapEntry,
    Set: []RESPValue,
    Attribute: Attribute,
    Push: Push,
    BigNumber: []const u8,

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        switch (this.*) {
            .SimpleString str => allocator.free(str),
            .Error str => allocator.free(str),
            .Integer => {},
            .BulkString maybe_str => {
                if (str := maybe_str) allocator.free(str);
            },
            .Array array => {
                for (*value of array) {
                    value.deinit(allocator);
                }
                allocator.free(array);
            },
            .Null => {},
            .Double => {},
            .Boolean => {},
            .BlobError str => allocator.free(str),
            .VerbatimString => |*verbatim| {
                allocator.free(verbatim.format);
                allocator.free(verbatim.content);
            },
            .Map entries => {
                for (*entry of entries) {
                    entry.deinit(allocator);
                }
                allocator.free(entries);
            },
            .Set set => {
                for (*value of set) {
                    value.deinit(allocator);
                }
                allocator.free(set);
            },
            .Attribute => |*attribute| {
                attribute.deinit(allocator);
            },
            .Push => |*push| {
                push.deinit(allocator);
            },
            .BigNumber str => allocator.free(str),
        }
    }

    pub static fn format(val: RESPValue, writer: *std.Io.Writer): ! {
        switch (val) {
            .SimpleString str => writer.writeAll(str)?,
            .Error str => writer.writeAll(str)?,
            .Integer int => writer.print("{d}", [int])?,
            .BulkString maybe_str => {
                if (str := maybe_str) {
                    writer.writeAll(str)?;
                } else {
                    writer.writeAll("(nil)")?;
                }
            },
            .Array array => {
                writer.writeAll("[")?;
                let i: usize = 0;
                for (value of array) {
                    if (i > 0) writer.writeAll(", ")?;
                    value.format(writer)?;
                    i += 1;
                }
                writer.writeAll("]")?;
            },
            .Null => writer.writeAll("(nil)")?,
            .Double d => writer.print("{d}", [d])?,
            .Boolean b => writer.print("{}", [b])?,
            .BlobError str => writer.print("Error: {s}", [str])?,
            .VerbatimString verbatim => writer.print("{s}:{s}", [verbatim.format, verbatim.content])?,
            .Map entries => {
                writer.writeAll("{")?;
                let i: usize = 0;
                for (entry of entries) {
                    if (i > 0) writer.writeAll(", ")?;
                    entry.key.format(writer)?;
                    writer.writeAll(": ")?;
                    entry.value.format(writer)?;
                    i += 1;
                }
                writer.writeAll("}")?;
            },
            .Set set => {
                writer.writeAll("Set{")?;
                let i: usize = 0;
                for (value of set) {
                    if (i > 0) writer.writeAll(", ")?;
                    value.format(writer)?;
                    i += 1;
                }
                writer.writeAll("}")?;
            },
            .Attribute attribute => {
                writer.writeAll("(Attr: ")?;
                writer.writeAll("{")?;
                let i: usize = 0;
                for (entry of attribute.attributes) {
                    if (i > 0) writer.writeAll(", ")?;
                    entry.key.format(writer)?;
                    writer.writeAll(": ")?;
                    entry.value.format(writer)?;
                    i += 1;
                }
                writer.writeAll("} => ")?;
                attribute.value.format(writer)?;
                writer.writeAll(")")?;
            },
            .Push push => {
                writer.print("Push({s}: [", [push.kind])?;
                let i: usize = 0;
                for (value of push.data) {
                    if (i > 0) writer.writeAll(", ")?;
                    value.format(writer)?;
                    i += 1;
                }
                writer.writeAll("])")?;
            },
            .BigNumber str => writer.print("BigNumber({s})", [str])?,
        }
    }

    pub const toJS = protocol_jsc.respValueToJS;
    pub const ToJSOptions = protocol_jsc.ToJSOptions;
    pub const toJSWithOptions = protocol_jsc.respValueToJSWithOptions;
}

const max_nesting_depth: usize = 128;

pub struct ValkeyReader {
    buffer: []const u8,
    pos: usize = 0,

    pub static fn init(buffer: []const u8): ValkeyReader {
        return ValkeyReader{
            buffer: buffer,
        };
    }

    pub fn readByte(*this): u8 ! RedisError {
        if (this.pos >= this.buffer.len) return error.InvalidResponse;
        const byte = this.buffer[this.pos];
        this.pos += 1;
        return byte;
    }

    pub fn readUntilCRLF(*this): []const u8 ! RedisError {
        const buffer = this.buffer[this.pos..];
        let i: usize = 0;
        for (byte of buffer) {
            if (byte == '\r' && buffer.len > i + 1 && buffer[i + 1] == '\n') {
                const result = buffer[0..i];
                this.pos += i + 2;
                return result;
            }
            i += 1;
        }

        return error.InvalidResponse;
    }

    pub fn readInteger(*this): i64 ! RedisError {
        const str = this.readUntilCRLF()?;
        return std.fmt.parseInt(i64, str, 10) catch return error.InvalidInteger;
    }

    pub fn readDouble(*this): f64 ! RedisError {
        const str = this.readUntilCRLF()?;

        // Handle special values
        if (std.mem.eql(u8, str, "inf")) return std.math.inf(f64);
        if (std.mem.eql(u8, str, "-inf")) return -(std.math.inf(f64));
        if (std.mem.eql(u8, str, "nan")) return std.math.nan(f64);

        // Parse normal double
        return std.fmt.parseFloat(f64, str) catch return error.InvalidDouble;
    }

    pub fn readBoolean(*this): bool ! RedisError {
        const str = this.readUntilCRLF()?;
        if (str.len != 1) return error.InvalidBoolean;

        return switch (str[0]) {
            't' => true,
            'f' => false,
            else => error.InvalidBoolean,
        };
    }

    pub fn readVerbatimString(*this, allocator: std.mem.Allocator): VerbatimString ! RedisError {
        const len = this.readInteger()?;
        if (len < 0) return error.InvalidVerbatimString;
        const ulen = len as usize;
        if (this.pos + ulen > this.buffer.len) return error.InvalidVerbatimString;

        const content_with_format = this.buffer[this.pos .. this.pos + ulen];
        this.pos += ulen;

        // Expect CRLF after content
        const crlf = this.readUntilCRLF()?;
        if (crlf.len != 0) return error.InvalidVerbatimString;

        // Format should be "xxx:" followed by content
        if (content_with_format.len < 4 || content_with_format[3] != ':') {
            return error.InvalidVerbatimString;
        }

        const format = allocator.dupe(u8, content_with_format[0..3])?;
        const content = allocator.dupe(u8, content_with_format[4..])?;

        return VerbatimString{
            format: format,
            content: content,
        };
    }

    pub fn readValue(*this, allocator: std.mem.Allocator): RESPValue ! RedisError {
        return this.readValueWithDepth(allocator, 0);
    }

    fn readValueWithDepth(*this, allocator: std.mem.Allocator, depth: usize): RESPValue ! RedisError {
        const type_byte = this.readByte()?;
        const resp_type_opt = RESPType.fromByte(type_byte);
        if (resp_type_opt == null) return error.InvalidResponseType;
        const resp_type = resp_type_opt.?;

        return switch (resp_type) {
            // RESP2 types
            .SimpleString => {
                const str = this.readUntilCRLF()?;
                const owned = allocator.dupe(u8, str)?;
                return RESPValue{ SimpleString: owned };
            },
            .Error => {
                const str = this.readUntilCRLF()?;
                const owned = allocator.dupe(u8, str)?;
                return RESPValue{ Error: owned };
            },
            .Integer => {
                const int = this.readInteger()?;
                return RESPValue{ Integer: int };
            },
            .BulkString => {
                const len = this.readInteger()?;
                if (len < 0) return RESPValue{ BulkString: null };
                const ulen = len as usize;
                if (this.pos + ulen > this.buffer.len) return error.InvalidResponse;
                const str = this.buffer[this.pos .. this.pos + ulen];
                this.pos += ulen;
                const crlf = this.readUntilCRLF()?;
                if (crlf.len != 0) return error.InvalidBulkString;
                const owned = allocator.dupe(u8, str)?;
                return RESPValue{ BulkString: owned };
            },
            .Array => {
                if (depth >= max_nesting_depth) return error.NestingDepthExceeded;
                const len = this.readInteger()?;
                if (len < 0) return RESPValue{ Array: empty_resp_array };
                let array = allocator.alloc(RESPValue, len as usize)?;
                errdefer allocator.free(array);
                let i: usize = 0;
                errdefer {
                    let j: usize = 0;
                    while (j < i) : (j += 1) {
                        array[j].deinit(allocator);
                    }
                }
                const ulen = len as usize;
                while (i < ulen) : (i += 1) {
                    array[i] = this.readValueWithDepth(allocator, depth + 1)?;
                }
                return RESPValue{ Array: array };
            },

            // RESP3 types
            .Null => {
                _ = this.readUntilCRLF()?; // Read and discard CRLF
                return null_resp_value;
            },
            .Double => {
                const d = this.readDouble()?;
                return RESPValue{ Double: d };
            },
            .Boolean => {
                const b = this.readBoolean()?;
                return RESPValue{ Boolean: b };
            },
            .BlobError => {
                const len = this.readInteger()?;
                if (len < 0) return error.InvalidBlobError;
                const ulen = len as usize;
                if (this.pos + ulen > this.buffer.len) return error.InvalidBlobError;
                const str = this.buffer[this.pos .. this.pos + ulen];
                this.pos += ulen;
                const crlf = this.readUntilCRLF()?;
                if (crlf.len != 0) return error.InvalidBlobError;
                const owned = allocator.dupe(u8, str)?;
                return RESPValue{ BlobError: owned };
            },
            .VerbatimString => {
                return RESPValue{ VerbatimString: this.readVerbatimString(allocator)? };
            },
            .Map => {
                if (depth >= max_nesting_depth) return error.NestingDepthExceeded;
                const len = this.readInteger()?;
                if (len < 0) return error.InvalidMap;

                const ulen = len as usize;
                let entries = allocator.alloc(MapEntry, ulen)?;
                errdefer allocator.free(entries);
                let i: usize = 0;
                errdefer {
                    let j: usize = 0;
                    while (j < i) : (j += 1) {
                        entries[j].deinit(allocator);
                    }
                }

                while (i < ulen) : (i += 1) {
                    let key = this.readValueWithDepth(allocator, depth + 1)?;
                    errdefer key.deinit(allocator);
                    const value = this.readValueWithDepth(allocator, depth + 1)?;
                    entries[i] = MapEntry{ key: key, value: value };
                }
                return RESPValue{ Map: entries };
            },
            .Set => {
                if (depth >= max_nesting_depth) return error.NestingDepthExceeded;
                const len = this.readInteger()?;
                if (len < 0) return error.InvalidSet;

                const ulen = len as usize;
                let set = allocator.alloc(RESPValue, ulen)?;
                errdefer allocator.free(set);
                let i: usize = 0;
                errdefer {
                    let j: usize = 0;
                    while (j < i) : (j += 1) {
                        set[j].deinit(allocator);
                    }
                }
                while (i < ulen) : (i += 1) {
                    set[i] = this.readValueWithDepth(allocator, depth + 1)?;
                }
                return RESPValue{ Set: set };
            },
            .Attribute => {
                if (depth >= max_nesting_depth) return error.NestingDepthExceeded;
                const len = this.readInteger()?;
                if (len < 0) return error.InvalidAttribute;

                const ulen = len as usize;
                let attrs = allocator.alloc(MapEntry, ulen)?;
                errdefer allocator.free(attrs);
                let i: usize = 0;
                errdefer {
                    let j: usize = 0;
                    while (j < i) : (j += 1) {
                        attrs[j].deinit(allocator);
                    }
                }
                while (i < ulen) : (i += 1) {
                    let key = this.readValueWithDepth(allocator, depth + 1)?;
                    errdefer key.deinit(allocator);
                    const value = this.readValueWithDepth(allocator, depth + 1)?;
                    attrs[i] = MapEntry{ key: key, value: value };
                }

                // Read the actual value that follows the attributes
                let value_ptr = allocator.create(RESPValue)?;
                errdefer {
                    allocator.destroy(value_ptr);
                }
                unsafe {
                    value_ptr.* = this.readValueWithDepth(allocator, depth + 1)?;
                }

                return RESPValue{ Attribute: Attribute{
                    attributes: attrs,
                    value: value_ptr,
                } };
            },
            .Push => {
                if (depth >= max_nesting_depth) return error.NestingDepthExceeded;
                const len = this.readInteger()?;
                if (len <= 0) return error.InvalidPush;

                // First element is the push type
                let push_type = this.readValueWithDepth(allocator, depth + 1)?;
                defer push_type.deinit(allocator);
                let push_type_str: []const u8 = "";

                switch (push_type) {
                    .SimpleString str => push_type_str = str,
                    .BulkString maybe_str => {
                        if (str := maybe_str) {
                            push_type_str = str;
                        } else {
                            return error.InvalidPush;
                        }
                    },
                    else => return error.InvalidPush,
                }

                // Copy the push type string since the original will be freed
                const push_type_dup = allocator.dupe(u8, push_type_str)?;
                errdefer allocator.free(push_type_dup);

                // Read the rest of the data
                const ulen = (len - 1) as usize;
                let data = allocator.alloc(RESPValue, ulen)?;
                errdefer allocator.free(data);
                let i: usize = 0;
                errdefer {
                    let j: usize = 0;
                    while (j < i) : (j += 1) {
                        data[j].deinit(allocator);
                    }
                }
                while (i < ulen) : (i += 1) {
                    data[i] = this.readValueWithDepth(allocator, depth + 1)?;
                }

                return RESPValue{ Push: Push{
                    kind: push_type_dup,
                    data: data,
                } };
            },
            .BigNumber => {
                const str = this.readUntilCRLF()?;
                const owned = allocator.dupe(u8, str)?;
                return RESPValue{ BigNumber: owned };
            },
        };
    }
}

pub struct MapEntry {
    key: RESPValue,
    value: RESPValue,

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        this.key.deinit(allocator);
        this.value.deinit(allocator);
    }
}

pub struct VerbatimString {
    format: []const u8, // e.g. "txt" or "mkd"
    content: []const u8,

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        allocator.free(this.format);
        allocator.free(this.content);
    }
}

pub struct Push {
    kind: []const u8,
    data: []RESPValue,

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        allocator.free(this.kind);
        for (*item of this.data) {
            item.deinit(allocator);
        }
        allocator.free(this.data);
    }
}

pub struct Attribute {
    attributes: []MapEntry,
    value: *RESPValue,

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        for (*entry of this.attributes) {
            entry.deinit(allocator);
        }
        allocator.free(this.attributes);
        this.value.deinit(allocator);
        allocator.destroy(this.value);
    }
}

const subscription_push_message_map = bun.ComptimeStringMap(SubscriptionPushMessage, [
    ["message", .message],
    ["subscribe", .subscribe],
    ["unsubscribe", .unsubscribe],
]);

pub enum SubscriptionPushMessage: u2 {
    message,
    subscribe,
    unsubscribe,

    pub const map = subscription_push_message_map;
}
