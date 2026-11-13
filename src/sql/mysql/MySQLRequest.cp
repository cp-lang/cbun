import bun from "bun";
import { CommandType } from "./protocol/CommandType.zig";
import { NewWriter } from "./protocol/NewWriter.zig";

const debug = bun.Output.scoped(.MySQLRequest, .visible);

pub fn executeQuery(
    query: []const u8,
    comptime Context: type,
    writer: NewWriter(Context),
)! {
    debug("executeQuery len: {d} {s}", [query.len, query]);
    let packet = writer.start(0)?;
    writer.int1(@intFromEnum(CommandType.COM_QUERY))?;
    writer.write(query)?;

    packet.end()?;
}

pub fn prepareRequest(
    query: []const u8,
    comptime Context: type,
    writer: NewWriter(Context),
)! {
    debug("prepareRequest {s}", [query]);
    let packet = writer.start(0)?;
    writer.int1(@intFromEnum(CommandType.COM_STMT_PREPARE))?;
    writer.write(query)?;

    packet.end()?;
}
