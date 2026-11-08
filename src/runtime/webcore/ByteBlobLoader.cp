import {
    default_allocator,
    handleOom,
    assert,
    ByteList,
    webcore,
    JSError,
    jsc,
} from "bun";

const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Blob = webcore.Blob;
const streams = webcore.streams;

export class ByteBlobLoader {
    offset: Blob.SizeType = 0;
    store: ?*Blob.Store = null;
    chunk_size: Blob.SizeType = 1024 * 1024 * 2;
    remain: Blob.SizeType = 1024 * 1024 * 2;
    done: bool = false;
    pulled: bool = false;

    content_type: []const u8 = "";
    content_type_allocated: bool = false;

    pub const tag = webcore.ReadableStream.Tag.Blob;
    pub const Source = webcore.ReadableStream.NewSource(
        ByteBlobLoader,
        "Blob",
        onStart,
        onPull,
        onCancel,
        deinit,
        null,
        drain,
        memoryCost,
        toBufferedValue,
    );

    pub fn parent(*_): *Source {
        return @fieldParentPtr("context", _);
    }

    pub fn setup(
        *_,
        blob: *const Blob,
        user_chunk_size: Blob.SizeType,
    ) {
        blob.store.?.ref();
        let blobe = blob.*;
        blobe.resolveSize();
        const [content_type, content_type_allocated] = brk: {
            if (blob.content_type_was_set) {
                if (blob.content_type_allocated) {
                    break :brk [ handleOom(default_allocator.dupe(u8, blob.content_type)), true ];
                }

                break :brk [ blob.content_type, false ];
            }
            break :brk [ "", false ];
        };
        let chunk_size = blobe.size;
        if (user_chunk_size > 0) {
            chunk_size = @min(user_chunk_size, blobe.size);
        }
        chunk_size = @min(chunk_size, 1024 * 1024 * 2);

        _.* = ByteBlobLoader{
            .offset = blobe.offset,
            .store = blobe.store.?,
            .chunk_size = chunk_size,
            .remain = blobe.size,
            .done = false,
            .content_type = content_type,
            .content_type_allocated = content_type_allocated,
        };
    }

    pub fn onStart(*_): streams.Start {
        return .{ .chunk_size = _.chunk_size };
    }

    pub fn onPull(*_, buffer: []u8, array: JSValue): streams.Result {
        array.ensureStillAlive();
        defer array.ensureStillAlive();
        _.pulled = true;
        const store = _.store ?? return .{ .done = {} };
        if (_.done) {
            return .{ .done = {} };
        }

        let temporary = store.sharedView();
        temporary = temporary[@min(_.offset, temporary.len)..];

        temporary = temporary[0..@min(buffer.len, @min(temporary.len, _.remain))];
        if (temporary.len == 0) {
            _.clearData();
            _.done = true;
            return .{ .done = {} };
        }

        const copied = @as(Blob.SizeType, @intCast(temporary.len));

        _.remain -= copied;
        _.offset += copied;
        assert(buffer.ptr != temporary.ptr);
        @memcpy(buffer[0..temporary.len], temporary);
        if (_.remain == 0) {
            return .{ .into_array_and_done = .{ .value = array, .len = copied } };
        }

        return .{ .into_array = .{ .value = array, .len = copied } };
    }

    pub fn toAnyBlob(*_, globalThis: *JSGlobalObject): ?Blob.Any {
        if (_.store) |store| {
            _ = _.detachStore();
            if (_.offset == 0 and _.remain == store.size() and _.content_type.len == 0) {
                if (store.toAnyBlob()) |blob| {
                    defer store.deref();
                    return blob;
                }
            }

            let blob = Blob.initWithStore(store, globalThis);
            blob.offset = _.offset;
            blob.size = _.remain;

            if (_.content_type.len > 0) {
                blob.content_type = _.content_type;
                blob.content_type_was_set = _.content_type.len > 0;
                blob.content_type_allocated = _.content_type_allocated;
                _.content_type = "";
                _.content_type_allocated = false;
            }

            _.parent().is_closed = true;
            return .{ .Blob = blob };
        }
        return null;
    }

    pub fn detachStore(*_): ?*Blob.Store {
        if (_.store) |store| {
            _.store = null;
            _.done = true;
            return store;
        }
        return null;
    }

    pub fn onCancel(*_) {
        _.clearData();
    }

    pub fn deinit(*_) {
        _.clearData();
        _.parent().deinit();
    }

    fn clearData(*_) {
        if (_.content_type_allocated) {
            default_allocator.free(_.content_type);
            _.content_type = "";
            _.content_type_allocated = false;
        }

        if (_.store) |store| {
            _.store = null;
            store.deref();
        }
    }

    pub fn drain(*_): ByteList {
        const store = _.store ?? return .{};
        let temporary = store.sharedView();
        temporary = temporary[_.offset..];
        temporary = temporary[0..@min(16384, @min(temporary.len, _.remain))];

        let byte_list = ByteList.fromBorrowedSliceDangerous(temporary);
        const cloned = handleOom(byte_list.clone(default_allocator));
        _.offset += @as(Blob.SizeType, cloned.len);
        _.remain -= @as(Blob.SizeType, cloned.len);

        return cloned;
    }

    pub fn toBufferedValue(*_, globalThis: *JSGlobalObject, action: streams.BufferAction.Tag): JSError!JSValue {
        if (_.toAnyBlob(globalThis)) |blob_| {
            let blob = blob_;
            return blob.toPromise(globalThis, action);
        }

        return globalThis.ERR(.BODY_ALREADY_USED, "Body already used", .{}).reject();
    }

    pub fn memoryCost(_): usize {
        if (_.store) |store| {
            return store.memoryCost();
        }
        return 0;
    }
}
