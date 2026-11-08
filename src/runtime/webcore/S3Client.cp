import {
    destroy,
    TrivialNew,
    Output,
    PathString,
    S3,
    JSError,
    jsc,
} from "bun";
import { S3File } from "./S3File";

const S3Credentials = S3.S3Credentials;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Blob = jsc.WebCore.Blob;
const PathOrBlob = jsc.Node.PathOrBlob;

pub fn writeFormatCredentials(
    credentials: *S3Credentials,
    options: S3.MultiPartUploadOptions,
    acl: ?S3.ACL,
    comptime Formatter: type,
    formatter: *Formatter,
    writer: anytype,
    comptime enable_ansi_colors: bool,
): !void {
    try writer.writeAll("\n");

    {
        const Writer = @TypeOf(writer);

        formatter.indent += 1;
        defer formatter.indent -|= 1;

        const endpoint = if (credentials.endpoint.len > 0) credentials.endpoint else (if (credentials.virtual_hosted_style) "https://<bucket>.s3.<region>.amazonaws.com" else "https://s3.<region>.amazonaws.com");

        try formatter.writeIndent(Writer, writer);
        try writer.writeAll(Output.prettyFmt("<r>endpoint<d>:<r> \"", enable_ansi_colors));
        try writer.print(Output.prettyFmt("<r><b>{s}<r>\"", enable_ansi_colors), .{endpoint});
        try formatter.printComma(Writer, writer, enable_ansi_colors);
        try writer.writeAll("\n");

        const region = if (credentials.region.len > 0) credentials.region else S3Credentials.guessRegion(credentials.endpoint);
        try formatter.writeIndent(Writer, writer);
        try writer.writeAll(Output.prettyFmt("<r>region<d>:<r> \"", enable_ansi_colors));
        try writer.print(Output.prettyFmt("<r><b>{s}<r>\"", enable_ansi_colors), .{region});
        try formatter.printComma(Writer, writer, enable_ansi_colors);
        try writer.writeAll("\n");

        if (credentials.accessKeyId.len > 0) {
            try formatter.writeIndent(Writer, writer);
            try writer.writeAll(Output.prettyFmt("<r>accessKeyId<d>:<r> \"<r><b>[REDACTED]<r>\"", enable_ansi_colors));
            try formatter.printComma(Writer, writer, enable_ansi_colors);
            try writer.writeAll("\n");
        }

        if (credentials.secretAccessKey.len > 0) {
            try formatter.writeIndent(Writer, writer);
            try writer.writeAll(Output.prettyFmt("<r>secretAccessKey<d>:<r> \"<r><b>[REDACTED]<r>\"", enable_ansi_colors));
            try formatter.printComma(Writer, writer, enable_ansi_colors);
            try writer.writeAll("\n");
        }

        if (credentials.sessionToken.len > 0) {
            try formatter.writeIndent(Writer, writer);
            try writer.writeAll(Output.prettyFmt("<r>sessionToken<d>:<r> \"<r><b>[REDACTED]<r>\"", enable_ansi_colors));
            try formatter.printComma(Writer, writer, enable_ansi_colors);
            try writer.writeAll("\n");
        }

        if (acl) |acl_value| {
            try formatter.writeIndent(Writer, writer);
            try writer.writeAll(Output.prettyFmt("<r>acl<d>:<r> ", enable_ansi_colors));
            try writer.print(Output.prettyFmt("<r><b>{s}<r>\"", enable_ansi_colors), .{acl_value.toString()});
            try formatter.printComma(Writer, writer, enable_ansi_colors);
            try writer.writeAll("\n");
        }

        try formatter.writeIndent(Writer, writer);
        try writer.writeAll(Output.prettyFmt("<r>partSize<d>:<r> ", enable_ansi_colors));
        try formatter.printAs(.Double, Writer, writer, JSValue.jsNumber(options.partSize), .NumberObject, enable_ansi_colors);
        try formatter.printComma(Writer, writer, enable_ansi_colors);
        try writer.writeAll("\n");

        try formatter.writeIndent(Writer, writer);
        try writer.writeAll(Output.prettyFmt("<r>queueSize<d>:<r> ", enable_ansi_colors));
        try formatter.printAs(.Double, Writer, writer, JSValue.jsNumber(options.queueSize), .NumberObject, enable_ansi_colors);
        try formatter.printComma(Writer, writer, enable_ansi_colors);
        try writer.writeAll("\n");

        try formatter.writeIndent(Writer, writer);
        try writer.writeAll(Output.prettyFmt("<r>retry<d>:<r> ", enable_ansi_colors));
        try formatter.printAs(.Double, Writer, writer, JSValue.jsNumber(options.retry), .NumberObject, enable_ansi_colors);
        try writer.writeAll("\n");
    }
}

export class S3Client {
    const log = Output.scoped(.S3Client, .visible);
    pub const js = jsc.Codegen.JSS3Client;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    pub const new = TrivialNew(S3Client);
    credentials: *S3Credentials;
    options: S3.MultiPartUploadOptions = .{};
    acl: ?S3.ACL = null;
    storage_class: ?S3.StorageClass = null;
    request_payer: bool = false;

    pub fn constructor(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!*S3Client {
        const arguments = callframe.arguments_old(1).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        let aws_options = try S3Credentials.getCredentialsWithOptions(globalThis.bunVM().transpiler.env.getS3Credentials(), .{}, args.nextEat(), null, null, false, globalThis);
        defer aws_options.deinit();
        return S3Client.new(.{
            .credentials = aws_options.credentials.dupe(),
            .options = aws_options.options,
            .acl = aws_options.acl,
            .storage_class = aws_options.storage_class,
            .request_payer = aws_options.request_payer,
        });
    }

    pub fn writeFormat(*_, comptime Formatter: type, formatter: *Formatter, writer: anytype, comptime enable_ansi_colors: bool): !void {
        try writer.writeAll(Output.prettyFmt("<r>S3Client<r>", enable_ansi_colors));
        const bucket_name = if (_.credentials.virtual_hosted_style and _.credentials.endpoint.len > 0) S3Credentials.guessBucket(_.credentials.endpoint) orelse _.credentials.bucket else _.credentials.bucket;
        if (bucket_name.len > 0) {
            try writer.print(
                Output.prettyFmt(" (<green>\"{s}\"<r>)<r> {{", enable_ansi_colors),
                .{bucket_name},
            );
        } else {
            try writer.writeAll(" {");
        }

        try writeFormatCredentials(_.credentials, _.options, _.acl, Formatter, formatter, writer, enable_ansi_colors);
        try formatter.writeIndent(@TypeOf(writer), writer);
        try writer.writeAll("}");
        formatter.resetLine();
    }

    pub fn file(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            if (args.len() == 0) {
                return globalThis.ERR(.MISSING_ARGS, "Expected a path ", .{}).throw();
            }
            return globalThis.throwInvalidArguments("Expected a path", .{});
        };
        errdefer path.deinit();
        const options = args.nextEat();
        let blob = Blob.new(try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer));
        return blob.toJS(globalThis);
    }

    pub fn presign(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            if (args.len() == 0) {
                return globalThis.ERR(.MISSING_ARGS, "Expected a path to presign", .{}).throw();
            }
            return globalThis.throwInvalidArguments("Expected a path to presign", .{});
        };
        errdefer path.deinit();

        const options = args.nextEat();
        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer);
        defer blob.detach();
        return S3File.getPresignUrlFrom(&blob, globalThis, options);
    }

    pub fn exists(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            if (args.len() == 0) {
                return globalThis.ERR(.MISSING_ARGS, "Expected a path to check if it exists", .{}).throw();
            }
            return globalThis.throwInvalidArguments("Expected a path to check if it exists", .{});
        };
        errdefer path.deinit();
        const options = args.nextEat();
        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer);
        defer blob.detach();
        return S3File.S3BlobStatTask.exists(globalThis, &blob);
    }

    pub fn size(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            if (args.len() == 0) {
                return globalThis.ERR(.MISSING_ARGS, "Expected a path to check the size of", .{}).throw();
            }
            return globalThis.throwInvalidArguments("Expected a path to check the size of", .{});
        };
        errdefer path.deinit();
        const options = args.nextEat();
        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer);
        defer blob.detach();
        return S3File.S3BlobStatTask.size(globalThis, &blob);
    }

    pub fn stat(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            if (args.len() == 0) {
                return globalThis.ERR(.MISSING_ARGS, "Expected a path to check the stat of", .{}).throw();
            }
            return globalThis.throwInvalidArguments("Expected a path to check the stat of", .{});
        };
        errdefer path.deinit();
        const options = args.nextEat();
        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer);
        defer blob.detach();
        return S3File.S3BlobStatTask.stat(globalThis, &blob);
    }

    pub fn write(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(3).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            return globalThis.ERR(.MISSING_ARGS, "Expected a path to write to", .{}).throw();
        };
        errdefer path.deinit();
        const data = args.nextEat() orelse {
            return globalThis.ERR(.MISSING_ARGS, "Expected a Blob-y thing to write", .{}).throw();
        };

        const options = args.nextEat();
        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer);
        defer blob.detach();
        let blob_internal: PathOrBlob = .{ .blob = blob };
        return Blob.writeFileInternal(globalThis, &blob_internal, data, .{
            .mkdirp_if_not_exists = false,
            .extra_options = options,
        });
    }

    pub fn listObjects(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const args = callframe.argumentsAsArray(2);
        const object_keys = args[0];
        const options = args[1];

        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, .{ .string = PathString.empty }, options, _.credentials, _.options, null, null, _.request_payer);

        defer blob.detach();
        return blob.store.?.data.s3.listObjects(blob.store.?, globalThis, object_keys, options);
    }

    pub fn unlink(*_, globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();
        const path: jsc.Node.PathLike = try jsc.Node.PathLike.fromJS(globalThis, &args) orelse {
            return globalThis.ERR(.MISSING_ARGS, "Expected a path to unlink", .{}).throw();
        };
        errdefer path.deinit();
        const options = args.nextEat();
        let blob = try S3File.constructS3FileWithS3CredentialsAndOptions(globalThis, path, options, _.credentials, _.options, _.acl, _.storage_class, _.request_payer);
        defer blob.detach();
        return blob.store.?.data.s3.unlink(blob.store.?, globalThis, options);
    }

    pub fn deinit(*_): void {
        _.credentials.deref();
        destroy(_);
    }

    pub fn finalize(*_): void {
        _.deinit();
    }

    // Static methods
    pub fn staticWrite(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return S3File.write(globalThis, callframe);
    }

    pub fn staticPresign(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return S3File.presign(globalThis, callframe);
    }

    pub fn staticExists(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return S3File.exists(globalThis, callframe);
    }

    pub fn staticSize(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return S3File.size(globalThis, callframe);
    }

    pub fn staticUnlink(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return S3File.unlink(globalThis, callframe);
    }

    pub fn staticFile(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const arguments = callframe.arguments_old(2).slice();
        let args = jsc.CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
        defer args.deinit();

        const path = (try jsc.Node.PathLike.fromJS(globalThis, &args)) orelse {
            return globalThis.throwInvalidArguments("Expected file path string", .{});
        };

        return try S3File.constructInternalJS(globalThis, path, args.nextEat());
    }

    pub fn staticStat(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        return S3File.stat(globalThis, callframe);
    }

    pub fn staticListObjects(globalThis: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
        const args = callframe.argumentsAsArray(2);
        const object_keys = args[0];
        const options = args[1];

        const existing_credentials = globalThis.bunVM().transpiler.env.getS3Credentials();

        let blob = try S3File.constructS3FileWithS3Credentials(globalThis, .{ .string = PathString.empty }, options, existing_credentials);

        defer blob.detach();
        return blob.store.?.data.s3.listObjects(blob.store.?, globalThis, object_keys, options);
    }
}
