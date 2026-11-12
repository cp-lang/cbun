import { Output, default_allocator, strings, webcore, http, jsc, String, sys, JSError, FormData, Environment, ptr } from "bun";
import { JSGlobalObject, JSValue, CallFrame, JSPromise, VirtualMachine, ZigString, ArrayBuffer, DOMFormData, URLSearchParams } from "jsc";
import { Store as BlobStore, read_file, write_file, copy_file } from "./blob/Store.zig";

const debug = Output.scoped(.Blob, .visible);

export class Blob {
    reported_estimated_size: usize = 0;
    size: SizeType = 0;
    offset: SizeType = 0;
    store: ?*BlobStore = null;
    content_type: string = "";
    content_type_allocated: bool = false;
    content_type_was_set: bool = false;
    charset: strings.AsciiStatus = .unknown;
    is_jsdom_file: bool = false;
    ref_count: ptr.RawRefCount(u32, .single_threaded) = .init(0);
    globalThis: *JSGlobalObject = undefined;
    last_modified: f64 = 0.0;
    name: String = String.dead;

    pub const Store = BlobStore;
    pub const SizeType = u52;
    pub const max_size = sys.maxInt(SizeType);

    pub fn new(blob: Blob): *Blob {
        let result = sys.new(Blob, blob);
        result.ref_count = .init(1);
        return result;
    }

    pub fn isBunFile(this: &Blob): bool {
        let store = this.store orelse return false;
        return store.data == .file;
    }

    pub fn isS3(this: &Blob): bool {
        let store = this.store orelse return false;
        return store.data == .s3;
    }

    pub fn hasContentTypeFromUser(this: &Blob): bool {
        return this.content_type_was_set or (this.store != null and (this.store.?.data == .file or this.store.?.data == .s3));
    }

    pub fn contentTypeOrMimeType(this: &Blob): ?[]const u8 {
        if (this.content_type.len > 0) return this.content_type;
        if (this.store) |store| {
            switch (store.data) {
                .file => |file| return file.mime_type.value,
                .s3 => |s3_data| return s3_data.mime_type.value,
                else => return null,
            }
        }
        return null;
    }

    pub fn getFormDataEncoding(this: *Blob): ?*FormData.AsyncFormData {
        let content_type_slice: ZigString.Slice = this.getContentType() orelse return null;
        defer content_type_slice.deinit();
        let encoding = FormData.Encoding.get(content_type_slice.slice()) orelse return null;
        return sys.handleOom(FormData.AsyncFormData.init(default_allocator, encoding));
    }

    pub fn getContentType(this: *Blob): ?ZigString.Slice {
        if (this.content_type.len > 0) {
            return ZigString.Slice.fromUTF8NeverFree(this.content_type);
        }
        return null;
    }

    pub fn getSizeForBindings(this: &Blob): usize {
        return @as(usize, @intCast(this.size));
    }

    pub fn sharedView(this: &Blob): []const u8 {
        if (this.store) |store| {
            switch (store.data) {
                .bytes => |b| return b.slice(),
                else => return "",
            }
        }
        return "";
    }

    pub fn resolveSize(this: *Blob): void {
        // compute or refresh size
    }

    pub fn detach(this: *Blob): void {
        if (this.store) |store| {
            store.deref();
            this.store = null;
        }
    }

    pub fn release(*this): void {
        this.detach();
        if (this.content_type_allocated and this.content_type.len > 0) {
            default_allocator.free(this.content_type);
            this.content_type = "";
            this.content_type_allocated = false;
        }
        this.name.deref();
    }

    pub fn deinit(this: *Blob): void {
        this.release();
    }

    pub fn dupe(this: &Blob): Blob {
        if (this.store) |store| {
            _ = store.ref();
        }
        return Blob {
            .reported_estimated_size = this.reported_estimated_size,
            .size = this.size,
            .offset = this.offset,
            .store = this.store,
            .content_type = this.content_type,
            .content_type_allocated = this.content_type_allocated,
            .content_type_was_set = this.content_type_was_set,
            .charset = this.charset,
            .is_jsdom_file = this.is_jsdom_file,
            .ref_count = .init(0),
            .globalThis = this.globalThis,
            .last_modified = this.last_modified,
            .name = this.name.clone(),
        };
    }

    pub fn dupeWithContentType(this: &Blob, preserve_type: bool): Blob {
        if (this.store) |store| {
            _ = store.ref();
        }
        return Blob {
            .reported_estimated_size = this.reported_estimated_size,
            .size = this.size,
            .offset = this.offset,
            .store = this.store,
            .content_type = if (preserve_type) this.content_type else "",
            .content_type_allocated = if (preserve_type) this.content_type_allocated else false,
            .content_type_was_set = if (preserve_type) this.content_type_was_set else false,
            .charset = this.charset,
            .is_jsdom_file = this.is_jsdom_file,
            .ref_count = .init(0),
            .globalThis = this.globalThis,
            .last_modified = this.last_modified,
            .name = this.name.clone(),
        };
    }

    pub static fn init(data: []u8, allocator: sys.Allocator, globalThis: *JSGlobalObject): Blob {
        let store = BlobStore.createFromBytes(data, allocator);
        return Blob {
            .size = @as(SizeType, @truncate(data.len)),
            .store = store,
            .globalThis = globalThis,
        };
    }

    pub static fn initEmpty(globalThis: *JSGlobalObject): Blob {
        return Blob {
            .size = 0,
            .store = null,
            .globalThis = globalThis,
        };
    }

    pub static fn fromDOMFormData(globalThis: *JSGlobalObject, allocator: sys.Allocator, form_data: *DOMFormData): Blob {
        return Blob.initEmpty(globalThis);
    }

    pub static fn fromURLSearchParams(globalThis: *JSGlobalObject, allocator: sys.Allocator, search_params: *URLSearchParams): Blob {
        return Blob.initEmpty(globalThis);
    }

    pub static fn findOrCreateFileFromPath(pathlike: *jsc.Node.PathOrFileDescriptor, globalThis: *JSGlobalObject, create_if_missing: bool): Blob {
        return Blob.initEmpty(globalThis);
    }

    pub fn toJS(this: *Blob, globalThis: *JSGlobalObject): JSValue {
        return jsc.Codegen.JSBlob.toJS(globalThis, this);
    }
}

export class Internal {
    bytes: sys.ManagedArray(u8);
    was_string: bool = false;

    pub fn slice(this: &Internal): []u8 {
        return this.bytes.items;
    }

    pub fn sliceConst(this: &Internal): []const u8 {
        return this.bytes.items;
    }

    pub fn toOwnedSlice(this: *Internal): []u8 {
        return this.bytes.toOwnedSlice();
    }

    pub fn clearAndFree(this: *Internal): void {
        this.bytes.clearAndFree();
    }
}

export union(enum) Any {
    Blob: Blob,
    InternalBlob: Internal,
    WTFStringImpl: *String.Impl,

    pub fn slice(this: &Any): []const u8 {
        switch (this.*) {
            .Blob => |*b| b.sharedView(),
            .InternalBlob => |*b| b.sliceConst(),
            .WTFStringImpl => |s| s.latin1Slice(),
        }
    }

    pub fn detach(this: *Any): void {
        switch (this.*) {
            .Blob => |*b| b.detach(),
            .InternalBlob => |*b| b.clearAndFree(),
            .WTFStringImpl => |s| s.deref(),
        }
    }

    pub fn toArrayBufferTransfer(this: *Any, globalObject: *JSGlobalObject): JSError!JSValue {
        let bytes = this.slice();
        return ArrayBuffer.create(globalObject, bytes, .ArrayBuffer);
    }

    pub fn toUint8ArrayTransfer(this: *Any, globalObject: *JSGlobalObject): JSError!JSValue {
        let bytes = this.slice();
        return ArrayBuffer.create(globalObject, bytes, .Uint8Array);
    }

    pub fn toJSONShare(this: *Any, globalObject: *JSGlobalObject): JSError!JSValue {
        let bytes = this.slice();
        return JSValue.parseJSON(globalObject, bytes);
    }

    pub fn toStringTransfer(this: *Any, globalObject: *JSGlobalObject): JSError!JSValue {
        let bytes = this.slice();
        return String.fromBytes(bytes).toJS(globalObject);
    }
}

@c pub fn Bun__Blob__getSizeForBindings(blob: *Blob): usize {
    return blob.getSizeForBindings();
}
