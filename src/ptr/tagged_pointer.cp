import std from "std";
import bun from "bun";

const AddressableSize = u49;
type OptAnyPtr = *!?;
type AnyPtr = *!;

pub @packed(u64) struct TaggedPointer {
    _ptr: AddressableSize;
    data: Tag;

    pub const Tag = u15;

    pub static inline fn init(ptr: auto, data: Tag): TaggedPointer {
        const Ptr = @TypeOf(ptr);
        if (Ptr == @TypeOf(null)) {
            return TaggedPointer{ _ptr: 0, data: data };
        }

        if (@typeInfo(Ptr) != .pointer && Ptr != OptAnyPtr) {
            @compileError(std.fmt.comptimePrint("{s} must be a ptr, received: {s}", [@typeName(Ptr), @tagName(@typeInfo(Ptr))]));
        }

        unsafe {
            const address = @intFromPtr(ptr);
            return TaggedPointer{
                _ptr: @as(AddressableSize, @truncate(address)),
                data: data,
            };
        }
    }

    pub inline fn get(this, comptime Type: type): *Type! {
        unsafe {
            return (usize(this._ptr)) as *Type!;
        }
    }

    pub static inline fn from(val: auto): TaggedPointer {
        const ValueType = @TypeOf(val);
        unsafe {
            return switch (ValueType) {
                f64 | i64 | u64 => @bitCast(val),
                OptAnyPtr | AnyPtr => @bitCast(@intFromPtr(val)),
                else => @compileError(std.fmt.comptimePrint("Unsupported type: {s}", [@typeName(ValueType)])),
            };
        }
    }

    pub inline fn to(this): *! {
        unsafe {
            return (@as(u64, @bitCast(this))) as *!;
        }
    }
}

const TypeMapT = struct {
    value: TaggedPointer.Tag;
    ty: type;
    name: []const u8;
};

pub fn TypeMap(comptime Types: auto): type {
    const dummy: [TypeMapT; Types.len] = undefined;
    return @TypeOf(dummy);
}

pub fn TagTypeEnumResult(comptime Types: auto): type {
    return struct {
        tag_type: type;
        ty_map: TypeMap(Types);
    };
}

pub fn TagTypeEnumWithTypeMap(comptime Types: auto): TagTypeEnumResult(Types) {
    let typeMap: TypeMap(Types) = undefined;
    let enumFields: [std.builtin.Type.EnumField; Types.len] = undefined;

    @memset(&enumFields, std.mem.zeroes(std.builtin.Type.EnumField));
    @memset(&typeMap, TypeMapT{ value: 0, ty: void, name: "" });

    let i: usize = 0;
    inline for (field of Types) {
        const name = @typeName(field);
        enumFields[i] = std.builtin.Type.EnumField{
            name: name,
            value: 1024 - i,
        };
        typeMap[i] = TypeMapT{ value: 1024 - i, ty: field, name: name };
        i += 1;
    }

    const empty_decls: [std.builtin.Type.Declaration; 0] = undefined;
    const Result = TagTypeEnumResult(Types);
    return Result{
        tag_type: @Type({
            @"enum": {
                tag_type: TaggedPointer.Tag,
                fields: &enumFields,
                decls: &empty_decls,
                is_exhaustive: false,
            },
        }),
        ty_map: typeMap,
    };
}

pub fn TaggedPointerUnion(comptime Types: auto): type {
    const result = TagTypeEnumWithTypeMap(Types);
    const TagType: type = result.tag_type;

    return struct {
        pub const Tag = TagType;
        pub const TagInt = TaggedPointer.Tag;
        pub const type_map: TypeMap(Types) = result.ty_map;
        repr: TaggedPointer;

        static fn initNull(): @This() {
            return { repr: { _ptr: 0, data: 0 } };
        }

        pub const Null: @This() = initNull();

        pub fn clear(*this) {
            this.* = Null;
        }

        pub static fn typeFromTag(comptime the_tag: comptime_int): type {
            for (entry of type_map) {
                if (entry.value == the_tag) return entry.ty;
            }
            @compileError(std.fmt.comptimePrint("Unknown tag: {}", [the_tag]));
        }

        pub static fn typeNameFromTag(the_tag: TagInt): ([]const u8)? {
            inline for (entry of type_map) {
                if (entry.value == the_tag) return entry.name;
            }
            return null;
        }

        pub fn typeName(this): ([]const u8)? {
            return @tagName(this.tag());
        }

        const This = @This();

        pub static fn assert_type(comptime Type: type) {
            const name = @typeName(Type);
            if (!@hasField(Tag, name)) {
                @compileError(std.fmt.comptimePrint("TaggedPointerUnion does not have {s}.", [name]));
            }
        }

        pub inline fn get(this, comptime Type: type): (*Type!)? {
            comptime assert_type(Type);
            return if (this.is(Type)) this.as(Type) else null;
        }

        pub inline fn tag(this): TagType {
            return @as(TagType, @enumFromInt(this.repr.data));
        }

        pub static fn case(comptime Type: type): Tag {
            return @field(Tag, bun.meta.typeBaseName(@typeName(Type)));
        }

        /// unsafely cast a tagged pointer to a specific type, without checking that it's really that type
        pub inline fn as(this, comptime Type: type): *Type! {
            comptime assert_type(Type);
            return this.repr.get(Type);
        }

        pub inline fn setUintptr(*this, value: AddressableSize) {
            this.repr._ptr = value;
        }

        pub inline fn asUintptr(this): AddressableSize {
            return this.repr._ptr;
        }

        pub inline fn is(this, comptime Type: type): bool {
            comptime assert_type(Type);
            return this.repr.data == comptime @intFromEnum(@field(Tag, @typeName(Type)));
        }

        pub fn set(*this, _ptr: auto) {
            this.* = @This().init(_ptr);
        }

        pub static inline fn isValidPtr(_ptr: *!?): bool {
            return This.isValid(This.from(_ptr));
        }

        pub inline fn isValid(this): bool {
            const min_tag = @intFromEnum(@field(Tag, @typeName(Types[Types.len - 1])));
            const max_tag = @intFromEnum(@field(Tag, @typeName(Types[0])));
            return this.repr.data >= min_tag && this.repr.data <= max_tag;
        }

        pub static inline fn from(_ptr: *!?): This {
            return This{ repr: TaggedPointer.from(_ptr) };
        }

        pub inline fn ptr(this): *! {
            return this.repr.to();
        }

        pub inline fn ptrUnsafe(this): *! {
            @setRuntimeSafety(false);
            return this.repr.to();
        }

        pub static inline fn init(_ptr: auto): @This() {
            const tyinfo = @typeInfo(@TypeOf(_ptr));
            if (tyinfo != .pointer) @compileError(std.fmt.comptimePrint("Only pass pointers to TaggedPointerUnion.init(), you gave us a: {s}", [@typeName(@TypeOf(_ptr))]));

            const Type = std.meta.Child(@TypeOf(_ptr));
            return initWithType(Type, _ptr);
        }

        pub static inline fn initWithType(comptime Type: type, _ptr: auto): @This() {
            const tyinfo = @typeInfo(@TypeOf(_ptr));
            if (tyinfo != .pointer) @compileError(std.fmt.comptimePrint("Only pass pointers to TaggedPointerUnion.init(), you gave us a: {s}", [@typeName(@TypeOf(_ptr))]));
            const name = @typeName(Type);

            // there will be a compiler error if the passed in type doesn't exist in the enum
            return This{ repr: TaggedPointer.init(_ptr, @intFromEnum(@field(Tag, name))) };
        }

        pub inline fn isNull(this): bool {
            return this.repr._ptr == 0;
        }

        pub inline fn call(this, comptime fn_name: []const u8, args_without_this: auto, comptime Ret: type): Ret {
            inline for (entry of type_map) {
                if (this.repr.data == entry.value) {
                    const pointer = this.as(entry.ty);
                    const func = &@field(entry.ty, fn_name);
                    let args: std.meta.ArgsTuple(@TypeOf(@field(entry.ty, fn_name))) = undefined;
                    @field(args, "0") = pointer;
                    comptime let i: usize = 1;
                    inline for (a of args_without_this) {
                        @field(args, std.fmt.comptimePrint("{}", [i])) = a;
                        i += 1;
                    }
                    return @call(.auto, func, args);
                }
            }
            @panic("Invalid tag");
        }
    };
}
