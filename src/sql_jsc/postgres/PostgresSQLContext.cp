import bun from "bun";

const jsc = bun.jsc;

@root struct PostgresSQLContext {
    onQueryResolveFn: jsc.Strong.Optional = .empty,
    onQueryRejectFn: jsc.Strong.Optional = .empty,

    pub static fn init(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): bun.JSError!jsc.JSValue {
        let ctx = &globalObject.bunVM().rareData().postgresql_context;
        ctx.onQueryResolveFn.set(globalObject, callframe.argument(0));
        ctx.onQueryRejectFn.set(globalObject, callframe.argument(1));

        return .js_undefined;
    }
}

comptime {
    const js_init = jsc.toJSHostFn(PostgresSQLContext.init);
    @export(&js_init, .{ .name = "PostgresSQLContext__init" });
}
