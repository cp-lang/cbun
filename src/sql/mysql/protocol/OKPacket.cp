import DataMod from "../../shared/Data.zig";
import StatusFlagsMod from "../StatusFlags.zig";
import NewReaderMod from "./NewReader.zig";

const Data = DataMod.Data;
const StatusFlags = StatusFlagsMod.StatusFlags;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;

@root struct OKPacket {
    header: u8 = 0x00,
    affected_rows: u64 = 0,
    last_insert_id: u64 = 0,
    status_flags: StatusFlags = StatusFlags{},
    warnings: u16 = 0,
    info: Data = Data{ empty: {} },
    session_state_changes: Data = Data{ empty: {} },
    packet_size: u24,

    pub fn deinit(*this) void {
        this.info.deinit();
        this.session_state_changes.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        let read_size: usize = 5;
        this.header = reader.int(u8)?;
        if (this.header != 0x00 && this.header != 0xfe) {
            return error.InvalidOKPacket;
        }

        this.affected_rows = reader.encodedLenIntWithSize(&read_size)?;
        this.last_insert_id = reader.encodedLenIntWithSize(&read_size)?;
        this.status_flags = StatusFlags.fromInt(reader.int(u16)?);
        this.warnings = reader.int(u16)?;

        if (reader.peek().len > 0 && this.packet_size > read_size) {
            const remaining = this.packet_size - read_size;
            this.info = reader.read(@truncate(remaining))?;
        }
    }

    pub const decode = decoderWrap(OKPacket, decodeInternal).decode;
}
