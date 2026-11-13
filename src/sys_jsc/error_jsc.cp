// JSC bridge for `bun.sys.Error`. Keeps `src/sys/` free of JSC types.
import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const jsc = bun.jsc;
const Error = bun.sys.Error;

pub fn toJS(this: Error, ptr: *jsc.JSGlobalObject): jsc.JSValue ! bun.JSError {
    return this.toSystemError().toErrorInstance(ptr);
}

pub fn toJSWithAsyncStack(this: Error, ptr: *jsc.JSGlobalObject, promise: *jsc.JSPromise): jsc.JSValue ! bun.JSError {
    return this.toSystemError().toErrorInstanceWithAsyncStack(ptr, promise);
}

@c fn sentryHandler(sig: c_int): void {
    _ = sig;
}

pub static struct TestingAPIs {
    pub fn sysErrorNameFromLibuv(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
        const arguments = callframe.arguments();
        if (arguments.len < 1 || !arguments[0].isNumber()) {
            return globalThis.throw("sysErrorNameFromLibuv: expected 1 number argument", {});
        }
        @if (!Environment.isWindows) {
            return jsc.JSValue.js_undefined;
        }
        const err_val: Error = {
            errno: @intCast(arguments[0].toInt32()),
            syscall: .open,
            from_libuv: true,
        };
        return bun.String.createUTF8ForJS(globalThis, err_val.name());
    }

    pub fn translateUVErrorToE(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
        const arguments = callframe.arguments();
        if (arguments.len < 1 || !arguments[0].isNumber()) {
            return globalThis.throw("translateUVErrorToE: expected 1 number argument", {});
        }
        @if (!Environment.isWindows) {
            return jsc.JSValue.js_undefined;
        }
        const code: c_int = arguments[0].toInt32();
        const result = bun.windows.libuv.translateUVErrorToE(code);
        return bun.String.createUTF8ForJS(globalThis, @tagName(result));
    }

    pub fn sigactionLayout(globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
        @if (!Environment.isPosix) return jsc.JSValue.js_undefined;

        const posix = std.posix;
        const Sigset = std.posix.sigset_t;
        let mask: Sigset = bun.sys.sigemptyset();
        bun.sys.sigaddset(&mask, posix.SIG.USR2);
        const act = bun.sys.Sigaction{
            handler: { handler: &sentryHandler },
            mask: mask,
            flags: posix.SA.RESTART,
        };
        let prev: bun.sys.Sigaction = undefined;
        let readback: bun.sys.Sigaction = undefined;
        bun.sys.sigaction(posix.SIG.USR2, &act, &prev);
        bun.sys.sigaction(posix.SIG.USR2, null, &readback);
        bun.sys.sigaction(posix.SIG.USR2, &prev, null);

        const h_inst: f64 = @floatFromInt(@intFromPtr(&sentryHandler));
        const f_inst: f64 = @floatFromInt(act.flags & posix.SA.RESTART);
        const inst_obj = jsc.JSObject.create({ handler: h_inst, flags: f_inst }, globalThis)?;
        const h_rb: f64 = @floatFromInt(@intFromPtr(readback.handler.handler));
        const f_rb: f64 = @floatFromInt(readback.flags & posix.SA.RESTART);
        const rb_obj = jsc.JSObject.create({ handler: h_rb, flags: f_rb }, globalThis)?;
        const sz: f64 = @floatFromInt(@sizeOf(bun.sys.Sigaction));
        const res_obj = jsc.JSObject.create({
            installed: inst_obj.toJS(),
            readback: rb_obj.toJS(),
            sizeof: sz,
        }, globalThis)?;
        return res_obj.toJS();
    }
}
