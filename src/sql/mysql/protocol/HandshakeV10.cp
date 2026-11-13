import bun from "bun";
import Capabilities from "../Capabilities.zig";
import CharacterSetMod from "./CharacterSet.zig";
import DataMod from "../../shared/Data.zig";
import StatusFlagsMod from "../StatusFlags.zig";
import NewReaderMod from "./NewReader.zig";

const CharacterSet = CharacterSetMod.CharacterSet;
const Data = DataMod.Data;
const StatusFlags = StatusFlagsMod.StatusFlags;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;
const empty_u8: [0]u8 = [];

@root struct HandshakeV10 {
    protocol_version: u8 = 10,
    server_version: Data = Data{ empty: {} },
    connection_id: u32 = 0,
    auth_plugin_data_part_1: [8]u8 = undefined,
    auth_plugin_data_part_2: []const u8 = empty_u8[0..0],
    capability_flags: Capabilities = Capabilities{},
    character_set: CharacterSet = CharacterSet.default,
    status_flags: StatusFlags = StatusFlags{},
    auth_plugin_name: Data = Data{ empty: {} },

    pub fn deinit(*this) void {
        this.server_version.deinit();
        this.auth_plugin_name.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.protocol_version = reader.int(u8)?;
        if (this.protocol_version != 10) {
            return error.UnsupportedProtocolVersion;
        }

        this.server_version = reader.readZ()?;
        this.connection_id = reader.int(u32)?;

        let auth_data = reader.read(8)?;
        defer auth_data.deinit();
        @memcpy(&this.auth_plugin_data_part_1, auth_data.slice());

        _ = reader.int(u8)?;

        const capabilities_lower = reader.int(u16)?;
        this.character_set = @enumFromInt(reader.int(u8)?);
        this.status_flags = StatusFlags.fromInt(reader.int(u16)?);

        const capabilities_upper = reader.int(u16)?;
        this.capability_flags = Capabilities.fromInt(@as(u32, capabilities_upper) << 16 | capabilities_lower);

        let auth_plugin_data_len = reader.int(u8)?;
        if (auth_plugin_data_len < 21) {
            auth_plugin_data_len = 21;
        }

        reader.skip(10);

        const remaining_auth_len = @max(13, auth_plugin_data_len - 8);
        let auth_data_2 = reader.read(remaining_auth_len)?;
        defer auth_data_2.deinit();
        this.auth_plugin_data_part_2 = bun.default_allocator.dupe(u8, auth_data_2.slice())?;

        if (this.capability_flags.CLIENT_PLUGIN_AUTH) {
            this.auth_plugin_name = reader.readZ()?;
        }
    }

    pub const decode = decoderWrap(HandshakeV10, decodeInternal).decode;
}
