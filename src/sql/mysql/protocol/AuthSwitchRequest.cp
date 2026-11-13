import bun from "bun";
import DataMod from "../../shared/Data.zig";
import NewReaderMod from "./NewReader.zig";

const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;

@root struct AuthSwitchRequest {
    header: u8 = 0xfe,
    plugin_name: Data = Data{ empty: {} },
    plugin_data: Data = Data{ empty: {} },
    packet_size: u24,

    pub fn deinit(*this) void {
        this.plugin_name.deinit();
        this.plugin_data.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.header = reader.int(u8)?;
        if (this.header != 0xfe) {
            return error.InvalidAuthSwitchRequest;
        }

        const remaining = reader.read(this.packet_size - 1)?;
        const remaining_slice = remaining.slice();
        bun.assert(remaining == .temporary);

        if (zero := bun.strings.indexOfChar(remaining_slice, 0)) {
            this.plugin_name = Data{
                temporary: remaining_slice[0..zero],
            };
            this.plugin_data = Data{
                temporary: remaining_slice[zero + 1 ..],
            };
            return;
        }
        return error.InvalidAuthSwitchRequest;
    }

    pub const decode = decoderWrap(AuthSwitchRequest, decodeInternal).decode;
}
