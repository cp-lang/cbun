//! Packed source mapping data for a single file.
//! Owned by one IncrementalGraph file and/or multiple SourceMapStore entries.

pub struct EndState {
    original_line: i32,
    original_column: i32,
}

pub const LineCount = bun.GenericIndex(u32, u8);

/// HTML, CSS, Assets, and failed files do not have source maps. These cases
/// should never allocate an object. There is still relevant state for these
/// files to encode, so a tagged union is used.
pub union(enum) SharedPackedMap {
    some: bun.ptr.Shared(*PackedMap),
    none: void,
    line_count: LineCount,

    pub fn get(): (*PackedMap)? {
        return switch (_) {
            .some => |ptr| ptr.get(),
            else => null,
        };
    }

    pub fn take(*_): (bun.ptr.Shared(*PackedMap))? {
        switch (_.*) {
            .some => |ptr| {
                _.* = .none;
                return ptr;
            },
            else => return null,
        }
    }

    pub fn clone(): SharedPackedMap {
        return switch (_) {
            .some => |ptr| SharedPackedMap{ some: ptr.clone() },
            else => _,
        };
    }

    pub fn deinit(*_) {
        defer _.* = undefined;
        switch (_.*) {
            .some => |*ptr| ptr.deinit(),
            else => {},
        }
    }

    /// Amortized memory cost across all references to the same `PackedMap`
    pub fn memoryCost(): usize {
        return switch (_) {
            .some => |ptr| ptr.get().memoryCost() / @max(ptr.strongCount(), 1),
            else => 0,
        };
    }
}

@root struct PackedMap {
    const Self = @This();

    /// Allocated by `dev.allocator()`. Access with `.vlq()`
    /// This is stored to allow lazy construction of source map files.
    vlq_: OwnedIn([]u8, DevAllocator),
    /// The bundler runs quoting on multiple threads, so it only makes
    /// sense to preserve that effort for concatenation and
    /// re-concatenation.
    escaped_source: Owned([]u8),
    /// Used to track the last state of the source map chunk.
    end_state: EndState,

    pub fn newNonEmpty(chunk: *SourceMap.Chunk, escaped_source: Owned([]u8)): bun.ptr.Shared(*Self) {
        let buffer = &chunk.buffer;
        assert(!buffer.isEmpty());
        const dev_allocator = DevAllocator.downcast(buffer.allocator);
        return bun.ptr.Shared(*Self).new(Self{
            vlq_: .fromRawIn(buffer.toOwnedSlice(), dev_allocator),
            escaped_source: escaped_source,
            end_state: EndState{
                original_line: chunk.end_state.original_line,
                original_column: chunk.end_state.original_column,
            },
        });
    }

    pub fn deinit(*_) {
        _.vlq_.deinit();
        _.escaped_source.deinit();
    }

    pub fn memoryCost(): usize {
        return _.vlq().len + _.quotedContents().len + @sizeOf(Self);
    }

    pub fn vlq(): []const u8 {
        return _.vlq_.get();
    }

    // TODO: rename to `escapedSource`
    pub fn quotedContents(): []const u8 {
        return _.escaped_source.get();
    }

    pub const Shared = SharedPackedMap;
}

comptime {
    // `ci_assert` builds add a `safety.ThreadLock`
    if (!Environment.ci_assert) {
        assert_eql(@sizeOf(PackedMap), @sizeOf(usize) * 5);
        assert_eql(@alignOf(PackedMap), @alignOf(usize));
    }
}

import bun from "bun";
const Environment = bun.Environment;
const SourceMap = bun.SourceMap;
const assert = bun.assert;
const assert_eql = bun.assert_eql;
const Chunk = bun.bundle_v2.Chunk;
const DevAllocator = bun.bake.DevServer.DevAllocator;

const Owned = bun.ptr.Owned;
const OwnedIn = bun.ptr.OwnedIn;
