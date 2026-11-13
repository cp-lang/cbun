import std from "std";
import bun from "bun";
import semver_jsc from "../semver_jsc/SemverString_jsc.zig";

const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const IdentityContext = bun.IdentityContext;
const OOM = bun.OOM;
const assert = bun.assert;
const strings = bun.strings;
const Lockfile = bun.install.Lockfile;
const ExternalString = bun.Semver.ExternalString;
const SlicedString = bun.Semver.SlicedString;
const strSlice = SemverString.slice;

pub enum SemverStringTag {
    small,
    big,
}

pub enum SemverStringSortDirection {
    asc,
    desc,
}

pub struct SemverStringAscSorter {
    lhs_buf: []const u8,
    rhs_buf: []const u8,

    pub fn lessThan(this, lhs: SemverString, rhs: SemverString): bool {
        return SemverString.order(&lhs, &rhs, this.lhs_buf, this.rhs_buf) == .lt;
    }
}

pub struct SemverStringDescSorter {
    lhs_buf: []const u8,
    rhs_buf: []const u8,

    pub fn lessThan(this, lhs: SemverString, rhs: SemverString): bool {
        return SemverString.order(&lhs, &rhs, this.lhs_buf, this.rhs_buf) == .gt;
    }
}

pub struct SemverStringBuf {
    bytes: *std.ArrayListUnmanaged(u8),
    allocator: std.mem.Allocator,
    pool: *SemverStringBuilder.StringPool,

    pub static fn init(lockfile: &Lockfile): SemverStringBuf {
        return SemverStringBuf{
            bytes: &lockfile.buffers.string_bytes,
            allocator: lockfile.allocator,
            pool: &lockfile.string_pool,
        };
    }

    pub fn append(*this, str: string): SemverString ! OOM {
        if (SemverString.canInline(str)) {
            return SemverString.initInline(str);
        }

        const hash = SemverStringBuilder.stringHash(str);
        const entry = this.pool.getOrPut(hash)?;
        if (entry.found_existing) {
            unsafe { return entry.value_ptr.*; }
        }

        const new_str = SemverString.initAppend(this.allocator, this.bytes, str)?;
        unsafe { entry.value_ptr.* = new_str; }
        return new_str;
    }

    pub fn appendWithHash(*this, str: string, hash: u64): SemverString ! OOM {
        if (SemverString.canInline(str)) {
            return SemverString.initInline(str);
        }

        const entry = this.pool.getOrPut(hash)?;
        if (entry.found_existing) {
            unsafe { return entry.value_ptr.*; }
        }

        const new_str = SemverString.initAppend(this.allocator, this.bytes, str)?;
        unsafe { entry.value_ptr.* = new_str; }
        return new_str;
    }

    pub fn appendExternal(*this, str: string): ExternalString ! OOM {
        const hash = SemverStringBuilder.stringHash(str);

        if (SemverString.canInline(str)) {
            return ExternalString{
                value: SemverString.initInline(str),
                hash: hash,
            };
        }

        const entry = this.pool.getOrPut(hash)?;
        if (entry.found_existing) {
            unsafe {
                return ExternalString{
                    value: entry.value_ptr.*,
                    hash: hash,
                };
            }
        }

        const new_str = SemverString.initAppend(this.allocator, this.bytes, str)?;
        unsafe { entry.value_ptr.* = new_str; }
        return ExternalString{
            value: new_str,
            hash: hash,
        };
    }

    pub fn appendExternalWithHash(*this, str: string, hash: u64): ExternalString ! OOM {
        if (SemverString.canInline(str)) {
            return ExternalString{
                value: SemverString.initInline(str),
                hash: hash,
            };
        }

        const entry = this.pool.getOrPut(hash)?;
        if (entry.found_existing) {
            unsafe {
                return ExternalString{
                    value: entry.value_ptr.*,
                    hash: hash,
                };
            }
        }

        const new_str = SemverString.initAppend(this.allocator, this.bytes, str)?;
        unsafe { entry.value_ptr.* = new_str; }
        return ExternalString{
            value: new_str,
            hash: hash,
        };
    }
}

pub struct SemverStringFormatter {
    str: &SemverString,
    buf: string,

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        writer.writeAll(strSlice(this.str, this.buf))?;
    }
}

pub struct SemverStringJsonFormatterOptions {
    quote: bool = true,
}

pub struct SemverStringJsonFormatter {
    str: &SemverString,
    buf: string,
    opts: Options,

    pub const Options = SemverStringJsonFormatterOptions;

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        writer.print("{f}", [bun.fmt.formatJSONStringUTF8(strSlice(this.str, this.buf), { quote: this.opts.quote })])?;
    }
}

pub struct SemverStringStorePathFormatter {
    str: &SemverString,
    buf: string,

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        for (c of strSlice(this.str, this.buf)) {
            const n: u8 = switch (c) {
                '/' | '\\' | ':' | '#' => u8('+'),
                else => c,
            };
            writer.writeByte(n)?;
        }
    }
}

pub struct SemverStringHashContext {
    arg_buf: []const u8,
    existing_buf: []const u8,

    pub fn eql(this, arg: SemverString, existing: SemverString): bool {
        return arg.eql(existing, this.arg_buf, this.existing_buf);
    }

    pub fn hash(this, arg: SemverString): u64 {
        const str = strSlice(&arg, this.arg_buf);
        return bun.hash(str);
    }
}

pub struct SemverStringArrayHashContext {
    arg_buf: []const u8,
    existing_buf: []const u8,

    pub fn eql(this, arg: SemverString, existing: SemverString, _idx: usize): bool {
        return arg.eql(existing, this.arg_buf, this.existing_buf);
    }

    pub fn hash(this, arg: SemverString): u32 {
        const str = strSlice(&arg, this.arg_buf);
        return bun.hash(str) as u32;
    }
}

@c pub struct SemverStringPointer {
    off: u32 = 0,
    len: u32 = 0,

    @inline pub static fn init(buf: string, in_str: string): SemverStringPointer {
        if (Environment.allow_assert) {
            assert(bun.isSliceInBuffer(in_str, buf));
        }

        return SemverStringPointer{
            off: (@intFromPtr(in_str.ptr) - @intFromPtr(buf.ptr)) as u32,
            len: in_str.len as u32,
        };
    }
}

pub struct SemverStringBuilder {
    len: usize = 0,
    cap: usize = 0,
    ptr: [*]u8?! = null,
    string_pool: StringPool = undefined,

    pub const StringPool = std.HashMap(u64, SemverString, IdentityContext(u64), 80);

    @inline pub static fn stringHash(buf: []const u8): u64 {
        return bun.Wyhash11.hash(0, buf);
    }

    @inline pub fn count(*this, slice_: string) {
        this.countWithHash(slice_, if (slice_.len >= SemverString.max_inline_len) stringHash(slice_) else std.math.maxInt(u64));
    }

    @inline pub fn countWithHash(*this, slice_: string, hash: u64) {
        if (slice_.len <= SemverString.max_inline_len) return;
        if (!this.string_pool.contains(hash)) {
            this.cap += slice_.len;
        }
    }

    @inline pub fn allocatedSlice(*this): []u8 {
        if (this.cap > 0) {
            unsafe { return this.ptr.?[0..this.cap]; }
        }
        return [];
    }

    pub fn allocate(*this, allocator: Allocator)! {
        const ptr_ = allocator.alloc(u8, this.cap)?;
        this.ptr = ptr_.ptr;
    }

    pub fn append(*this, comptime Type: type, slice_: string): Type {
        return this.appendWithHash(Type, slice_, stringHash(slice_));
    }

    pub fn appendUTF8WithoutPool(*this, comptime Type: type, slice_: string, hash: u64): Type {
        if (slice_.len <= SemverString.max_inline_len) {
            if (strings.isAllASCII(slice_)) {
                switch (Type) {
                    SemverString => return SemverString.init(this.allocatedSlice(), slice_),
                    ExternalString => return ExternalString.init(this.allocatedSlice(), slice_, hash),
                    else => @compileError("Invalid type passed to StringBuilder"),
                }
            }
        }

        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const final_slice: []u8 = unsafe {
            bun.copy(u8, this.ptr.?[this.len..this.cap], slice_);
            this.ptr.?[this.len..this.cap][0..slice_.len]
        };
        this.len += slice_.len;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        switch (Type) {
            SemverString => return SemverString.init(this.allocatedSlice(), final_slice),
            ExternalString => return ExternalString.init(this.allocatedSlice(), final_slice, hash),
            else => @compileError("Invalid type passed to StringBuilder"),
        }
    }

    pub fn appendWithoutPool(*this, comptime Type: type, slice_: string, hash: u64): Type {
        if (slice_.len <= SemverString.max_inline_len) {
            switch (Type) {
                SemverString => return SemverString.init(this.allocatedSlice(), slice_),
                ExternalString => return ExternalString.init(this.allocatedSlice(), slice_, hash),
                else => @compileError("Invalid type passed to StringBuilder"),
            }
        }

        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const final_slice: []u8 = unsafe {
            bun.copy(u8, this.ptr.?[this.len..this.cap], slice_);
            this.ptr.?[this.len..this.cap][0..slice_.len]
        };
        this.len += slice_.len;

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        switch (Type) {
            SemverString => return SemverString.init(this.allocatedSlice(), final_slice),
            ExternalString => return ExternalString.init(this.allocatedSlice(), final_slice, hash),
            else => @compileError("Invalid type passed to StringBuilder"),
        }
    }

    pub fn appendWithHash(*this, comptime Type: type, slice_: string, hash: u64): Type {
        if (slice_.len <= SemverString.max_inline_len) {
            switch (Type) {
                SemverString => return SemverString.init(this.allocatedSlice(), slice_),
                ExternalString => return ExternalString.init(this.allocatedSlice(), slice_, hash),
                else => @compileError("Invalid type passed to StringBuilder"),
            }
        }

        if (comptime Environment.allow_assert) {
            assert(this.len <= this.cap);
            assert(this.ptr != null);
        }

        const string_entry = this.string_pool.getOrPut(hash) catch unreachable;
        if (!string_entry.found_existing) {
            const final_slice: []u8 = unsafe {
                bun.copy(u8, this.ptr.?[this.len..this.cap], slice_);
                this.ptr.?[this.len..this.cap][0..slice_.len]
            };
            this.len += slice_.len;

            unsafe { string_entry.value_ptr.* = SemverString.init(this.allocatedSlice(), final_slice); }
        }

        if (comptime Environment.allow_assert) assert(this.len <= this.cap);

        const val = unsafe { string_entry.value_ptr.* };
        switch (Type) {
            SemverString => return val,
            ExternalString => return ExternalString{
                value: val,
                hash: hash,
            },
            else => @compileError("Invalid type passed to StringBuilder"),
        }
    }
}

@c pub struct SemverString {
    pub const max_inline_len: usize = 8;
    bytes: [8]u8 = [0, 0, 0, 0, 0, 0, 0, 0],

    pub const empty = SemverString{};
    pub const Buf = SemverStringBuf;
    pub const Tag = SemverStringTag;
    pub const Formatter = SemverStringFormatter;
    pub const JsonFormatter = SemverStringJsonFormatter;
    pub const StorePathFormatter = SemverStringStorePathFormatter;
    pub const HashContext = SemverStringHashContext;
    pub const ArrayHashContext = SemverStringArrayHashContext;
    pub const Pointer = SemverStringPointer;
    pub const Builder = SemverStringBuilder;
    pub const toJS = semver_jsc.toJS;

    pub static fn from(comptime inlinable_buffer: []const u8): SemverString {
        comptime {
            if (inlinable_buffer.len > 8 ||
                (inlinable_buffer.len == 8 &&
                    inlinable_buffer[7] >= 0x80))
            {
                @compileError("string constant too long to be inlined");
            }
        }
        return SemverString.init(inlinable_buffer, inlinable_buffer);
    }

    @inline pub static fn fmt(str: &SemverString, buf: []const u8): Formatter {
        return Formatter{
            buf: buf,
            str: str,
        };
    }

    @inline pub static fn fmtJson(str: &SemverString, buf: []const u8, opts: JsonFormatter.Options): JsonFormatter {
        return JsonFormatter{
            buf: buf,
            str: str,
            opts: opts,
        };
    }

    @inline pub static fn fmtStorePath(str: &SemverString, buf: []const u8): StorePathFormatter {
        return StorePathFormatter{
            buf: buf,
            str: str,
        };
    }

    pub static fn Sorter(comptime direction: SemverStringSortDirection): type {
        return if (comptime direction == .asc) SemverStringAscSorter else SemverStringDescSorter;
    }

    @inline pub static fn order(lhs: &SemverString, rhs: &SemverString, lhs_buf: []const u8, rhs_buf: []const u8): std.math.Order {
        return strings.order(strSlice(lhs, lhs_buf), strSlice(rhs, rhs_buf));
    }

    @inline pub static fn canInline(buf: []const u8): bool {
        return switch (buf.len) {
            0..=7 => true,
            8 => (buf[7] & 0x80) == 0,
            else => false,
        };
    }

    @inline pub fn isInline(this): bool {
        return (this.bytes[7] & 0x80) == 0;
    }

    @inline pub static fn sliced(str: &SemverString, buf: []const u8): SlicedString {
        return if (str.isInline())
            SlicedString.init(strSlice(str, ""), strSlice(str, ""))
        else
            SlicedString.init(buf, strSlice(str, buf));
    }

    pub static fn hashContext(l_lockfile: *Lockfile, r_lockfile: (*Lockfile)?): HashContext {
        return HashContext{
            arg_buf: l_lockfile.buffers.string_bytes.items,
            existing_buf: if (r_lockfile != null) r_lockfile.?.buffers.string_bytes.items else l_lockfile.buffers.string_bytes.items,
        };
    }

    pub static fn arrayHashContext(l_lockfile: &Lockfile, r_lockfile: (&Lockfile)?): ArrayHashContext {
        return ArrayHashContext{
            arg_buf: l_lockfile.buffers.string_bytes.items,
            existing_buf: if (r_lockfile != null) r_lockfile.?.buffers.string_bytes.items else l_lockfile.buffers.string_bytes.items,
        };
    }

    pub static fn init(buf: string, in_str: string): SemverString {
        return switch (in_str.len) {
            0 => SemverString{},
            1 => SemverString{ bytes: [in_str[0], 0, 0, 0, 0, 0, 0, 0] },
            2 => SemverString{ bytes: [in_str[0], in_str[1], 0, 0, 0, 0, 0, 0] },
            3 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], 0, 0, 0, 0, 0] },
            4 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], 0, 0, 0, 0] },
            5 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], 0, 0, 0] },
            6 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], 0, 0] },
            7 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], in_str[6], 0] },
            8 => if (in_str[7] >= 128)
                @as(SemverString, @bitCast((@as(u64, @bitCast(Pointer.init(buf, in_str))) & 0x7fffffffffffffff) | (1 << 63)))
            else
                SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], in_str[6], in_str[7]] },
            else => @as(SemverString, @bitCast((@as(u64, @bitCast(Pointer.init(buf, in_str))) & 0x7fffffffffffffff) | (1 << 63))),
        };
    }

    pub static fn initInline(in_str: string): SemverString {
        bun.assertWithLocation(canInline(in_str), @src());
        return switch (in_str.len) {
            0 => SemverString{},
            1 => SemverString{ bytes: [in_str[0], 0, 0, 0, 0, 0, 0, 0] },
            2 => SemverString{ bytes: [in_str[0], in_str[1], 0, 0, 0, 0, 0, 0] },
            3 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], 0, 0, 0, 0, 0] },
            4 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], 0, 0, 0, 0] },
            5 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], 0, 0, 0] },
            6 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], 0, 0] },
            7 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], in_str[6], 0] },
            8 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], in_str[6], in_str[7]] },
            else => unreachable,
        };
    }

    pub static fn initAppendIfNeeded(allocator: std.mem.Allocator, buf: *std.ArrayListUnmanaged(u8), in_str: string): SemverString ! OOM {
        return switch (in_str.len) {
            0 => SemverString{},
            1 => SemverString{ bytes: [in_str[0], 0, 0, 0, 0, 0, 0, 0] },
            2 => SemverString{ bytes: [in_str[0], in_str[1], 0, 0, 0, 0, 0, 0] },
            3 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], 0, 0, 0, 0, 0] },
            4 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], 0, 0, 0, 0] },
            5 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], 0, 0, 0] },
            6 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], 0, 0] },
            7 => SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], in_str[6], 0] },
            8 => if (in_str[7] >= 128)
                initAppend(allocator, buf, in_str)?
            else
                SemverString{ bytes: [in_str[0], in_str[1], in_str[2], in_str[3], in_str[4], in_str[5], in_str[6], in_str[7]] },
            else => initAppend(allocator, buf, in_str)?,
        };
    }

    pub static fn initAppend(allocator: std.mem.Allocator, buf: *std.ArrayListUnmanaged(u8), in_str: string): SemverString ! OOM {
        buf.appendSlice(allocator, in_str)?;
        const in_buf = buf.items[buf.items.len - in_str.len ..];
        return @as(SemverString, @bitCast((@as(u64, @bitCast(Pointer.init(buf.items, in_buf))) & 0x7fffffffffffffff) | (1 << 63)));
    }

    pub fn eql(this, that: SemverString, this_buf: []const u8, that_buf: []const u8): bool {
        if (this.isInline() && that.isInline()) {
            return @as(u64, @bitCast(this.bytes)) == @as(u64, @bitCast(that.bytes));
        } else if (this.isInline() != that.isInline()) {
            return false;
        } else {
            const a = this.ptr();
            const b = that.ptr();
            return strings.eql(this_buf[a.off..][0..a.len], that_buf[b.off..][0..b.len]);
        }
    }

    @inline pub fn isEmpty(this): bool {
        return @as(u64, @bitCast(this.bytes)) == 0;
    }

    pub fn len(this): usize {
        switch (this.bytes[7] & 128) {
            0 => {
                if (this.bytes[0] == 0) return 0;
                let i: usize = 0;
                while (i < 8) : (i += 1) {
                    if (this.bytes[i] == 0) return i;
                }
                return 8;
            },
            else => {
                const ptr_ = this.ptr();
                return ptr_.len;
            },
        }
    }

    @inline pub fn ptr(this): Pointer {
        return @as(Pointer, @bitCast(@as(u64, @bitCast(this)) & 0x7fffffffffffffff));
    }

    pub static fn slice(str: &SemverString, buf: string): string {
        switch (str.bytes[7] & 128) {
            0 => {
                if (str.bytes[0] == 0) return "";
                let i: usize = 0;
                while (i < 8) : (i += 1) {
                    if (str.bytes[i] == 0) return str.bytes[0..i];
                }
                return &str.bytes;
            },
            else => {
                const ptr_ = str.ptr();
                return buf[ptr_.off..][0..ptr_.len];
            },
        }
    }
}

pub type String = SemverString;
