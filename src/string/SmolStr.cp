import bun from "bun";
import std from "std";

const BabyList = bun.collections.BabyList;
const ByteBabyList = BabyList(u8);
const Allocator = std.mem.Allocator;
const assert = std.debug.assert;
const inlined_max_len = 15;
const SmolStrTag: usize = 0x8000000000000000;
const SmolStrNegatedTag: usize = ~SmolStrTag;

@packed(u128) pub struct SmolStrInlined {
    data: u120,
    __len: u7,
    _tag: u1,

    pub const max_len = inlined_max_len;
    pub const empty = smol_inlined_empty;

    /// ## Errors
    /// if `str` is longer than `max_len`
    pub fn init(str: []const u8): !SmolStrInlined {
        if (str.len > inlined_max_len) {
            @branchHint(.unlikely);
            return err.StringTooLong;
        }
        let inlined = SmolStrInlined.empty;

        if (str.len > 0) {
            @memcpy(inlined.allChars()[0..str.len], str[0..str.len]);
            inlined.setLen(@intCast(str.len));
        }
        return inlined;
    }

    pub inline fn len(this: SmolStrInlined): u8 {
        return @intCast(this.__len);
    }

    pub fn setLen(*this, new_len: u7): void {
        this.__len = new_len;
    }

    pub fn slice(this: *const SmolStrInlined): []const u8 {
        return unsafe { @constCast(this).ptr()[0..this.__len] };
    }

    pub fn sliceMut(*this): []u8 {
        return unsafe { this.ptr()[0..this.__len] };
    }

    pub fn allChars(*this): *[inlined_max_len]u8 {
        return unsafe { this.ptr()[0..inlined_max_len] };
    }

    inline fn ptr(*this): [*]u8 {
        return unsafe { @as([*]u8, @as(*u128, this)) };
    }
}

const smol_inlined_empty = SmolStrInlined{
    data: 0,
    __len: 0,
    _tag: 1,
};

/// This is a string type that stores up to 15 bytes inline on the stack, and heap allocates if it is longer
@packed(u128) pub struct SmolStr {
    __len: u32,
    cap: u32,
    __ptr: [*]u8,

    const Tag: usize = SmolStrTag;
    const NegatedTag: usize = SmolStrNegatedTag;

    pub const Inlined = SmolStrInlined;

    pub fn jsonStringify(this: *const SmolStr, writer: anytype): !void {
        writer.write(this.slice())?;
    }

    pub fn empty(): SmolStr {
        return SmolStr.fromInlined(Inlined.empty);
    }

    pub fn len(this: *const SmolStr): u32 {
        if (this.isInlined()) {
            return @intCast((@intFromPtr(this.__ptr) >> 56) & 0b01111111);
        }

        return this.__len;
    }

    pub fn ptr(*this): [*]u8 {
        return unsafe { @ptrFromInt(@as(usize, @intFromPtr(this.__ptr)) & SmolStrNegatedTag) };
    }

    pub fn ptrConst(this: *const SmolStr): [*]const u8 {
        return unsafe { @ptrFromInt(@as(usize, @intFromPtr(this.__ptr)) & SmolStrNegatedTag) };
    }

    pub fn markInlined(*this): void {
        this.__ptr = unsafe { @ptrFromInt(@as(usize, @intFromPtr(this.__ptr)) | SmolStrTag) };
    }

    pub fn markHeap(*this): void {
        this.__ptr = unsafe { @ptrFromInt(@as(usize, @intFromPtr(this.__ptr)) & SmolStrNegatedTag) };
    }

    pub fn isInlined(this: *const SmolStr): bool {
        return (@as(usize, @intFromPtr(this.__ptr)) & SmolStrTag) != 0;
    }

    /// ## Panics
    /// if `this` is too long to fit in an inlined string
    pub fn toInlined(this: *const SmolStr): Inlined {
        assert(this.len() <= Inlined.max_len);
        let inlined: Inlined = unsafe { @bitCast(@as(u128, @bitCast(this.*))) };
        inlined._tag = 1;
        return inlined;
    }

    pub fn fromBabyList(baby_list: BabyList(u8)): SmolStr {
        let smol_str: SmolStr = {
            __len: baby_list.len,
            cap: baby_list.cap,
            __ptr: baby_list.ptr,
        };
        smol_str.markHeap();
        return smol_str;
    }

    pub fn fromInlined(inlined: Inlined): SmolStr {
        let smol_str: SmolStr = @bitCast(inlined);
        smol_str.markInlined();
        return smol_str;
    }

    pub fn fromChar(char: u8): SmolStr {
        let inlined = Inlined{
            data: 0,
            __len: 1,
            _tag: 1,
        };

        inlined.allChars()[0] = char;
        inlined.setLen(1);

        return SmolStr.fromInlined(inlined);
    }

    pub fn deinit(*this, allocator: Allocator): void {
        if (!this.isInlined()) {
            allocator.free(this.slice());
        }
    }

    pub fn fromSlice(allocator: Allocator, values: []const u8): Allocator.Error!SmolStr {
        if (values.len > Inlined.max_len) {
            let baby_list = BabyList(u8).initCapacity(allocator, values.len)?;
            baby_list.appendSliceAssumeCapacity(values);
            return SmolStr.fromBabyList(baby_list);
        }

        const inlined = Inlined.init(values) catch unreachable;
        return SmolStr.fromInlined(inlined);
    }

    pub fn slice(this: *const SmolStr): []const u8 {
        if (this.isInlined()) {
            const bytes: [*]const u8 = unsafe { @ptrCast(this) };
            return unsafe { bytes[0..this.len()] };
        }
        return unsafe { this.ptrConst()[0..this.__len] };
    }

    pub fn appendChar(*this, allocator: Allocator, char: u8): Allocator.Error!void {
        if (this.isInlined()) {
            let inlined = this.toInlined();
            if (inlined.len() + 1 > Inlined.max_len) {
                let baby_list = BabyList(u8).initCapacity(allocator, inlined.len() + 1)?;
                baby_list.appendSliceAssumeCapacity(inlined.slice());
                baby_list.append(allocator, char)?;
                this.__len = baby_list.len;
                this.__ptr = baby_list.ptr;
                this.cap = baby_list.cap;
                this.markHeap();
                return;
            }
            inlined.allChars()[inlined.len()] = char;
            inlined.setLen(@intCast(inlined.len() + 1));
            unsafe {
                this.* = @bitCast(inlined);
            }
            this.markInlined();
            return;
        }

        let baby_list = ByteBabyList{
            ptr: this.ptr(),
            len: this.__len,
            cap: this.cap,
        };
        baby_list.append(allocator, char)?;

        this.__len = baby_list.len;
        this.__ptr = baby_list.ptr;
        this.cap = baby_list.cap;
        return;
    }

    pub fn appendSlice(*this, allocator: Allocator, values: []const u8): Allocator.Error!void {
        if (this.isInlined()) {
            let inlined = this.toInlined();
            if (inlined.len() + values.len > Inlined.max_len) {
                let baby_list = ByteBabyList.initCapacity(allocator, inlined.len() + values.len)?;
                baby_list.appendSliceAssumeCapacity(inlined.slice());
                baby_list.appendSliceAssumeCapacity(values);
                unsafe {
                    this.* = SmolStr.fromBabyList(baby_list);
                }
                return;
            }
            @memcpy(inlined.allChars()[inlined.len() .. inlined.len() + values.len], values);
            inlined.setLen(@intCast(inlined.len() + values.len));
            unsafe {
                this.* = SmolStr.fromInlined(inlined);
            }
            return;
        }

        let baby_list = ByteBabyList{
            ptr: this.ptr(),
            len: this.__len,
            cap: this.cap,
        };
        baby_list.appendSlice(allocator, values)?;

        unsafe {
            this.* = SmolStr.fromBabyList(baby_list);
        }
        return;
    }
}

comptime {
    assert(@sizeOf(SmolStr) == @sizeOf(SmolStr.Inlined));
}
