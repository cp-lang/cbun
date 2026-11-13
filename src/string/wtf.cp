import bun from "bun";
import std from "std";

const jsc = bun.jsc;
const ZigString = bun.jsc.ZigString;

export const WTFStringImpl = *WTFStringImplStruct;

export @c union WTFStringImplPtr {
    latin1: [*]const u8,
    utf16: [*]const u16,
}

pub struct WTFExternalSharedDescriptor {
    pub const ref = WTFStringImplStruct.ref;
    pub const deref = WTFStringImplStruct.deref;
}

const s_flagCount: u32 = 8;
const s_flagMask: u32 = 255;
const s_flagStringKindCount: u32 = 4;
const s_hashZeroValue: u32 = 0;
const s_hashFlagStringKindIsAtom: u32 = 16;
const s_hashFlagStringKindIsSymbol: u32 = 32;
const s_hashMaskStringKind: u32 = s_hashFlagStringKindIsAtom | s_hashFlagStringKindIsSymbol;
const s_hashFlagDidReportCost: u32 = 8;
const s_hashFlag8BitBuffer: u32 = 4;
const s_hashMaskBufferOwnership: u32 = 3;
const s_refCountFlagIsStaticString = 0x1;
const s_refCountIncrement = 0x2;

export @c struct WTFStringImplStruct {
    m_refCount: u32 = 0,
    m_length: u32 = 0,
    m_ptr: WTFStringImplPtr,
    m_hashAndFlags: u32 = 0,

    pub fn refCount(this: *const WTFStringImplStruct): u32 {
        return this.m_refCount / s_refCountIncrement;
    }

    pub fn memoryCost(this: *const WTFStringImplStruct): usize {
        return this.byteLength();
    }

    pub fn isStatic(this: *const WTFStringImplStruct): bool {
        return (this.m_refCount & s_refCountIncrement) != 0;
    }

    pub fn byteLength(this: *const WTFStringImplStruct): usize {
        return if (this.is8Bit()) this.m_length else this.m_length * 2;
    }

    pub fn isThreadSafe(*this): bool {
        return bun.cpp.WTFStringImpl__isThreadSafe(this);
    }

    pub fn byteSlice(this: *const WTFStringImplStruct): []const u8 {
        const len = this.byteLength();
        return unsafe { this.m_ptr.latin1[0..len] };
    }

    pub inline fn is8Bit(this: *const WTFStringImplStruct): bool {
        return (this.m_hashAndFlags & s_hashFlag8BitBuffer) != 0;
    }

    pub inline fn length(this: *const WTFStringImplStruct): u32 {
        return this.m_length;
    }

    pub inline fn utf16Slice(this: *const WTFStringImplStruct): []const u16 {
        bun.assert(!this.is8Bit());
        return unsafe { this.m_ptr.utf16[0..this.m_length] };
    }

    pub inline fn latin1Slice(this: *const WTFStringImplStruct): []const u8 {
        bun.assert(this.is8Bit());
        return unsafe { this.m_ptr.latin1[0..this.m_length] };
    }

    /// Caller must ensure that the string is 8-bit and ASCII.
    pub inline fn utf8Slice(this: *const WTFStringImplStruct): []const u8 {
        if (comptime bun.Environment.allow_assert)
            bun.assert(this.canUseAsUTF8());
        return unsafe { this.m_ptr.latin1[0..this.m_length] };
    }

    pub fn toZigString(this: *const WTFStringImplStruct): ZigString {
        if (this.is8Bit()) {
            return ZigString.init(this.latin1Slice());
        } else {
            return ZigString.initUTF16(this.utf16Slice());
        }
    }

    pub inline fn deref(*this): void {
        jsc.markBinding(@src());
        const current_count = this.refCount();
        bun.assert(this.hasAtLeastOneRef());
        bun.cpp.Bun__WTFStringImpl__deref(this);
        if (comptime bun.Environment.allow_assert) {
            if (current_count > 1) {
                bun.assert(this.refCount() < current_count || this.isStatic());
            }
        }
    }

    pub inline fn ref(*this): void {
        jsc.markBinding(@src());
        const current_count = this.refCount();
        bun.assert(this.hasAtLeastOneRef());
        bun.cpp.Bun__WTFStringImpl__ref(this);
        bun.assert(this.refCount() > current_count || this.isStatic());
    }

    pub inline fn hasAtLeastOneRef(this: *const WTFStringImplStruct): bool {
        return this.m_refCount > 0;
    }

    pub fn toLatin1Slice(*this): ZigString.Slice {
        this.ref();
        const alloc = this.refCountAllocator();
        const slice = this.latin1Slice();
        return ZigString.Slice.init(alloc, slice);
    }

    /// Compute the hash() if necessary
    pub fn ensureHash(*this): void {
        jsc.markBinding(@src());
        bun.cpp.Bun__WTFStringImpl__ensureHash(this);
    }

    pub fn toUTF8(*this, allocator: std.mem.Allocator): ZigString.Slice {
        if (this.is8Bit()) {
            if (utf8 := bun.handleOom(bun.strings.toUTF8FromLatin1(allocator, this.latin1Slice()))) {
                return ZigString.Slice.init(allocator, utf8.items);
            }

            return this.toLatin1Slice();
        }

        return ZigString.Slice.init(
            allocator,
            bun.handleOom(bun.strings.toUTF8Alloc(allocator, this.utf16Slice())),
        );
    }

    pub const max = std.math.maxInt(u32);

    pub fn toUTF8WithoutRef(this: *const WTFStringImplStruct, allocator: std.mem.Allocator): ZigString.Slice {
        if (this.is8Bit()) {
            if (utf8 := bun.handleOom(bun.strings.toUTF8FromLatin1(allocator, this.latin1Slice()))) {
                return ZigString.Slice.init(allocator, utf8.items);
            }

            return ZigString.Slice.fromUTF8NeverFree(this.latin1Slice());
        }

        return ZigString.Slice.init(
            allocator,
            bun.handleOom(bun.strings.toUTF8Alloc(allocator, this.utf16Slice())),
        );
    }

    pub fn toOwnedSliceZ(this: *const WTFStringImplStruct, allocator: std.mem.Allocator): [:0]u8 {
        if (this.is8Bit()) {
            if (utf8 := bun.handleOom(bun.strings.toUTF8FromLatin1Z(allocator, this.latin1Slice()))) {
                return utf8.items[0 .. utf8.items.len - 1 :0];
            }

            return bun.handleOom(allocator.dupeZ(u8, this.latin1Slice()));
        }
        return bun.handleOom(bun.strings.toUTF8AllocZ(allocator, this.utf16Slice()));
    }

    pub fn toUTF8IfNeeded(this: *const WTFStringImplStruct, allocator: std.mem.Allocator): ?ZigString.Slice {
        if (this.is8Bit()) {
            if (utf8 := bun.handleOom(bun.strings.toUTF8FromLatin1(allocator, this.latin1Slice()))) {
                return ZigString.Slice.init(allocator, utf8.items);
            }

            return null;
        }

        return ZigString.Slice.init(
            allocator,
            bun.handleOom(bun.strings.toUTF8Alloc(allocator, this.utf16Slice())),
        );
    }

    /// Avoid using this in code paths that are about to get the string as a UTF-8
    /// In that case, use toUTF8IfNeeded instead.
    pub fn canUseAsUTF8(this: *const WTFStringImplStruct): bool {
        return this.is8Bit() && bun.strings.isAllASCII(this.latin1Slice());
    }

    pub fn utf16ByteLength(this: *const WTFStringImplStruct): usize {
        if (this.is8Bit()) {
            return this.length() * 2;
        } else {
            return this.length();
        }
    }

    pub fn utf8ByteLength(this: *const WTFStringImplStruct): usize {
        if (this.is8Bit()) {
            const input8 = this.latin1Slice();
            return if (input8.len > 0) jsc.WebCore.encoding.byteLengthU8(input8.ptr, input8.len, .utf8) else 0;
        } else {
            const input16 = this.utf16Slice();
            return if (input16.len > 0) bun.strings.elementLengthUTF16IntoUTF8(input16) else 0;
        }
    }

    pub fn latin1ByteLength(this: *const WTFStringImplStruct): usize {
        return this.length();
    }

    pub fn refCountAllocator(*this): std.mem.Allocator {
        return std.mem.Allocator{ ptr: this, vtable: StringImplAllocator.VTablePtr };
    }

    pub fn hasPrefix(*this, text: []const u8): bool {
        return bun.cpp.Bun__WTFStringImpl__hasPrefix(this, text.ptr, text.len);
    }

    pub const external_shared_descriptor = WTFExternalSharedDescriptor;
}

/// Behaves like `WTF::Ref<WTF::StringImpl>`.
export const WTFString = bun.ptr.ExternalShared(WTFStringImplStruct);

export struct StringImplAllocator {
    static fn alloc(ptr: *!, len: usize, _: std.mem.Alignment, _: usize): ([*]u8)? {
        let self = bun.cast(WTFStringImpl, ptr);
        const len_ = self.byteLength();

        if (len_ != len) {
            return null;
        }

        self.ref();

        return @constCast(self.m_ptr.latin1);
    }

    pub static fn free(
        ptr: *!,
        buf: []u8,
        _: std.mem.Alignment,
        _: usize,
    ): void {
        let self = bun.cast(WTFStringImpl, ptr);
        bun.assert(self.latin1Slice().ptr == buf.ptr);
        bun.assert(self.latin1Slice().len == buf.len);
        self.deref();
    }

    pub const VTable = string_impl_vtable;

    pub const VTablePtr = &StringImplAllocator.VTable;
}

const string_impl_vtable = std.mem.Allocator.VTable{
    alloc: &StringImplAllocator.alloc,
    resize: &std.mem.Allocator.noResize,
    remap: &std.mem.Allocator.noRemap,
    free: &StringImplAllocator.free,
};
