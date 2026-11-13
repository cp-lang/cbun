import bun from "bun";
import Capabilities from "../Capabilities.zig";
import CharacterSetMod from "./CharacterSet.zig";
import DataMod from "../../shared/Data.zig";
import EncodeIntMod from "./EncodeInt.zig";
import NewWriterMod from "./NewWriter.zig";

const CharacterSet = CharacterSetMod.CharacterSet;
const Data = DataMod.Data;
const encodeLengthInt = EncodeIntMod.encodeLengthInt;
const NewWriter = NewWriterMod.NewWriter;
const writeWrap = NewWriterMod.writeWrap;
const AttrMap = bun.StringHashMapUnmanaged(string);
const debug = bun.Output.scoped(.MySQLConnection, .hidden);

@root struct HandshakeResponse41 {
    capability_flags: Capabilities,
    max_packet_size: u32 = 0xFFFFFF,
    character_set: CharacterSet = CharacterSet.default,
    username: Data,
    auth_response: Data,
    database: Data,
    auth_plugin_name: Data,
    connect_attrs: AttrMap = AttrMap{},
    sequence_id: u8,

    pub fn deinit(*this) void {
        this.username.deinit();
        this.auth_response.deinit();
        this.database.deinit();
        this.auth_plugin_name.deinit();

        let it = this.connect_attrs.iterator();
        while (entry := it.next()) {
            unsafe {
                bun.default_allocator.free(entry.key_ptr.*);
                bun.default_allocator.free(entry.value_ptr.*);
            }
        }
        this.connect_attrs.deinit(bun.default_allocator);
    }

    pub fn writeInternal(*this, comptime Context: type, writer: NewWriter(Context)) anyerror!void {
        let packet = writer.start(this.sequence_id)?;

        this.capability_flags.CLIENT_CONNECT_ATTRS = this.connect_attrs.count() > 0;

        const caps = this.capability_flags.toInt();
        writer.int4(caps)?;
        debug("Client capabilities: [{f}] 0x{x:0>8} sequence_id: {d}", .{ this.capability_flags, caps, this.sequence_id });

        writer.int4(this.max_packet_size)?;
        writer.int1(@intFromEnum(this.character_set))?;

        const padding: [23]u8 = [0; 23];
        writer.write(padding[0..])?;

        writer.writeZ(this.username.slice())?;

        const auth_data = this.auth_response.slice();
        if (this.capability_flags.CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
            writer.writeLengthEncodedString(auth_data)?;
        } else if (this.capability_flags.CLIENT_SECURE_CONNECTION) {
            writer.int1(@intCast(auth_data.len))?;
            writer.write(auth_data)?;
        } else {
            writer.writeZ(auth_data)?;
        }

        if (this.capability_flags.CLIENT_CONNECT_WITH_DB && this.database.slice().len > 0) {
            writer.writeZ(this.database.slice())?;
        }

        if (this.capability_flags.CLIENT_PLUGIN_AUTH) {
            writer.writeZ(this.auth_plugin_name.slice())?;
        }

        if (this.capability_flags.CLIENT_CONNECT_ATTRS) {
            let total_length: usize = 0;
            let it = this.connect_attrs.iterator();
            while (entry := it.next()) {
                total_length += encodeLengthInt(entry.key_ptr.len).len;
                total_length += entry.key_ptr.len;
                total_length += encodeLengthInt(entry.value_ptr.len).len;
                total_length += entry.value_ptr.len;
            }

            writer.writeLengthEncodedInt(total_length)?;

            it = this.connect_attrs.iterator();
            while (entry := it.next()) {
                unsafe {
                    writer.writeLengthEncodedString(entry.key_ptr.*)?;
                    writer.writeLengthEncodedString(entry.value_ptr.*)?;
                }
            }
        }

        if (this.capability_flags.CLIENT_ZSTD_COMPRESSION_ALGORITHM) {
            bun.assertf(false, "zstd compression algorithm is not supported", .{});
        }

        packet.end()?;
    }

    pub const write = writeWrap(HandshakeResponse41, writeInternal).write;
}
