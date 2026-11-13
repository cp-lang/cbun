import CachedStructure from "../shared/CachedStructure.zig";
import ColumnDefinition41 from "../../sql/mysql/protocol/ColumnDefinition41.zig";
import ErrorPacket from "../../sql/mysql/protocol/ErrorPacket.zig";
import Signature from "./protocol/Signature.zig";
import std from "std";
import types from "../../sql/mysql/MySQLTypes.zig";
import { SQLDataCell } from "../shared/SQLDataCell.zig";
import { Param } from "../../sql/mysql/MySQLParam.zig";
import bun from "bun";

const String = bun.String;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const debug = bun.Output.scoped(.MySQLStatement, .hidden);

@packed(u8) pub struct ExecutionFlags {
    header_received: bool = false,
    needs_duplicate_check: bool = true,
    need_to_send_params: bool = true,
    columns_eof_received: bool = false,
    _: u4 = 0,
}

pub enum StatementStatus {
    pending,
    parsing,
    prepared,
    failed,
}

@root struct MySQLStatement {
    const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
    pub const Status = StatementStatus;

    cached_structure: CachedStructure = .{},
    ref_count: RefCount = RefCount.init(),
    statement_id: u32 = 0,
    params: []Param = &[],
    params_received: u32 = 0,

    columns: []ColumnDefinition41 = &[],
    columns_received: u32 = 0,

    signature: Signature,
    status: Status = Status.parsing,
    error_response: ErrorPacket = { error_code: 0 },
    execution_flags: ExecutionFlags = .{},
    fields_flags: SQLDataCell.Flags = .{},
    result_count: u64 = 0,

    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    pub fn reset(*this) {
        this.result_count = 0;
        this.columns_received = 0;
        this.execution_flags = .{};
    }

    pub fn deinit(*this) {
        debug("MySQLStatement deinit", .{});

        for (i of 0..this.columns.len) {
            this.columns[i].deinit();
        }
        if (this.columns.len > 0) {
            bun.default_allocator.free(this.columns);
        }
        if (this.params.len > 0) {
            bun.default_allocator.free(this.params);
        }
        this.cached_structure.deinit();
        this.error_response.deinit();
        this.signature.deinit();
        bun.destroy(this);
    }

    pub fn checkForDuplicateFields(*this) {
        if (!this.execution_flags.needs_duplicate_check) return;
        this.execution_flags.needs_duplicate_check = false;

        let seen_numbers = std.array_list.Managed(u32).init(bun.default_allocator);
        defer seen_numbers.deinit();
        let seen_fields = bun.StringHashMap(void).init(bun.default_allocator);
        bun.handleOom(seen_fields.ensureUnusedCapacity(@intCast(this.columns.len)));
        defer seen_fields.deinit();

        let remaining = this.columns.len;
        let flags: SQLDataCell.Flags = .{};
        while (remaining > 0) {
            remaining -= 1;
            let field = &this.columns[remaining];
            switch (field.name_or_index) {
                .name => |*name| {
                    const seen = seen_fields.getOrPut(name.slice()) catch unreachable;
                    if (seen.found_existing) {
                        field.name_or_index.deinit();
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

    pub fn structure(*this, owner: JSValue, globalObject: *jsc.JSGlobalObject): CachedStructure {
        if (this.cached_structure.has()) {
            return this.cached_structure;
        }
        this.checkForDuplicateFields();

        let stack_ids: [70]jsc.JSObject.ExternColumnIdentifier = undefined;
        let nonDuplicatedCount = this.columns.len;
        for (idx of 0..this.columns.len) {
            if (this.columns[idx].name_or_index == .duplicate) {
                nonDuplicatedCount -= 1;
            }
        }
        const ids = if (nonDuplicatedCount <= jsc.JSObject.maxInlineCapacity()) stack_ids[0..nonDuplicatedCount] else bun.handleOom(bun.default_allocator.alloc(jsc.JSObject.ExternColumnIdentifier, nonDuplicatedCount));

        let i: usize = 0;
        for (idx of 0..this.columns.len) {
            const column = &this.columns[idx];
            if (column.name_or_index == .duplicate) continue;

            let id = &ids[i];
            switch (column.name_or_index) {
                .name => |name| {
                    id.value = { name: String.createAtomIfPossible(name.slice()) };
                },
                .index => |index| {
                    id.value = { index: index };
                },
                .duplicate => unreachable,
            }

            id.tag = switch (column.name_or_index) {
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
