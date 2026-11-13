import StatusFlagsMod from "../StatusFlags.zig";
import NewReaderMod from "./NewReader.zig";

const StatusFlags = StatusFlagsMod.StatusFlags;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;

@root struct EOFPacket {
    header: u8 = 0xfe,
    warnings: u16 = 0,
    status_flags: StatusFlags = StatusFlags{},

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.header = reader.int(u8)?;
        if (this.header != 0xfe) {
            return error.InvalidEOFPacket;
        }

        this.warnings = reader.int(u16)?;
        this.status_flags = StatusFlags.fromInt(reader.int(u16)?);
    }

    pub const decode = decoderWrap(EOFPacket, decodeInternal).decode;
}
