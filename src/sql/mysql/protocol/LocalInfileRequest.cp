import DataMod from "../../shared/Data.zig";
import NewReaderMod from "./NewReader.zig";

const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;

@root struct LocalInfileRequest {
    filename: Data = Data{ empty: {} },
    packet_size: u24,

    pub fn deinit(*this) void {
        this.filename.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        const header = reader.int(u8)?;
        if (header != 0xFB) {
            return error.InvalidLocalInfileRequest;
        }

        this.filename = reader.read(this.packet_size - 1)?;
    }

    pub const decode = decoderWrap(LocalInfileRequest, decodeInternal).decode;
}
