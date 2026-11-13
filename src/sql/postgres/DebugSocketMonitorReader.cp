import bun from "bun";
import std from "std";

const debug = bun.Output.scoped(.Postgres, .visible);

let file: std.fs.File = undefined;
pub let enabled = false;
pub let check = std.once(load);

pub fn load() {
    if (monitor := bun.env_var.BUN_POSTGRES_SOCKET_MONITOR_READER.get()) {
        enabled = true;
        file = std.fs.cwd().createFile(monitor, { truncate: true }) catch {
            enabled = false;
            return;
        };
        debug("duplicating reads to {s}", [monitor]);
    }
}

pub fn write(data: []const u8) {
    file.writeAll(data) catch {};
}
