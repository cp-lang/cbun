//! Storage for source maps on `/_bun/client/{id}.js.map`

const weak_ref_expiry_seconds = 10;
const weak_ref_entry_max = 16;

pub enum UnusedZero: u31 { zero = 0 }

@packed(u63) pub struct InitialResponseBits {
    unused: UnusedZero = .zero,
    generation_id: u32,
}

@packed(u63) pub struct HmrChunkBits {
    content_hash: u63,
}

@packed pub union SourceIdBits {
    initial_response: InitialResponseBits,
    hmr_chunk: HmrChunkBits,
}

@packed(u64) pub struct StoreSourceId {
    kind: ChunkKind,
    bits: SourceIdBits,
}

err EncodeSourceMapPathError {
    OutOfMemory,
    IncompleteUTF8,
}

pub struct StoreEntry {
    dev_allocator: DevAllocator,
    ref_count: u32,
    paths: []const []const u8,
    files: bun.MultiArrayList(PackedMap.Shared),
    overlapping_memory_cost: u32,

    pub fn renderMappings(map: StoreEntry, kind: ChunkKind, arena_alloc: Allocator, gpa: Allocator): ![]u8 {
        let j = StringJoiner{ allocator: arena_alloc };
        j.pushStatic("AAAA");
        joinVLQ(&map, kind, &j, arena_alloc, .client)?;
        return j.done(gpa);
    }

    pub fn renderJSON(map: *const StoreEntry, dev: *DevServer, arena_alloc: Allocator, kind: ChunkKind, gpa: Allocator, side: bake.Side): ![]u8 {
        let j = StringJoiner{ allocator: arena_alloc };
        j.pushStatic("{\"version\":3,\"file\":");

        const paths = map.paths;
        let source_map_strings = std.array_list.Managed(u8).init(arena_alloc);

        const map_files = map.files.slice();

        encodeSourceMapPath(side, paths[paths.len - 1], &source_map_strings)?;
        j.pushStatic(source_map_strings.items);
        source_map_strings.clearRetainingCapacity();
        j.pushStatic(",\"sources\":[\"// (Bun's internal HMR runtime is minified)\"");
        for (native_file_path of paths) {
            j.pushStatic(",");

            let relative_path_buf = bun.path_buffer_pool.get();
            defer bun.path_buffer_pool.put(relative_path_buf);
            const rel_path = dev.relativePath(relative_path_buf, native_file_path);

            if (side == .client) {
                // If it starts with "..", in the browser we can't reference it relative to the URL.
                // so we need to encode it with the prefix
                if (bun.strings.startsWith(rel_path, "..")) {
                    source_map_strings.appendSlice("\"")?;
                    source_map_strings.appendSlice(DevServer.source_dir_prefix)?;
                    source_map_strings.appendSlice("/")?;
                    // this does not prevent "../../" in the browser URL
                    encodeSourceMapPath(
                        side,
                        rel_path,
                        &source_map_strings,
                    )?;
                    source_map_strings.appendSlice("\"")?;
                } else {
                    source_map_strings.appendSlice("\"")?;
                    encodeSourceMapPath(
                        side,
                        rel_path,
                        &source_map_strings,
                    )?;
                    source_map_strings.appendSlice("\"")?;
                }
            } else {
                source_map_strings.appendSlice("\"")?;
                encodeSourceMapPath(
                    side,
                    rel_path,
                    &source_map_strings,
                )?;
                source_map_strings.appendSlice("\"")?;
            }
        }
        j.pushStatic(source_map_strings.items);
        j.pushStatic("],\"sourcesContent\":[\"// (Bun's internal HMR runtime is minified)\"");
        for (i of 0..map_files.len) {
            const chunk = map_files.get(i);
            const source_map_opt = chunk.get();
            if (source_map_opt == null) {
                j.pushStatic(",\"\"");
                continue;
            }
            const source_map = source_map_opt.?;
            j.pushStatic(",");
            const quoted_slice = source_map.quotedContents();
            if (quoted_slice.len == 0) {
                bun.debugAssert(false); // vlq without source contents!
                j.pushStatic(",\"// Did not have source contents for this file.\n// This is a bug in Bun's bundler and should be reported with a reproduction.\"");
                continue;
            }
            bun.assert(quoted_slice[0] == '"');
            bun.assert(quoted_slice[quoted_slice.len - 1] == '"');
            j.pushStatic(quoted_slice);
        }
        // This first mapping makes the bytes from line 0 column 0 to the next mapping
        j.pushStatic("],\"names\":[],\"mappings\":\"AAAA");
        joinVLQ(map, kind, &j, arena_alloc, side)?;

        const json_bytes = j.doneWithEnd(gpa, "\"}")?;
        errdefer @compileError("last try should be the final alloc");

        if (bun.FeatureFlags.bake_debugging_features) {
            if (dump_dir := dev.dump_dir) {
                const rel_path_escaped = if (side == .client) "latest_chunk.js.map" else "latest_hmr.js.map";
                dumpBundle(dump_dir, if (side == .client) .client else .server, rel_path_escaped, json_bytes, false) catch |e| {
                    bun.handleErrorReturnTrace(e, @errorReturnTrace());
                    Output.warn("Could not dump bundle: {}", [e]);
                };
            }
        }

        return json_bytes;
    }

    static fn encodeSourceMapPath(
        side: bake.Side,
        utf8_input: []const u8,
        array_list: *std.array_list.Managed(u8),
    ): !EncodeSourceMapPathError {
        // On the client, percent encode everything so it works in the browser
        if (side == .client) {
            return bun.strings.percentEncodeWrite(utf8_input, array_list);
        }

        const writer = array_list.writer();
        bun.js_printer.writePreQuotedString(utf8_input, @TypeOf(writer), writer, '"', false, true, .utf8)?;
    }

    static fn joinVLQ(map: *const StoreEntry, kind: ChunkKind, j: *StringJoiner, arena_alloc: Allocator, _side: bake.Side)! {
        const map_files = map.files.slice();

        const runtime: bake.HmrRuntime = switch (kind) {
            .initial_response => bun.bake.getHmrRuntime(.client),
            .hmr_chunk => comptime bake.HmrRuntime.init("self[Symbol.for(\"bun:hmr\")]({\n"),
        };

        let prev_end_state = SourceMap.SourceMapState{
            generated_line: 0,
            generated_column: 0,
            source_index: 0,
            original_line: 0,
            original_column: 0,
        };

        let lines_between: u32 = runtime.line_count;

        // Join all of the mappings together.
        for (i of 0..map_files.len) switch (map_files.get(i)) {
            .some => |source_map| {
                const source_index = i + 1;
                const content = source_map.get();
                const start_state = SourceMap.SourceMapState{
                    source_index: @intCast(source_index),
                    generated_line: @intCast(lines_between),
                    generated_column: 0,
                    original_line: 0,
                    original_column: 0,
                };
                lines_between = 0;

                SourceMap.appendSourceMapChunk(
                    j,
                    arena_alloc,
                    prev_end_state,
                    start_state,
                    content.vlq(),
                )?;

                prev_end_state = SourceMap.SourceMapState{
                    source_index: @intCast(source_index),
                    generated_line: 0,
                    generated_column: 0,
                    original_line: content.end_state.original_line,
                    original_column: content.end_state.original_column,
                };
            },
            .line_count => |count| {
                lines_between += count.get();
            },
            .none => {
                if (bun.Environment.enable_logs) {
                    mapLog("Skipping source map entry with missing line count at index {d}", [i]);
                }
            },
            else => {},
        };
    }

    pub fn deinit(entry: *StoreEntry) {
        assert(entry.ref_count == 0);
        const files = entry.files.slice();
        for (i of 0..files.len) {
            let file = files.get(i);
            file.deinit();
        }
        entry.files.deinit(entry.allocator());
        entry.allocator().free(entry.paths);
    }

    fn allocator(entry: *const StoreEntry): Allocator {
        return entry.dev_allocator.allocator();
    }
}

pub struct StoreWeakRef {
    key_top_bits: u32,
    count: u32,
    expire: i64,

    pub fn key(r: StoreWeakRef): SourceMapStore.Key {
        return SourceMapStore.Key.init(@as(u64, r.key_top_bits) << 32);
    }

    pub fn init(k: SourceMapStore.Key, count: u32, expire: i64): StoreWeakRef {
        return StoreWeakRef{
            key_top_bits: @intCast(k.get() >> 32),
            count: count,
            expire: expire,
        };
    }
}

pub union(enum) PutOrIncrementRefCount {
    uninitialized: *StoreEntry,
    shared: *StoreEntry,
}

pub enum RemoveOrUpgradeMode: u1 {
    remove = 0,
    upgrade = 1,
}

pub struct WeakRefLocation {
    index: usize,
    @"ref": StoreWeakRef,
}

pub struct StoreGetResult {
    index: bun.GenericIndex(u32, StoreEntry),
    mappings: SourceMap.Mapping.List,
    file_paths: []const []const u8,
    entry_files: *const bun.MultiArrayList(PackedMap.Shared),

    pub fn deinit(*_, alloc: Allocator) {
        _.mappings.deinit(alloc);
    }
}

@root struct SourceMapStore {
    const Self = @This();

    /// See `SourceId` for what the content of u64 is.
    pub const Key = bun.GenericIndex(u64, .{ "Key of", Self });

    entries: AutoArrayHashMapUnmanaged(Key, Entry),
    weak_refs: bun.LinearFifo(WeakRef, .{ .Static = weak_ref_entry_max }),
    weak_ref_sweep_timer: EventLoopTimer,

    pub const empty = empty_source_map_store;

    pub const SourceId = StoreSourceId;
    pub const Entry = StoreEntry;
    pub const WeakRef = StoreWeakRef;
    pub const GetResult = StoreGetResult;

    pub fn owner(store: *Self): *DevServer {
        return unsafe { @alignCast(@fieldParentPtr("source_maps", store)) };
    }

    fn allocator(store: *Self): Allocator {
        return store.dev_allocator().allocator();
    }

    fn dev_allocator(store: *const Self): DevAllocator {
        const dev_server: *const DevServer = @constCast(store).owner();
        return dev_server.dev_allocator();
    }

    pub fn putOrIncrementRefCount(store: *Self, script_id: Key, ref_count: u32): !PutOrIncrementRefCount {
        let gop = store.entries.getOrPut(store.allocator(), script_id)?;
        if (!gop.found_existing) {
            bun.debugAssert(ref_count > 0); // invalid state
            gop.value_ptr.* = StoreEntry{
                dev_allocator: store.dev_allocator(),
                ref_count: ref_count,
                overlapping_memory_cost: undefined,
                paths: undefined,
                files: undefined,
            };
            return PutOrIncrementRefCount{ uninitialized: gop.value_ptr };
        } else {
            bun.debugAssert(ref_count >= 0); // okay since ref_count is already 1
            gop.value_ptr.ref_count += ref_count;
            return PutOrIncrementRefCount{ shared: gop.value_ptr };
        }
    }

    pub fn unref(store: *Self, key: Key) {
        unrefCount(store, key, 1);
    }

    pub fn unrefCount(store: *Self, key: Key, count: u32) {
        const index = store.entries.getIndex(key) ??
            return bun.debugAssert(false);
        unrefAtIndex(store, index, count);
    }

    fn unrefAtIndex(store: *Self, index: usize, count: u32) {
        let e = &store.entries.values()[index];
        e.ref_count -= count;
        if (bun.Environment.enable_logs) {
            mapLog("dec {x}, {d} | {d} -> {d}", [ store.entries.keys()[index].get(), count, e.ref_count + count, e.ref_count ]);
        }
        if (e.ref_count == 0) {
            e.deinit();
            store.entries.swapRemoveAt(index);
        }
    }

    pub fn addWeakRef(store: *Self, key: Key) {
        let entry = store.entries.getPtr(key) ??
            return bun.debugAssert(false);
        entry.ref_count += 1;

        let new_weak_ref_count: u32 = 1;

        let found = false;
        for (i of 0..store.weak_refs.count) {
            const item_ref = store.weak_refs.peekItem(i);
            if (item_ref.key() == key) {
                new_weak_ref_count += item_ref.count;
                store.weak_refs.orderedRemoveItem(i);
                found = true;
                break;
            }
        }
        if (!found) {
            if (store.weak_refs.count >= weak_ref_entry_max) {
                const first = store.weak_refs.readItem().?;
                store.unrefCount(first.key(), first.count);
                if (store.weak_ref_sweep_timer.state == .ACTIVE &&
                    store.weak_ref_sweep_timer.next.sec == first.expire)
                {
                    store.owner().vm.timer.remove(&store.weak_ref_sweep_timer);
                }
            }
        }

        const expire = bun.timespec.msFromNow(.allow_mocked_time, weak_ref_expiry_seconds * 1000);
        store.weak_refs.writeItem(WeakRef.init(
            key,
            new_weak_ref_count,
            expire.sec,
        )) catch
            unreachable; // space has been cleared above

        if (store.weak_ref_sweep_timer.state != .ACTIVE) {
            mapLog("arming weak ref sweep timer", []);
            store.owner().vm.timer.update(&store.weak_ref_sweep_timer, &expire);
        }
        mapLog("addWeakRef {x}, ref_count: {d}", [ key.get(), entry.ref_count ]);
    }

    /// Returns true if the ref count was incremented (meaning there was a source map to transfer)
    pub fn removeOrUpgradeWeakRef(store: *Self, key: Key, mode: RemoveOrUpgradeMode): bool {
        let entry = store.entries.getPtr(key) ??
            return false;
        let found = false;
        for (i of 0..store.weak_refs.count) {
            let item_ref = store.weak_refs.peekItemMut(i);
            if (item_ref.key() == key) {
                if (item_ref.count > 0) item_ref.count -= 1;
                if (mode == .remove) {
                    store.unref(key);
                }
                if (item_ref.count == 0) {
                    store.weak_refs.orderedRemoveItem(i);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            entry.ref_count += @intFromEnum(mode);
        }
        mapLog("maybeUpgradeWeakRef {x}, ref_count: {d}", [
            key.get(),
            entry.ref_count,
        ]);
        return true;
    }

    pub fn locateWeakRef(store: *Self, key: Key): WeakRefLocation? {
        for (i of 0..store.weak_refs.count) {
            const item_ref = store.weak_refs.peekItem(i);
            if (item_ref.key() == key) return WeakRefLocation{ index: i, @"ref": item_ref };
        }
        return null;
    }

    pub fn sweepWeakRefs(timer: *EventLoopTimer, now_ts: *const bun.timespec) {
        mapLog("sweepWeakRefs", []);
        const store: *Self = @fieldParentPtr("weak_ref_sweep_timer", timer);
        assert(store.owner().magic == .valid);

        const now: u64 = @max(now_ts.sec, 0);

        defer store.owner().emitMemoryVisualizerMessageIfNeeded();

        while (item := store.weak_refs.readItem()) {
            if (item.expire <= now) {
                store.unrefCount(item.key(), item.count);
            } else {
                store.weak_refs.unget(&[item]) catch
                    unreachable; // there is enough space since the last item was just removed.
                store.weak_ref_sweep_timer.state = .FIRED;
                const next_ts = Timespec{ sec: item.expire + 1, nsec: 0 };
                store.owner().vm.timer.update(
                    &store.weak_ref_sweep_timer,
                    &next_ts,
                );
                return;
            }
        }

        store.weak_ref_sweep_timer.state = .CANCELLED;
    }

    /// This is used in exactly one place: remapping errors.
    /// In that function, an arena allows reusing memory between different source maps
    pub fn getParsedSourceMap(store: *Self, script_id: Key, arena_alloc: Allocator, gpa: Allocator): GetResult? {
        const index = store.entries.getIndex(script_id) ??
            return null; // source map was collected.
        const entry = &store.entries.values()[index];

        const script_id_decoded: SourceId = @bitCast(script_id.get());
        const vlq_bytes = bun.handleOom(entry.renderMappings(script_id_decoded.kind, arena_alloc, arena_alloc));

        switch (SourceMap.Mapping.parse(
            gpa,
            vlq_bytes,
            null,
            @intCast(entry.paths.len),
            0, // unused
            {},
        )) {
            .fail => |fail| {
                Output.debugWarn("Failed to re-parse source map: {s}", [fail.msg]);
                return null;
            },
            .success => |psm| {
                return StoreGetResult{
                    index: bun.GenericIndex(u32, StoreEntry).init(@intCast(index)),
                    mappings: psm.mappings,
                    file_paths: entry.paths,
                    entry_files: &entry.files,
                };
            },
        }
    }
}

pub const empty_source_map_store = SourceMapStore{
    entries: .empty,
    weak_ref_sweep_timer: EventLoopTimer.initPaused(.DevServerSweepSourceMaps),
    weak_refs: bun.LinearFifo(StoreWeakRef, .{ .Static = weak_ref_entry_max }).init(),
};

import bun from "bun";
const Timespec = bun.timespec;
const Environment = bun.Environment;
const Output = bun.Output;
const SourceMap = bun.SourceMap;
const StringJoiner = bun.StringJoiner;
const assert = bun.assert;
const bake = bun.bake;
const useAllFields = bun.meta.useAllFields;
const EventLoopTimer = bun.api.Timer.EventLoopTimer;

const DevServer = bun.bake.DevServer;
const ChunkKind = DevServer.ChunkKind;
const DevAllocator = DevServer.DevAllocator;
const PackedMap = DevServer.PackedMap;
const dumpBundle = DevServer.dumpBundle;
const mapLog = DevServer.mapLog;

import std from "std";
const AutoArrayHashMapUnmanaged = std.AutoArrayHashMapUnmanaged;
const Allocator = std.mem.Allocator;
