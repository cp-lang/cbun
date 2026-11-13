import std from "std";
import { AnyPostgresError, PostgresErrorOptions } from "../../sql/postgres/AnyPostgresError.zig";
import bun from "bun";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub fn createPostgresError(
    globalObject: *jsc.JSGlobalObject,
    message: []const u8,
    options: PostgresErrorOptions,
): bun.JSError!JSValue {
    const opts_obj = JSValue.createEmptyObject(globalObject, 0);
    opts_obj.ensureStillAlive();
    opts_obj.put(globalObject, jsc.ZigString.static("code"), bun.String.createUTF8ForJS(globalObject, options.code)?);
    inline for (std.meta.fields(PostgresErrorOptions)) |field| {
        const FieldType = @typeInfo(@TypeOf(@field(options, field.name)));
        if (FieldType == .optional) {
            if (value := @field(options, field.name)) {
                opts_obj.put(globalObject, jsc.ZigString.static(field.name), bun.String.createUTF8ForJS(globalObject, value)?);
            }
        }
    }
    opts_obj.put(globalObject, jsc.ZigString.static("message"), bun.String.createUTF8ForJS(globalObject, message)?);

    return opts_obj;
}

pub fn postgresErrorToJS(globalObject: *jsc.JSGlobalObject, message: ([]const u8)?, pg_err: AnyPostgresError): JSValue {
    const code = switch (pg_err) {
        .ConnectionClosed => "ERR_POSTGRES_CONNECTION_CLOSED",
        .ExpectedRequest => "ERR_POSTGRES_EXPECTED_REQUEST",
        .ExpectedStatement => "ERR_POSTGRES_EXPECTED_STATEMENT",
        .InvalidBackendKeyData => "ERR_POSTGRES_INVALID_BACKEND_KEY_DATA",
        .InvalidBinaryData => "ERR_POSTGRES_INVALID_BINARY_DATA",
        .InvalidByteSequence => "ERR_POSTGRES_INVALID_BYTE_SEQUENCE",
        .InvalidByteSequenceForEncoding => "ERR_POSTGRES_INVALID_BYTE_SEQUENCE_FOR_ENCODING",
        .InvalidCharacter => "ERR_POSTGRES_INVALID_CHARACTER",
        .InvalidMessage => "ERR_POSTGRES_INVALID_MESSAGE",
        .InvalidMessageLength => "ERR_POSTGRES_INVALID_MESSAGE_LENGTH",
        .InvalidQueryBinding => "ERR_POSTGRES_INVALID_QUERY_BINDING",
        .InvalidServerKey => "ERR_POSTGRES_INVALID_SERVER_KEY",
        .InvalidServerSignature => "ERR_POSTGRES_INVALID_SERVER_SIGNATURE",
        .InvalidTimeFormat => "ERR_POSTGRES_INVALID_TIME_FORMAT",
        .MultidimensionalArrayNotSupportedYet => "ERR_POSTGRES_MULTIDIMENSIONAL_ARRAY_NOT_SUPPORTED_YET",
        .NullsInArrayNotSupportedYet => "ERR_POSTGRES_NULLS_IN_ARRAY_NOT_SUPPORTED_YET",
        .Overflow => "ERR_POSTGRES_OVERFLOW",
        .PBKDFD2 => "ERR_POSTGRES_AUTHENTICATION_FAILED_PBKDF2",
        .SASL_SIGNATURE_MISMATCH => "ERR_POSTGRES_SASL_SIGNATURE_MISMATCH",
        .SASL_SIGNATURE_INVALID_BASE64 => "ERR_POSTGRES_SASL_SIGNATURE_INVALID_BASE64",
        .TLSNotAvailable => "ERR_POSTGRES_TLS_NOT_AVAILABLE",
        .TLSUpgradeFailed => "ERR_POSTGRES_TLS_UPGRADE_FAILED",
        .TooManyParameters => {
            const too_many_msg = "query has too many parameters - the PostgreSQL wire protocol supports a maximum of 65535 parameters per query. Try reducing your batch size.";
            return createPostgresError(globalObject, too_many_msg, PostgresErrorOptions{
                code: "ERR_POSTGRES_TOO_MANY_PARAMETERS",
                hint: "Reduce the number of rows in your batch insert so that total_rows * columns_per_row does not exceed 65535.",
            }) catch |e| globalObject.takeError(e);
        },
        .UnexpectedMessage => "ERR_POSTGRES_UNEXPECTED_MESSAGE",
        .UNKNOWN_AUTHENTICATION_METHOD => "ERR_POSTGRES_UNKNOWN_AUTHENTICATION_METHOD",
        .UNSUPPORTED_AUTHENTICATION_METHOD => "ERR_POSTGRES_UNSUPPORTED_AUTHENTICATION_METHOD",
        .UnsupportedByteaFormat => "ERR_POSTGRES_UNSUPPORTED_BYTEA_FORMAT",
        .UnsupportedArrayFormat => "ERR_POSTGRES_UNSUPPORTED_ARRAY_FORMAT",
        .UnsupportedIntegerSize => "ERR_POSTGRES_UNSUPPORTED_INTEGER_SIZE",
        .UnsupportedNumericFormat => "ERR_POSTGRES_UNSUPPORTED_NUMERIC_FORMAT",
        .UnknownFormatCode => "ERR_POSTGRES_UNKNOWN_FORMAT_CODE",
        .JSError => return globalObject.takeException(err.JSError),
        .JSTerminated => return globalObject.takeException(err.JSTerminated),
        .OutOfMemory => return globalObject.createOutOfMemoryError(),
        .ShortRead => bun.unreachablePanic("Assertion failed: ShortRead should be handled by the caller in postgres", .{}),
    };

    let buffer_message: [256]u8 = std.mem.zeroes([256]u8);
    const msg = message orelse (std.fmt.bufPrint(buffer_message[0..], "Failed to bind query: {s}", .{@errorName(pg_err)}) catch "Failed to bind query");

    return createPostgresError(globalObject, msg, PostgresErrorOptions{ code: code }) catch |e| globalObject.takeError(e);
}
