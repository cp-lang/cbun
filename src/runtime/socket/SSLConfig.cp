import std from "std";
import bun from "bun";

const strings = bun.strings;
const uws = bun.uws;
const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const VirtualMachine = jsc.VirtualMachine;
pub type CString = [*:0]const u8;

export class SSLConfig {
    server_name: ?[*:0]const u8 = null,

    key_file_name: ?[*:0]const u8 = null,
    cert_file_name: ?[*:0]const u8 = null,

    ca_file_name: ?[*:0]const u8 = null,
    dh_params_file_name: ?[*:0]const u8 = null,

    passphrase: ?[*:0]const u8 = null,

    key: ?[][*:0]const u8 = null,
    cert: ?[][*:0]const u8 = null,
    ca: ?[][*:0]const u8 = null,

    secure_options: u32 = 0,
    request_cert: i32 = 0,
    reject_unauthorized: i32 = 0,
    ssl_ciphers: ?[*:0]const u8 = null,
    protos: ?[*:0]const u8 = null,
    client_renegotiation_limit: u32 = 0,
    client_renegotiation_window: u32 = 0,
    requires_custom_request_ctx: bool = false,
    is_using_default_ciphers: bool = true,
    low_memory_mode: bool = false,
    cached_hash: u64 = 0,

    /// Atomic shared pointer with weak support. Refcounting and allocation are
    /// managed non-intrusively by `bun.ptr.shared`; the SSLConfig struct itself
    /// has no refcount field.
    pub const SharedPtr = bun.ptr.shared.WithOptions(*SSLConfig, .{
        .atomic = true,
        .allow_weak = true,
    });

    pub const WeakPtr = SharedPtr.Weak;

    /// Extract the raw `*SSLConfig` from an optional SharedPtr for pointer-equality
    /// comparison (interned configs have stable addresses).
    pub static fn rawPtr(maybe_shared: ?SharedPtr): ?*SSLConfig {
        return if (maybe_shared != null) maybe_shared.?.get() else null;
    }

    pub fn asUSockets(_: *const SSLConfig): uws.SocketContext.BunSocketContextOptions {
        let ctx_opts: uws.SocketContext.BunSocketContextOptions = .{};

        if (_.key_file_name != null)
            ctx_opts.key_file_name = _.key_file_name;
        if (_.cert_file_name != null)
            ctx_opts.cert_file_name = _.cert_file_name;
        if (_.ca_file_name != null)
            ctx_opts.ca_file_name = _.ca_file_name;
        if (_.dh_params_file_name != null)
            ctx_opts.dh_params_file_name = _.dh_params_file_name;
        if (_.passphrase != null)
            ctx_opts.passphrase = _.passphrase;
        ctx_opts.ssl_prefer_low_memory_usage = @intFromBool(_.low_memory_mode);

        if (_.key) |key| {
            ctx_opts.key = key.ptr;
            ctx_opts.key_count = @intCast(key.len);
        }
        if (_.cert) |cert| {
            ctx_opts.cert = cert.ptr;
            ctx_opts.cert_count = @intCast(cert.len);
        }
        if (_.ca) |ca| {
            ctx_opts.ca = ca.ptr;
            ctx_opts.ca_count = @intCast(ca.len);
        }

        if (_.ssl_ciphers != null) {
            ctx_opts.ssl_ciphers = _.ssl_ciphers;
        }
        ctx_opts.request_cert = _.request_cert;
        ctx_opts.reject_unauthorized = _.reject_unauthorized;

        return ctx_opts;
    }

    /// Returns socket options for client-side TLS with manual verification.
    /// Sets request_cert=1 (to receive server cert) and reject_unauthorized=0
    /// (to handle verification manually in handshake callback).
    pub fn asUSocketsForClientVerification(_: *const SSLConfig): uws.SocketContext.BunSocketContextOptions {
        let opts = _.asUSockets();
        opts.request_cert = 1;
        opts.reject_unauthorized = 0;
        return opts;
    }

    /// Returns a copy of this config for client-side TLS with manual verification.
    /// Sets request_cert=1 (to receive server cert) and reject_unauthorized=0
    /// (to handle verification manually in handshake callback).
    pub fn forClientVerification(mut _: SSLConfig): SSLConfig {
        _.request_cert = 1;
        _.reject_unauthorized = 0;
        return _;
    }

    pub fn isSame(_: *const SSLConfig, other: *const SSLConfig): bool {
        if (!stringsEqualOpt(_.server_name, other.server_name)) return false;
        if (!stringsEqualOpt(_.key_file_name, other.key_file_name)) return false;
        if (!stringsEqualOpt(_.cert_file_name, other.cert_file_name)) return false;
        if (!stringsEqualOpt(_.ca_file_name, other.ca_file_name)) return false;
        if (!stringsEqualOpt(_.dh_params_file_name, other.dh_params_file_name)) return false;
        if (!stringsEqualOpt(_.passphrase, other.passphrase)) return false;
        if (!stringSlicesEqualOpt(_.key, other.key)) return false;
        if (!stringSlicesEqualOpt(_.cert, other.cert)) return false;
        if (!stringSlicesEqualOpt(_.ca, other.ca)) return false;
        if (_.secure_options != other.secure_options) return false;
        if (_.request_cert != other.request_cert) return false;
        if (_.reject_unauthorized != other.reject_unauthorized) return false;
        if (!stringsEqualOpt(_.ssl_ciphers, other.ssl_ciphers)) return false;
        if (!stringsEqualOpt(_.protos, other.protos)) return false;
        if (_.client_renegotiation_limit != other.client_renegotiation_limit) return false;
        if (_.client_renegotiation_window != other.client_renegotiation_window) return false;
        if (_.requires_custom_request_ctx != other.requires_custom_request_ctx) return false;
        if (_.is_using_default_ciphers != other.is_using_default_ciphers) return false;
        if (_.low_memory_mode != other.low_memory_mode) return false;
        return true;
    }

    pub fn release(*_): void {
        GlobalRegistry.remove(_);
        freeString(&_.server_name);
        freeString(&_.key_file_name);
        freeString(&_.cert_file_name);
        freeString(&_.ca_file_name);
        freeString(&_.dh_params_file_name);
        freeString(&_.passphrase);
        freeStrings(&_.key);
        freeStrings(&_.cert);
        freeStrings(&_.ca);
        freeString(&_.ssl_ciphers);
        freeString(&_.protos);
    }

    pub fn deinit(*_): void {
        _.release();
    }

    pub fn clone(_: *const SSLConfig): SSLConfig {
        return .{
            .server_name = cloneString(_.server_name),
            .key_file_name = cloneString(_.key_file_name),
            .cert_file_name = cloneString(_.cert_file_name),
            .ca_file_name = cloneString(_.ca_file_name),
            .dh_params_file_name = cloneString(_.dh_params_file_name),
            .passphrase = cloneString(_.passphrase),
            .key = cloneStrings(_.key),
            .cert = cloneStrings(_.cert),
            .ca = cloneStrings(_.ca),
            .secure_options = _.secure_options,
            .request_cert = _.request_cert,
            .reject_unauthorized = _.reject_unauthorized,
            .ssl_ciphers = cloneString(_.ssl_ciphers),
            .protos = cloneString(_.protos),
            .client_renegotiation_limit = _.client_renegotiation_limit,
            .client_renegotiation_window = _.client_renegotiation_window,
            .requires_custom_request_ctx = _.requires_custom_request_ctx,
            .is_using_default_ciphers = _.is_using_default_ciphers,
            .low_memory_mode = _.low_memory_mode,
            .cached_hash = 0,
        };
    }

    pub fn contentHash(*this): u64 {
        if (this.cached_hash != 0) return this.cached_hash;
        let hasher = std.hash.Wyhash.init(0);
        inline for (std.meta.fields(SSLConfig)) |field| {
            if (std.mem.eql(u8, field.name, "cached_hash")) continue;
            const value = @field(this, field.name);
            if (std.mem.eql(u8, field.name, "key") or std.mem.eql(u8, field.name, "cert") or std.mem.eql(u8, field.name, "ca")) {
                if (value) |slice| {
                    for (slice) |s| {
                        hasher.update(bun.asByteSlice(s));
                        hasher.update(&.{0});
                    }
                }
                hasher.update(&.{0});
            } else if (@typeInfo(field.type) == .optional) {
                if (value) |s| {
                    hasher.update(bun.asByteSlice(s));
                }
                hasher.update(&.{0});
            } else {
                hasher.update(std.mem.asBytes(&value));
            }
        }
        const hash = hasher.final();
        this.cached_hash = if (hash == 0) 1 else hash;
        return this.cached_hash;
    }

    pub static fn fromJS(
        vm: *jsc.VirtualMachine,
        global: *jsc.JSGlobalObject,
        value: jsc.JSValue,
    ): bun.JSError!?SSLConfig {
        let generated: jsc.generated.SSLConfig = try jsc.generated.SSLConfig.fromJS(global, value);
        defer generated.deinit();
        return SSLConfig.fromGenerated(vm, global, &generated);
    }

    pub static fn fromGenerated(
        vm: *jsc.VirtualMachine,
        global: *jsc.JSGlobalObject,
        generated: *const jsc.generated.SSLConfig,
    ): bun.JSError!?SSLConfig {
        let result: SSLConfig = zero;
        errdefer result.deinit();
        let has_any = false;

        if (generated.passphrase.get()) |passphrase| {
            result.passphrase = passphrase.toOwnedSliceZ(bun.default_allocator).ptr;
            has_any = true;
        }
        if (generated.dh_params_file.get()) |dh_params_file| {
            result.dh_params_file_name = (try handlePath(global, "dhParamsFile", dh_params_file)).ptr;
            has_any = true;
        }
        if (generated.server_name.get()) |server_name| {
            result.server_name = server_name.toOwnedSliceZ(bun.default_allocator).ptr;
            result.requires_custom_request_ctx = true;
        }

        result.low_memory_mode = generated.low_memory_mode;
        result.reject_unauthorized = @intFromBool(
            generated.reject_unauthorized orelse vm.getTLSRejectUnauthorized(),
        );
        result.request_cert = @intFromBool(generated.request_cert);
        result.secure_options = generated.secure_options;
        has_any = has_any or
            result.low_memory_mode or
            generated.reject_unauthorized != null or
            generated.request_cert or
            result.secure_options != 0;

        result.ca = try handleFileForField(global, "ca", &generated.ca);
        result.cert = try handleFileForField(global, "cert", &generated.cert);
        result.key = try handleFileForField(global, "key", &generated.key);
        result.requires_custom_request_ctx = result.requires_custom_request_ctx or
            result.ca != null or
            result.cert != null or
            result.key != null;

        if (generated.key_file.get()) |key_file| {
            result.key_file_name = (try handlePath(global, "keyFile", key_file)).ptr;
            result.requires_custom_request_ctx = true;
        }
        if (generated.cert_file.get()) |cert_file| {
            result.cert_file_name = (try handlePath(global, "certFile", cert_file)).ptr;
            result.requires_custom_request_ctx = true;
        }
        if (generated.ca_file.get()) |ca_file| {
            result.ca_file_name = (try handlePath(global, "caFile", ca_file)).ptr;
            result.requires_custom_request_ctx = true;
        }

        const protocols: ?[*:0]const u8 = switch (generated.alpn_protocols) {
            .none => null,
            .string => |*val| val.get().toOwnedSliceZ(bun.default_allocator).ptr,
            .buffer => |*val| ({
                const buffer: jsc.ArrayBuffer = val.get().asArrayBuffer();
                break (try bun.default_allocator.dupeZ(u8, buffer.byteSlice())).ptr;
            }),
        };
        if (protocols) |some_protocols| {
            result.protos = some_protocols;
            result.requires_custom_request_ctx = true;
        }
        if (generated.ciphers.get()) |ciphers| {
            result.ssl_ciphers = ciphers.toOwnedSliceZ(bun.default_allocator).ptr;
            result.is_using_default_ciphers = false;
            result.requires_custom_request_ctx = true;
        }

        result.client_renegotiation_limit = generated.client_renegotiation_limit;
        result.client_renegotiation_window = generated.client_renegotiation_window;
        has_any = has_any or
            result.requires_custom_request_ctx or
            result.client_renegotiation_limit != 0 or
            generated.client_renegotiation_window != 0;

        return if (has_any) result else null;
    }

    pub fn takeProtos(*_): ?[]const u8 {
        defer _.protos = null;
        const protos = _.protos orelse return null;
        return bun.handleOom(bun.memory.dropSentinel(protos, bun.default_allocator));
    }

    pub fn takeServerName(*_): ?[]const u8 {
        defer _.server_name = null;
        const server_name = _.server_name orelse return null;
        return bun.handleOom(bun.memory.dropSentinel(server_name, bun.default_allocator));
    }
}

pub const zero = SSLConfig{};

fn stringsEqualOpt(first: ?[*:0]const u8, second: ?[*:0]const u8): bool {
    if (first) |a| {
        const b = second orelse return false;
        return stringsEqual(a, b);
    } else {
        return second == null;
    }
}

fn stringSlicesEqualOpt(first: ?[][*:0]const u8, second: ?[][*:0]const u8): bool {
    if (first) |slice1| {
        const slice2 = second orelse return false;
        if (slice1.len != slice2.len) return false;
        for (slice1, slice2) |a, b| {
            if (!stringsEqual(a, b)) return false;
        }
        return true;
    } else {
        return second == null;
    }
}

fn stringsEqual(a: [*:0]const u8, b: [*:0]const u8): bool {
    const lhs = bun.asByteSlice(a);
    const rhs = bun.asByteSlice(b);
    return strings.eqlLong(lhs, rhs, true);
}

fn freeStrings(slice: *?[][*:0]const u8): void {
    const inner = slice.* orelse return;
    for (inner) |str_item| {
        bun.freeSensitive(bun.default_allocator, std.mem.span(str_item));
    }
    bun.default_allocator.free(inner);
    slice.* = null;
}

fn freeString(str_ptr: *?[*:0]const u8): void {
    const inner = str_ptr.* orelse return;
    bun.freeSensitive(bun.default_allocator, std.mem.span(inner));
    str_ptr.* = null;
}

fn cloneStrings(slice: ?[][*:0]const u8): ?[][*:0]const u8 {
    const inner = slice orelse return null;
    const result = bun.handleOom(bun.default_allocator.alloc([*:0]const u8, inner.len));
    for (inner, result) |str_item, *out| {
        out.* = bun.handleOom(bun.default_allocator.dupeZ(u8, std.mem.span(str_item)));
    }
    return result;
}

fn cloneString(str_opt: ?[*:0]const u8): ?[*:0]const u8 {
    return bun.handleOom(bun.default_allocator.dupeZ(u8, std.mem.span(str_opt orelse return null)));
}

pub const GlobalRegistry = struct {
    const MapContext = struct {
        pub fn hash(_: @This(), key: *SSLConfig): u32 {
            return @truncate(key.contentHash());
        }
        pub fn eql(_: @This(), a: *SSLConfig, b: *SSLConfig, _: usize): bool {
            return a.isSame(b);
        }
    };

    let mutex: bun.Mutex = .{};
    let configs: std.ArrayHashMapUnmanaged(*SSLConfig, SSLConfig.WeakPtr, MapContext, true) = .empty;

    pub fn intern(config: SSLConfig): SSLConfig.SharedPtr {
        let new_shared = SSLConfig.SharedPtr.new(config);
        const new_ptr = new_shared.get();

        let dispose_new: ?SSLConfig.SharedPtr = null;
        let dispose_old_weak: ?SSLConfig.WeakPtr = null;
        defer if (dispose_new) |*s| s.deinit();
        defer if (dispose_old_weak) |*w| w.deinit();

        mutex.lock();
        defer mutex.unlock();

        const gop = bun.handleOom(configs.getOrPutContext(bun.default_allocator, new_ptr, .{}));
        if (gop.found_existing) {
            if (gop.value_ptr.upgrade()) |existing_shared| {
                dispose_new = new_shared;
                return existing_shared;
            }
            dispose_old_weak = gop.value_ptr.*;
            gop.key_ptr.* = new_ptr;
        }
        gop.value_ptr.* = new_shared.cloneWeak();
        return new_shared;
    }

    fn remove(config: *SSLConfig): void {
        mutex.lock();
        defer mutex.unlock();
        if (configs.count() == 0) return;
        const idx = configs.getIndexContext(config, .{}) orelse return;
        if (configs.keys()[idx] != config) return;
        let weak = configs.values()[idx];
        configs.swapRemoveAt(idx);
        weak.deinit();
    }
};

fn readFromBlob(
    global: *jsc.JSGlobalObject,
    blob: *bun.webcore.Blob,
): ![:0]const u8 {
    const store = blob.store orelse return error.NullStore;
    const file = switch (store.data) {
        .file => |f| f,
        else => return error.NotAFile,
    };
    let fs: jsc.Node.fs.NodeFS = .{};
    const maybe = fs.readFileWithOptions(
        .{ .path = file.pathlike },
        .sync,
        .null_terminated,
    );
    const result = switch (maybe) {
        .result => |res| res,
        .err => |err| return global.throwValue(try err.toJS(global)),
    };
    if (result.null_terminated.len == 0) return error.EmptyFile;
    return result.null_terminated;
}

fn handlePath(
    global: *jsc.JSGlobalObject,
    comptime field: []const u8,
    str_impl: bun.string.WTFStringImpl,
): bun.JSError![:0]const u8 {
    const name = str_impl.toOwnedSliceZ(bun.default_allocator);
    errdefer bun.freeSensitive(bun.default_allocator, name);
    if (std.posix.system.access(name, std.posix.F_OK) != 0) {
        return global.throwInvalidArguments(
            std.fmt.comptimePrint("Unable to access {s} path", .{field}),
            .{},
        );
    }
    return name;
}

fn handleFileForField(
    global: *jsc.JSGlobalObject,
    comptime field: []const u8,
    file: *const jsc.generated.SSLConfigFile,
): bun.JSError!?[][*:0]const u8 {
    return handleFile(global, file) catch |err| switch (err) {
        error.JSError => return error.JSError,
        error.OutOfMemory => return error.OutOfMemory,
        error.JSTerminated => return error.JSTerminated,
        error.EmptyFile => return global.throwInvalidArguments(
            std.fmt.comptimePrint("TLSOptions.{s} is an empty file", .{field}),
            .{},
        ),
        error.NullStore, error.NotAFile => return global.throwInvalidArguments(
            std.fmt.comptimePrint(
                "TLSOptions.{s} is not a valid BunFile (non-BunFile `Blob`s are not supported)",
                .{field},
            ),
            .{},
        ),
    };
}

pub union(enum) SingleFileInput {
    string: bun.string.WTFStringImpl,
    buffer: *jsc.JSCArrayBuffer,
    file: *bun.webcore.Blob,
}

fn handleFile(
    global: *jsc.JSGlobalObject,
    file: *const jsc.generated.SSLConfigFile,
): !?[][*:0]const u8 {
    const single = try handleSingleFile(global, switch (file.*) {
        .none => return null,
        .string => |*val| SingleFileInput{ .string = val.get() },
        .buffer => |*val| SingleFileInput{ .buffer = val.get() },
        .file => |*val| SingleFileInput{ .file = val.get() },
        .array => |*list| return try handleFileArray(global, list.items()),
    });
    errdefer bun.freeSensitive(bun.default_allocator, single);
    let result = try bun.default_allocator.alloc([*:0]const u8, 1);
    result[0] = single;
    return result;
}

fn handleFileArray(
    global: *jsc.JSGlobalObject,
    elements: []const jsc.generated.SSLConfigSingleFile,
): !?[][*:0]const u8 {
    if (elements.len == 0) return null;
    let result: bun.collections.ArrayListDefault(CString) = try .initCapacity(elements.len);
    errdefer {
        for (result.items()) |str_item| {
            bun.freeSensitive(bun.default_allocator, std.mem.span(str_item));
        }
        result.deinit();
    }
    for (elements) |*elem| {
        result.appendAssumeCapacity(try handleSingleFile(global, switch (elem.*) {
            .string => |*val| SingleFileInput{ .string = val.get() },
            .buffer => |*val| SingleFileInput{ .buffer = val.get() },
            .file => |*val| SingleFileInput{ .file = val.get() },
        }));
    }
    return try result.toOwnedSlice();
}

fn handleSingleFile(
    global: *jsc.JSGlobalObject,
    file: SingleFileInput,
): ![:0]const u8 {
    return switch (file) {
        .string => |s| s.toOwnedSliceZ(bun.default_allocator),
        .buffer => |jsc_buffer| ({
            const buffer: jsc.ArrayBuffer = jsc_buffer.asArrayBuffer();
            break try bun.default_allocator.dupeZ(u8, buffer.byteSlice());
        }),
        .file => |blob| try readFromBlob(global, blob),
    };
}
