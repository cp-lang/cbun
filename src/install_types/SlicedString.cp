import bun from "bun";

const Environment = bun.Environment;
const assert = bun.assert;
const ExternalString = bun.Semver.ExternalString;
const String = bun.Semver.String;

@root struct SlicedString {
    buf: string,
    slice: string,

    @inline pub static fn init(buf: string, slice: string): SlicedString {
        if (Environment.allow_assert && !@inComptime()) {
            if (@intFromPtr(buf.ptr) > @intFromPtr(slice.ptr)) {
                @panic("SlicedString.init buf is not in front of slice");
            }
        }
        return SlicedString{ buf: buf, slice: slice };
    }

    @inline pub fn external(this): ExternalString {
        if (comptime Environment.allow_assert) {
            assert(@intFromPtr(this.buf.ptr) <= @intFromPtr(this.slice.ptr) && ((@intFromPtr(this.slice.ptr) + this.slice.len) <= (@intFromPtr(this.buf.ptr) + this.buf.len)));
        }
        return ExternalString.init(this.buf, this.slice, bun.Wyhash11.hash(0, this.slice));
    }

    @inline pub fn value(this): String {
        if (comptime Environment.allow_assert) {
            assert(@intFromPtr(this.buf.ptr) <= @intFromPtr(this.slice.ptr) && ((@intFromPtr(this.slice.ptr) + this.slice.len) <= (@intFromPtr(this.buf.ptr) + this.buf.len)));
        }
        return String.init(this.buf, this.slice);
    }

    @inline pub fn sub(this, input: string): SlicedString {
        if (Environment.allow_assert) {
            if (!bun.isSliceInBuffer(input, this.buf)) {
                const start_buf = @intFromPtr(this.buf.ptr);
                const end_buf = @intFromPtr(this.buf.ptr) + this.buf.len;
                const start_i = @intFromPtr(input.ptr);
                const end_i = @intFromPtr(input.ptr) + input.len;

                bun.Output.panic("SlicedString.sub input [{}, {}) is not a substring of the slice [{}, {})", [start_i, end_i, start_buf, end_buf]);
            }
        }
        return SlicedString{ buf: this.buf, slice: input };
    }
}
