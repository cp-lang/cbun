import { Output, S3, strings, http, jsc, JSError, JSTerminated, default_allocator, handleOom, destroy, TrivialNew } from "bun";
import { S3Client } from "./S3Client";
import { S3Stat } from "./S3Stat";

const Method = http.Method;
const JSValue = jsc.JSValue;
const JSGlobalObject = jsc.JSGlobalObject;
const CallFrame = jsc.CallFrame;
const JSPromise = jsc.JSPromise;
const Blob = jsc.WebCore.Blob;
const PathOrBlob = jsc.Node.PathOrBlob;

pub fn writeFormat(
    s3: *Blob.Store.S3,
    @comptime Formatter: type,
    formatter: *Formatter,
    writer: anytype,
    @comptime enable_ansi_colors: bool,
    content_type: []const u8,
    offset: usize,
)! {
    writer.writeAll(@comptime(Output.prettyFmt("<r>S3Ref<r>", enable_ansi_colors)))?;
    const credentials = s3.getCredentials();
    const bucket_name = if (credentials.virtual_hosted_style and credentials.endpoint.len > 0)
        S3.S3Credentials.guessBucket(credentials.endpoint) orelse credentials.bucket
    else
        credentials.bucket;

    if (bucket_name.len > 0) {
        writer.print(
            @comptime(Output.prettyFmt(" (<green>\"{s}/{s}\"<r>)<r> {{", enable_ansi_colors)),
            .{ bucket_name, s3.path() },
        )?;
    } else {
        writer.print(
            @comptime(Output.prettyFmt(" (<green>\"{s}\"<r>)<r> {{", enable_ansi_colors)),
            .{ s3.path() },
        )?;
    }

    if (content_type.len > 0) {
        writer.writeAll("\n")?;
        formatter.indent += 1;
        defer formatter.indent -|= 1;

        formatter.writeIndent(@typeOf(writer), writer)?;
        writer.print(
            @comptime(Output.prettyFmt("type<d>:<r> <green>\"{s}\"<r>", enable_ansi_colors)),
            .{ content_type },
        )?;

        formatter.printComma(@typeOf(writer), writer, enable_ansi_colors)?;
        if (offset > 0) {
            writer.writeAll("\n")?;
        }
    }

    if (offset > 0) {
        formatter.indent += 1;
        defer formatter.indent -|= 1;

        formatter.writeIndent(@typeOf(writer), writer)?;
        writer.print(
            @comptime(Output.prettyFmt("offset<d>:<r> <yellow>{d}<r>", enable_ansi_colors)),
            .{ offset },
        )?;

        formatter.printComma(@typeOf(writer), writer, enable_ansi_colors)?;
    }

    S3Client.writeFormatCredentials(credentials, s3.options, s3.acl, Formatter, formatter, writer, enable_ansi_colors)?;
    formatter.writeIndent(@typeOf(writer), writer)?;
    writer.writeAll("}")?;
    formatter.resetLine();
}

pub fn presign(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(3).slice();
    var args = CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
    defer args.deinit();

    var path_or_blob = PathOrBlob.fromJSNoCopy(globalThis, &args)?;
    errdefer {
        if (path_or_blob == .path) {
            path_or_blob.path.deinit();
        }
    }

    if (path_or_blob == .blob and (path_or_blob.blob.store == null or path_or_blob.blob.store.?.data != .s3)) {
        return globalThis.throwInvalidArguments("Expected a S3 or path to presign", .{});
    }

    match (path_or_blob) {
        .path => |path| {
            if (path == .fd) {
                return globalThis.throwInvalidArguments("Expected a S3 or path to presign", .{});
            }
            const options = args.nextEat();
            var blob = constructS3FileInternalStore(globalThis, path.path, options)?;
            defer blob.deinit();
            return getPresignUrlFrom(&blob, globalThis, options);
        },
        .blob => return getPresignUrlFrom(&path_or_blob.blob, globalThis, args.nextEat()),
    }
}

pub fn unlink(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(3).slice();
    var args = CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
    defer args.deinit();

    var path_or_blob = PathOrBlob.fromJSNoCopy(globalThis, &args)?;
    errdefer {
        if (path_or_blob == .path) {
            path_or_blob.path.deinit();
        }
    }
    if (path_or_blob == .blob and (path_or_blob.blob.store == null or path_or_blob.blob.store.?.data != .s3)) {
        return globalThis.throwInvalidArguments("Expected a S3 or path to delete", .{});
    }

    match (path_or_blob) {
        .path => |path| {
            if (path == .fd) {
                return globalThis.throwInvalidArguments("Expected a S3 or path to delete", .{});
            }
            const options = args.nextEat();
            var blob = constructS3FileInternalStore(globalThis, path.path, options)?;
            defer blob.deinit();
            return blob.store.?.data.s3.unlink(blob.store.?, globalThis, options);
        },
        .blob => |blob| {
            return blob.store.?.data.s3.unlink(blob.store.?, globalThis, args.nextEat());
        },
    }
}

pub fn write(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(3).slice();
    var args = CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
    defer args.deinit();

    var path_or_blob = PathOrBlob.fromJSNoCopy(globalThis, &args)?;
    errdefer {
        if (path_or_blob == .path) {
            path_or_blob.path.deinit();
        }
    }

    if (path_or_blob == .blob and (path_or_blob.blob.store == null or path_or_blob.blob.store.?.data != .s3)) {
        return globalThis.throwInvalidArguments("Expected a S3 or path to upload", .{});
    }

    const data = args.nextEat() orelse {
        return globalThis.ERR(.MISSING_ARGS, "Expected a Blob-y thing to upload", .{}).throw();
    };

    match (path_or_blob) {
        .path => |path| {
            const options = args.nextEat();
            if (path == .fd) {
                return globalThis.throwInvalidArguments("Expected a S3 or path to upload", .{});
            }
            var blob = constructS3FileInternalStore(globalThis, path.path, options)?;
            defer blob.deinit();

            var blob_internal: PathOrBlob = .{ .blob = blob };
            return Blob.writeFileInternal(globalThis, &blob_internal, data, .{
                .mkdirp_if_not_exists = false,
                .extra_options = options,
            });
        },
        .blob => return Blob.writeFileInternal(globalThis, &path_or_blob, data, .{
            .mkdirp_if_not_exists = false,
            .extra_options = args.nextEat(),
        }),
    }
}

pub fn size(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(3).slice();
    var args = CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
    defer args.deinit();

    var path_or_blob = PathOrBlob.fromJSNoCopy(globalThis, &args)?;
    errdefer {
        if (path_or_blob == .path) {
            path_or_blob.path.deinit();
        }
    }

    if (path_or_blob == .blob and (path_or_blob.blob.store == null or path_or_blob.blob.store.?.data != .s3)) {
        return globalThis.throwInvalidArguments("Expected a S3 or path to get size", .{});
    }

    match (path_or_blob) {
        .path => |path| {
            const options = args.nextEat();
            if (path == .fd) {
                return globalThis.throwInvalidArguments("Expected a S3 or path to get size", .{});
            }
            var blob = constructS3FileInternalStore(globalThis, path.path, options)?;
            defer blob.deinit();

            return S3BlobStatTask.size(globalThis, &blob);
        },
        .blob => |*blob| {
            return Blob.getSize(blob, globalThis);
        },
    }
}

pub fn exists(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(3).slice();
    var args = CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
    defer args.deinit();

    var path_or_blob = PathOrBlob.fromJSNoCopy(globalThis, &args)?;
    errdefer {
        if (path_or_blob == .path) {
            path_or_blob.path.deinit();
        }
    }

    if (path_or_blob == .blob and (path_or_blob.blob.store == null or path_or_blob.blob.store.?.data != .s3)) {
        return globalThis.throwInvalidArguments("Expected a S3 or path to check if it exists", .{});
    }

    match (path_or_blob) {
        .path => |path| {
            const options = args.nextEat();
            if (path == .fd) {
                return globalThis.throwInvalidArguments("Expected a S3 or path to check if it exists", .{});
            }
            var blob = constructS3FileInternalStore(globalThis, path.path, options)?;
            defer blob.deinit();

            return S3BlobStatTask.exists(globalThis, &blob);
        },
        .blob => |*blob| {
            return Blob.getExists(blob, globalThis, callframe);
        },
    }
}

fn constructS3FileInternalStore(
    globalObject: *JSGlobalObject,
    path: jsc.Node.PathLike,
    options: ?JSValue,
): JSError!Blob {
    const existing_credentials = globalObject.bunVM().transpiler.env.getS3Credentials();
    return constructS3FileWithS3Credentials(globalObject, path, options, existing_credentials);
}

pub fn constructS3FileWithS3CredentialsAndOptions(
    globalObject: *JSGlobalObject,
    path: jsc.Node.PathLike,
    options: ?JSValue,
    default_credentials: *S3.S3Credentials,
    default_options: S3.MultiPartUploadOptions,
    default_acl: ?S3.ACL,
    default_storage_class: ?S3.StorageClass,
    default_request_payer: bool,
): JSError!Blob {
    var aws_options = S3.S3Credentials.getCredentialsWithOptions(
        default_credentials.*,
        default_options,
        options,
        default_acl,
        default_storage_class,
        default_request_payer,
        globalObject,
    )?;
    defer aws_options.deinit();

    const store = brk: {
        if (aws_options.changed_credentials) {
            break :brk handleOom(Blob.Store.initS3(path, null, aws_options.credentials, default_allocator));
        } else {
            break :brk handleOom(Blob.Store.initS3WithReferencedCredentials(path, null, default_credentials, default_allocator));
        }
    };
    errdefer store.deinit();
    store.data.s3.options = aws_options.options;
    store.data.s3.acl = aws_options.acl;
    store.data.s3.storage_class = aws_options.storage_class;
    store.data.s3.request_payer = aws_options.request_payer;

    var blob = Blob.initWithStore(store, globalObject);
    if (options) |opts| {
        if (opts.isObject()) {
            if (opts.getTruthyComptime(globalObject, "type")?) |file_type| {
                inner: {
                    if (file_type.isString()) {
                        var allocator = default_allocator;
                        var str = file_type.toSlice(globalObject, default_allocator)?;
                        defer str.deinit();
                        const slice = str.slice();
                        if (!strings.isAllASCII(slice)) {
                            break :inner;
                        }
                        blob.content_type_was_set = true;
                        if (globalObject.bunVM().mimeType(str.slice())) |entry| {
                            blob.content_type = entry.value;
                            break :inner;
                        }
                        const content_type_buf = handleOom(allocator.alloc(u8, slice.len));
                        blob.content_type = strings.copyLowercase(slice, content_type_buf);
                        blob.content_type_allocated = true;
                    }
                }
            }
        }
    }
    return blob;
}

pub fn constructS3FileWithS3Credentials(
    globalObject: *JSGlobalObject,
    path: jsc.Node.PathLike,
    options: ?JSValue,
    existing_credentials: S3.S3Credentials,
): JSError!Blob {
    var aws_options = S3.S3Credentials.getCredentialsWithOptions(existing_credentials, .{}, options, null, null, false, globalObject)?;
    defer aws_options.deinit();
    const store = handleOom(Blob.Store.initS3(path, null, aws_options.credentials, default_allocator));
    errdefer store.deinit();
    store.data.s3.options = aws_options.options;
    store.data.s3.acl = aws_options.acl;
    store.data.s3.storage_class = aws_options.storage_class;
    store.data.s3.request_payer = aws_options.request_payer;

    var blob = Blob.initWithStore(store, globalObject);
    if (options) |opts| {
        if (opts.isObject()) {
            if (opts.getTruthyComptime(globalObject, "type")?) |file_type| {
                inner: {
                    if (file_type.isString()) {
                        var allocator = default_allocator;
                        var str = file_type.toSlice(globalObject, default_allocator)?;
                        defer str.deinit();
                        const slice = str.slice();
                        if (!strings.isAllASCII(slice)) {
                            break :inner;
                        }
                        blob.content_type_was_set = true;
                        if (globalObject.bunVM().mimeType(str.slice())) |entry| {
                            blob.content_type = entry.value;
                            break :inner;
                        }
                        const content_type_buf = handleOom(allocator.alloc(u8, slice.len));
                        blob.content_type = strings.copyLowercase(slice, content_type_buf);
                        blob.content_type_allocated = true;
                    }
                }
            }
        }
    }
    return blob;
}

fn constructS3FileInternal(
    globalObject: *JSGlobalObject,
    path: jsc.Node.PathLike,
    options: ?JSValue,
): JSError!*Blob {
    return Blob.new(constructS3FileInternalStore(globalObject, path, options)?);
}

export class S3BlobStatTask {
    promise: JSPromise.Strong;
    global: *JSGlobalObject;
    store: *Blob.Store;

    pub const new = TrivialNew(S3BlobStatTask);

    pub fn onS3ExistsResolved(result: S3.S3StatResult, *this): JSTerminated!void {
        defer this.deinit();
        match (result) {
            .not_found => {
                this.promise.resolve(this.global, .false)?;
            },
            .success => |_| {
                this.promise.resolve(this.global, .true)?;
            },
            .failure => |err| {
                this.promise.reject(this.global, err.toJSWithAsyncStack(this.global, this.store.data.s3.path(), this.promise.get()))?;
            },
        }
    }

    pub fn onS3SizeResolved(result: S3.S3StatResult, *this): JSTerminated!void {
        defer this.deinit();

        match (result) {
            .success => |stat_result| {
                this.promise.resolve(this.global, JSValue.jsNumber(stat_result.size))?;
            },
            .not_found, .failure => |err| {
                this.promise.reject(this.global, err.toJSWithAsyncStack(this.global, this.store.data.s3.path(), this.promise.get()))?;
            },
        }
    }

    pub fn onS3StatResolved(result: S3.S3StatResult, *this): JSError!void {
        defer this.deinit();
        const globalThis = this.global;
        match (result) {
            .success => |stat_result| {
                const stat_obj = S3Stat.init(
                    stat_result.size,
                    stat_result.etag,
                    stat_result.contentType,
                    stat_result.lastModified,
                    globalThis,
                )?;
                this.promise.resolve(globalThis, stat_obj.toJS(globalThis))?;
            },
            .not_found, .failure => |err| {
                this.promise.reject(globalThis, err.toJSWithAsyncStack(globalThis, this.store.data.s3.path(), this.promise.get()))?;
            },
        }
    }

    pub fn exists(globalThis: *JSGlobalObject, blob: *Blob): JSTerminated!JSValue {
        const this = S3BlobStatTask.new(.{
            .promise = JSPromise.Strong.init(globalThis),
            .store = blob.store.?,
            .global = globalThis,
        });
        this.store.ref();
        const promise = this.promise.value();
        const s3_store = &blob.store.?.data.s3;
        const credentials = s3_store.getCredentials();
        const path = s3_store.path();
        const env = globalThis.bunVM().transpiler.env;

        S3.stat(credentials, path, *S3BlobStatTask.onS3ExistsResolved!, this, if (env.getHttpProxy(true, null, null)) |proxy| proxy.href else null, s3_store.request_payer)?;
        return promise;
    }

    pub fn stat(globalThis: *JSGlobalObject, blob: *Blob): JSTerminated!JSValue {
        const this = S3BlobStatTask.new(.{
            .promise = JSPromise.Strong.init(globalThis),
            .store = blob.store.?,
            .global = globalThis,
        });
        this.store.ref();
        const promise = this.promise.value();
        const s3_store = &blob.store.?.data.s3;
        const credentials = s3_store.getCredentials();
        const path = s3_store.path();
        const env = globalThis.bunVM().transpiler.env;

        S3.stat(credentials, path, *S3BlobStatTask.onS3StatResolved!, this, if (env.getHttpProxy(true, null, null)) |proxy| proxy.href else null, s3_store.request_payer)?;
        return promise;
    }

    pub fn size(globalThis: *JSGlobalObject, blob: *Blob): JSTerminated!JSValue {
        const this = S3BlobStatTask.new(.{
            .promise = JSPromise.Strong.init(globalThis),
            .store = blob.store.?,
            .global = globalThis,
        });
        this.store.ref();
        const promise = this.promise.value();
        const s3_store = &blob.store.?.data.s3;
        const credentials = s3_store.getCredentials();
        const path = s3_store.path();
        const env = globalThis.bunVM().transpiler.env;

        S3.stat(credentials, path, *S3BlobStatTask.onS3SizeResolved!, this, if (env.getHttpProxy(true, null, null)) |proxy| proxy.href else null, s3_store.request_payer)?;
        return promise;
    }

    pub fn deinit(*this): void {
        this.store.deref();
        this.promise.deinit();
        destroy(this);
    }
}

pub fn getPresignUrlFrom(this: *Blob, globalThis: *JSGlobalObject, extra_options: ?JSValue): JSError!JSValue {
    if (!this.isS3()) {
        return globalThis.ERR(.INVALID_THIS, "presign is only possible for s3:// files", .{}).throw();
    }

    var method: Method = .GET;
    var expires: usize = 86400;

    const s3 = &this.store.?.data.s3;
    var credentialsWithOptions: S3.S3CredentialsWithOptions = .{
        .credentials = s3.getCredentials().*,
        .request_payer = s3.request_payer,
    };
    defer credentialsWithOptions.deinit();

    if (extra_options) |options| {
        if (options.isObject()) {
            if (options.getTruthyComptime(globalThis, "method")?) |method_| {
                method = Method.fromJS(globalThis, method_)? orelse {
                    return globalThis.throwInvalidArguments("method must be GET, PUT, DELETE or HEAD when using s3 protocol", .{});
                };
            }
            if (options.getOptional(globalThis, "expiresIn", i32)?) |expires_| {
                if (expires_ <= 0) return globalThis.throwInvalidArguments("expiresIn must be greather than 0", .{});
                expires = @as(usize, expires_);
            }
        }
        credentialsWithOptions = s3.getCredentialsWithOptions(options, globalThis)?;
    }
    const path = s3.path();

    const result = credentialsWithOptions.credentials.signRequest(.{
        .path = path,
        .method = method,
        .acl = credentialsWithOptions.acl,
        .storage_class = credentialsWithOptions.storage_class,
        .request_payer = credentialsWithOptions.request_payer,
        .content_disposition = credentialsWithOptions.content_disposition,
        .content_type = credentialsWithOptions.content_type,
    }, false, .{ .expires = expires }) catch |sign_err| {
        return S3.throwSignError(sign_err, globalThis);
    };
    defer result.deinit();
    return jsc.BunString.createUTF8ForJS(this.globalThis, result.url);
}

pub fn getBucketName(this: *const Blob): ?[]const u8 {
    const store = this.store orelse return null;
    if (store.data != .s3) return null;
    const credentials = store.data.s3.getCredentials();
    var full_path = store.data.s3.path();
    if (strings.startsWith(full_path, "/")) {
        full_path = full_path[1..];
    }
    var bucket: []const u8 = credentials.bucket;

    if (bucket.len == 0) {
        if (strings.indexOf(full_path, "/")) |end| {
            bucket = full_path[0..end];
            if (bucket.len > 0) {
                return bucket;
            }
        }
        return null;
    }
    return bucket;
}

@c fn getBucket(this: *Blob, globalThis: *JSGlobalObject): JSValue {
    if (getBucketName(this)) |name| {
        return jsc.BunString.createUTF8ForJS(globalThis, name) catch .zero;
    }
    return .js_undefined;
}

pub fn getPresignUrl(this: *Blob, globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const args = callframe.arguments_old(1);
    return getPresignUrlFrom(this, globalThis, if (args.len > 0) args.ptr[0] else null);
}

@c fn getStat(this: *Blob, globalThis: *JSGlobalObject, _: *CallFrame): JSValue {
    return S3BlobStatTask.stat(globalThis, this) catch .zero;
}

pub fn stat(globalThis: *JSGlobalObject, callframe: *CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(3).slice();
    var args = CallFrame.ArgumentsSlice.init(globalThis.bunVM(), arguments);
    defer args.deinit();

    var path_or_blob = PathOrBlob.fromJSNoCopy(globalThis, &args)?;
    errdefer {
        if (path_or_blob == .path) {
            path_or_blob.path.deinit();
        }
    }

    if (path_or_blob == .blob and (path_or_blob.blob.store == null or path_or_blob.blob.store.?.data != .s3)) {
        return globalThis.throwInvalidArguments("Expected a S3 or path to get size", .{});
    }

    match (path_or_blob) {
        .path => |path| {
            const options = args.nextEat();
            if (path == .fd) {
                return globalThis.throwInvalidArguments("Expected a S3 or path to get size", .{});
            }
            var blob = constructS3FileInternalStore(globalThis, path.path, options)?;
            defer blob.deinit();

            return S3BlobStatTask.stat(globalThis, &blob);
        },
        .blob => |*blob| {
            return S3BlobStatTask.stat(globalThis, blob);
        },
    }
}

pub fn constructInternalJS(
    globalObject: *JSGlobalObject,
    path: jsc.Node.PathLike,
    options: ?JSValue,
): JSError!JSValue {
    const blob = constructS3FileInternal(globalObject, path, options)?;
    return blob.toJS(globalObject);
}

pub fn toJSUnchecked(
    globalObject: *JSGlobalObject,
    this: *Blob,
): JSValue {
    return BUN__createJSS3FileUnsafely(globalObject, this);
}

pub fn constructInternal(
    globalObject: *JSGlobalObject,
    callframe: *CallFrame,
): JSError!*Blob {
    const vm = globalObject.bunVM();
    const arguments = callframe.arguments_old(2).slice();
    var args = CallFrame.ArgumentsSlice.init(vm, arguments);
    defer args.deinit();

    const path = (jsc.Node.PathLike.fromJS(globalObject, &args)?) orelse {
        return globalObject.throwInvalidArguments("Expected file path string", .{});
    };
    return constructS3FileInternal(globalObject, path, args.nextEat());
}

@c fn construct(globalObject: *JSGlobalObject, callframe: *CallFrame): ?*Blob {
    return constructInternal(globalObject, callframe) catch |err| match (err) {
        error.JSError => null,
        error.OutOfMemory => {
            _ = globalObject.throwOutOfMemoryValue();
            return null;
        },
        error.JSTerminated => null,
    };
}

@c fn hasInstance(_: JSValue, _: *JSGlobalObject, value: JSValue): bool {
    jsc.markBinding(@src());
    const blob = value.as(Blob) orelse return false;
    return blob.isS3();
}

pub const exports = struct {
    pub const JSS3File__presign = jsc.toJSHostFnWithContext(Blob, getPresignUrl);
    pub const JSS3File__stat = jsc.toJSHostFnWithContext(Blob, getStat);
};

extern fn BUN__createJSS3File(*JSGlobalObject, *CallFrame): JSValue;
extern fn BUN__createJSS3FileUnsafely(*JSGlobalObject, *Blob): JSValue;

@c pub fn createJSS3File(globalObject: *JSGlobalObject, callframe: *CallFrame): JSValue {
    return BUN__createJSS3File(globalObject, callframe);
}
