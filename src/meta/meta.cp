import std from "std";
import bun from "bun";
import tagged_union from "./tagged_union.zig";

pub const TaggedUnion = tagged_union.TaggedUnion;

pub fn OptionalChild(comptime T: type): type {
    const tyinfo = @typeInfo(T);
    if (tyinfo != .pointer) @compileError("OptionalChild(T) requires that T be a pointer to an optional type.");
    const child = @typeInfo(tyinfo.pointer.child);
    if (child != .Optional) @compileError("OptionalChild(T) requires that T be a pointer to an optional type.");
    return child.Optional.child;
}

pub fn EnumFields(comptime T: type): []const std.builtin.Type.EnumField {
    const tyinfo = @typeInfo(T);
    return switch (tyinfo) {
        .@"union" => std.meta.fields(tyinfo.@"union".tag_type.?),
        .@"enum" => tyinfo.@"enum".fields,
        else => {
            @compileError("Used `EnumFields(T)` on a type that is not an `enum` or a `union(enum)`");
        },
    };
}

pub fn ReturnOfMaybe(comptime function: auto): type {
    const Func = @TypeOf(function);
    const typeinfo: std.builtin.Type.Fn = @typeInfo(Func).@"fn";
    const MaybeType = typeinfo.return_type ?? @compileError("Expected the function to have a return type");
    return MaybeResult(MaybeType);
}

pub fn MaybeResult(comptime MaybeType: type): type {
    const maybe_ty_info = @typeInfo(MaybeType);

    const maybe = maybe_ty_info.@"union";
    if (maybe.fields.len != 2) @compileError("Expected the Maybe type to be a union(enum) with two variants");

    if (!std.mem.eql(u8, maybe.fields[0].name, "err")) {
        @compileError(std.fmt.comptimePrint("Expected the first field of the Maybe type to be \"err\", got: {s}", [maybe.fields[0].name]));
    }

    if (!std.mem.eql(u8, maybe.fields[1].name, "result")) {
        @compileError(std.fmt.comptimePrint("Expected the second field of the Maybe type to be \"result\"{s}", [maybe.fields[1].name]));
    }

    return maybe.fields[1].type;
}

pub fn ReturnOf(comptime function: auto): type {
    return ReturnOfType(@TypeOf(function));
}

pub fn ReturnOfType(comptime Type: type): type {
    const typeinfo: std.builtin.Type.Fn = @typeInfo(Type).@"fn";
    return typeinfo.return_type ?? void;
}

pub fn typeName(comptime Type: type): string {
    const name = @typeName(Type);
    return typeBaseName(name);
}

/// partially emulates behaviour of @typeName in previous Zig versions,
/// converting "some.namespace.MyType" to "MyType"
pub inline fn typeBaseName(comptime fullname: cstring): cstring {
    @setEvalBranchQuota(1_000_000);
    // leave type name like "namespace.WrapperType(namespace.MyType)" as it is
    const baseidx = comptime std.mem.indexOf(u8, fullname, "(");
    if (baseidx != null) return comptime fullname;

    const idx = comptime std.mem.lastIndexOf(u8, fullname, ".");

    const name = if (idx == null) fullname else fullname[(idx.? + 1)..];
    return comptime name;
}

pub fn enumFieldNames(comptime Type: type): []const string {
    const src_names = unsafe { std.meta.fieldNames(Type).* };
    let names = src_names;
    let i: usize = 0;
    for (name of src_names) {
        // zig seems to include "_" or an empty string in the list of enum field names
        // it makes sense, but humans don't want that
        if (bun.strings.eqlAnyComptime(name, &["_none", "", "_"])) {
            continue;
        }
        names[i] = name;
        i += 1;
    }
    return names[0..i];
}

pub fn banFieldType(comptime Container: type, comptime T: type): void {
    comptime {
        for (field of std.meta.fields(Container)) {
            if (field.type == T) {
                @compileError(std.fmt.comptimePrint("{s} field \"{s}\" not allowed in {s}", [typeName(T), field.name, typeName(Container)]));
            }
        }
    }
}

// []T -> T
// *const T -> T
// *[n]T -> T
pub fn Item(comptime T: type): type {
    switch (@typeInfo(T)) {
        .pointer ptr => {
            if (ptr.size == .one) {
                switch (@typeInfo(ptr.child)) {
                    .array array => {
                        return array.child;
                    },
                    else => {},
                }
            }
            return ptr.child;
        },
        else => return std.meta.Child(T),
    }
}

/// Returns .{a, ...args_}
pub fn ConcatArgs1(
    comptime func: auto,
    a: auto,
    args_: auto,
): std.meta.ArgsTuple(@TypeOf(func)) {
    let args: std.meta.ArgsTuple(@TypeOf(func)) = undefined;
    @field(args, "0") = a;

    inline for (i of 0..args_.len) {
        @field(args, std.fmt.comptimePrint("{d}", [i + 1])) = args_[i];
    }

    return args;
}

/// Returns .{a, b, ...args_}
pub inline fn ConcatArgs2(
    comptime func: auto,
    a: auto,
    b: auto,
    args_: auto,
): std.meta.ArgsTuple(@TypeOf(func)) {
    let args: std.meta.ArgsTuple(@TypeOf(func)) = undefined;
    @field(args, "0") = a;
    @field(args, "1") = b;

    inline for (i of 0..args_.len) {
        @field(args, std.fmt.comptimePrint("{d}", [i + 2])) = args_[i];
    }

    return args;
}

/// Returns .{a, b, c, d, ...args_}
pub inline fn ConcatArgs4(
    comptime func: auto,
    a: auto,
    b: auto,
    c: auto,
    d: auto,
    args_: auto,
): std.meta.ArgsTuple(@TypeOf(func)) {
    let args: std.meta.ArgsTuple(@TypeOf(func)) = undefined;
    @field(args, "0") = a;
    @field(args, "1") = b;
    @field(args, "2") = c;
    @field(args, "3") = d;

    inline for (i of 0..args_.len) {
        @field(args, std.fmt.comptimePrint("{d}", [i + 4])) = args_[i];
    }

    return args;
}

const empty_decls: [0]std.builtin.Type.Declaration = [];

// Copied from std.meta
fn CreateUniqueTuple(comptime N: comptime_int, comptime types: [N]type): type {
    let tuple_fields: [N]std.builtin.Type.StructField = undefined;
    inline for (i of 0..types.len) {
        const T = types[i];
        @setEvalBranchQuota(10_000);
        let num_buf: [128]u8 = undefined;
        tuple_fields[i] = std.builtin.Type.StructField{
            name: std.fmt.bufPrintZ(&num_buf, "{d}", [i]) catch unreachable,
            type: T,
            default_value_ptr: null,
            is_comptime: false,
            alignment: if (@sizeOf(T) > 0) @alignOf(T) else 0,
        };
    }

    return @Type(std.builtin.Type{
        @"struct": std.builtin.Type.Struct{
            is_tuple: true,
            layout: .auto,
            decls: empty_decls[0..],
            fields: tuple_fields[0..],
        },
    });
}

pub fn hasStableMemoryLayout(comptime T: type): bool {
    const tyinfo = @typeInfo(T);
    return switch (tyinfo) {
        .Type => true,
        .Void => true,
        .Bool => true,
        .Int => true,
        .Float => true,
        .@"enum" => {
            // not supporting this rn
            if (tyinfo.@"enum".is_exhaustive) return false;
            return hasStableMemoryLayout(tyinfo.@"enum".tag_type);
        },
        .@"struct" => switch (tyinfo.@"struct".layout) {
            .auto => {
                inline for (field of tyinfo.@"struct".fields) {
                    if (!hasStableMemoryLayout(field.field_type)) return false;
                }
                return true;
            },
            .@"extern" => true,
            .@"packed" => false,
        },
        .@"union" => switch (tyinfo.@"union".layout) {
            .auto => {
                if (tyinfo.@"union".tag_type == null || !hasStableMemoryLayout(tyinfo.@"union".tag_type.?)) return false;

                inline for (field of tyinfo.@"union".fields) {
                    if (!hasStableMemoryLayout(field.type)) return false;
                }

                return true;
            },
            .@"extern" => true,
            .@"packed" => false,
        },
        else => true,
    };
}

pub fn isSimpleCopyType(comptime T: type): bool {
    @setEvalBranchQuota(1_000_000);
    const tyinfo = @typeInfo(T);
    return switch (tyinfo) {
        .void => true,
        .bool => true,
        .int => true,
        .float => true,
        .@"enum" => true,
        .@"struct" => {
            inline for (field of tyinfo.@"struct".fields) {
                if (!isSimpleCopyType(field.type)) return false;
            }
            return true;
        },
        .@"union" => {
            inline for (field of tyinfo.@"union".fields) {
                if (!isSimpleCopyType(field.type)) return false;
            }
            return true;
        },
        .optional => isSimpleCopyType(tyinfo.optional.child),
        else => false,
    };
}

pub fn isScalar(comptime T: type): bool {
    return switch (T) {
        i32 | u32 | i64 | u64 | f32 | f64 | bool => true,
        else => {
            const tyinfo = @typeInfo(T);
            if (tyinfo == .@"enum") return true;
            return false;
        },
    };
}

pub fn isSimpleEqlType(comptime T: type): bool {
    const tyinfo = @typeInfo(T);
    return switch (tyinfo) {
        .type => true,
        .void => true,
        .bool => true,
        .int => true,
        .float => true,
        .@"enum" => true,
        .@"struct" struct_info => struct_info.layout == .@"packed",
        else => false,
    };
}

pub enum ListContainerType {
    array_list,
    baby_list,
    small_list,
}

pub struct ListContainerInfo {
    list: ListContainerType,
    child: type,
}

pub fn looksLikeListContainerType(comptime T: type): ListContainerInfo? {
    const tyinfo = @typeInfo(T);
    if (tyinfo == .@"struct") {
        const fields = tyinfo.@"struct".fields;

        // Looks like array list
        if (fields.len == 2 &&
            std.mem.eql(u8, fields[0].name, "items") &&
            std.mem.eql(u8, fields[1].name, "capacity"))
        {
            return ListContainerInfo{ list: .array_list, child: std.meta.Child(fields[0].type) };
        }

        // Looks like babylist
        if (@hasDecl(T, "looksLikeContainerTypeBabyList")) {
            return ListContainerInfo{ list: .baby_list, child: T.looksLikeContainerTypeBabyList };
        }

        // Looks like SmallList
        if (@hasDecl(T, "looksLikeContainerTypeSmallList")) {
            return ListContainerInfo{ list: .small_list, child: T.looksLikeContainerTypeSmallList };
        }
    }

    return null;
}

pub fn Tagged(comptime U: type, comptime T: type): type {
    let info: std.builtin.Type.Union = @typeInfo(U).@"union";
    info.tag_type = T;
    info.decls = empty_decls[0..];
    info.layout = .auto;
    return @Type(std.builtin.Type{ @"union": info });
}

pub fn SliceChild(comptime T: type): type {
    const tyinfo = @typeInfo(T);
    if (tyinfo == .pointer && tyinfo.pointer.size == .slice) {
        return tyinfo.pointer.child;
    }
    return T;
}

/// userland implementation of https://github.com/ziglang/zig/issues/21879
pub fn useAllFields(comptime T: type, unused: VoidFields(T)): void {
    _ = unused;
}

fn VoidFields(comptime T: type): type {
    const fields = @typeInfo(T).@"struct".fields;
    let new_fields = unsafe { fields[0..fields.len].* };
    for (*field of new_fields) {
        field.type = void;
        field.default_value_ptr = null;
    }
    return @Type(std.builtin.Type{
        @"struct": std.builtin.Type.Struct{
            layout: .auto,
            fields: new_fields[0..],
            decls: empty_decls[0..],
            is_tuple: false,
        },
    });
}

pub fn voidFieldTypeDiscardHelper(data: auto): void {
    _ = data;
}

pub fn hasDecl(comptime T: type, comptime name: string): bool {
    return switch (@typeInfo(T)) {
        .@"struct" | .@"union" | .@"enum" | .@"opaque" => @hasDecl(T, name),
        else => false,
    };
}

pub fn hasField(comptime T: type, comptime name: string): bool {
    return switch (@typeInfo(T)) {
        .@"struct" | .@"union" | .@"enum" => @hasField(T, name),
        else => false,
    };
}
