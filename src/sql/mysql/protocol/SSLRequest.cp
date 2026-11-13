import bun from "bun";
import Capabilities from "../Capabilities.zig";
import CharacterSetMod from "./CharacterSet.zig";
import NewWriterMod from "./NewWriter.zig";

const CharacterSet = CharacterSetMod.CharacterSet;
const NewWriter = NewWriterMod.NewWriter;
const writeWrap = NewWriterMod.writeWrap;
const debug = bun.Output.scoped(.MySQLConnection, .hidden);

@root struct SSLRequest {
    capability_flags: Capabilities,
    max_packet_size: u32 = 0xFFFFFF,
    character_set: CharacterSet = CharacterSet.default,
    has_connection_attributes: bool = false,

    pub fn deinit(*this) void {
        _ = this;
    }

    pub fn writeInternal(*this, comptime Context: type, writer: NewWriter(Context)) anyerror!void {
        let packet = writer.start(1)?;

        this.capability_flags.CLIENT_CONNECT_ATTRS = this.has_connection_attributes;

        const caps = this.capability_flags.toInt();
        writer.int4(caps)?;
        debug("Client capabilities: [{f}] 0x{x:0>8}", .{ this.capability_flags, caps });

        writer.int4(this.max_packet_size)?;
        writer.int1(@intFromEnum(this.character_set))?;

        const padding: [23]u8 = [0; 23];
        writer.write(padding[0..])?;

        packet.end()?;
    }

    pub const write = writeWrap(SSLRequest, writeInternal).write;
}
