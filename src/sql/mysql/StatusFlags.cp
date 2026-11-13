import std from "std";

pub enum StatusFlag: u16 {
    SERVER_STATUS_IN_TRANS = 1,
    SERVER_STATUS_AUTOCOMMIT = 2,
    SERVER_MORE_RESULTS_EXISTS = 8,
    SERVER_STATUS_NO_GOOD_INDEX_USED = 16,
    SERVER_STATUS_NO_INDEX_USED = 32,
    SERVER_STATUS_CURSOR_EXISTS = 64,
    SERVER_STATUS_LAST_ROW_SENT = 128,
    SERVER_STATUS_DB_DROPPED = 256,
    SERVER_STATUS_NO_BACKSLASH_ESCAPES = 512,
    SERVER_STATUS_METADATA_CHANGED = 1024,
    SERVER_QUERY_WAS_SLOW = 2048,
    SERVER_PS_OUT_PARAMS = 4096,
    SERVER_STATUS_IN_TRANS_READONLY = 8192,
    SERVER_SESSION_STATE_CHANGED = 16384,
}

pub struct StatusFlags {
    _value: u16 = 0,

    pub fn format(this: StatusFlags, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        let first = true;
        @for (field of comptime std.meta.fieldNames(StatusFlags)) {
            if (@TypeOf(@field(this, field)) == bool) {
                if (@field(this, field)) {
                    if (!first) {
                        writer.writeAll(", ")?;
                    }
                    first = false;
                    writer.writeAll(field)?;
                }
            }
        }
    }

    pub fn has(this: StatusFlags, flag: StatusFlag): bool {
        return (this._value & @as(u16, @intFromEnum(flag))) != 0;
    }

    pub fn toInt(this: StatusFlags): u16 {
        return this._value;
    }

    pub static fn fromInt(flags: u16): StatusFlags {
        return StatusFlags{
            _value: flags,
        };
    }
}
