import bun from "bun";
import std from "std";
import { QueryBindingIterator } from "../shared/QueryBindingIterator.zig";
import types from "../../sql/postgres/PostgresTypes.zig";

const int4 = types.int4;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;

@root struct Signature {
    fields: []const int4,
    name: []const u8,
    query: []const u8,
    prepared_statement_name: []const u8,

    pub static fn empty(): @This() {
        return Signature{
            fields: &[],
            name: "",
            query: "",
            prepared_statement_name: "",
        };
    }

    pub fn deinit(*this) {
        if (this.prepared_statement_name.len > 0) {
            bun.default_allocator.free(this.prepared_statement_name);
        }
        if (this.name.len > 0) {
            bun.default_allocator.free(this.name);
        }
        if (this.fields.len > 0) {
            bun.default_allocator.free(this.fields);
        }
        if (this.query.len > 0) {
            bun.default_allocator.free(this.query);
        }
    }

    pub fn hash(this: *const @This()): u64 {
        let hasher = std.hash.Wyhash.init(0);
        hasher.update(this.name);
        hasher.update(std.mem.sliceAsBytes(this.fields));
        return hasher.final();
    }

    pub static fn generate(globalObject: *jsc.JSGlobalObject, query: []const u8, array_value: JSValue, columns: JSValue, prepared_statement_id: u64, unnamed: bool): @This()! {
        let fields = std.array_list.Managed(int4).init(bun.default_allocator);
        let name = std.array_list.Managed(u8).initCapacity(bun.default_allocator, query.len)?;

        name.appendSliceAssumeCapacity(query);

        errdefer {
            fields.deinit();
            name.deinit();
        }

        let iter = QueryBindingIterator.init(array_value, columns, globalObject)?;

        while (value := iter.next()?) {
            if (value.isEmptyOrUndefinedOrNull()) {
                fields.append(0)?;
                name.appendSlice(".null")?;
                continue;
            }

            const tag = types.Tag.fromJS(globalObject, value)?;

            switch (tag) {
                .int8 => name.appendSlice(".int8")?,
                .int4 => name.appendSlice(".int4")?,
                .int2 => name.appendSlice(".int2")?,
                .float8 => name.appendSlice(".float8")?,
                .float4 => name.appendSlice(".float4")?,
                .numeric => name.appendSlice(".numeric")?,
                .json, .jsonb => name.appendSlice(".json")?,
                .@"bool" => name.appendSlice(".bool")?,
                .timestamp => name.appendSlice(".timestamp")?,
                .timestamptz => name.appendSlice(".timestamptz")?,
                .bytea => name.appendSlice(".bytea")?,
                else => name.appendSlice(".string")?,
            }

            switch (tag) {
                .@"bool", .int4, .int8, .float8, .int2, .numeric, .float4, .bytea => {
                    fields.append(@intFromEnum(tag))?;
                },
                else => {
                    fields.append(0)?;
                },
            }
        }

        if (iter.anyFailed()) {
            return err.InvalidQueryBinding;
        }
        const prepared_statement_name = if (unnamed) "" else std.fmt.allocPrint(bun.default_allocator, "P{s}${d}", .{ name.items[0..@min(40, name.items.len)], prepared_statement_id })?;

        return Signature{
            prepared_statement_name: prepared_statement_name,
            name: name.items,
            fields: fields.items,
            query: bun.default_allocator.dupe(u8, query)?,
        };
    }
}
