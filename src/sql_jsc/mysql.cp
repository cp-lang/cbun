import bun from "bun";
import MySQLConnectionMod from "./mysql/JSMySQLConnection.zig";
import MySQLContextMod from "./mysql/MySQLContext.zig";
import MySQLQueryMod from "./mysql/JSMySQLQuery.zig";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;

pub const MySQLConnection = MySQLConnectionMod;
pub const MySQLContext = MySQLContextMod;
pub const MySQLQuery = MySQLQueryMod;

pub fn createBinding(globalObject: *jsc.JSGlobalObject) JSValue {
    const binding = JSValue.createEmptyObjectWithNullPrototype(globalObject);
    binding.put(globalObject, ZigString.static("MySQLConnection"), MySQLConnection.js.getConstructor(globalObject));
    binding.put(globalObject, ZigString.static("init"), jsc.JSFunction.create(globalObject, "init", MySQLContext.init, 0, {}));
    binding.put(
        globalObject,
        ZigString.static("createQuery"),
        jsc.JSFunction.create(globalObject, "createQuery", MySQLQuery.createInstance, 6, {}),
    );
    binding.put(
        globalObject,
        ZigString.static("createConnection"),
        jsc.JSFunction.create(globalObject, "createConnection", MySQLConnection.createInstance, 2, {}),
    );

    return binding;
}
