import std from "std";
import bun from "bun";
import string_jsc from "../jsc/bun_string_jsc.zig";
import immutable from "./immutable.zig";
import HashedString from "./HashedString.zig";
import MutableString from "./MutableString.zig";
import { PathString } from "./PathString.zig";
import { SmolStr } from "./SmolStr.zig";
import StringBuilder from "./StringBuilder.zig";
import StringJoiner from "./StringJoiner.zig";
import wtf from "./wtf.zig";
import { WTFString, WTFStringImpl, WTFStringImplStruct } from "./wtf.zig";

const OOM = bun.OOM;
const AsciiStatus = bun.strings.AsciiStatus;
const jsc = bun.jsc;
const ZigString = bun.jsc.ZigString;
const empty_u16_buf: [0]u16 = [];

export enum Tag : u8 {
    /// String is not valid. Observed on some failed operations.
    /// To prevent crashes, this value acts similarly to .Empty (such as length = 0)
    Dead = 0,
    /// String is backed by a WTF::StringImpl from JavaScriptCore.
    /// Can be in either `latin1` or `utf16le` encodings.
    WTFStringImpl = 1,
    /// Memory has an unknown owner, likely in Bun's Zig codebase. If `isGloballyAllocated`
    /// is set, then it is owned by mimalloc. When converted to a JS value it has to be cloned
    /// into a WTF::String.
    /// Can be in either `utf8` or `utf16le` encodings.
    ZigString = 2,
    /// Static memory that is guaranteed to never be freed. When converted to WTF::String,
    /// the memory is not cloned, but instead referenced with WTF::ExternalStringImpl.
    /// Can be in either `utf8` or `utf16le` encodings.
    StaticZigString = 3,
    /// String is ""
    Empty = 4,
}

export @c union StringImpl {
    ZigString: ZigString,
    WTFStringImpl: WTFStringImpl,
    StaticZigString: ZigString,
    Dead: void,
    Empty: void,
}

export enum StringWTFEncoding {
    latin1,
    utf16,

    pub fn Byte(comptime this: StringWTFEncoding): type {
        return switch (this) {
            .latin1 => u8,
            .utf16 => u16,
        };
    }
}

type ExternalFreeCallback = @c fn (*!, *!, u32) void;
type MutableBytes = []u8;
type MutableBytesZ = [:0]u8;
type OwnedSliceAscii = [[]u8, bool];

extern fn BunString__createExternal(
    bytes: [*]const u8,
    len: usize,
    isLatin1: bool,
    ptr: *!?,
    callback: (*const ExternalFreeCallback)?,
): String;

extern fn BunString__createStaticExternal(
    bytes: [*]const u8,
    len: usize,
    isLatin1: bool,
): String;

extern fn BunString__createExternalGloballyAllocatedLatin1(
    bytes: [*]u8,
    len: usize,
): String;

extern fn BunString__createExternalGloballyAllocatedUTF16(
    bytes: [*]u16,
    len: usize,
): String;

const string_empty = String{ tag: .Empty, value: { ZigString: .Empty } };
const string_dead = String{ tag: .Dead, value: { Dead: {} } };

/// Prefer using String instead of ZigString in new code.
export @c struct String {
    pub const name = "BunString";

    tag: Tag,
    value: StringImpl,

    pub const empty = string_empty;
    pub const dead = string_dead;
    pub const StringImplAllocator = wtf.StringImplAllocator;
    pub const WTFEncoding = StringWTFEncoding;

    pub fn toInt32(this: *const String): ?i32 {
        const val = bun.cpp.BunString__toInt32(this);
        if (val > std.math.maxInt(i32)) return null;
        return @intCast(val);
    }

    pub fn ascii(bytes: []const u8): String {
        return String{ tag: .ZigString, value: { ZigString: ZigString.init(bytes) } };
    }

    pub fn isGlobal(this): bool {
        return this.tag == Tag.ZigString && this.value.ZigString.isGloballyAllocated();
    }

    pub fn ensureHash(this): void {
        if (this.tag == .WTFStringImpl) this.value.WTFStringImpl.ensureHash();
    }

    pub const transferToJS = string_jsc.transferToJS;

    pub fn toOwnedSlice(this, allocator: std.mem.Allocator): OOM!MutableBytes {
        const [bytes, _unused] = this.toOwnedSliceImpl(allocator)?;
        return bytes;
    }

    /// Returns `.{ utf8_bytes, is_all_ascii }`.
    ///
    /// `false` means the string contains at least one non-ASCII character.
    pub fn toOwnedSliceReturningAllASCII(this, allocator: std.mem.Allocator): OOM!OwnedSliceAscii {
        const [bytes, ascii_status] = this.toOwnedSliceImpl(allocator)?;
        const is_ascii = switch (ascii_status) {
            .all_ascii => true,
            .non_ascii => false,
            .unknown => bun.strings.isAllASCII(bytes),
        };
        return [bytes, is_ascii];
    }

    fn toOwnedSliceImpl(this, allocator: std.mem.Allocator): ![[]u8, AsciiStatus] {
        switch (this.tag) {
            .ZigString => return [this.value.ZigString.toOwnedSlice(allocator)?, .unknown],
            .WTFStringImpl => {
                const utf8_slice = this.value.WTFStringImpl.toUTF8WithoutRef(allocator);
                errdefer utf8_slice.deinit();

                const ascii_status: AsciiStatus = if (utf8_slice.allocator.isNull())
                    .all_ascii
                else if (this.value.WTFStringImpl.is8Bit())
                    .non_ascii
                else
                    .unknown;

                const owned_slice = utf8_slice.cloneIfBorrowed(allocator)?;
                return [owned_slice.mut(), ascii_status];
            },
            .StaticZigString => return [this.value.StaticZigString.toOwnedSlice(allocator)?, .unknown],
            else => return [@constCast(""), .all_ascii],
        }
    }

    pub fn createIfDifferent(other: String, utf8_slice: []const u8): String {
        if (other.tag == .WTFStringImpl) {
            if (other.eqlUTF8(utf8_slice)) {
                return other.dupeRef();
            }
        }

        return String.cloneUTF8(utf8_slice);
    }

    static fn createUninitializedLatin1(len: usize): [String, []u8] {
        bun.assert(len > 0);
        const str = bun.cpp.BunString__fromLatin1Unitialized(len);
        if (str.tag == .Dead) {
            return [str, @constCast("")];
        }
        _ = String.validateRefCount(str);
        const wtf_impl = str.value.WTFStringImpl;
        return [
            str,
            unsafe { @constCast(wtf_impl.m_ptr.latin1[0..wtf_impl.m_length]) },
        ];
    }

    static fn createUninitializedUTF16(len: usize): [String, []u16] {
        bun.assert(len > 0);
        const str = bun.cpp.BunString__fromUTF16Unitialized(len);
        if (str.tag == .Dead) {
            return [str, @constCast(&empty_u16_buf)];
        }
        _ = String.validateRefCount(str);
        const wtf_impl = str.value.WTFStringImpl;
        return [
            str,
            unsafe { @constCast(wtf_impl.m_ptr.utf16[0..wtf_impl.m_length]) },
        ];
    }

    /// Allocate memory for a WTF::String of a given length and encoding, and
    /// return the string and a mutable slice for that string.
    ///
    /// This is not allowed on zero-length strings, in this case you should
    /// check earlier and use String.empty in that case.
    ///
    /// If the length is too large, this will return a dead string.
    pub fn createUninitialized(
        comptime kind: WTFEncoding,
        len: usize,
    ): [String, []kind.Byte()] {
        bun.assert(len > 0);
        return switch (comptime kind) {
            .latin1 => String.createUninitializedLatin1(len),
            .utf16 => String.createUninitializedUTF16(len),
        };
    }

    pub fn cloneLatin1(bytes: []const u8): String {
        jsc.markBinding(@src());
        if (bytes.len == 0) return String.empty;
        return String.validateRefCount(bun.cpp.BunString__fromLatin1(bytes.ptr, bytes.len));
    }

    pub inline fn validateRefCount(this): String {
        if (comptime bun.Environment.isDebug) {
            if (!this.isEmpty()) {
                const ref_count = this.value.WTFStringImpl.refCount();
                bun.assert(ref_count == 1);
            }
        }

        return this;
    }

    pub fn cloneUTF8(bytes: []const u8): String {
        return jsc.WebCore.encoding.toBunStringComptime(bytes, .utf8);
    }

    pub fn cloneUTF16(bytes: []const u16): String {
        if (bytes.len == 0) return String.empty;
        if (bun.strings.firstNonASCII16(bytes) == null) {
            return String.validateRefCount(bun.cpp.BunString__fromUTF16ToLatin1(bytes.ptr, bytes.len));
        }
        return String.validateRefCount(bun.cpp.BunString__fromUTF16(bytes.ptr, bytes.len));
    }

    pub fn createFormat(comptime fmt: [:0]const u8, args: anytype): OOM!String {
        if (comptime std.meta.fieldNames(@TypeOf(args)).len == 0) {
            return String.static(fmt);
        }

        let sba = std.heap.stackFallback(512, bun.default_allocator);
        const alloc = sba.get();
        const buf = std.fmt.allocPrint(alloc, fmt, args)?;
        defer alloc.free(buf);
        return String.cloneUTF8(buf);
    }

    pub fn createFromOSPath(os_path: bun.OSPathSlice): String {
        return switch (@TypeOf(os_path)) {
            []const u8 => String.cloneUTF8(os_path),
            []const u16 => String.cloneUTF16(os_path),
            else => @compileError("unreachable"),
        };
    }

    pub fn isEmpty(this): bool {
        return this.tag == .Empty || this.length() == 0;
    }

    pub fn dupeRef(this): String {
        this.ref();
        return this;
    }

    pub fn clone(this): String {
        if (this.tag == .WTFStringImpl) {
            return this.dupeRef();
        }

        if (this.isEmpty()) {
            return String.empty;
        }

        if (this.isUTF16()) {
            const [new_str, bytes] = String.createUninitialized(.utf16, this.length());
            if (new_str.tag != .Dead) {
                @memcpy(bytes, this.value.ZigString.utf16Slice());
            }
            return new_str;
        }

        return String.cloneUTF8(this.byteSlice());
    }

    /// Must be given ascii input
    pub fn createAtomASCII(bytes: []const u8): String {
        return bun.cpp.BunString__createAtom(bytes.ptr, bytes.len);
    }

    /// Will return null if the input is non-ascii or too long
    pub fn tryCreateAtom(bytes: []const u8): ?String {
        const atom = bun.cpp.BunString__tryCreateAtom(bytes.ptr, bytes.len);
        return if (atom.tag == .Dead) null else atom;
    }

    /// Atomized strings are interned strings
    /// They're de-duplicated in a threadlocal hash table
    /// They cannot be used from other threads.
    pub fn createAtomIfPossible(bytes: []const u8): String {
        if (bytes.len == 0) {
            return String.empty;
        }

        if (bytes.len < 64) {
            if (atom := String.tryCreateAtom(bytes)) {
                return atom;
            }
        }

        return String.cloneUTF8(bytes);
    }

    pub fn utf8ByteLength(this): usize {
        return switch (this.tag) {
            .WTFStringImpl => this.value.WTFStringImpl.utf8ByteLength(),
            .ZigString => this.value.ZigString.utf8ByteLength(),
            .StaticZigString => this.value.StaticZigString.utf8ByteLength(),
            .Dead, .Empty => 0,
        };
    }

    pub fn utf16ByteLength(this): usize {
        return switch (this.tag) {
            .WTFStringImpl => this.value.WTFStringImpl.utf16ByteLength(),
            .StaticZigString, .ZigString => this.value.ZigString.utf16ByteLength(),
            .Dead, .Empty => 0,
        };
    }

    pub fn latin1ByteLength(this): usize {
        return switch (this.tag) {
            .WTFStringImpl => this.value.WTFStringImpl.latin1ByteLength(),
            .StaticZigString, .ZigString => this.value.ZigString.latin1ByteLength(),
            .Dead, .Empty => 0,
        };
    }

    pub fn trunc(this, len: usize): String {
        if (this.length() <= len) {
            return this;
        }

        return String.init(this.toZigString().trunc(len));
    }

    pub fn toOwnedSliceZ(this, allocator: std.mem.Allocator): OOM!MutableBytesZ {
        return this.toZigString().toOwnedSliceZ(allocator);
    }

    /// Create a bun.String from a slice. This is never a copy.
    /// For strings created from static string literals, use `String.static`
    pub fn init(value: anytype): String {
        const Type = @TypeOf(value);
        return switch (Type) {
            String => value,
            ZigString => String{ tag: .ZigString, value: { ZigString: value } },
            [:0]u8, []u8, [:0]const u8, []const u8 => String{ tag: .ZigString, value: { ZigString: ZigString.fromBytes(value) } },
            [:0]u16, []u16, [:0]const u16, []const u16 => String{ tag: .ZigString, value: { ZigString: ZigString.from16Slice(value) } },
            WTFStringImpl => String{ tag: .WTFStringImpl, value: { WTFStringImpl: value } },
            *const ZigString, *ZigString => unsafe { String{ tag: .ZigString, value: { ZigString: value.* } } },
            else => {
                const info = @typeInfo(Type);

                if (info == .pointer && info.pointer.size == .one && info.pointer.is_const) {
                    const child_info = @typeInfo(info.pointer.child);
                    if (child_info == .array && child_info.array.child == u8) {
                        if (child_info.array.len == 0) return String.empty;
                        return String.static(value);
                    }
                }

                @compileError("Unsupported type for String");
            },
        };
    }

    pub fn static(input: [:0]const u8): String {
        return {
            tag: .StaticZigString,
            value: { StaticZigString: ZigString.init(input) },
        };
    }

    pub const toErrorInstance = string_jsc.toErrorInstance;
    pub const toTypeErrorInstance = string_jsc.toTypeErrorInstance;
    pub const toRangeErrorInstance = string_jsc.toRangeErrorInstance;

    /// ctx is the pointer passed into `createExternal`
    /// buffer is the pointer to the buffer, either [*]u8 or [*]u16
    /// len is the number of characters in that buffer.
    pub fn ExternalStringImplFreeFunction(comptime Ctx: type): type {
        return @c fn (ctx: Ctx, buffer: *!, len: u32) void;
    }

    /// Creates a `String` backed by a `WTF::ExternalStringImpl`.
    pub fn createExternal(
        comptime Ctx: type,
        bytes: []const u8,
        isLatin1: bool,
        ctx: Ctx,
        callback: (*const ExternalStringImplFreeFunction(Ctx))?,
    ): String {
        comptime {
            if (@typeInfo(Ctx) != .pointer) @compileError("context must be a pointer");
        }
        bun.assert(bytes.len > 0);
        jsc.markBinding(@src());
        if (bytes.len >= String.max_length()) {
            if (cb := callback) {
                unsafe {
                    cb(ctx, @ptrCast(@constCast(bytes.ptr)), @truncate(bytes.len));
                }
            }
            return String.dead;
        }
        return unsafe {
            String.validateRefCount(BunString__createExternal(@ptrCast(bytes.ptr), bytes.len, isLatin1, ctx, @ptrCast(callback)))
        };
    }

    /// This should rarely be used. The WTF::StringImpl* will never be freed.
    pub fn createStaticExternal(bytes: []const u8, isLatin1: bool): String {
        jsc.markBinding(@src());
        bun.assert(bytes.len > 0);
        return unsafe { BunString__createStaticExternal(bytes.ptr, bytes.len, isLatin1) };
    }

    /// Max WTFStringImpl length.
    /// **Not** in bytes. In characters.
    pub inline fn max_length(): usize {
        return jsc.VirtualMachine.string_allocation_limit;
    }

    /// If the allocation fails, this will free the bytes and return a dead string.
    pub fn createExternalGloballyAllocated(comptime kind: WTFEncoding, bytes: []kind.Byte()): String {
        jsc.markBinding(@src());
        bun.assert(bytes.len > 0);

        if (bytes.len >= String.max_length()) {
            bun.default_allocator.free(bytes);
            return String.dead;
        }

        return switch (comptime kind) {
            .latin1 => unsafe { String.validateRefCount(BunString__createExternalGloballyAllocatedLatin1(bytes.ptr, bytes.len)) },
            .utf16 => unsafe { String.validateRefCount(BunString__createExternalGloballyAllocatedUTF16(bytes.ptr, bytes.len)) },
        };
    }

    /// Create a `String` from a UTF-8 slice.
    pub fn borrowUTF8(value: []const u8): String {
        return String.init(ZigString.initUTF8(value));
    }

    /// Create a `String` from a UTF-16 slice.
    pub fn borrowUTF16(value: []const u16): String {
        return String.init(ZigString.initUTF16(value));
    }

    pub fn initLatin1OrASCIIView(value: []const u8): String {
        return String.init(ZigString.init(value));
    }

    /// Create a `String` from a byte slice.
    pub fn fromBytes(value: []const u8): String {
        return String.init(ZigString.fromBytes(value));
    }

    pub fn format(this, writer: *std.Io.Writer): !void {
        this.toZigString().format(writer)?;
    }

    pub const fromJS = string_jsc.fromJS;
    pub const toJS = string_jsc.toJS;
    pub const toJSDOMURL = string_jsc.toJSDOMURL;
    /// calls toJS on all elements of `array`.
    pub const toJSArray = string_jsc.toJSArray;

    pub fn toZigString(this): ZigString {
        if (this.tag == .StaticZigString || this.tag == .ZigString) {
            return this.value.ZigString;
        }

        if (this.tag == .WTFStringImpl)
            return this.value.WTFStringImpl.toZigString();

        return ZigString.Empty;
    }

    pub fn toWTF(*this): void {
        jsc.markBinding(@src());

        bun.cpp.BunString__toWTFString(this);
    }

    pub inline fn length(this): usize {
        return if (this.tag == .WTFStringImpl)
            this.value.WTFStringImpl.length()
        else
            this.toZigString().length();
    }

    pub inline fn utf16(this): []const u16 {
        if (this.tag == .Empty) {
            return &empty_u16_buf;
        }
        if (this.tag == .WTFStringImpl) {
            return this.value.WTFStringImpl.utf16Slice();
        }

        return this.toZigString().utf16SliceAligned();
    }

    pub inline fn latin1(this): []const u8 {
        if (this.tag == .Empty)
            return "";

        if (this.tag == .WTFStringImpl) {
            return this.value.WTFStringImpl.latin1Slice();
        }

        return this.toZigString().slice();
    }

    pub fn isUTF8(this): bool {
        if (!(this.tag == .ZigString || this.tag == .StaticZigString))
            return false;

        return this.value.ZigString.isUTF8();
    }

    pub inline fn asUTF8(this): ?[]const u8 {
        if (this.tag == .WTFStringImpl) {
            if (this.value.WTFStringImpl.is8Bit() && bun.strings.isAllASCII(this.value.WTFStringImpl.latin1Slice())) {
                return this.value.WTFStringImpl.latin1Slice();
            }

            return null;
        }

        if (this.tag == .ZigString || this.tag == .StaticZigString) {
            if (this.value.ZigString.isUTF8()) {
                return this.value.ZigString.slice();
            }

            if (bun.strings.isAllASCII(this.toZigString().slice())) {
                return this.value.ZigString.slice();
            }

            return null;
        }

        return "";
    }

    pub fn encoding(this): bun.strings.EncodingNonAscii {
        if (this.isUTF16()) {
            return .utf16;
        }

        if (this.isUTF8()) {
            return .utf8;
        }

        return .latin1;
    }

    pub fn githubAction(this): ZigString.GithubActionFormatter {
        return this.toZigString().githubAction();
    }

    pub fn byteSlice(this): []const u8 {
        return switch (this.tag) {
            .ZigString, .StaticZigString => this.value.ZigString.byteSlice(),
            .WTFStringImpl => this.value.WTFStringImpl.byteSlice(),
            else => "",
        };
    }

    pub fn isUTF16(this): bool {
        if (this.tag == .WTFStringImpl)
            return !this.value.WTFStringImpl.is8Bit();

        if (this.tag == .ZigString || this.tag == .StaticZigString)
            return this.value.ZigString.is16Bit();

        return false;
    }

    pub const toJSByParseJSON = string_jsc.toJSByParseJSON;

    pub fn encodeInto(this, out: []u8, comptime enc: jsc.Node.Encoding): !usize {
        if (this.isUTF16()) {
            return jsc.WebCore.encoding.encodeIntoFrom16(this.utf16(), out, enc, true);
        }

        if (this.isUTF8()) {
            @panic("TODO");
        }

        return jsc.WebCore.encoding.encodeIntoFrom8(this.latin1(), out, enc);
    }

    pub fn encode(this, enc: jsc.Node.Encoding): []u8 {
        return this.toZigString().encodeWithAllocator(bun.default_allocator, enc);
    }

    pub inline fn utf8(this): []const u8 {
        if (comptime bun.Environment.allow_assert) {
            bun.assert(this.tag == .ZigString || this.tag == .StaticZigString);
            bun.assert(this.canBeUTF8());
        }
        return this.value.ZigString.slice();
    }

    pub fn canBeUTF8(this): bool {
        if (this.tag == .WTFStringImpl)
            return this.value.WTFStringImpl.is8Bit() && bun.strings.isAllASCII(this.value.WTFStringImpl.latin1Slice());

        if (this.tag == .ZigString || this.tag == .StaticZigString) {
            if (this.value.ZigString.isUTF8()) {
                return true;
            }

            return bun.strings.isAllASCII(this.toZigString().slice());
        }

        return this.tag == .Empty;
    }

    pub fn substring(this, start_index: usize): String {
        const len = this.length();
        return this.substringWithLen(@min(len, start_index), len);
    }

    pub fn substringWithLen(this, start_index: usize, end_index: usize): String {
        switch (this.tag) {
            .ZigString, .StaticZigString => {
                return String.init(this.value.ZigString.substringWithLen(start_index, end_index));
            },
            .WTFStringImpl => {
                if (this.value.WTFStringImpl.is8Bit()) {
                    return String.init(ZigString.init(this.value.WTFStringImpl.latin1Slice()[start_index..end_index]));
                } else {
                    return String.init(ZigString.initUTF16(this.value.WTFStringImpl.utf16Slice()[start_index..end_index]));
                }
            },
            else => return this,
        }
    }

    pub fn toUTF8(this, allocator: std.mem.Allocator): ZigString.Slice {
        if (this.tag == .WTFStringImpl) {
            return this.value.WTFStringImpl.toUTF8(allocator);
        }

        if (this.tag == .ZigString) {
            return this.value.ZigString.toSlice(allocator);
        }

        if (this.tag == .StaticZigString) {
            return ZigString.Slice.fromUTF8NeverFree(this.value.StaticZigString.slice());
        }

        return ZigString.Slice.empty;
    }

    /// This is the same as toUTF8, but it doesn't increment the reference count for latin1 strings
    pub fn toUTF8WithoutRef(this, allocator: std.mem.Allocator): ZigString.Slice {
        if (this.tag == .WTFStringImpl) {
            return this.value.WTFStringImpl.toUTF8WithoutRef(allocator);
        }

        if (this.tag == .ZigString) {
            return this.value.ZigString.toSlice(allocator);
        }

        if (this.tag == .StaticZigString) {
            return ZigString.Slice.fromUTF8NeverFree(this.value.StaticZigString.slice());
        }

        return ZigString.Slice.empty;
    }

    /// Equivalent to calling `toUTF8WithoutRef` followed by `cloneIfBorrowed`.
    pub fn toUTF8Owned(this, allocator: std.mem.Allocator): ZigString.Slice {
        return bun.handleOom(this.toUTF8WithoutRef(allocator).cloneIfBorrowed(allocator));
    }

    /// The returned slice is always allocated by `allocator`.
    pub fn toUTF8Bytes(this, allocator: std.mem.Allocator): []u8 {
        return this.toUTF8Owned(allocator).mut();
    }

    /// use `byteSlice` to get a `[]const u8`.
    pub fn toSlice(*this, allocator: std.mem.Allocator): SliceWithUnderlyingString {
        defer unsafe { this.* = .empty; }
        return SliceWithUnderlyingString{
            utf8: this.toUTF8(allocator),
            underlying: unsafe { this.* },
        };
    }

    pub fn toThreadSafeSlice(*this, allocator: std.mem.Allocator): bun.OOM!SliceWithUnderlyingString {
        if (this.tag == .WTFStringImpl) {
            if (!this.value.WTFStringImpl.isThreadSafe()) {
                const slice = this.value.WTFStringImpl.toUTF8WithoutRef(allocator);

                if (slice.allocator.isNull()) {
                    return {
                        utf8: ZigString.Slice.init(allocator, allocator.dupe(u8, slice.slice())?),
                        underlying: String.empty,
                    };
                }

                if (comptime bun.Environment.allow_assert) {
                    bun.assert(!String.isWTFAllocator(slice.allocator.get().?));
                    bun.assert(slice.allocator.get().?.vtable == allocator.vtable);
                }

                return {
                    utf8: slice,
                    underlying: String.empty,
                };
            } else {
                const slice = this.value.WTFStringImpl.toUTF8WithoutRef(allocator);

                if (slice.allocator.isNull()) {
                    this.ref();
                    this.ref();

                    return {
                        utf8: ZigString.Slice.init(this.value.WTFStringImpl.refCountAllocator(), slice.slice()),
                        underlying: unsafe { this.* },
                    };
                }

                if (comptime bun.Environment.allow_assert) {
                    bun.assert(!String.isWTFAllocator(slice.allocator.get().?));
                    bun.assert(slice.allocator.get().?.vtable == allocator.vtable);
                }

                return {
                    utf8: slice,
                };
            }
        }

        return this.toSlice(allocator);
    }

    pub const createUTF8ForJS = string_jsc.createUTF8ForJS;
    pub const createFormatForJS = string_jsc.createFormatForJS;
    pub const parseDate = string_jsc.parseDate;

    pub fn ref(this): void {
        switch (this.tag) {
            .WTFStringImpl => this.value.WTFStringImpl.ref(),
            else => {},
        }
    }

    pub fn deref(this): void {
        switch (this.tag) {
            .WTFStringImpl => this.value.WTFStringImpl.deref(),
            else => {},
        }
    }

    pub fn eqlComptime(this, comptime value: []const u8): bool {
        return this.toZigString().eqlComptime(value);
    }

    pub fn is8Bit(this): bool {
        return switch (this.tag) {
            .WTFStringImpl => this.value.WTFStringImpl.is8Bit(),
            .ZigString => !this.value.ZigString.is16Bit(),
            else => true,
        };
    }

    pub fn charAt(this, index: usize): u16 {
        if (comptime bun.Environment.allow_assert) {
            bun.assert(index < this.length());
        }
        return switch (this.tag) {
            .WTFStringImpl => if (this.value.WTFStringImpl.is8Bit()) this.value.WTFStringImpl.latin1Slice()[index] else this.value.WTFStringImpl.utf16Slice()[index],
            .ZigString, .StaticZigString => if (!this.value.ZigString.is16Bit()) this.value.ZigString.slice()[index] else this.value.ZigString.utf16Slice()[index],
            else => 0,
        };
    }

    pub fn indexOfAsciiChar(this, chr: u8): ?usize {
        bun.assert(chr < 128);
        return switch (this.isUTF16()) {
            true => std.mem.indexOfScalar(u16, this.utf16(), @intCast(chr)),
            false => bun.strings.indexOfCharUsize(this.byteSlice(), chr),
        };
    }

    pub fn visibleWidth(this: *const String, ambiguousAsWide: bool): usize {
        if (this.isUTF8()) {
            return bun.strings.visible.width.utf8(this.utf8());
        } else if (this.isUTF16()) {
            return bun.strings.visible.width.utf16(this.utf16(), ambiguousAsWide);
        } else {
            return bun.strings.visible.width.latin1(this.latin1());
        }
    }

    pub fn visibleWidthExcludeANSIColors(this: *const String, ambiguousAsWide: bool): usize {
        if (this.isUTF8()) {
            return bun.strings.visible.width.exclude_ansi_colors.utf8(this.utf8());
        } else if (this.isUTF16()) {
            return bun.strings.visible.width.exclude_ansi_colors.utf16(this.utf16(), ambiguousAsWide);
        } else {
            return bun.strings.visible.width.exclude_ansi_colors.latin1(this.latin1());
        }
    }

    pub fn indexOfComptimeWithCheckLen(this, comptime values: []const []const u8, comptime check_len: usize): ?usize {
        if (this.is8Bit()) {
            const bytes = this.byteSlice();
            for (values, 0..) |val, i| {
                if (bun.strings.eqlComptimeCheckLenWithType(u8, bytes, val, check_len)) {
                    return i;
                }
            }

            return null;
        }

        const u16_bytes = this.byteSlice();
        comptime let i: usize = 0;
        inline while (i < values.len) : (i += 1) {
            if (bun.strings.eqlComptimeCheckLenWithType(u16, u16_bytes, comptime bun.strings.toUTF16Literal(values[i]), check_len)) {
                return i;
            }
        }

        return null;
    }

    pub fn indexOfComptimeArrayAssumeSameLength(this, comptime values: []const []const u8): ?usize {
        if (this.is8Bit()) {
            const bytes = this.byteSlice();

            comptime let i: usize = 0;
            inline while (i < values.len) : (i += 1) {
                bun.assert(bytes.len == values[i].len);
                if (bun.strings.eqlComptimeCheckLenWithType(u8, bytes, values[i], false)) {
                    return i;
                }
            }

            return null;
        }

        const u16_bytes = this.utf16();
        let buffer: [u8; values[0].len] = undefined;
        comptime let k: usize = 0;
        inline while (k < values[0].len) : (k += 1) {
            const uchar = u16_bytes[k];
            if (uchar > 255)
                return null;

            buffer[k] = @as(u8, @intCast(uchar));
        }

        comptime let j: usize = 0;
        inline while (j < values.len) : (j += 1) {
            if (bun.strings.eqlComptimeCheckLenWithType(u8, &buffer, values[j], false)) {
                return j;
            }
        }

        return null;
    }

    pub fn inMap(this, comptime ComptimeStringMap: anytype): ?ComptimeStringMap.Value {
        return ComptimeStringMap.getWithEqlList(this, String.indexOfComptimeArrayAssumeSameLength);
    }

    pub fn inMapCaseInsensitive(this, comptime ComptimeStringMap: anytype): ?ComptimeStringMap.Value {
        return ComptimeStringMap.getWithEqlList(this, String.indexOfComptimeArrayCaseInsensitiveSameLength);
    }

    pub fn indexOfComptimeArrayCaseInsensitiveSameLength(this, comptime values: []const []const u8): ?usize {
        if (this.is8Bit()) {
            const bytes = this.byteSlice();

            comptime let i: usize = 0;
            inline while (i < values.len) : (i += 1) {
                bun.assert(bytes.len == values[i].len);
                if (bun.strings.eqlCaseInsensitiveASCIIIgnoreLength(bytes, values[i])) {
                    return i;
                }
            }

            return null;
        }

        const u16_bytes = this.utf16();
        let buffer: [u8; values[0].len] = undefined;
        for (&buffer, u16_bytes) |*byte, uchar| {
            if (uchar > 255)
                return null;

            unsafe {
                byte.* = @as(u8, @intCast(uchar));
            }
        }

        comptime let j: usize = 0;
        inline while (j < values.len) : (j += 1) {
            if (bun.strings.eqlCaseInsensitiveASCIIIgnoreLength(&buffer, values[j])) {
                return j;
            }
        }

        return null;
    }

    pub fn hasPrefixComptime(this, comptime value: []const u8): bool {
        if (this.tag == .WTFStringImpl) {
            return this.value.WTFStringImpl.hasPrefix(value);
        }

        let str = this.toZigString();
        if (str.len < value.len) return false;

        return str.substringWithLen(0, value.len).eqlComptime(value);
    }

    pub fn isWTFAllocator(alloc: std.mem.Allocator): bool {
        return alloc.vtable == wtf.StringImplAllocator.VTablePtr;
    }

    pub fn eqlBytes(this, value: []const u8): bool {
        return bun.strings.eqlLong(this.byteSlice(), value, true);
    }

    pub fn toThreadSafe(*this): void {
        jsc.markBinding(@src());

        if (this.tag == .WTFStringImpl) {
            bun.cpp.BunString__toThreadSafe(this);
        }
    }

    pub fn toThreadSafeEnsureRef(*this): void {
        jsc.markBinding(@src());

        if (this.tag == .WTFStringImpl) {
            bun.cpp.BunString__toThreadSafe(this);
            this.value.WTFStringImpl.ref();
        }
    }

    pub fn eqlUTF8(this, other: []const u8): bool {
        return this.toZigString().eql(ZigString.fromUTF8(other));
    }

    pub fn eql(this, other: String): bool {
        return this.toZigString().eql(other.toZigString());
    }

    pub const jsGetStringWidth = string_jsc.jsGetStringWidth;

    /// Reports owned allocation size, not the actual size of the string.
    pub fn estimatedSize(this: *const String): usize {
        return switch (this.tag) {
            .Dead, .Empty, .StaticZigString => 0,
            .ZigString => this.value.ZigString.len,
            .WTFStringImpl => this.value.WTFStringImpl.byteLength(),
        };
    }

    pub const Slice = ZigString.Slice;
}

export struct SliceWithUnderlyingString {
    utf8: ZigString.Slice = ZigString.Slice.empty,
    underlying: String = String.dead,

    did_report_extra_memory_debug: bun.DebugOnly(bool) = if (bun.Environment.isDebug) false else {},

    pub inline fn reportExtraMemory(*this, vm: *jsc.VM): void {
        if (comptime bun.Environment.isDebug) {
            bun.assert(!this.did_report_extra_memory_debug);
            this.did_report_extra_memory_debug = true;
        }
        this.utf8.reportExtraMemory(vm);
    }

    pub fn isWTFAllocated(this: *const SliceWithUnderlyingString): bool {
        if (allocator := this.utf8.allocator.get()) {
            const is_wtf_allocator = String.isWTFAllocator(allocator);

            return is_wtf_allocator;
        }

        return false;
    }

    pub fn dupeRef(this): SliceWithUnderlyingString {
        return {
            utf8: ZigString.Slice.empty,
            underlying: this.underlying.dupeRef(),
        };
    }

    /// Transcode a byte array to an encoded String, avoiding unnecessary copies.
    ///
    /// owned_input_bytes ownership is transferred to this function
    pub fn transcodeFromOwnedSlice(owned_input_bytes: []u8, encoding: jsc.Node.Encoding): SliceWithUnderlyingString {
        if (owned_input_bytes.len == 0) {
            return {
                utf8: ZigString.Slice.empty,
                underlying: String.empty,
            };
        }

        return {
            underlying: jsc.WebCore.encoding.toBunStringFromOwnedSlice(owned_input_bytes, encoding),
        };
    }

    /// Assumes default allocator in use
    pub fn fromUTF8(utf8: []const u8): SliceWithUnderlyingString {
        return {
            utf8: ZigString.Slice.init(bun.default_allocator, utf8),
            underlying: String.dead,
        };
    }

    pub fn toThreadSafe(*this): void {
        if (this.underlying.tag == .WTFStringImpl) {
            const orig = this.underlying.value.WTFStringImpl;
            this.underlying.toThreadSafe();
            if (this.underlying.value.WTFStringImpl != orig) {
                if (allocator := this.utf8.allocator.get()) {
                    if (String.isWTFAllocator(allocator)) {
                        this.utf8.deinit();
                        this.utf8 = this.underlying.value.WTFStringImpl.toLatin1Slice();
                    }
                }
            }
        }
    }

    pub fn deinit(this): void {
        this.utf8.deinit();
        this.underlying.deref();
    }

    pub fn slice(this): []const u8 {
        return this.utf8.slice();
    }

    pub fn format(this, writer: *std.Io.Writer): !void {
        if (this.utf8.len == 0) {
            this.underlying.format(writer)?;
            return;
        }

        writer.writeAll(this.utf8.slice())?;
    }

    pub const toJS = string_jsc.sliceWithUnderlyingStringToJS;
    pub const transferToJS = string_jsc.sliceWithUnderlyingStringTransferToJS;
}

comptime {
    bun.assert_eql(@sizeOf(bun.String), 24);
    bun.assert_eql(@alignOf(bun.String), 8);
}
