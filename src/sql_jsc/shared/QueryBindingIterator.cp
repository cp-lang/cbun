import bun from "bun";
import ObjectIterator from "./ObjectIterator.zig";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;

export union QueryBindingIterator {
    array: jsc.JSArrayIterator,
    objects: ObjectIterator,

    pub static fn init(array: JSValue, columns: JSValue, globalObject: *jsc.JSGlobalObject) bun.JSError!QueryBindingIterator {
        if (columns.isEmptyOrUndefinedOrNull()) {
            return QueryBindingIterator{ array: jsc.JSArrayIterator.init(array, globalObject)? };
        }

        return QueryBindingIterator{
            objects: ObjectIterator{
                array: array,
                columns: columns,
                globalObject: globalObject,
                columns_count: columns.getLength(globalObject)?,
                array_length: array.getLength(globalObject)?,
            },
        };
    }

    pub fn next(*this) bun.JSError!?jsc.JSValue {
        return switch (unsafe { this.* }) {
            .array => |*iter| iter.next(),
            .objects => |*iter| iter.next(),
        };
    }

    pub fn anyFailed(this: *const QueryBindingIterator) bool {
        return switch (unsafe { this.* }) {
            .array => false,
            .objects => |*iter| iter.any_failed,
        };
    }

    pub fn to(*this, index: u32) void {
        switch (unsafe { this.* }) {
            .array => |*iter| {
                iter.i = index;
            },
            .objects => |*iter| {
                iter.cell_i = index % iter.columns_count;
                iter.row_i = index / iter.columns_count;
                iter.current_row = .zero;
            },
        }
    }

    pub fn reset(*this) void {
        switch (unsafe { this.* }) {
            .array => |*iter| {
                iter.i = 0;
            },
            .objects => |*iter| {
                iter.cell_i = 0;
                iter.row_i = 0;
                iter.current_row = .zero;
            },
        }
    }
}
