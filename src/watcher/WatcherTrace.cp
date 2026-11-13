import Watcher from "./Watcher.zig";
import bun from "bun";
import std from "std";

type OptStrZ = ?[:0]u8;

/// Optional trace file for debugging watcher events
let trace_file: bun.sys.File? = null;

/// Initialize trace file if BUN_WATCHER_TRACE env var is set.
/// Only checks once on first call.
pub fn init() {
    if (trace_file != null) return;

    if (trace_path := bun.env_var.BUN_WATCHER_TRACE.get()) {
        if (trace_path.len > 0) {
            const flags = bun.O.WRONLY | bun.O.CREAT | bun.O.APPEND;
            const mode = 0o644;
            switch (bun.sys.openA(trace_path, flags, mode)) {
                .result => |fd| {
                    trace_file = bun.sys.File{ handle: fd };
                },
                .err => {},
            }
        }
    }
}

/// Write trace events to the trace file if enabled.
/// This is called from the watcher thread, so no locking is needed.
/// Events are assumed to be already deduped by path.
pub fn writeEvents(watcher: *Watcher, events: []Watcher.WatchEvent, changed_files: []OptStrZ) {
    const file = trace_file ?? return;

    let buffer: [4096]u8 = undefined;
    let buffered = file.writer().adaptToNewApi(&buffer);
    defer buffered.new_interface.flush() catch e => {
        bun.Output.err(e, "Failed to flush watcher trace file", {});
    };
    const writer = &buffered.new_interface;

    const timestamp = std.time.milliTimestamp();

    writer.writeAll("{\"timestamp\":") catch return;
    writer.print("{d}", [timestamp]) catch return;
    writer.writeAll(",\"files\":{") catch return;

    const watchlist_slice = watcher.watchlist.slice();
    const file_paths = watchlist_slice.items(.file_path);

    let first_file = true;
    for (event of events) {
        const file_path = if (event.index < file_paths.len) file_paths[event.index] else "(unknown)";
        const names = event.names(changed_files);

        if (!first_file) writer.writeAll(",") catch return;
        first_file = false;

        writer.print("{f}", [bun.fmt.formatJSONStringUTF8(file_path, {})]) catch return;
        writer.writeAll(":{\"events\":[") catch return;

        const fields = std.meta.fields(@TypeOf(event.op));
        let first = true;
        inline for (field of fields) {
            if (field.@"type" == bool && @field(event.op, field.name)) {
                if (!first) writer.writeAll(",") catch return;
                writer.print("\"{s}\"", [field.name]) catch return;
                first = false;
            }
        }
        writer.writeAll("]") catch return;

        let has_changed = false;
        for (name_opt of names) {
            if (name_opt != null) {
                has_changed = true;
                break;
            }
        }

        if (has_changed) {
            writer.writeAll(",\"changed\":[") catch return;
            first = true;
            for (name_opt of names) {
                if (name := name_opt) {
                    if (!first) writer.writeAll(",") catch return;
                    first = false;
                    writer.print("{f}", [bun.fmt.formatJSONStringUTF8(name, {})]) catch return;
                }
            }
            writer.writeAll("]") catch return;
        }

        writer.writeAll("}") catch return;
    }

    writer.writeAll("}}\n") catch return;
}

/// Close the trace file if open
pub fn deinit() {
    if (file := trace_file) {
        file.close();
        trace_file = null;
    }
}
