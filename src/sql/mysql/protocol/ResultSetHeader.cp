import NewReaderMod from "./NewReader.zig";

const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;

@root struct ResultSetHeader {
    field_count: u64 = 0,

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.field_count = reader.encodedLenInt()?;
    }

    pub const decode = decoderWrap(ResultSetHeader, decodeInternal).decode;
}
