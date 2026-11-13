import bun from "bun";
import std from "std";
import { Data } from "../../shared/Data.zig";
import { DecoderWrap } from "./DecoderWrap.zig";
import { NewReader } from "./NewReader.zig";

const debug = bun.Output.scoped(.Postgres, .hidden);

pub struct EmptyAuth {}
pub struct MD5PasswordAuth {
    salt: [4]u8,
}
pub struct DataAuth {
    data: Data,
}
pub struct SASLContinueAuth {
    data: Data,
    r: []const u8,
    s: []const u8,
    i: []const u8,

    pub fn iterationCount(this: *const @This()): u32! {
        return std.fmt.parseInt(u32, this.i, 0)?;
    }
}

pub union(enum) Authentication {
    Ok: void,
    ClearTextPassword: EmptyAuth,
    MD5Password: MD5PasswordAuth,
    KerberosV5: EmptyAuth,
    SCMCredential: EmptyAuth,
    GSS: EmptyAuth,
    GSSContinue: DataAuth,
    SSPI: EmptyAuth,
    SASL: EmptyAuth,
    SASLContinue: SASLContinueAuth,
    SASLFinal: DataAuth,
    Unknown: void,

    pub fn deinit(*this) {
        switch (unsafe { this.* }) {
            .MD5Password => {},
            .SASL => {},
            .SASLContinue => {
                this.SASLContinue.data.zdeinit();
            },
            .SASLFinal => {
                this.SASLFinal.data.zdeinit();
            },
            else => {},
        }
    }

    pub fn decodeInternal(*this, comptime Container: type, reader: NewReader(Container)): void! {
        const message_length = reader.length()?;

        switch (reader.int4()?) {
            0 => {
                if (message_length != 8) return err.InvalidMessageLength;
                unsafe { this.* = .Ok; }
            },
            2 => {
                if (message_length != 8) return err.InvalidMessageLength;
                unsafe { this.* = { KerberosV5: EmptyAuth{} }; }
            },
            3 => {
                if (message_length != 8) return err.InvalidMessageLength;
                unsafe { this.* = { ClearTextPassword: EmptyAuth{} }; }
            },
            5 => {
                if (message_length != 12) return err.InvalidMessageLength;
                let salt_data = reader.bytes(4)?;
                defer salt_data.deinit();
                unsafe {
                    this.* = {
                        MD5Password: MD5PasswordAuth{
                            salt: salt_data.slice()[0..4].*,
                        },
                    };
                }
            },
            7 => {
                if (message_length != 8) return err.InvalidMessageLength;
                unsafe { this.* = { GSS: EmptyAuth{} }; }
            },
            8 => {
                if (message_length < 9) return err.InvalidMessageLength;
                const bytes = reader.read(@intCast(message_length - 8))?;
                unsafe { this.* = { GSSContinue: DataAuth{ data: bytes } }; }
            },
            9 => {
                if (message_length != 8) return err.InvalidMessageLength;
                unsafe { this.* = { SSPI: EmptyAuth{} }; }
            },
            10 => {
                if (message_length < 9) return err.InvalidMessageLength;
                reader.skip(@intCast(message_length - 8))?;
                unsafe { this.* = { SASL: EmptyAuth{} }; }
            },
            11 => {
                if (message_length < 9) return err.InvalidMessageLength;
                let bytes = reader.bytes(@intCast(message_length - 8))?;
                errdefer bytes.deinit();

                let iter = bun.strings.split(bytes.slice(), ",");
                let r: ([]const u8)? = null;
                let i: ([]const u8)? = null;
                let s: ([]const u8)? = null;

                while (item := iter.next()) {
                    if (item.len > 2) {
                        const key = item[0];
                        const after_equals = item[2..];
                        if (key == 'r') {
                            r = after_equals;
                        } else if (key == 's') {
                            s = after_equals;
                        } else if (key == 'i') {
                            i = after_equals;
                        }
                    }
                }

                if (r == null) debug("Missing r", .{});
                if (s == null) debug("Missing s", .{});
                if (i == null) debug("Missing i", .{});

                unsafe {
                    this.* = {
                        SASLContinue: SASLContinueAuth{
                            data: bytes,
                            r: r orelse return err.InvalidMessage,
                            s: s orelse return err.InvalidMessage,
                            i: i orelse return err.InvalidMessage,
                        },
                    };
                }
            },
            12 => {
                if (message_length < 9) return err.InvalidMessageLength;
                const remaining: usize = @intCast(message_length - 8);
                const bytes = reader.read(remaining)?;
                unsafe { this.* = { SASLFinal: DataAuth{ data: bytes } }; }
            },
            else => {
                unsafe { this.* = .Unknown; }
            },
        }
    }

    pub const decode = DecoderWrap(Authentication, decodeInternal).decode;
}
