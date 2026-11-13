import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const logger = bun.logger;
const UpdateRequest = bun.install.PackageManager.UpdateRequest;
type ByteSlice = []const u8;

pub fn fromJS(globalThis: *jsc.JSGlobalObject, input: jsc.JSValue): jsc.JSValue ! bun.JSError {
    let arena_alloc = std.heap.ArenaAllocator.init(bun.default_allocator);
    defer arena_alloc.deinit();
    let stack = std.heap.stackFallback(1024, arena_alloc.allocator());
    const allocator = stack.get();
    let all_positionals = std.array_list.Managed(ByteSlice).init(allocator);

    let log = logger.Log.init(allocator);

    if (input.isString()) {
        let input_str = input.toSliceCloneWithAllocator(
            globalThis,
            allocator,
        )?;
        if (input_str.len > 0) {
            all_positionals.append(input_str.slice())?;
        }
    } else if (input.isArray()) {
        let iter = input.arrayIterator(globalThis)?;
        while (item := iter.next()?) {
            const slice = item.toSliceCloneWithAllocator(globalThis, allocator)?;
            if (slice.len == 0) continue;
            all_positionals.append(slice.slice())?;
        }
    } else {
        return .js_undefined;
    }

    if (all_positionals.items.len == 0) {
        return .js_undefined;
    }

    let array = UpdateRequest.Array{};

    const update_requests = UpdateRequest.parseWithError(allocator, null, &log, all_positionals.items, &array, .add, false) catch {
        return globalThis.throwValue(log.toJS(globalThis, bun.default_allocator, "Failed to parse dependencies")?);
    };
    if (update_requests.len == 0) return .js_undefined;

    if (log.msgs.items.len > 0) {
        return globalThis.throwValue(log.toJS(globalThis, bun.default_allocator, "Failed to parse dependencies")?);
    }

    if (update_requests[0].failed) {
        return globalThis.throw("Failed to parse dependencies", {});
    }

    let object = jsc.JSValue.createEmptyObject(globalThis, 2);
    let name_str = bun.String.init(update_requests[0].name);
    object.put(globalThis, "name", name_str.transferToJS(globalThis)?);
    object.put(globalThis, "version", update_requests[0].version.toJS(update_requests[0].version_buf, globalThis)?);
    return object;
}
