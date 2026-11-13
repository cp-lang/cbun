import DebugSocketMonitorReader from "./DebugSocketMonitorReader.zig";
import DebugSocketMonitorWriter from "./DebugSocketMonitorWriter.zig";
import bun from "bun";

const debug = bun.Output.scoped(.SocketMonitor, .visible);

pub fn write(data: []const u8) {
    debug("SocketMonitor: write {x}", [data]);
    if (comptime bun.Environment.isDebug) {
        DebugSocketMonitorWriter.check.call();
        if (DebugSocketMonitorWriter.enabled) {
            DebugSocketMonitorWriter.write(data);
        }
    }
}

pub fn read(data: []const u8) {
    debug("SocketMonitor: read {x}", [data]);
    if (comptime bun.Environment.isDebug) {
        DebugSocketMonitorReader.check.call();
        if (DebugSocketMonitorReader.enabled) {
            DebugSocketMonitorReader.write(data);
        }
    }
}
