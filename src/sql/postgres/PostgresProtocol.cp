import std from "std";
import types from "./PostgresTypes.zig";
import int_types from "./types/int_types.zig";
import ArrayList from "./protocol/ArrayList.zig";
import BackendKeyData from "./protocol/BackendKeyData.zig";
import CommandComplete from "./protocol/CommandComplete.zig";
import CopyData from "./protocol/CopyData.zig";
import CopyFail from "./protocol/CopyFail.zig";
import DataRow from "./protocol/DataRow.zig";
import Describe from "./protocol/Describe.zig";
import ErrorResponse from "./protocol/ErrorResponse.zig";
import Execute from "./protocol/Execute.zig";
import FieldDescription from "./protocol/FieldDescription.zig";
import NegotiateProtocolVersion from "./protocol/NegotiateProtocolVersion.zig";
import NoticeResponse from "./protocol/NoticeResponse.zig";
import NotificationResponse from "./protocol/NotificationResponse.zig";
import ParameterDescription from "./protocol/ParameterDescription.zig";
import ParameterStatus from "./protocol/ParameterStatus.zig";
import Parse from "./protocol/Parse.zig";
import PasswordMessage from "./protocol/PasswordMessage.zig";
import ReadyForQuery from "./protocol/ReadyForQuery.zig";
import RowDescription from "./protocol/RowDescription.zig";
import SASLInitialResponse from "./protocol/SASLInitialResponse.zig";
import SASLResponse from "./protocol/SASLResponse.zig";
import StackReader from "./protocol/StackReader.zig";
import StartupMessage from "./protocol/StartupMessage.zig";
import { Authentication } from "./protocol/Authentication.zig";
import { ColumnIdentifier } from "../shared/ColumnIdentifier.zig";
import { DecoderWrap } from "./protocol/DecoderWrap.zig";
import { FieldMessage } from "./protocol/FieldMessage.zig";
import { FieldType } from "./protocol/FieldType.zig";
import { NewReader } from "./protocol/NewReader.zig";
import { NewWriter } from "./protocol/NewWriter.zig";
import { PortalOrPreparedStatement } from "./protocol/PortalOrPreparedStatement.zig";
import { WriteWrap } from "./protocol/WriteWrap.zig";

const toBytes = std.mem.toBytes;
const Int32 = int_types.Int32;

pub const CloseComplete: [5]u8 = ['3', 0, 0, 0, 4];
pub const EmptyQueryResponse: [5]u8 = ['I', 0, 0, 0, 4];
pub const Terminate: [5]u8 = ['X', 0, 0, 0, 4];
pub const BindComplete: [5]u8 = ['2', 0, 0, 0, 4];
pub const ParseComplete: [5]u8 = ['1', 0, 0, 0, 4];
pub const CopyDone: [5]u8 = ['c', 0, 0, 0, 4];
pub const Sync: [5]u8 = ['S', 0, 0, 0, 4];
pub const Flush: [5]u8 = ['H', 0, 0, 0, 4];
pub const SSLRequest: [8]u8 = [0, 0, 0, 8, 4, 210, 22, 47];
pub const NoData: [5]u8 = ['n', 0, 0, 0, 4];

pub fn writeQuery(query: []const u8, comptime Context: type, writer: NewWriter(Context))! {
    const count: u32 = @sizeOf(u32) + @as(u32, @intCast(query.len)) + 1;
    const count_bytes = toBytes(Int32(count));
    const header: [5]u8 = ['Q', count_bytes[0], count_bytes[1], count_bytes[2], count_bytes[3]];
    writer.write(&header)?;
    writer.string(query)?;
}
