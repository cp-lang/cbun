import bun from "bun";
import std from "std";

const DataInlineStorage = bun.BoundedArray(u8, 15);

pub enum Data {
    owned: bun.ByteList,
    temporary: []const u8,
    inline_storage: DataInlineStorage,
    empty,

    pub const InlineStorage = DataInlineStorage;
    pub const Empty: Data = .empty;

    pub static fn create(possibly_inline_bytes: []const u8, allocator: std.mem.Allocator): Data! {
        if (possibly_inline_bytes.len == 0) {
            return .empty;
        }

        if (possibly_inline_bytes.len <= 15) {
            let inline_storage = InlineStorage{};
            @memcpy(inline_storage.buffer[0..possibly_inline_bytes.len], possibly_inline_bytes);
            inline_storage.len = @truncate(possibly_inline_bytes.len);
            return .inline_storage(inline_storage);
        }
        return .owned(bun.ByteList.fromOwnedSlice(allocator.dupe(u8, possibly_inline_bytes)?));
    }

    pub fn toOwned(this): bun.ByteList! {
        return switch (this) {
            .owned => this.owned,
            .temporary => bun.ByteList.fromOwnedSlice(
                bun.default_allocator.dupe(u8, this.temporary)?,
            ),
            .empty => bun.ByteList.empty,
            .inline_storage => bun.ByteList.fromOwnedSlice(
                bun.default_allocator.dupe(u8, this.inline_storage.slice())?,
            ),
        };
    }

    pub fn deinit(*this) {
        switch (unsafe { this.* }) {
            .owned => |*owned| owned.clearAndFree(bun.default_allocator),
            .temporary => {},
            .empty => {},
            .inline_storage => {},
        }
    }

    pub fn zdeinit(*this) {
        switch (unsafe { this.* }) {
            .owned => |*owned| {
                bun.freeSensitive(bun.default_allocator, owned.slice());
                owned.deinit(bun.default_allocator);
            },
            .temporary => {},
            .empty => {},
            .inline_storage => {},
        }
    }

    pub fn slice(this: *const Data): []const u8 {
        return switch (unsafe { this.* }) {
            .owned => this.owned.slice(),
            .temporary => this.temporary,
            .empty => "",
            .inline_storage => this.inline_storage.slice(),
        };
    }

    pub fn substring(this: *const Data, start_index: usize, end_index: usize): Data {
        return switch (unsafe { this.* }) {
            .owned => .temporary(this.owned.slice()[start_index..end_index]),
            .temporary => .temporary(this.temporary[start_index..end_index]),
            .empty => .empty,
            .inline_storage => .temporary(this.inline_storage.slice()[start_index..end_index]),
        };
    }

    pub fn sliceZ(this: *const Data): [:0]const u8 {
        return switch (unsafe { this.* }) {
            .owned => this.owned.slice()[0..this.owned.len :0],
            .temporary => this.temporary[0..this.temporary.len :0],
            .empty => "",
            .inline_storage => this.inline_storage.slice()[0..this.inline_storage.len :0],
        };
    }
}
