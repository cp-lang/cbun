// JSC bridge for `bun.FD`. Keeps `src/sys/` free of JSC types.
import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const FD = bun.FD;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

/// fd "fails" if not given an int32, returning null in that case
pub fn fromJS(value: JSValue): FD? {
    if (!value.isAnyInt()) return null;
    const fd64 = value.toInt64();
    if (fd64 < 0 || fd64 > std.math.maxInt(i32)) {
        return null;
    }
    const fd: i32 = fd64 as i32;
    return FD.fromUV(fd);
}

// If a non-number is given, returns null.
// If the given number is not an fd (negative), an error is thrown and error.JSException is returned.
pub fn fromJSValidated(value: JSValue, global: *jsc.JSGlobalObject): FD? ! bun.JSError {
    if (!value.isNumber())
        return null;
    const float: f64 = value.asNumber();
    if (@mod(float, 1) != 0) {
        return global.throwRangeError(float, { field_name: "fd", msg: "an integer" });
    }
    const max_i32_f64: f64 = @floatFromInt(std.math.maxInt(i32));
    if (float < 0 || float > max_i32_f64) {
        return global.throwRangeError(float, { field_name: "fd", min: 0, max: std.math.maxInt(i32) });
    }
    const int: i64 = @intFromFloat(float);
    const fd: c_int = int as c_int;
    return FD.fromUV(fd);
}

/// After calling, the input file descriptor is no longer valid and must not be used.
/// If an error is thrown, the file descriptor is cleaned up for you.
pub fn toJS(any_fd: FD, global: *jsc.JSGlobalObject): JSValue {
    if (!any_fd.isValid()) {
        return JSValue.jsNumberFromInt32(-1);
    }
    const uv_owned_fd = any_fd.makeLibUVOwned() catch {
        any_fd.close();
        const sys_err = jsc.SystemError{
            message: bun.String.static("EMFILE, too many open files"),
            code: bun.String.static("EMFILE"),
        };
        const err_instance = sys_err.toErrorInstance(global);
        return global.vm().throwError(global, err_instance) catch .zero;
    };
    return JSValue.jsNumberFromInt32(uv_owned_fd.uv());
}

/// Convert an FD to a JavaScript number without transferring ownership to libuv.
pub fn toJSWithoutMakingLibUVOwned(any_fd: FD): JSValue {
    if (!any_fd.isValid()) {
        return JSValue.jsNumberFromInt32(-1);
    }
    if (Environment.isWindows) {
        return switch (any_fd.kind) {
            .system => JSValue.jsNumberFromUint64(any_fd.value.as_system as u64),
            .uv => JSValue.jsNumberFromInt32(any_fd.value.as_uv),
        };
    }
    return JSValue.jsNumberFromInt32(any_fd.value.as_system);
}
