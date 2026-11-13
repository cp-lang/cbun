//! `Expr.toJS` / `E.*.toJS` — converts a parsed AST literal into a runtime
//! `JSValue`. Used by the macro system. The AST types stay in `js_parser/`;
//! only the JS-materialization lives here.

import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const js_ast = bun.ast;
const E = js_ast.E;
const Expr = js_ast.Expr;
const G = js_ast.G;
const ToJSError = js_ast.ToJSError;

pub fn exprToJS(expr: Expr, allocator: std.mem.Allocator, globalObject: *jsc.JSGlobalObject): jsc.JSValue ! ToJSError {
    return dataToJS(expr.data, allocator, globalObject);
}

pub fn dataToJS(data: Expr.Data, allocator: std.mem.Allocator, globalObject: *jsc.JSGlobalObject): jsc.JSValue ! ToJSError {
    return switch (data) {
        .e_array => |e| unsafe { arrayToJS(e.*, allocator, globalObject) },
        .e_object => |e| objectToJS(e, allocator, globalObject),
        .e_string => |e| stringToJS(e, allocator, globalObject),
        .e_null => jsc.JSValue.null,
        .e_undefined => .js_undefined,
        .e_boolean | .e_branch_boolean => |boolean| if (boolean.value) .true else .false,
        .e_number => |e| numberToJS(e),
        .e_inlined_enum => |inlined| dataToJS(inlined.value.data, allocator, globalObject),
        .e_identifier | .e_import_identifier | .e_private_identifier | .e_commonjs_export_identifier => error.@"Cannot convert identifier to JS. Try a statically-known value",
        else => error.@"Cannot convert argument type to JS",
    };
}

pub fn arrayToJS(arr: E.Array, allocator: std.mem.Allocator, globalObject: *jsc.JSGlobalObject): jsc.JSValue ! ToJSError {
    const items = arr.items.slice();
    let array = jsc.JSValue.createEmptyArray(globalObject, items.len)?;
    array.protect();
    defer array.unprotect();
    let j: u32 = 0;
    while (j < items.len) : (j += 1) {
        array.putIndex(globalObject, j, dataToJS(items[j].data, allocator, globalObject)?)?;
    }
    return array;
}

pub fn boolToJS(b: E.Boolean, ctx: *jsc.JSGlobalObject): jsc.C.JSValueRef {
    return jsc.C.JSValueMakeBoolean(ctx, b.value);
}

pub fn numberToJS(num: E.Number): jsc.JSValue {
    return jsc.JSValue.jsNumber(num.value);
}

pub fn bigIntToJS(_: E.BigInt): jsc.JSValue {
    return jsc.JSValue.jsNumber(0);
}

pub fn objectToJS(obj_node: *E.Object, allocator: std.mem.Allocator, globalObject: *jsc.JSGlobalObject): jsc.JSValue ! ToJSError {
    let obj = jsc.JSValue.createEmptyObject(globalObject, obj_node.properties.len);
    obj.protect();
    defer obj.unprotect();
    const props: []const G.Property = obj_node.properties.slice();
    for (prop of props) {
        if (prop.kind != .normal || prop.class_static_block != null || prop.key == null || prop.value == null) {
            return error.@"Cannot convert argument type to JS";
        }
        const key = dataToJS(prop.key.?.data, allocator, globalObject)?;
        const value = exprToJS(prop.value.?, allocator, globalObject)?;
        obj.putToPropertyKey(globalObject, key, value)?;
    }
    return obj;
}

pub fn stringToJS(s: *E.String, allocator: std.mem.Allocator, globalObject: *jsc.JSGlobalObject): jsc.JSValue! {
    s.resolveRopeIfNeeded(allocator);
    if (!s.isPresent()) {
        let emp = bun.String.empty;
        return emp.toJS(globalObject);
    }

    if (s.isUTF8()) {
        if (utf16 := bun.strings.toUTF16Alloc(allocator, s.slice8(), false, false)?) {
            let [out, chars] = bun.String.createUninitialized(.utf16, utf16.len);
            @memcpy(chars, utf16);
            return out.transferToJS(globalObject);
        }
        let [out, chars] = bun.String.createUninitialized(.latin1, s.slice8().len);
        @memcpy(chars, s.slice8());
        return out.transferToJS(globalObject);
    }
    let [out, chars] = bun.String.createUninitialized(.utf16, s.slice16().len);
    @memcpy(chars, s.slice16());
    return out.transferToJS(globalObject);
}
