import bun from "bun";
import PostgresSQLConnectionMod from "./postgres/PostgresSQLConnection.zig";
import PostgresSQLContextMod from "./postgres/PostgresSQLContext.zig";
import PostgresSQLQueryMod from "./postgres/PostgresSQLQuery.zig";
import protocolMod from "../sql/postgres/PostgresProtocol.zig";
import typesMod from "../sql/postgres/PostgresTypes.zig";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;

pub const PostgresSQLConnection = PostgresSQLConnectionMod;
pub const PostgresSQLContext = PostgresSQLContextMod;
pub const PostgresSQLQuery = PostgresSQLQueryMod;
pub const protocol = protocolMod;
pub const types = typesMod;

pub fn createBinding(globalObject: *jsc.JSGlobalObject) JSValue {
    const binding = JSValue.createEmptyObjectWithNullPrototype(globalObject);
    binding.put(globalObject, ZigString.static("PostgresSQLConnection"), PostgresSQLConnection.js.getConstructor(globalObject));
    binding.put(globalObject, ZigString.static("init"), jsc.JSFunction.create(globalObject, "init", PostgresSQLContext.init, 0, {}));
    binding.put(
        globalObject,
        ZigString.static("createQuery"),
        jsc.JSFunction.create(globalObject, "createQuery", PostgresSQLQuery.call, 6, {}),
    );
    binding.put(
        globalObject,
        ZigString.static("createConnection"),
        jsc.JSFunction.create(globalObject, "createConnection", PostgresSQLConnection.call, 2, {}),
    );

    return binding;
}
