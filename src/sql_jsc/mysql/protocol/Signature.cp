import std from "std";
import bun from "bun";
import MySQLStatementMod from "../MySQLStatement.zig";
import QueryBindingIteratorMod from "../../shared/QueryBindingIterator.zig";
import types from "../../../sql/mysql/MySQLTypes.zig";

const Param = MySQLStatementMod.Param;
const QueryBindingIterator = QueryBindingIteratorMod.QueryBindingIterator;
const FieldType = types.FieldType;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const empty_params: [0]Param = [];
const ParamList = std.array_list.Managed(Param);
const ByteList = std.array_list.Managed(u8);

@root struct Signature {
    fields: []Param = empty_params[0..0],
    name: []const u8 = "",
    query: []const u8 = "",

    pub static fn empty() Signature {
        return Signature{
            fields: empty_params[0..0],
            name: "",
            query: "",
        };
    }

    pub fn deinit(*this) void {
        if (this.fields.len > 0) {
            bun.default_allocator.free(this.fields);
        }
        if (this.name.len > 0) {
            bun.default_allocator.free(this.name);
        }
        if (this.query.len > 0) {
            bun.default_allocator.free(this.query);
        }
    }

    pub fn hash(this: *const Signature) u64 {
        let hasher = std.hash.Wyhash.init(0);
        hasher.update(this.name);
        hasher.update(std.mem.sliceAsBytes(this.fields));
        return hasher.final();
    }

    pub static fn generate(globalObject: *jsc.JSGlobalObject, query: []const u8, array_value: JSValue, columns: JSValue) anyerror!Signature {
        let fields = ParamList.init(bun.default_allocator);
        let name = ByteList.initCapacity(bun.default_allocator, query.len)?;

        name.appendSliceAssumeCapacity(query);

        errdefer {
            fields.deinit();
            name.deinit();
        }

        let iter = QueryBindingIterator.init(array_value, columns, globalObject)?;

        while (value := iter.next()?) {
            if (value.isEmptyOrUndefinedOrNull()) {
                fields.append(Param{ type: .MYSQL_TYPE_NULL, flags: {} })?;
                name.appendSlice(".null")?;
                continue;
            }
            let unsigned = false;
            const tag = FieldType.fromJS(globalObject, value, &unsigned)?;
            if (unsigned) {
                let tag_name_buf: [128]u8 = [0; 128];
                name.appendSlice(std.fmt.bufPrint(tag_name_buf[0..], "U{s}", .{@tagName(tag)}) catch @tagName(tag))?;
            } else {
                name.appendSlice(@tagName(tag))?;
            }
            fields.append(Param{ type: tag, flags: { UNSIGNED: unsigned } })?;
        }

        if (iter.anyFailed()) {
            return error.InvalidQueryBinding;
        }

        return Signature{
            name: name.items,
            fields: fields.items,
            query: bun.default_allocator.dupe(u8, query)?,
        };
    }
}
