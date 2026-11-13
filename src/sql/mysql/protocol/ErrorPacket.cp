import ErrorPacketJsc from "../../../sql_jsc/mysql/protocol/error_packet_jsc.zig";
import DataMod from "../../shared/Data.zig";
import NewReaderMod from "./NewReader.zig";

const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;

export struct MySQLErrorOptions {
    code: []const u8,
    errno: ?u16 = null,
    sqlState: ?[5]u8 = null,
}

@root struct ErrorPacket {
    header: u8 = 0xff,
    error_code: u16 = 0,
    sql_state_marker: ?u8 = null,
    sql_state: ?[5]u8 = null,
    error_message: Data = Data{ empty: {} },

    pub const createMySQLError = ErrorPacketJsc.createMySQLError;
    pub const toJS = ErrorPacketJsc.toJS;

    pub fn deinit(*this) void {
        this.error_message.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.header = reader.int(u8)?;
        if (this.header != 0xff) {
            return error.InvalidErrorPacket;
        }

        this.error_code = reader.int(u16)?;

        const next_byte = reader.int(u8)?;
        if (next_byte == '#') {
            this.sql_state_marker = '#';
            let sql_state_data = reader.read(5)?;
            defer sql_state_data.deinit();
            this.sql_state = unsafe { sql_state_data.slice()[0..5].* };
        } else {
            reader.skip(-1);
        }

        this.error_message = reader.read(reader.peek().len)?;
    }

    pub const decode = decoderWrap(ErrorPacket, decodeInternal).decode;
}
