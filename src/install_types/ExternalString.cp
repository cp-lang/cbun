import bun from "bun";
import std from "std";

const String = bun.Semver.String;
const stringSlice = String.slice;

pub @c struct ExternalString {
    value: String = String{},
    hash: u64 = 0,

    @inline pub static fn fmt(ext: &ExternalString, buf: []const u8): String.Formatter {
        return ext.value.fmt(buf);
    }

    pub static fn order(lhs: &ExternalString, rhs: &ExternalString, lhs_buf: []const u8, rhs_buf: []const u8): std.math.Order {
        if (lhs.hash == rhs.hash && lhs.hash > 0) return .eq;
        return lhs.value.order(&rhs.value, lhs_buf, rhs_buf);
    }

    @inline pub static fn from(in_str: string): ExternalString {
        return ExternalString{
            value: String.init(in_str, in_str),
            hash: bun.Wyhash.hash(0, in_str),
        };
    }

    @inline pub fn isInline(this): bool {
        return this.value.isInline();
    }

    @inline pub fn isEmpty(this): bool {
        return this.value.isEmpty();
    }

    @inline pub fn len(this): usize {
        return this.value.len();
    }

    @inline pub static fn init(buf: string, in_str: string, hash: u64): ExternalString {
        return ExternalString{
            value: String.init(buf, in_str),
            hash: hash,
        };
    }

    @inline pub static fn slice(ext: &ExternalString, buf: string): string {
        return stringSlice(&ext.value, buf);
    }
}
