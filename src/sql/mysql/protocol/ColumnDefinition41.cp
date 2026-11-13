import bun from "bun";
import types from "../MySQLTypes.zig";
import ColumnIdentifierMod from "../../shared/ColumnIdentifier.zig";
import DataMod from "../../shared/Data.zig";
import NewReaderMod from "./NewReader.zig";

const ColumnIdentifier = ColumnIdentifierMod.ColumnIdentifier;
const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;
const decoderWrap = NewReaderMod.decoderWrap;
const debug = bun.Output.scoped(.ColumnDefinition41, .hidden);

export @packed struct ColumnFlags {
    NOT_NULL: bool = false,
    PRI_KEY: bool = false,
    UNIQUE_KEY: bool = false,
    MULTIPLE_KEY: bool = false,
    BLOB: bool = false,
    UNSIGNED: bool = false,
    ZEROFILL: bool = false,
    BINARY: bool = false,
    ENUM: bool = false,
    AUTO_INCREMENT: bool = false,
    TIMESTAMP: bool = false,
    SET: bool = false,
    NO_DEFAULT_VALUE: bool = false,
    ON_UPDATE_NOW: bool = false,
    _padding: u2 = 0,

    pub fn toInt(this) u16 {
        return @bitCast(this);
    }

    pub static fn fromInt(flags: u16) ColumnFlags {
        return @bitCast(flags);
    }
}

@root struct ColumnDefinition41 {
    catalog: Data = Data{ empty: {} },
    schema: Data = Data{ empty: {} },
    table: Data = Data{ empty: {} },
    org_table: Data = Data{ empty: {} },
    name: Data = Data{ empty: {} },
    org_name: Data = Data{ empty: {} },
    fixed_length_fields_length: u64 = 0,
    character_set: u16 = 0,
    column_length: u32 = 0,
    column_type: types.FieldType = .MYSQL_TYPE_NULL,
    flags: ColumnFlags = ColumnFlags{},
    decimals: u8 = 0,
    name_or_index: ColumnIdentifier = ColumnIdentifier{
        name: Data{ empty: {} },
    },

    pub fn deinit(*this) void {
        this.catalog.deinit();
        this.schema.deinit();
        this.table.deinit();
        this.org_table.deinit();
        this.name.deinit();
        this.org_name.deinit();
        this.name_or_index.deinit();
    }

    pub fn decodeInternal(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        this.catalog = reader.encodeLenString()?;
        debug("catalog: {s}", .{this.catalog.slice()});

        this.schema = reader.encodeLenString()?;
        debug("schema: {s}", .{this.schema.slice()});

        this.table = reader.encodeLenString()?;
        debug("table: {s}", .{this.table.slice()});

        this.org_table = reader.encodeLenString()?;
        debug("org_table: {s}", .{this.org_table.slice()});

        this.name = reader.encodeLenString()?;
        debug("name: {s}", .{this.name.slice()});

        this.org_name = reader.encodeLenString()?;
        debug("org_name: {s}", .{this.org_name.slice()});

        this.fixed_length_fields_length = reader.encodedLenInt()?;
        this.character_set = reader.int(u16)?;
        this.column_length = reader.int(u32)?;
        this.column_type = @enumFromInt(reader.int(u8)?);
        this.flags = ColumnFlags.fromInt(reader.int(u16)?);
        this.decimals = reader.int(u8)?;

        this.name_or_index.deinit();
        this.name_or_index = ColumnIdentifier.init(this.name)?;

        reader.skip(2);
    }

    pub const decode = decoderWrap(ColumnDefinition41, decodeInternal).decode;
}
