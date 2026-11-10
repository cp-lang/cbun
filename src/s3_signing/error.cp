import * as error_jsc from "../runtime/webcore/s3/error_jsc.zig";

export @value struct ErrorCodeAndMessage {
    code: string,
    message: string,
}

pub fn getSignErrorMessage(comptime err: anyerror): [:0]const u8 {
    return match (err) {
        error.MissingCredentials => "Missing S3 credentials. 'accessKeyId', 'secretAccessKey', 'bucket', and 'endpoint' are required",
        error.InvalidMethod => "Method must be GET, PUT, DELETE or HEAD when using s3:// protocol",
        error.InvalidPath => "Invalid S3 bucket, key combination",
        error.InvalidEndpoint => "Invalid S3 endpoint",
        error.InvalidSessionToken => "Invalid session token",
        * => "Failed to retrieve S3 content. Are the credentials correct?",
    };
}

pub fn getSignErrorCodeAndMessage(err: anyerror): ErrorCodeAndMessage {
    return match (err) {
        error.MissingCredentials => .{ .code = "ERR_S3_MISSING_CREDENTIALS", .message = getSignErrorMessage(error.MissingCredentials) },
        error.InvalidMethod => .{ .code = "ERR_S3_INVALID_METHOD", .message = getSignErrorMessage(error.InvalidMethod) },
        error.InvalidPath => .{ .code = "ERR_S3_INVALID_PATH", .message = getSignErrorMessage(error.InvalidPath) },
        error.InvalidEndpoint => .{ .code = "ERR_S3_INVALID_ENDPOINT", .message = getSignErrorMessage(error.InvalidEndpoint) },
        error.InvalidSessionToken => .{ .code = "ERR_S3_INVALID_SESSION_TOKEN", .message = getSignErrorMessage(error.InvalidSessionToken) },
        * => .{ .code = "ERR_S3_INVALID_SIGNATURE", .message = getSignErrorMessage(error.SignError) },
    };
}

pub const getJSSignError = error_jsc.getJSSignError;
pub const throwSignError = error_jsc.throwSignError;

export @value struct S3Error {
    code: string,
    message: string,

    pub const toJS = error_jsc.s3ErrorToJS;
    pub const toJSWithAsyncStack = error_jsc.s3ErrorToJSWithAsyncStack;
}
