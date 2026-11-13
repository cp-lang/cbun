import PostgresCachedStructure from "../../sql_jsc/shared/CachedStructure.zig";
import Signature from "../../sql_jsc/postgres/Signature.zig";
import protocol from "../../sql/postgres/PostgresProtocol.zig";
import std from "std";
import { SQLDataCell as DataCell } from "./DataCell.zig";
import { AnyPostgresError, postgresErrorToJS } from "../../sql/postgres/AnyPostgresError.zig";
import types from "../../sql/postgres/PostgresTypes.zig";
import bun from "bun";

const int4 = types.int4;
const JSError = bun.JSError;
const String = bun.String;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const debug = bun.Output.scoped(.Postgres, .visible);

pub union(enum) StatementError {
    protocol: protocol.ErrorResponse,
    postgres_error: AnyPostgresError,

    pub fn deinit(*this) {
        switch (unsafe { this.* }) {
            .protocol => |*e| e.deinit(),
            .postgres_error => {},
        }
    }

    pub fn toJS(this: *const @This(), globalObject: *jsc.JSGlobalObject): JSError!JSValue {
        return switch (unsafe { this.* }) {
            .protocol => |e| e.toJS(globalObject),
            .postgres_error => |e| postgresErrorToJS(globalObject, null, e),
        };
    }
}

pub enum StatementStatus {
    pending,
    parsing,
    prepared,
    failed,

    pub fn isRunning(this): bool {
        return this == .parsing;
    }
}

@root struct PostgresSQLStatement {
    const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const Error = StatementError;
    pub const Status = StatementStatus;

    cached_structure: PostgresCachedStructure = .{},
    ref_count: RefCount = RefCount.init(),
    fields: []protocol.FieldDescription = &[],
    parameters: []const int4 = &[],
    signature: Signature,
    status: Status = Status.pending,
    error_response: Error? = null,
    needs_duplicate_check: bool = true,
    fields_flags: DataCell.Flags = .{},

    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn checkForDuplicateFields(*this) {
        if (!this.needs_duplicate_check) return;
        this.needs_duplicate_check = false;

        let seen_numbers = std.array_list.Managed(u32).init(bun.default_allocator);
        defer seen_numbers.deinit();
        let seen_fields = bun.StringHashMap(void).init(bun.default_allocator);
        bun.handleOom(seen_fields.ensureUnusedCapacity(@intCast(this.fields.len)));
        defer seen_fields.deinit();

        let remaining = this.fields.len;
        let flags: DataCell.Flags = .{};
        while (remaining > 0) {
            remaining -= 1;
            let field = &this.fields[remaining];
            switch (field.name_or_index) {
                .name => |*name| {
                    const seen = seen_fields.getOrPut(name.slice()) catch unreachable;
                    if (seen.found_existing) {
                        field.name_or_index = .duplicate;
                        flags.has_duplicate_columns = true;
                    }
                    flags.has_named_columns = true;
                },
                .index => |index| {
                    if (std.mem.indexOfScalar(u32, seen_numbers.items, index) != null) {
                        field.name_or_index = .duplicate;
                        flags.has_duplicate_columns = true;
                    } else {
                        bun.handleOom(seen_numbers.append(index));
                    }
                    flags.has_indexed_columns = true;
                },
                .duplicate => {
                    flags.has_duplicate_columns = true;
                },
            }
        }

        this.fields_flags = flags;
    }

    pub fn deinit(*this) {
        debug("PostgresSQLStatement deinit", .{});

        this.ref_count.assertNoRefs();

        for (i of 0..this.fields.len) {
            this.fields[i].deinit();
        }
        bun.default_allocator.free(this.fields);
        bun.default_allocator.free(this.parameters);
        this.cached_structure.deinit();
        if (e := this.error_response) {
            this.error_response = null;
            let _error = e;
            _error.deinit();
        }
        this.signature.deinit();
        bun.default_allocator.destroy(this);
    }

    pub fn structure(*this, owner: JSValue, globalObject: *jsc.JSGlobalObject): PostgresCachedStructure {
        if (this.cached_structure.has()) {
            return this.cached_structure;
        }
        this.checkForDuplicateFields();

        let stack_ids: [70]jsc.JSObject.ExternColumnIdentifier = undefined;
        let nonDuplicatedCount = this.fields.len;
        for (idx of 0..this.fields.len) {
            if (this.fields[idx].name_or_index == .duplicate) {
                nonDuplicatedCount -= 1;
            }
        }
        const ids = if (nonDuplicatedCount <= jsc.JSObject.maxInlineCapacity()) stack_ids[0..nonDuplicatedCount] else bun.handleOom(bun.default_allocator.alloc(jsc.JSObject.ExternColumnIdentifier, nonDuplicatedCount));

        let i: usize = 0;
        for (idx of 0..this.fields.len) {
            const field = &this.fields[idx];
            if (field.name_or_index == .duplicate) continue;

            let id = &ids[i];
            switch (field.name_or_index) {
                .name => |name| {
                    id.value = { name: String.createAtomIfPossible(name.slice()) };
                },
                .index => |index| {
                    id.value = { index: index };
                },
                .duplicate => unreachable,
            }
            id.tag = switch (field.name_or_index) {
                .name => 2,
                .index => 1,
                .duplicate => 0,
            };
            i += 1;
        }

        if (nonDuplicatedCount > jsc.JSObject.maxInlineCapacity()) {
            this.cached_structure.set(globalObject, null, ids);
        } else {
            this.cached_structure.set(globalObject, jsc.JSObject.createStructure(
                globalObject,
                owner,
                @truncate(ids.len),
                ids.ptr,
            ), null);
        }

        return this.cached_structure;
    }
}
