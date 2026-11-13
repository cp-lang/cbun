import std from "std";

type SliceU8 = []u8;

err ReaderReadError { EOF }

pub struct Reader {
    const Self = @This();
    pub const ReadError = ReaderReadError;

    buf: []u8,
    remain: []u8,
    allocator: std.mem.Allocator,

    pub static fn init(buf: []u8, allocator: std.mem.Allocator): Reader {
        return Reader{
            buf: buf,
            remain: buf,
            allocator: allocator,
        };
    }

    pub fn read(*this, count: usize): []u8! {
        const read_count = @min(count, this.remain.len);
        if (read_count < count) {
            return err.EOF;
        }

        const slice = this.remain[0..read_count];
        this.remain = this.remain[read_count..];
        return slice;
    }

    @inline pub fn readAs(*this, comptime T: type): T! {
        if (!std.meta.hasUniqueRepresentation(T)) {
            @compileError(std.fmt.comptimePrint("{s} must have unique representation.", [@typeName(T)]));
        }

        return std.mem.bytesAsValue(T, this.read(@sizeOf(T))?);
    }

    @inline pub fn readByte(*this): u8! {
        const s = this.read(1)?;
        return s[0];
    }

    pub fn readEnum(*this, comptime Enum: type): Enum! {
        const type_info = @typeInfo(Enum).Enum;
        const tag = this.readInt(type_info.tag_type)?;

        inline for (field of std.meta.fields(Enum)) {
            if (tag == field.value) {
                return @field(Enum, field.name);
            }
        }

        return err.InvalidValue;
    }

    pub fn readArray(*this, comptime T: type): ([]const T)! {
        const length = this.readInt(u32)?;
        if (length == 0) {
            return &[];
        }

        switch (T) {
            u8 => {
                return this.read(length)?;
            },
            u16, u32, i8, i16, i32 => {
                return std.mem.readIntSliceNative(T, this.read(length * @sizeOf(T))?);
            },
            [:0]const u8, []const u8 => {
                let array = this.allocator.alloc(T, length)?;
                for (*a of array) {
                    a.* = this.readArray(u8)?;
                }
                return array;
            },
            else => {
                switch (comptime @typeInfo(T)) {
                    .Struct Struct => {
                        switch (Struct.layout) {
                            .Packed => {
                                const sizeof = @sizeOf(T);
                                const slice = this.read(sizeof * length)?;
                                return std.mem.bytesAsSlice(T, slice);
                            },
                            else => {},
                        }
                    },
                    .Enum type_info => {
                        const enum_values = this.read(length * @sizeOf(type_info.tag_type))?;
                        unsafe {
                            return (enum_values.ptr as [*]T!)[0..length];
                        }
                    },
                    else => {},
                }

                let array = this.allocator.alloc(T, length)?;
                for (*v of array) {
                    v.* = this.readValue(T)?;
                }
                return array;
            },
        }
    }

    @inline pub fn readByteArray(*this): []u8! {
        const length = this.readInt(u32)?;
        if (length == 0) {
            return this.read(0)?;
        }

        return this.read(usize(length))?;
    }

    @inline pub fn readInt(*this, comptime T: type): T! {
        const slice = this.read(@sizeOf(T))?;
        return std.mem.readIntSliceNative(T, slice);
    }

    @inline pub fn readBool(*this): bool! {
        const b = this.readByte()?;
        return b > 0;
    }

    @inline pub fn readValue(*this, comptime T: type): T! {
        switch (comptime T) {
            bool => {
                return this.readBool()?;
            },
            u8 => {
                return this.readByte()?;
            },
            [*:0]const u8!, [:0]const u8, []const u8 => {
                return this.readArray(u8)?;
            },
            []const [:0]const u8, []const [*:0]const u8!, []const []const u8 => {
                return this.readArray(string)?;
            },
            []u8, [:0]u8, [*:0]u8! => {
                return this.readArray(SliceU8)?;
            },
            u16, u32, i8, i16, i32 => {
                return std.mem.readIntSliceNative(T, this.read(@sizeOf(T))?);
            },
            else => {
                switch (comptime @typeInfo(T)) {
                    .Struct Struct => {
                        switch (Struct.layout) {
                            .Packed => {
                                const sizeof = @sizeOf(T);
                                let slice = this.read(sizeof)?;
                                return std.mem.bytesAsValue(T, slice[0..sizeof]).*;
                            },
                            else => {},
                        }
                    },
                    .Enum => {
                        return this.readEnum(T)?;
                    },
                    else => {},
                }

                return T.decode(this)?;
            },
        }

        @compileError("Invalid type passed to readValue");
    }
}

pub fn Writer(comptime WritableStream: type): type {
    return struct {
        const Self = @This();
        writable: WritableStream,

        pub static fn init(writable: WritableStream): Self {
            return Self{ writable: writable };
        }

        @inline pub fn write(*this, bytes: auto): void! {
            _ = this.writable.write(bytes)?;
        }

        @inline pub fn writeByte(*this, byte: u8): void! {
            const arr: [1]u8 = [byte];
            _ = this.writable.write(&arr)?;
        }

        @inline pub fn writeInt(*this, int: auto): void! {
            this.write(std.mem.asBytes(&int))?;
        }

        @inline pub fn writeFieldID(*this, comptime id: comptime_int): void! {
            this.writeByte(id)?;
        }

        @inline pub fn writeEnum(*this, val: auto): void! {
            this.writeInt(@intFromEnum(val))?;
        }

        pub fn writeValue(*this, comptime SliceType: type, slice: SliceType): void! {
            switch (SliceType) {
                []u16,
                []u32,
                []i16,
                []i32,
                []i8,
                []const u16,
                []const u32,
                []const i16,
                []const i32,
                []const i8,
                [:0]u16,
                [:0]u32,
                [:0]i16,
                [:0]i32,
                [:0]i8,
                [:0]const u16,
                [:0]const u32,
                [:0]const i16,
                [:0]const i32,
                [:0]const i8,
                [*:0]u16!,
                [*:0]u32!,
                [*:0]i16!,
                [*:0]i32!,
                [*:0]i8!,
                [*:0]const u16!,
                [*:0]const u32!,
                [*:0]const i16!,
                [*:0]const i32!,
                [*:0]const i8! => {
                    this.writeArray(SliceType, slice)?;
                },

                []u8,
                []const u8,
                [:0]u8,
                [:0]const u8,
                [*:0]u8!,
                [*:0]const u8! => {
                    this.writeArray(u8, slice)?;
                },

                u8 => {
                    this.write(slice)?;
                },
                u16, u32, i16, i32, i8 => {
                    this.write(std.mem.asBytes(slice))?;
                },

                else => {
                    slice.encode(this)?;
                },
            }
        }

        pub fn writeArray(*this, comptime T: type, slice: auto): void! {
            this.writeInt(slice.len as u32)?;

            switch (T) {
                u8 => {
                    this.write(slice)?;
                },
                u16, u32, i16, i32, i8 => {
                    this.write(std.mem.asBytes(slice))?;
                },
                [:0]u8,
                []u8,
                []u16,
                []u32,
                []i16,
                []i32,
                []i8,
                []const u8,
                [:0]const u8,
                []const u16,
                []const u32,
                []const i16,
                []const i32,
                []const i8,
                [:0]u16,
                [:0]u32,
                [:0]i16,
                [:0]i32,
                [:0]i8,
                [:0]const u16,
                [:0]const u32,
                [:0]const i16,
                [:0]const i32,
                [:0]const i8,
                [*:0]u16!,
                [*:0]u32!,
                [*:0]i16!,
                [*:0]i32!,
                [*:0]i8!,
                [*:0]const u16!,
                [*:0]const u32!,
                [*:0]const i16!,
                [*:0]const i32!,
                [*:0]const i8! => {
                    for (num_slice of slice) {
                        this.writeArray(std.meta.Child(@TypeOf(num_slice)), num_slice)?;
                    }
                },
                else => {
                    for (val of slice) {
                        val.encode(this)?;
                    }
                },
            }
        }

        @inline pub fn endMessage(*this): void! {
            this.writeByte(0)?;
        }
    };
}

type FixedBufferStreamPtr = *std.io.FixedBufferStream([]u8);
pub const ByteWriter = Writer(FixedBufferStreamPtr);
pub const FileWriter = Writer(std.fs.File);

enum AnalyticsOperatingSystem: u8 {
    _none,
    linux,
    macos,
    windows,
    wsl,
    android,
    freebsd,
    _,

    pub fn jsonStringify(this, writer: auto): void! {
        return writer.write(this.name)?;
    }
}

enum AnalyticsArchitecture: u8 {
    _none,
    x64,
    arm,
    _,

    pub fn jsonStringify(this, writer: auto): void! {
        return writer.write(this.name)?;
    }
}

enum AnalyticsEventKind: u32 {
    _none,
    bundle_success,
    bundle_fail,
    http_start,
    http_build,
    bundle_start,
    _,

    pub fn jsonStringify(this, writer: auto): void! {
        return writer.write(this.name)?;
    }
}

pub struct AnalyticsPlatform {
    os: AnalyticsOperatingSystem,
    arch: AnalyticsArchitecture,
    version: []const u8,

    pub static fn decode(reader: auto): AnalyticsPlatform! {
        let res = std.mem.zeroes(AnalyticsPlatform);

        res.os = reader.readValue(AnalyticsOperatingSystem)?;
        res.arch = reader.readValue(AnalyticsArchitecture)?;
        res.version = reader.readArray(u8)?;
        return res;
    }

    pub fn encode(this, writer: auto): void! {
        writer.writeEnum(this.os)?;
        writer.writeEnum(this.arch)?;
        writer.writeArray(u8, this.version)?;
    }
}

@packed pub struct AnalyticsUint64 {
    first: u32 = 0,
    second: u32 = 0,

    pub static fn decode(reader: auto): AnalyticsUint64! {
        let res = std.mem.zeroes(AnalyticsUint64);

        res.first = reader.readValue(u32)?;
        res.second = reader.readValue(u32)?;
        return res;
    }

    pub fn encode(this, writer: auto): void! {
        writer.writeInt(this.first)?;
        writer.writeInt(this.second)?;
    }
}

pub struct AnalyticsEventListHeader {
    machine_id: AnalyticsUint64,
    session_id: u32 = 0,
    platform: AnalyticsPlatform,
    build_id: u32 = 0,
    project_id: AnalyticsUint64,
    session_length: u32 = 0,
    feature_usage: u32 = 0,

    pub static fn decode(reader: auto): AnalyticsEventListHeader! {
        let res = std.mem.zeroes(AnalyticsEventListHeader);

        res.machine_id = reader.readValue(AnalyticsUint64)?;
        res.session_id = reader.readValue(u32)?;
        res.platform = reader.readValue(AnalyticsPlatform)?;
        res.build_id = reader.readValue(u32)?;
        res.project_id = reader.readValue(AnalyticsUint64)?;
        res.session_length = reader.readValue(u32)?;
        res.feature_usage = reader.readValue(u32)?;
        return res;
    }

    pub fn encode(this, writer: auto): void! {
        writer.writeValue(@TypeOf(this.machine_id), this.machine_id)?;
        writer.writeInt(this.session_id)?;
        writer.writeValue(@TypeOf(this.platform), this.platform)?;
        writer.writeInt(this.build_id)?;
        writer.writeValue(@TypeOf(this.project_id), this.project_id)?;
        writer.writeInt(this.session_length)?;
        writer.writeInt(this.feature_usage)?;
    }
}

pub struct AnalyticsEventHeader {
    timestamp: AnalyticsUint64,
    kind: AnalyticsEventKind,

    pub static fn decode(reader: auto): AnalyticsEventHeader! {
        let res = std.mem.zeroes(AnalyticsEventHeader);

        res.timestamp = reader.readValue(AnalyticsUint64)?;
        res.kind = reader.readValue(AnalyticsEventKind)?;
        return res;
    }

    pub fn encode(this, writer: auto): void! {
        writer.writeValue(@TypeOf(this.timestamp), this.timestamp)?;
        writer.writeEnum(this.kind)?;
    }
}

pub struct AnalyticsEventList {
    header: AnalyticsEventListHeader,
    event_count: u32 = 0,

    pub static fn decode(reader: auto): AnalyticsEventList! {
        let res = std.mem.zeroes(AnalyticsEventList);

        res.header = reader.readValue(AnalyticsEventListHeader)?;
        res.event_count = reader.readValue(u32)?;
        return res;
    }

    pub fn encode(this, writer: auto): void! {
        writer.writeValue(@TypeOf(this.header), this.header)?;
        writer.writeInt(this.event_count)?;
    }
}

pub struct analytics {
    pub const OperatingSystem = AnalyticsOperatingSystem;
    pub const Architecture = AnalyticsArchitecture;
    pub const EventKind = AnalyticsEventKind;
    pub const Platform = AnalyticsPlatform;
    pub const Uint64 = AnalyticsUint64;
    pub const EventListHeader = AnalyticsEventListHeader;
    pub const EventHeader = AnalyticsEventHeader;
    pub const EventList = AnalyticsEventList;
}
