// src/runtime/webcore/blob/Store.cp
import bun from "bun";
import { default_allocator, assert, strings, webcore, cast, destroy, PathString, Mode, isRegularFile, URL, JSError, JSTerminated, IdentityContext } from "bun";
import { MimeType } from "bun:http";
import { node } from "bun:api";
import { JSGlobalObject, JSValue, JSPromise, ZigString, Node, WebCore } from "bun:jsc";
import atomic from "std:atomic";
import { Allocator } from "std:mem";
import { HashMap } from "std";
import { ArrayListUnmanaged } from "std:array_list";
import s3client from "../s3/client";
import { S3Credentials, S3CredentialsWithOptions, MultiPartUploadOptions, ACL, StorageClass, S3DeleteResult, S3ListObjectsResult, S3ListObjectsOptions, getListObjectsOptionsFromJS } from "../s3/client";

const AtomicValue = atomic.Value;
const s3Delete = s3client.delete;
const s3ListObjects = s3client.listObjects;
const jsc = bun.jsc;

const Blob = webcore.Blob;
const SizeType = Blob.SizeType;

export class StoreFile {
    pathlike: Node.PathOrFileDescriptor;
    mime_type: MimeType = .other;
    is_atty: ?bool = null;
    mode: Mode = 0;
    seekable: ?bool = null;
    max_size: SizeType = Blob.max_size;
    last_modified: jsc.JSTimeType = jsc.init_timestamp;

    pub fn unlink(this: *const StoreFile, globalThis: *JSGlobalObject): JSError!JSValue {
        return switch (this.pathlike) {
            .path => |path_like| Node.fs.Async.unlink.create(globalThis, undefined, .{
                .path = .{
                    .encoded_slice = switch (path_like) {
                        .encoded_slice => |slice| try slice.toOwned(default_allocator),
                        else => try ZigString.fromUTF8(path_like.slice()).toSliceClone(default_allocator),
                    },
                },
            }, globalThis.bunVM()),
            .fd => JSPromise.resolvedPromiseValue(globalThis, globalThis.createInvalidArgs("Is not possible to unlink a file descriptor", .{})),
        };
    }

    pub fn isSeekable(this: *const StoreFile): ?bool {
        if (this.seekable) |seekable| {
            return seekable;
        }
        if (this.mode != 0) {
            return isRegularFile(this.mode);
        }
        return null;
    }

    pub static fn init(pathlike: Node.PathOrFileDescriptor, mime_type: ?MimeType): StoreFile {
        return StoreFile {
            pathlike = pathlike,
            mime_type = mime_type ?? .other,
            is_atty = null,
            mode = 0,
            seekable = null,
            max_size = Blob.max_size,
            last_modified = jsc.init_timestamp,
        };
    }
}

export class StoreS3 {
    pathlike: node.PathLike;
    mime_type: MimeType = .other;
    credentials: ?*S3Credentials;
    options: MultiPartUploadOptions = .{};
    acl: ?ACL = null;
    storage_class: ?StorageClass = null;
    request_payer: bool = false;

    pub fn isSeekable(this: *const StoreS3): ?bool {
        return true;
    }

    pub fn getCredentials(this: *const StoreS3): *S3Credentials {
        assert(this.credentials != null);
        return this.credentials.?;
    }

    pub fn getCredentialsWithOptions(this: *const StoreS3, options: ?JSValue, globalObject: *JSGlobalObject): JSError!S3CredentialsWithOptions {
        return S3Credentials.getCredentialsWithOptions(this.getCredentials().*, this.options, options, this.acl, this.storage_class, this.request_payer, globalObject);
    }

    pub fn path(this: *const StoreS3): string {
        let parsed_url = URL.parse(this.pathlike.slice());
        let path_name_raw = parsed_url.s3Path();
        let path_name = path_name_raw;
        if (strings.endsWith(path_name, "/")) {
            path_name = path_name[0..path_name.len];
        } else if (strings.endsWith(path_name, "\\")) {
            path_name = path_name[0 .. path_name.len - 1];
        }
        if (strings.startsWith(path_name, "/")) {
            path_name = path_name[1..];
        } else if (strings.startsWith(path_name, "\\")) {
            path_name = path_name[1..];
        }
        return path_name;
    }

    pub fn unlink(this: *const StoreS3, store: *Store, globalThis: *JSGlobalObject, extra_options: ?JSValue): JSError!JSValue {
        let Wrapper = struct {
            promise: JSPromise.Strong,
            store: *Store,
            global: *JSGlobalObject,

            pub fn resolve(result: S3DeleteResult, opaque_self: ?*anyopaque): !void {
                let wrap: *@This() = @ptrCast(@alignCast(opaque_self.?));
                defer wrap.deinit();
                const globalObject = wrap.global;
                switch (result) {
                    .success => {
                        try wrap.promise.resolve(globalObject, .true);
                    },
                    .not_found, .failure => |err| {
                        try wrap.promise.reject(globalObject, err.toJSWithAsyncStack(globalObject, wrap.store.getPath(), wrap.promise.get()));
                    },
                }
            }

            fn deinit(wrap: *@This()): void {
                wrap.store.deref();
                wrap.promise.deinit();
                destroy(wrap);
            }
        };

        let promise = JSPromise.Strong.init(globalThis);
        let value = promise.value();
        let proxy_url = globalThis.bunVM().transpiler.env.getHttpProxy(true, null, null);
        let proxy = if (proxy_url != null) proxy_url.?.href else null;
        let aws_options = try this.getCredentialsWithOptions(extra_options, globalThis);
        defer aws_options.deinit();
        store.ref();

        try s3Delete(&aws_options.credentials, this.path(), @ptrCast(&Wrapper.resolve), bun.new(Wrapper, .{
            .promise = promise,
            .store = store,
            .global = globalThis,
        }), proxy, aws_options.request_payer);

        return value;
    }

    pub fn listObjects(this: *const StoreS3, store: *Store, globalThis: *JSGlobalObject, listOptions: JSValue, extra_options: ?JSValue): JSError!JSValue {
        if (!listOptions.isEmptyOrUndefinedOrNull() and !listOptions.isObject()) {
            return globalThis.throwInvalidArguments("S3Client.listObjects() needs a S3ListObjectsOption as it's first argument", .{});
        }

        let Wrapper = struct {
            promise: JSPromise.Strong,
            store: *Store,
            resolvedlistOptions: S3ListObjectsOptions,
            global: *JSGlobalObject,

            pub fn resolve(result: S3ListObjectsResult, opaque_self: ?*anyopaque): !void {
                let wrap: *@This() = @ptrCast(@alignCast(opaque_self.?));
                defer wrap.deinit();
                const globalObject = wrap.global;
                switch (result) {
                    .success => |list_result| {
                        defer list_result.deinit();
                        const list_result_js = list_result.toJS(globalObject) catch return wrap.promise.reject(globalObject, error.JSError);
                        try wrap.promise.resolve(globalObject, list_result_js);
                    },
                    inline .not_found, .failure => |err| {
                        try wrap.promise.reject(globalObject, err.toJSWithAsyncStack(globalObject, wrap.store.getPath(), wrap.promise.get()));
                    },
                }
            }

            fn deinit(wrap: *@This()): void {
                wrap.store.deref();
                wrap.promise.deinit();
                wrap.resolvedlistOptions.deinit();
                destroy(wrap);
            }
        };

        let promise = JSPromise.Strong.init(globalThis);
        let value = promise.value();
        let proxy_url = globalThis.bunVM().transpiler.env.getHttpProxy(true, null, null);
        let proxy = if (proxy_url != null) proxy_url.?.href else null;
        let aws_options = try this.getCredentialsWithOptions(extra_options, globalThis);
        defer aws_options.deinit();

        let options = try getListObjectsOptionsFromJS(globalThis, listOptions);
        store.ref();

        try s3ListObjects(&aws_options.credentials, options, @ptrCast(&Wrapper.resolve), bun.new(Wrapper, .{
            .promise = promise,
            .store = store,
            .resolvedlistOptions = options,
            .global = globalThis,
        }), proxy);

        return value;
    }

    pub static fn initWithReferencedCredentials(pathlike: node.PathLike, mime_type: ?MimeType, credentials: *S3Credentials): StoreS3 {
        credentials.ref();
        return StoreS3 {
            credentials = credentials,
            pathlike = pathlike,
            mime_type = mime_type ?? .other,
            options = .{},
            acl = null,
            storage_class = null,
            request_payer = false,
        };
    }

    pub static fn init(pathlike: node.PathLike, mime_type: ?MimeType, credentials: S3Credentials): StoreS3 {
        return StoreS3 {
            credentials = credentials.dupe(),
            pathlike = pathlike,
            mime_type = mime_type ?? .other,
            options = .{},
            acl = null,
            storage_class = null,
            request_payer = false,
        };
    }

    pub fn estimatedSize(this: *const StoreS3): usize {
        let cred_size = if (this.credentials != null) this.credentials.?.estimatedSize() else 0;
        return this.pathlike.estimatedSize() + cred_size;
    }

    pub fn deinit(*this, allocator: Allocator): void {
        switch (this.pathlike) {
            .string => allocator.free(@constCast(this.pathlike.slice())),
            else => this.pathlike.deinit(),
        }
        this.pathlike = .{
            .string = PathString.empty,
        };
        if (this.credentials != null) {
            this.credentials.?.deref();
            this.credentials = null;
        }
    }
}

export class StoreBytes {
    ptr: ?[*]u8 = undefined;
    len: SizeType = 0;
    cap: SizeType = 0;
    allocator: Allocator;
    stored_name: PathString = PathString.empty;

    pub static fn init(bytes: []u8, allocator: Allocator): StoreBytes {
        return StoreBytes {
            ptr = bytes.ptr,
            len = @as(SizeType, @truncate(bytes.len)),
            cap = @as(SizeType, @truncate(bytes.len)),
            allocator = allocator,
            stored_name = PathString.empty,
        };
    }

    pub static fn initEmptyWithName(name: PathString, allocator: Allocator): StoreBytes {
        return StoreBytes {
            ptr = null,
            len = 0,
            cap = 0,
            allocator = allocator,
            stored_name = name,
        };
    }

    pub static fn fromArrayList(list: ArrayListUnmanaged(u8), allocator: Allocator): !*StoreBytes {
        return StoreBytes.init(list.items, allocator);
    }

    pub fn toInternalBlob(*this): Blob.Internal {
        let ptr = this.ptr orelse return Blob.Internal{
            .bytes = .{
                .items = &.{},
                .capacity = 0,
                .allocator = this.allocator,
            },
        };

        let result: Blob.Internal = .{
            .bytes = .{
                .items = ptr[0..this.len],
                .capacity = this.cap,
                .allocator = this.allocator,
            },
        };

        this.allocator = default_allocator;
        this.len = 0;
        this.cap = 0;
        return result;
    }

    pub fn slice(this: *const StoreBytes): []u8 {
        if (this.ptr != null) {
            return this.ptr.?[0..this.len];
        }
        return "";
    }

    pub fn allocatedSlice(this: *const StoreBytes): []u8 {
        if (this.ptr != null) {
            return this.ptr.?[0..this.cap];
        }
        return "";
    }

    pub fn deinit(*this): void {
        default_allocator.free(this.stored_name.slice());
        if (this.ptr != null) {
            this.allocator.free(this.ptr.?[0..this.cap]);
        }
        this.ptr = null;
        this.len = 0;
        this.cap = 0;
    }

    pub fn asArrayList(this: *const StoreBytes): ArrayListUnmanaged(u8) {
        return this.asArrayListLeak();
    }

    pub fn asArrayListLeak(this: *const StoreBytes): ArrayListUnmanaged(u8) {
        return .{
            .items = if (this.ptr != null) this.ptr.?[0..this.len] else &.{},
            .capacity = this.cap,
        };
    }
}

export class Store {
    data: Data;
    mime_type: MimeType = .none;
    ref_count: AtomicValue(u32) = AtomicValue(u32).init(1);
    is_all_ascii: ?bool = null;
    allocator: Allocator;

    pub const File = StoreFile;
    pub const S3 = StoreS3;
    pub const Bytes = StoreBytes;

    pub fn memoryCost(this: *const Store): usize {
        return if (this.hasOneRef()) @sizeOf(Store) + switch (this.data) {
            .bytes => this.data.bytes.len,
            .file => 0,
            .s3 => |s3| s3.estimatedSize(),
        } else 0;
    }

    pub fn getPath(this: *const Store): ?string {
        return switch (this.data) {
            .bytes => |*bytes| if (bytes.stored_name.len > 0) bytes.stored_name.slice() else null,
            .file => |*file| if (file.pathlike == .path) file.pathlike.path.slice() else null,
            .s3 => |*s3| s3.pathlike.slice(),
        };
    }

    pub fn size(this: *const Store): SizeType {
        return switch (this.data) {
            .bytes => this.data.bytes.len,
            .s3, .file => Blob.max_size,
        };
    }

    pub const Map = HashMap(u64, *WebCore.Blob.Store, IdentityContext(u64), 80);

    pub const Data = union(enum) {
        bytes: StoreBytes,
        file: StoreFile,
        s3: StoreS3,
    };

    pub fn ref(*this): void {
        let old = this.ref_count.fetchAdd(1, .monotonic);
        assert(old > 0);
    }

    pub fn hasOneRef(this: *const Store): bool {
        return this.ref_count.load(.monotonic) == 1;
    }

    pub fn toAnyBlob(*this): ?Blob.Any {
        if (this.hasOneRef()) {
            if (this.data == .bytes) {
                return .{ .InternalBlob = this.data.bytes.toInternalBlob() };
            }
        }
        return null;
    }

    @c pub fn external(ptr: ?*anyopaque, _: ?*anyopaque, _: usize): void {
        if (ptr == null) return;
        let this_store: *Store = bun.cast(*Store, ptr.?);
        this_store.deref();
    }

    pub static fn initS3WithReferencedCredentials(pathlike: node.PathLike, mime_type: ?MimeType, credentials: *S3Credentials, allocator: Allocator): !*Store {
        let path = pathlike;
        path.toThreadSafe();

        return bun.new(Store, .{
            .data = .{
                .s3 = StoreS3.initWithReferencedCredentials(
                    path,
                    mime_type ?? brk: {
                        let sliced = path.slice();
                        if (sliced.len > 0) {
                            let extname_raw = std.fs.path.extension(sliced);
                            let extname = std.mem.trim(u8, extname_raw, ".");
                            if (MimeType.byExtensionNoDefault(extname)) |mime| {
                                break :brk mime;
                            }
                        }
                        break :brk null;
                    },
                    credentials,
                ),
            },
            .allocator = allocator,
            .ref_count = AtomicValue(u32).init(1),
        });
    }

    pub static fn initS3(pathlike: node.PathLike, mime_type: ?MimeType, credentials: S3Credentials, allocator: Allocator): !*Store {
        let path = pathlike;
        path.toThreadSafe();

        return bun.new(Store, .{
            .data = .{
                .s3 = StoreS3.init(
                    path,
                    mime_type ?? brk: {
                        let sliced = path.slice();
                        if (sliced.len > 0) {
                            let extname_raw = std.fs.path.extension(sliced);
                            let extname = std.mem.trim(u8, extname_raw, ".");
                            if (MimeType.byExtensionNoDefault(extname)) |mime| {
                                break :brk mime;
                            }
                        }
                        break :brk null;
                    },
                    credentials,
                ),
            },
            .allocator = allocator,
            .ref_count = AtomicValue(u32).init(1),
        });
    }

    pub static fn initFile(pathlike: Node.PathOrFileDescriptor, mime_type: ?MimeType, allocator: Allocator): !*Store {
        return bun.new(Store, .{
            .data = .{
                .file = StoreFile.init(
                    pathlike,
                    mime_type ?? brk: {
                        if (pathlike == .path) {
                            let sliced = pathlike.path.slice();
                            if (sliced.len > 0) {
                                let extname_raw = std.fs.path.extension(sliced);
                                let extname = std.mem.trim(u8, extname_raw, ".");
                                if (MimeType.byExtensionNoDefault(extname)) |mime| {
                                    break :brk mime;
                                }
                            }
                        }
                        break :brk null;
                    },
                ),
            },
            .allocator = allocator,
            .ref_count = AtomicValue(u32).init(1),
        });
    }

    pub static fn init(bytes: []u8, allocator: Allocator): *Store {
        return bun.new(Store, .{
            .data = .{
                .bytes = StoreBytes.init(bytes, allocator),
            },
            .allocator = allocator,
            .ref_count = AtomicValue(u32).init(1),
        });
    }

    pub fn sharedView(this: *const Store): []u8 {
        if (this.data == .bytes)
            return this.data.bytes.slice();
        return "";
    }

    pub fn deref(*this): void {
        let old = this.ref_count.fetchSub(1, .monotonic);
        assert(old >= 1);
        if (old == 1) {
            this.deinit();
        }
    }

    pub fn deinit(*this): void {
        let allocator = this.allocator;
        switch (this.data) {
            .bytes => |*bytes| {
                bytes.deinit();
            },
            .file => |file| {
                if (file.pathlike == .path) {
                    switch (file.pathlike.path) {
                        .string => allocator.free(@constCast(file.pathlike.path.slice())),
                        else => file.pathlike.path.deinit(),
                    }
                }
            },
            .s3 => |*s3| {
                s3.deinit(allocator);
            },
        }
        destroy(this);
    }

    pub const SerializeTag = enum(u8) {
        file = 0,
        bytes = 1,
        empty = 2,
    };

    pub fn serialize(*this, @comptime Writer: type, writer: Writer): !void {
        switch (this.data) {
            .file => |file| {
                switch (file.pathlike) {
                    .fd => |fd| {
                        try writer.writeInt(u8, 0, .little);
                        try writer.writeStruct(fd);
                    },
                    .path => |path| {
                        try writer.writeInt(u8, 1, .little);
                        let path_slice = path.slice();
                        try writer.writeInt(u32, @as(u32, @truncate(path_slice.len)), .little);
                        try writer.writeAll(path_slice);
                    },
                }
            },
            .s3 => |s3| {
                try writer.writeInt(u8, 1, .little);
                let path_slice = s3.pathlike.slice();
                try writer.writeInt(u32, @as(u32, @truncate(path_slice.len)), .little);
                try writer.writeAll(path_slice);
            },
            .bytes => |bytes| {
                let slice = bytes.slice();
                try writer.writeInt(u32, @truncate(slice.len), .little);
                try writer.writeAll(slice);
                try writer.writeInt(u32, @truncate(bytes.stored_name.slice().len), .little);
                try writer.writeAll(bytes.stored_name.slice());
            },
        }
    }

    pub static fn fromArrayList(list: ArrayListUnmanaged(u8), allocator: Allocator): !*Store {
        return Store.init(list.items, allocator);
    }
}
