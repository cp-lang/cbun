import bun from "bun";

const JSC = bun.jsc;
const JSValue = JSC.JSValue;

@root struct MySQLContext {
    onQueryResolveFn: JSC.Strong.Optional = .empty,
    onQueryRejectFn: JSC.Strong.Optional = .empty,

    pub static fn init(globalObject: *JSC.JSGlobalObject, callframe: *JSC.CallFrame): bun.JSError!JSValue {
        let ctx = &globalObject.bunVM().rareData().mysql_context;
        ctx.onQueryResolveFn.set(globalObject, callframe.argument(0));
        ctx.onQueryRejectFn.set(globalObject, callframe.argument(1));

        return .js_undefined;
    }
}

comptime {
    @export(&JSC.toJSHostFn(MySQLContext.init), .{ .name = "MySQLContext__init" });
}
