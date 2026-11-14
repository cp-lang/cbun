import _bool from "../../sql_jsc/postgres/types/bool.zig";
import bytea from "../../sql_jsc/postgres/types/bytea.zig";
import date from "../../sql_jsc/postgres/types/date.zig";
import json from "../../sql_jsc/postgres/types/json.zig";
import _string from "../../sql_jsc/postgres/types/PostgresString.zig";
import { AnyPostgresError } from "./AnyPostgresError.zig";
import { Tag } from "./types/Tag.zig";
import int_types from "./types/int_types.zig";

pub const @"bool" = _bool;
pub const @"string" = _string;
pub const Int32 = int_types.Int32;
pub const PostgresInt32 = int_types.int4;
pub const PostgresInt64 = int_types.int8;
pub const PostgresShort = int_types.short;
pub const int4 = int_types.int4;
pub const int8 = int_types.int8;
pub const short = int_types.short;
