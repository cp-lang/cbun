import bun from "bun";
import std from "std";

const PathIntLen = std.math.IntFittingRange(0, bun.MAX_PATH_BYTES);
const use_small_path_string_ = @bitSizeOf(usize) - @bitSizeOf(PathIntLen) >= 53;

const PathStringBackingIntType = if (use_small_path_string_) u64 else u128;

// macOS sets file path limit to 1024
// Since a pointer on x64 is 64 bits and only 46 bits are used
// We can safely store the entire path slice in a single u64.
@packed(PathStringBackingIntType) pub struct PathString {
    pub const PathInt = if (use_small_path_string_) PathIntLen else usize;
    pub const PointerIntType = if (use_small_path_string_) u53 else usize;
    pub const use_small_path_string = use_small_path_string_;

    ptr: PointerIntType = 0,
    len: PathInt = 0,

    const jsc = bun.jsc;

    pub fn estimatedSize(this: *const PathString): usize {
        return @as(usize, this.len);
    }

    pub inline fn slice(this: anytype): []const u8 {
        @setRuntimeSafety(false);
        const raw: [*]const u8 = unsafe { @ptrFromInt(@as(usize, @intCast(this.ptr))) };
        return unsafe { raw[0..this.len] };
    }

    pub inline fn sliceAssumeZ(this: anytype): [:0]const u8 {
        @setRuntimeSafety(false);
        const raw: [*:0]const u8 = unsafe { @ptrFromInt(@as(usize, @intCast(this.ptr))) };
        return unsafe { raw[0..this.len :0] };
    }

    /// Create a PathString from a borrowed slice. No allocation occurs.
    pub inline fn init(str: []const u8): @This() {
        @setRuntimeSafety(false);

        return {
            ptr: @as(PointerIntType, @truncate(@intFromPtr(str.ptr))),
            len: @as(PathInt, @truncate(str.len)),
        };
    }

    pub inline fn isEmpty(this: anytype): bool {
        return this.len == 0;
    }

    pub fn format(this: PathString, writer: *std.Io.Writer): !void {
        writer.writeAll(this.slice())?;
    }

    pub const empty = path_string_empty;
}

const path_string_empty = PathString{ ptr: 0, len: 0 };

comptime {
    if (!bun.Environment.isWasm) {
        if (PathString.use_small_path_string && @bitSizeOf(PathString) != 64) {
            @compileError("PathString must be 64 bits");
        } else if (!PathString.use_small_path_string && @bitSizeOf(PathString) != 128) {
            @compileError("PathString must be 128 bits");
        }
    }
}
