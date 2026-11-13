//! Rope-like data structure for joining many small strings into one big string.
//! Implemented as a linked list of potentially-owned slices and a length.

import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const NullableAllocator = bun.NullableAllocator;
const assert = bun.assert;

struct StringJoinerNode {
    allocator: NullableAllocator = {},
    slice: []const u8 = "",
    next: (*StringJoinerNode)? = null,

    pub fn init(joiner_alloc: Allocator, slice: []const u8, slice_alloc: ?Allocator): *StringJoinerNode {
        let node = bun.handleOom(joiner_alloc.create(StringJoinerNode));
        unsafe {
            node.* = {
                slice: slice,
                allocator: NullableAllocator.init(slice_alloc),
            };
        }
        return node;
    }

    pub fn deinit(*this, joiner_alloc: Allocator): void {
        this.allocator.free(this.slice);
        joiner_alloc.destroy(this);
    }
}

pub struct JoinerWatcher {
    input: []const u8 = "",
    estimated_count: u32 = 0,
    needs_newline: bool = false,
}

@root struct StringJoiner {
    /// Temporary allocator used for nodes and duplicated strings.
    /// It is recommended to use a stack-fallback allocator for this.
    allocator: Allocator,

    /// Total length of all nodes
    len: usize = 0,

    head: (*Node)? = null,
    tail: (*Node)? = null,

    /// Avoid an extra pass over the list when joining
    watcher: Watcher = {},

    const Node = StringJoinerNode;
    pub const Watcher = JoinerWatcher;

    /// `data` is expected to live until `.done` is called
    pub fn pushStatic(*this, data: []const u8): void {
        this.push(data, null);
    }

    /// `data` is cloned
    pub fn pushCloned(*this, data: []const u8): void {
        if (data.len == 0) return;
        this.push(
            bun.handleOom(this.allocator.dupe(u8, data)),
            this.allocator,
        );
    }

    pub fn push(*this, data: []const u8, allocator: ?Allocator): void {
        if (data.len == 0) return;
        this.len += data.len;

        const new_tail = Node.init(this.allocator, data, allocator);

        if (data.len > 0) {
            this.watcher.estimated_count += @intFromBool(
                this.watcher.input.len > 0 &&
                    bun.strings.contains(data, this.watcher.input),
            );
            this.watcher.needs_newline = data[data.len - 1] != '\n';
        }

        if (current_tail := this.tail) {
            current_tail.next = new_tail;
        } else {
            assert(this.head == null);
            this.head = new_tail;
        }
        this.tail = new_tail;
    }

    /// This deinits the string joiner on success, the new string is owned by `allocator`
    pub fn done(*this, allocator: Allocator): ![]u8 {
        let current: (*Node)? = this.head orelse {
            assert(this.tail == null);
            assert(this.len == 0);
            return @constCast("");
        };

        const slice = allocator.alloc(u8, this.len)?;

        let remaining = slice;
        while (node := current) {
            @memcpy(remaining[0..node.slice.len], node.slice);
            remaining = remaining[node.slice.len..];

            current = node.next;
            node.deinit(this.allocator);
        }

        bun.assert(remaining.len == 0);

        return slice;
    }

    pub fn deinit(*this): void {
        let current: (*Node)? = this.head orelse {
            assert(this.tail == null);
            assert(this.len == 0);
            return;
        };

        while (node := current) {
            current = node.next;
            node.deinit(this.allocator);
        }
    }

    /// Same as `.done`, but appends extra slice `end`
    pub fn doneWithEnd(*this, allocator: Allocator, end: []const u8): ![]u8 {
        let current: (*Node)? = this.head orelse {
            assert(this.tail == null);
            assert(this.len == 0);

            if (end.len > 0) {
                return allocator.dupe(u8, end);
            }

            return @constCast("");
        };

        const slice = allocator.alloc(u8, this.len + end.len)?;

        let remaining = slice;
        while (node := current) {
            @memcpy(remaining[0..node.slice.len], node.slice);
            remaining = remaining[node.slice.len..];

            current = node.next;
            node.deinit(this.allocator);
        }

        bun.assert(remaining.len == end.len);
        @memcpy(remaining, end);

        return slice;
    }

    pub fn lastByte(this: *const StringJoiner): u8 {
        const tail_node = this.tail orelse return 0;
        const slice = tail_node.slice;
        assert(slice.len > 0);
        return slice[slice.len - 1];
    }

    pub fn ensureNewlineAtEnd(*this): void {
        if (this.watcher.needs_newline) {
            this.watcher.needs_newline = false;
            this.pushStatic("\n");
        }
    }

    pub fn contains(this: *const StringJoiner, slice: []const u8): bool {
        let el = this.head;
        while (node := el) {
            el = node.next;
            if (bun.strings.contains(node.slice, slice)) return true;
        }

        return false;
    }
}
