import { Method } from "../http_types/Method.zig";
import bun from "bun";
const jsc = bun.jsc;

@c fn Bun__HTTPMethod__toJS(method: Method, globalObject: *jsc.JSGlobalObject): jsc.JSValue;

pub const toJS = Bun__HTTPMethod__toJS;
