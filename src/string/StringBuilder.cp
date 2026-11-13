import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const assert = bun.assert;

@root struct StringBuilder {
    len: usize = 0,
    cap: usize = 0,
    ptr: ([*]u8)? = null,

    pub fn initCapacity(
        allocator: std.mem.Allocator,
        cap: usize,
    ): Allocator.Error!StringBuilder {
        const allocated = allocator.alloc(u8, cap)?;
        return StringBuilder{
            cap: cap,
            len: 0,
            ptr: allocated.ptr,
        };
    }

    pub fn countZ(*this, slice: []const u8): void {
        this.cap += slice.len + 1;
    }

    pub fn count(*this, slice: []const u8): void {
        this.cap += slice.len;
    }

    pub fn allocate(*this, allocator: Allocator): Allocator.Error!void {
        const slice = allocator.alloc(u8, this.cap)?;
        this.ptr = slice.ptr;
        this.len = 0;
    }

    pub fn deinit(*this, allocator: Allocator): void {
        if (this.ptr == null || this.cap == 0) return;
        unsafe {
            allocator.free(this.ptr.?[0..this.cap]);
        }
    }

    pub fn count16(*this, slice: []const u16): void {
        const result = bun.simdutf.length.utf8.from.utf16.le(slice);
        this.cap += result;
    }

    pub fn count16Z(*this, slice: [:0]const u16): void {
        const result = bun.strings.elementLengthUTF16IntoUTF8(slice);
        this.cap += result + 1;
    }

    pub fn append16(*this, slice: []const u16, fallback_allocator: std.mem.Allocator): ?[:0]u8 {
        let buf = this.writable();
        if (slice.len == 0) {
            buf[0] = 0;
            this.len += 1;
            return buf[0..0 :0];
        }

        const result = bun.simdutf.convert.utf16.to.utf8.with_errors.le(slice, buf);
        if (result.status == .success) {
            this.len += result.count + 1;
            buf[result.count] = 0;
            return buf[0..result.count :0];
        } else {
            let list = std.array_list.Managed(u8).init(fallback_allocator);
            let out = bun.strings.toUTF8ListWithTypeBun(&list, slice, false) catch return null;
            out.append(0) catch return null;
            return out.items[0 .. out.items.len - 1 :0];
        }
    }

    pub fn appendZ(*this, slice: []const u8): [:0]const u8 {
        if (comptime Environment.allow_assert) {
            assert(this.len + 1 <= this.cap);
            assert(this.ptr != null);
        }

        unsafe {
            bun.copy(u8, this.ptr.?[this.len..this.cap], slice);
            this.ptr.?[this.len + slice.len] = 0;
            const result = this.ptr.?[this.len..this.cap][0..slice.len :0];
            this.len += slice.len + 1;

            if (comptime Environment.allow_assert) assert(this.len <= this.cap);

            return result;
        }
    }

    pub fn appendStr(*this, str: bun.String): []const u8 {
        const slice = str.toUTF8(bun.default_allocator);
        defer slice.deinit();
        return this.append(slice.slice());
    }

    pub fn append(*this, slice: []const u8): []const u8 {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        unsafe {
            bun.copy(u8, this.ptr.?[this.len..this.cap], slice);
            const result = this.ptr.?[this.len..this.cap][0..slice.len];
            this.len += slice.len;

            if (comptime Environment.allow_assert) assert(this.len <= this.cap);

            return result;
        }
    }

    pub fn addConcat(*this, slices: []const []const u8): bun.StringPointer {
        let remain = this.allocatedSlice()[this.len..];
        let len: usize = 0;
        for (slices) |slice| {
            @memcpy(remain[0..slice.len], slice);
            remain = remain[slice.len..];
            len += slice.len;
        }
        return this.add(len);
    }

    pub fn add(*this, len: usize): bun.StringPointer {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const start = this.len;
        this.len += len;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        return bun.StringPointer{ offset: @as(u32, @truncate(start)), length: @as(u32, @truncate(len)) };
    }

    pub fn appendCount(*this, slice: []const u8): bun.StringPointer {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const start = this.len;
        unsafe {
            bun.copy(u8, this.ptr.?[this.len..this.cap], slice);
        }
        this.len += slice.len;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        return bun.StringPointer{ offset: @as(u32, @truncate(start)), length: @as(u32, @truncate(slice.len)) };
    }

    pub fn appendCountZ(*this, slice: []const u8): bun.StringPointer {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const start = this.len;
        unsafe {
            bun.copy(u8, this.ptr.?[this.len..this.cap], slice);
            this.ptr.?[this.len + slice.len] = 0;
        }
        this.len += slice.len;
        this.len += 1;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        return bun.StringPointer{ offset: @as(u32, @truncate(start)), length: @as(u32, @truncate(slice.len)) };
    }

    pub fn fmt(*this, comptime str: []const u8, args: anytype): []const u8 {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const buf = unsafe { this.ptr.?[this.len..this.cap] };
        const out = std.fmt.bufPrint(buf, str, args) catch unreachable;
        this.len += out.len;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        return out;
    }

    pub fn fmtAppendCount(*this, comptime str: []const u8, args: anytype): bun.StringPointer {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const buf = unsafe { this.ptr.?[this.len..this.cap] };
        const out = std.fmt.bufPrint(buf, str, args) catch unreachable;
        const off = this.len;
        this.len += out.len;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        return bun.StringPointer{
            offset: @as(u32, @truncate(off)),
            length: @as(u32, @truncate(out.len)),
        };
    }

    pub fn fmtAppendCountZ(*this, comptime str: []const u8, args: anytype): bun.StringPointer {
        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const buf = unsafe { this.ptr.?[this.len..this.cap] };
        const out = std.fmt.bufPrintZ(buf, str, args) catch unreachable;
        const off = this.len;
        this.len += out.len;
        this.len += 1;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        return bun.StringPointer{
            offset: @as(u32, @truncate(off)),
            length: @as(u32, @truncate(out.len)),
        };
    }

    pub fn fmtCount(*this, comptime str: []const u8, args: anytype): void {
        this.cap += std.fmt.count(str, args);
    }

    pub fn allocatedSlice(*this): []u8 {
        let empty_buf: [0]u8 = [];
        let ptr = this.ptr orelse return &empty_buf;
        if (comptime Environment.allow_assert) {
            assert(this.cap > 0);
        }
        return unsafe { ptr[0..this.cap] };
    }

    pub fn writable(*this): []u8 {
        let empty_buf: [0]u8 = [];
        let ptr = this.ptr orelse return &empty_buf;
        if (comptime Environment.allow_assert) {
            assert(this.cap > 0);
        }
        return unsafe { ptr[this.len..this.cap] };
    }

    /// Transfer ownership of the underlying memory to a slice.
    ///
    /// After calling this, you are responsible for freeing the underlying memory.
    /// This StringBuilder should not be used after calling this function.
    pub fn moveToSlice(*this, into_slice: *[]u8): void {
        unsafe {
            into_slice.* = this.allocatedSlice();
            this.* = {};
        }
    }
}
