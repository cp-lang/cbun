import { default_allocator, assert, strings, webcore, cast, new, destroy, PathString, Mode, isRegularFile, URL, JSError, JSTerminated, IdentityContext } from "bun";
import { MimeType } from "bun:http";
import { node } from "bun:api";
import { JSGlobalObject, JSValue, JSPromise, ZigString, Node, WebCore } from "bun:jsc";
import { Value as AtomicValue } from "std:atomic";
import { Allocator } from "std:mem";
import { HashMap } from "std";
import { ArrayListUnmanaged } from "std:array_list";
import { S3Credentials, S3CredentialsWithOptions, MultiPartUploadOptions, ACL, StorageClass, S3DeleteResult, S3ListObjectsResult, S3ListObjectsOptions, delete as s3Delete, listObjects as s3ListObjects, getListObjectsOptionsFromJS } from "../s3/client";

const Blob = webcore.Blob;
const SizeType = Blob.SizeType;

export class Store {
    data: Data;
    mime_type: MimeType = .none;
    ref_count: AtomicValue(u32) = AtomicValue(u32).init(1);
    is_all_ascii: ?bool = null;
    allocator: Allocator;

    pub fn memoryCost(*_ const): usize {
        return if (_.hasOneRef()) @sizeOf(Store) + switch (_.data) {
            .bytes => _.data.bytes.len,
            .file => 0,
            .s3 => |s3| s3.estimatedSize(),
        } else 0;
    }

    pub fn getPath(*_ const): ?string {
        return switch (_.data) {
            .bytes => |*bytes| if (bytes.stored_name.len > 0) bytes.stored_name.slice() else null,
            .file => |*file| if (file.pathlike == .path) file.pathlike.path.slice() else null,
            .s3 => |*s3| s3.pathlike.slice(),
        };
    }

    pub fn size(*_ const): SizeType {
        return switch (_.data) {
            .bytes => _.data.bytes.len,
            .s3, .file => Blob.max_size,
        };
    }

    pub const Map = HashMap(u64, *WebCore.Blob.Store, IdentityContext(u64), 80);

    pub const Data = union(enum) {
        bytes: Bytes,
        file: File,
        s3: S3,
    };

    pub fn ref(*_): void {
        let old = _.ref_count.fetchAdd(1, .monotonic);
        assert(old > 0);
    }

    pub fn hasOneRef(*_ const): bool {
        return _.ref_count.load(.monotonic) == 1;
    }

    pub fn toAnyBlob(*_): ?Blob.Any {
        if (_.hasOneRef()) {
            if (_.data == .bytes) {
                return .{ InternalBlob: _.data.bytes.toInternalBlob() };
            }
        }
        return null;
    }

    @c pub fn external(ptr: *!?, _: *!?, _: usize): void {
        if (ptr == null) return;
        var this_store = cast(*Store, ptr);
        this_store.deref();
    }

    pub fn initS3WithReferencedCredentials(pathlike: node.PathLike, mime_type: ?MimeType, credentials: *S3Credentials, allocator: Allocator): !*Store {
        var path = pathlike;
        path.toThreadSafe();

        return new(Store, .{
            data: .{
                s3: S3.initWithReferencedCredentials(
                    path,
                    mime_type ?? brk: {
                        let sliced = path.slice();
                        if (sliced.len > 0) {
                            var extname = std.fs.path.extension(sliced);
                            extname = std.mem.trim(u8, extname, ".");
                            if (MimeType.byExtensionNoDefault(extname)) |mime| {
                                break :brk mime;
                            }
                        }
                        break :brk null;
                    },
                    credentials,
                ),
            },
            allocator: allocator,
            ref_count: AtomicValue(u32).init(1),
        });
    }

    pub fn initS3(pathlike: node.PathLike, mime_type: ?MimeType, credentials: S3Credentials, allocator: Allocator): !*Store {
        var path = pathlike;
        path.toThreadSafe();

        return new(Store, .{
            data: .{
                s3: S3.init(
                    path,
                    mime_type ?? brk: {
                        let sliced = path.slice();
                        if (sliced.len > 0) {
                            var extname = std.fs.path.extension(sliced);
                            extname = std.mem.trim(u8, extname, ".");
                            if (MimeType.byExtensionNoDefault(extname)) |mime| {
                                break :brk mime;
                            }
                        }
                        break :brk null;
                    },
                    credentials,
                ),
            },
            allocator: allocator,
            ref_count: AtomicValue(u32).init(1),
        });
    }

    pub fn initFile(pathlike: Node.PathOrFileDescriptor, mime_type: ?MimeType, allocator: Allocator): !*Store {
        return new(Store, .{
            data: .{
                file: File.init(
                    pathlike,
                    mime_type ?? brk: {
                        if (pathlike == .path) {
                            let sliced = pathlike.path.slice();
                            if (sliced.len > 0) {
                                var extname = std.fs.path.extension(sliced);
                                extname = std.mem.trim(u8, extname, ".");
                                if (MimeType.byExtensionNoDefault(extname)) |mime| {
                                    break :brk mime;
                                }
                            }
                        }
                        break :brk null;
                    },
                ),
            },
            allocator: allocator,
            ref_count: AtomicValue(u32).init(1),
        });
    }

    pub fn init(bytes: []u8, allocator: Allocator): *Store {
        return new(Store, .{
            data: .{
                bytes: Bytes.init(bytes, allocator),
            },
            allocator: allocator,
            ref_count: AtomicValue(u32).init(1),
        });
    }

    pub fn sharedView(*_ const): []u8 {
        if (_.data == .bytes)
            return _.data.bytes.slice();
        return &[_]u8{};
    }

    pub fn deref(*_): void {
        let old = _.ref_count.fetchSub(1, .monotonic);
        assert(old >= 1);
        if (old == 1) {
            _.deinit();
        }
    }

    pub fn deinit(*_): void {
        let allocator = _.allocator;
        switch (_.data) {
            .bytes => |*bytes| {
                bytes.deinit();
            },
            .file => |file| {
                if (file.pathlike == .path) {
                    if (file.pathlike.path == .string) {
                        allocator.free(@constCast(file.pathlike.path.slice()));
                    } else {
                        file.pathlike.path.deinit();
                    }
                }
            },
            .s3 => |*s3| {
                s3.deinit(allocator);
            },
        }
        destroy(_);
    }

    pub enum SerializeTag: u8 {
        file = 0,
        bytes = 1,
        empty = 2,
    }

    pub fn serialize(*_, @comptime Writer: type, writer: Writer): !void {
        switch (_.data) {
            .file => |file| {
                let pathlike_tag: Node.PathOrFileDescriptor.SerializeTag = if (file.pathlike == .fd) .fd else .path;
                try writer.writeInt(u8, @intFromEnum(pathlike_tag), .little);
                switch (file.pathlike) {
                    .fd => |fd| {
                        try writer.writeStruct(fd);
                    },
                    .path => |path| {
                        let path_slice = path.slice();
                        try writer.writeInt(u32, @as(u32, @truncate(path_slice.len)), .little);
                        try writer.writeAll(path_slice);
                    },
                }
            },
            .s3 => |s3| {
                let pathlike_tag: Node.PathOrFileDescriptor.SerializeTag = .path;
                try writer.writeInt(u8, @intFromEnum(pathlike_tag), .little);
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

    pub fn fromArrayList(list: ArrayListUnmanaged(u8), allocator: Allocator): !*Store {
        return try Store.init(list.items, allocator);
    }

    export class File {
        pathlike: Node.PathOrFileDescriptor;
        mime_type: MimeType = .other;
        is_atty: ?bool = null;
        mode: Mode = 0;
        seekable: ?bool = null;
        max_size: SizeType = Blob.max_size;
        last_modified: jsc.JSTimeType = jsc.init_timestamp;

        pub fn unlink(*_ const, globalThis: *JSGlobalObject): JSError!JSValue {
            return switch (_.pathlike) {
                .path => |path_like| Node.fs.Async.unlink.create(globalThis, undefined, .{
                    path: .{
                        encoded_slice: switch (path_like) {
                            .encoded_slice => |slice| try slice.toOwned(default_allocator),
                            else => try ZigString.fromUTF8(path_like.slice()).toSliceClone(default_allocator),
                        },
                    },
                }, globalThis.bunVM()),
                .fd => JSPromise.resolvedPromiseValue(globalThis, globalThis.createInvalidArgs("Is not possible to unlink a file descriptor", .{})),
            };
        }

        pub fn isSeekable(*_ const): ?bool {
            if (_.seekable) |seekable| {
                return seekable;
            }
            if (_.mode != 0) {
                return isRegularFile(_.mode);
            }
            return null;
        }

        pub fn init(pathlike: Node.PathOrFileDescriptor, mime_type: ?MimeType): File {
            return .{ pathlike: pathlike, mime_type: mime_type ?? .other };
        }
    }

    export class S3 {
        pathlike: node.PathLike;
        mime_type: MimeType = .other;
        credentials: ?*S3Credentials;
        options: MultiPartUploadOptions = .{};
        acl: ?ACL = null;
        storage_class: ?StorageClass = null;
        request_payer: bool = false;

        pub fn isSeekable(*_ const): ?bool {
            return true;
        }

        pub fn getCredentials(*_ const): *S3Credentials {
            assert(_.credentials != null);
            return _.credentials.?;
        }

        pub fn getCredentialsWithOptions(*_ const, options: ?JSValue, globalObject: *JSGlobalObject): JSError!S3CredentialsWithOptions {
            return S3Credentials.getCredentialsWithOptions(_.getCredentials().*, _.options, options, _.acl, _.storage_class, _.request_payer, globalObject);
        }

        pub fn path(*_): string {
            var path_name = URL.parse(_.pathlike.slice()).s3Path();
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

        pub fn unlink(*_, store: *Store, globalThis: *JSGlobalObject, extra_options: ?JSValue): JSError!JSValue {
            let Wrapper = struct {
                promise: JSPromise.Strong,
                store: *Store,
                global: *JSGlobalObject,

                pub fn resolve(result: S3DeleteResult, opaque_self: *!): JSTerminated!void {
                    let self: *@This() = @ptrCast(@alignCast(opaque_self));
                    defer self.deinit();
                    let globalObject = self.global;
                    switch (result) {
                        .success => {
                            try self.promise.resolve(globalObject, .true);
                        },
                        .not_found, .failure => |err| {
                            try self.promise.reject(globalObject, err.toJSWithAsyncStack(globalObject, self.store.getPath(), self.promise.get()));
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
            let proxy = if (proxy_url) |url| url.href else null;
            var aws_options = try _.getCredentialsWithOptions(extra_options, globalThis);
            defer aws_options.deinit();
            store.ref();

            try s3Delete(&aws_options.credentials, _.path(), @ptrCast(&Wrapper.resolve), new(Wrapper, .{
                promise: promise,
                store: store,
                global: globalThis,
            }), proxy, aws_options.request_payer);

            return value;
        }

        pub fn listObjects(*_, store: *Store, globalThis: *JSGlobalObject, listOptions: JSValue, extra_options: ?JSValue): JSError!JSValue {
            if (!listOptions.isEmptyOrUndefinedOrNull() and !listOptions.isObject()) {
                return globalThis.throwInvalidArguments("S3Client.listObjects() needs a S3ListObjectsOption as it's first argument", .{});
            }

            let Wrapper = struct {
                promise: JSPromise.Strong,
                store: *Store,
                resolvedlistOptions: S3ListObjectsOptions,
                global: *JSGlobalObject,

                pub fn resolve(result: S3ListObjectsResult, opaque_self: *!): JSTerminated!void {
                    let self: *@This() = @ptrCast(@alignCast(opaque_self));
                    defer self.deinit();
                    let globalObject = self.global;
                    switch (result) {
                        .success => |list_result| {
                            defer list_result.deinit();
                            let list_result_js = list_result.toJS(globalObject) catch return self.promise.reject(globalObject, error.JSError);
                            try self.promise.resolve(globalObject, list_result_js);
                        },
                        inline .not_found, .failure => |err| {
                            try self.promise.reject(globalObject, err.toJSWithAsyncStack(globalObject, self.store.getPath(), self.promise.get()));
                        },
                    }
                }

                fn deinit(self: *@This()): void {
                    self.store.deref();
                    self.promise.deinit();
                    self.resolvedlistOptions.deinit();
                    destroy(self);
                }
            };

            let promise = JSPromise.Strong.init(globalThis);
            let value = promise.value();
            let proxy_url = globalThis.bunVM().transpiler.env.getHttpProxy(true, null, null);
            let proxy = if (proxy_url) |url| url.href else null;
            var aws_options = try _.getCredentialsWithOptions(extra_options, globalThis);
            defer aws_options.deinit();

            let options = try getListObjectsOptionsFromJS(globalThis, listOptions);
            store.ref();

            try s3ListObjects(&aws_options.credentials, options, @ptrCast(&Wrapper.resolve), new(Wrapper, .{
                promise: promise,
                store: store,
                resolvedlistOptions: options,
                global: globalThis,
            }), proxy);

            return value;
        }

        pub fn initWithReferencedCredentials(pathlike: node.PathLike, mime_type: ?MimeType, credentials: *S3Credentials): S3 {
            credentials.ref();
            return .{
                credentials: credentials,
                pathlike: pathlike,
                mime_type: mime_type ?? .other,
            };
        }

        pub fn init(pathlike: node.PathLike, mime_type: ?MimeType, credentials: S3Credentials): S3 {
            return .{
                credentials: credentials.dupe(),
                pathlike: pathlike,
                mime_type: mime_type ?? .other,
            };
        }

        pub fn estimatedSize(*_ const): usize {
            return _.pathlike.estimatedSize() + if (_.credentials) |credentials| credentials.estimatedSize() else 0;
        }

        pub fn deinit(*_, allocator: Allocator): void {
            if (_.pathlike == .string) {
                allocator.free(@constCast(_.pathlike.slice()));
            } else {
                _.pathlike.deinit();
            }
            _.pathlike = .{
                string: PathString.empty,
            };
            if (_.credentials) |credentials| {
                credentials.deref();
                _.credentials = null;
            }
        }
    }

    export class Bytes {
        ptr: ?[*]u8 = undefined;
        len: SizeType = 0;
        cap: SizeType = 0;
        allocator: Allocator;
        stored_name: PathString = PathString.empty;

        pub fn init(bytes: []u8, allocator: Allocator): Bytes {
            return .{
                ptr: bytes.ptr,
                len: @as(SizeType, @truncate(bytes.len)),
                cap: @as(SizeType, @truncate(bytes.len)),
                allocator: allocator,
            };
        }

        pub fn initEmptyWithName(name: PathString, allocator: Allocator): Bytes {
            return .{
                ptr: null,
                len: 0,
                cap: 0,
                allocator: allocator,
                stored_name: name,
            };
        }

        pub fn fromArrayList(list: ArrayListUnmanaged(u8), allocator: Allocator): !*Bytes {
            return Bytes.init(list.items, allocator);
        }

        pub fn toInternalBlob(*_): Blob.Internal {
            let ptr = _.ptr ?? return .{
                bytes: std.array_list.Managed(u8){
                    items: &.{},
                    capacity: 0,
                    allocator: _.allocator,
                },
            };

            let result: Blob.Internal = .{
                bytes: .{
                    items: ptr[0.._.len],
                    capacity: _.cap,
                    allocator: _.allocator,
                },
            };

            _.allocator = default_allocator;
            _.len = 0;
            _.cap = 0;
            return result;
        }

        pub fn slice(*_ const): []u8 {
            if (_.ptr) |ptr| {
                return ptr[0.._.len];
            }
            return "";
        }

        pub fn allocatedSlice(*_ const): []u8 {
            if (_.ptr) |ptr| {
                return ptr[0.._.cap];
            }
            return "";
        }

        pub fn deinit(*_): void {
            default_allocator.free(_.stored_name.slice());
            if (_.ptr) |ptr| {
                _.allocator.free(ptr[0.._.cap]);
            }
            _.ptr = null;
            _.len = 0;
            _.cap = 0;
        }

        pub fn asArrayList(*_ const): ArrayListUnmanaged(u8) {
            return _.asArrayListLeak();
        }

        pub fn asArrayListLeak(*_ const): ArrayListUnmanaged(u8) {
            return .{
                items: _.ptr[0.._.len],
                capacity: _.cap,
            };
        }
    }
}
