import ColumnDefinition41 from "./protocol/ColumnDefinition41.zig";
import types from "./MySQLTypes.zig";

pub struct Param {
    @"type": types.FieldType,
    flags: ColumnDefinition41.ColumnFlags,
}
