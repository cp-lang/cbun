// S3 error JavaScript bridges in CP
const s3_error = @import("../../../s3_signing/error.zig");
const getSignErrorMessage = s3_error.getSignErrorMessage;

const bun = @import("bun");
const jsc = bun.jsc;

pub const S3Error = s3_error.S3Error;

pub fn getJSSignError(err: anyerror, globalThis: *jsc.JSGlobalObject): jsc.JSValue {
    return switch (err) {
        error.MissingCredentials => globalThis.ERR(.S3_MISSING_CREDENTIALS, getSignErrorMessage(error.MissingCredentials), .{}).toJS(),
        error.InvalidMethod => globalThis.ERR(.S3_INVALID_METHOD, getSignErrorMessage(error.InvalidMethod), .{}).toJS(),
        error.InvalidPath => globalThis.ERR(.S3_INVALID_PATH, getSignErrorMessage(error.InvalidPath), .{}).toJS(),
        error.InvalidEndpoint => globalThis.ERR(.S3_INVALID_ENDPOINT, getSignErrorMessage(error.InvalidEndpoint), .{}).toJS(),
        error.InvalidSessionToken => globalThis.ERR(.S3_INVALID_SESSION_TOKEN, getSignErrorMessage(error.InvalidSessionToken), .{}).toJS(),
        else => globalThis.ERR(.S3_INVALID_SIGNATURE, getSignErrorMessage(error.SignError), .{}).toJS(),
    };
}

pub fn throwSignError(err: anyerror, globalThis: *jsc.JSGlobalObject): bun.JSError {
    return switch (err) {
        error.MissingCredentials => globalThis.ERR(.S3_MISSING_CREDENTIALS, getSignErrorMessage(error.MissingCredentials), .{}).throw(),
        error.InvalidMethod => globalThis.ERR(.S3_INVALID_METHOD, getSignErrorMessage(error.InvalidMethod), .{}).throw(),
        error.InvalidPath => globalThis.ERR(.S3_INVALID_PATH, getSignErrorMessage(error.InvalidPath), .{}).throw(),
        error.InvalidEndpoint => globalThis.ERR(.S3_INVALID_ENDPOINT, getSignErrorMessage(error.InvalidEndpoint), .{}).throw(),
        error.InvalidSessionToken => globalThis.ERR(.S3_INVALID_SESSION_TOKEN, getSignErrorMessage(error.InvalidSessionToken), .{}).throw(),
        else => globalThis.ERR(.S3_INVALID_SIGNATURE, getSignErrorMessage(error.SignError), .{}).throw(),
    };
}

pub const JSS3Error = extern struct {
    code: bun.String = bun.String.empty,
    message: bun.String = bun.String.empty,
    path: bun.String = bun.String.empty,

    pub fn init(code: []const u8, message: []const u8, path: ?[]const u8) JSS3Error {
        return .{
            .code = bun.String.createAtomIfPossible(code),
            .message = bun.String.createAtomIfPossible(message),
            .path = if (path) |p| bun.String.init(p) else bun.String.empty,
        };
    }

    pub fn release(self: *JSS3Error) void {
        self.path.deref();
        self.code.deref();
        self.message.deref();
    }

    pub fn deinit(self: *const JSS3Error) void {
        self.path.deref();
        self.code.deref();
        self.message.deref();
    }

    pub fn toErrorInstance(self: *const JSS3Error, global: *jsc.JSGlobalObject) jsc.JSValue {
        defer self.deinit();
        return S3Error__toErrorInstance(self, global);
    }

    extern fn S3Error__toErrorInstance(this: *const JSS3Error, global: *jsc.JSGlobalObject) callconv(jsc.conv) jsc.JSValue;
};

pub fn s3ErrorToJS(s3_err: *const S3Error, globalObject: *jsc.JSGlobalObject, path: ?[]const u8): jsc.JSValue {
    const value = JSS3Error.init(s3_err.code, s3_err.message, path).toErrorInstance(globalObject);
    bun.assert(!globalObject.hasException());
    return value;
}

pub fn s3ErrorToJSWithAsyncStack(
    s3_err: *const S3Error,
    globalObject: *jsc.JSGlobalObject,
    path: ?[]const u8,
    promise: *jsc.JSPromise,
): jsc.JSValue {
    const value = s3ErrorToJS(s3_err, globalObject, path);
    value.attachAsyncStackFromPromise(globalObject, promise);
    return value;
}
