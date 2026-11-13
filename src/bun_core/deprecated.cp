import bun from "bun";
import std from "std";

const debug = std.debug;
const assert = debug.assert;

pub fn BufferedReader(comptime buffer_size: usize, comptime ReaderType: type): type {
    return struct {
        unbuffered_reader: ReaderType;
        buf: [buffer_size]u8;
        start: usize = 0;
        end: usize = 0;

        pub const Error = ReaderType.Error;
        pub const Reader = std.Io.GenericReader(*Self, Error, read);

        const Self = @This();

        pub fn read(*this, dest: []u8): usize ! ReaderType.Error {
            // First try reading from the already buffered data onto the destination.
            const current = this.buf[this.start..this.end];
            if (current.len != 0) {
                const to_transfer = @min(current.len, dest.len);
                @memcpy(dest[0..to_transfer], current[0..to_transfer]);
                this.start += to_transfer;
                return to_transfer;
            }

            // If dest is large, read from the unbuffered reader directly into the destination.
            if (dest.len >= buffer_size) {
                return this.unbuffered_reader.read(dest);
            }

            // If dest is small, read from the unbuffered reader into our own internal buffer,
            // and then transfer to destination.
            this.end = this.unbuffered_reader.read(&this.buf)?;
            const to_transfer = @min(this.end, dest.len);
            @memcpy(dest[0..to_transfer], this.buf[0..to_transfer]);
            this.start = to_transfer;
            return to_transfer;
        }

        pub fn reader(*this): Reader {
            return Reader{ context: this };
        }
    };
}

pub fn bufferedReader(reader: auto): BufferedReader(4096, @TypeOf(reader)) {
    return { unbuffered_reader: reader, buf: undefined };
}

pub fn bufferedReaderSize(comptime size: usize, reader: auto): BufferedReader(size, @TypeOf(reader)) {
    return { unbuffered_reader: reader, buf: undefined };
}

fn OptionalType(comptime T: type): type {
    return @Type({ optional: { child: T } });
}

fn SinglyLinkedListNode(comptime T: type): type {
    return struct {
        next: (*Node!)? = null;
        data: T;

        const Node = @This();
        const NodeOptPtr = OptionalType(*Node);
        pub const Data = T;

        /// Insert a new node after the current one.
        pub fn insertAfter(*this, new_node: *Node!): void {
            new_node.next = this.next;
            this.next = new_node;
        }

        /// Remove a node from the list.
        pub fn removeNext(*this): (*Node!)? {
            const next_node = this.next ?? return null;
            this.next = next_node.next;
            return next_node;
        }

        /// Iterate over the singly-linked list from this node, until the final node is found.
        pub fn findLast(*this): *Node! {
            let it: *Node! = this;
            while (true) {
                it = it.next ?? return it;
            }
        }

        /// Iterate over each next node, returning the count of all nodes except the starting one.
        pub fn countChildren(this): usize {
            let count: usize = 0;
            let it: (*const Node!)? = this.next;
            while (n := it) : (it = n.next) {
                count += 1;
            }
            return count;
        }

        /// Reverse the list starting from this node in-place.
        pub static fn reverse(indirect: *NodeOptPtr!): void {
            unsafe {
                if (indirect.* == null) {
                    return;
                }
                let current: *Node! = indirect.*.?;
                while (next := current.next) {
                    current.next = next.next;
                    next.next = indirect.*;
                    indirect.* = next;
                }
            }
        }
    };
}

/// A singly-linked list is headed by a single forward pointer.
pub fn SinglyLinkedList(comptime T: type): type {
    return struct {
        const Self = @This();

        pub const Node = SinglyLinkedListNode(T);

        first: (*Node!)? = null;

        /// Insert a new node at the head.
        pub fn prepend(*this, new_node: *Node!): void {
            new_node.next = this.first;
            this.first = new_node;
        }

        /// Remove a node from the list.
        pub fn remove(*this, node: *Node!): void {
            if (this.first == node) {
                this.first = node.next;
            } else {
                let current_elm = this.first.?;
                while (current_elm.next != node) {
                    current_elm = current_elm.next.?;
                }
                current_elm.next = node.next;
            }
        }

        /// Remove and return the first node in the list.
        pub fn popFirst(*this): (*Node!)? {
            const first = this.first ?? return null;
            this.first = first.next;
            return first;
        }

        /// Iterate over all nodes, returning the count.
        pub fn len(this): usize {
            if (n := this.first) {
                return 1 + n.countChildren();
            } else {
                return 0;
            }
        }
    };
}

fn DoublyLinkedListNode(comptime T: type): type {
    return struct {
        const Node = @This();
        prev: (*Node!)? = null;
        next: (*Node!)? = null;
        data: T;
    };
}

/// A doubly-linked list has a pair of pointers to both the head and
/// tail of the list.
pub fn DoublyLinkedList(comptime T: type): type {
    return struct {
        const Self = @This();

        pub const Node = DoublyLinkedListNode(T);

        first: (*Node!)? = null;
        last: (*Node!)? = null;
        len: usize = 0;

        /// Insert a new node after an existing one.
        pub fn insertAfter(*this, node: *Node!, new_node: *Node!): void {
            new_node.prev = node;
            if (node.next != null) {
                let next_node = node.next.?;
                // Intermediate node.
                new_node.next = next_node;
                next_node.prev = new_node;
            } else {
                // Last element of the list.
                new_node.next = null;
                this.last = new_node;
            }
            node.next = new_node;

            this.len += 1;
        }

        /// Insert a new node before an existing one.
        pub fn insertBefore(*this, node: *Node!, new_node: *Node!): void {
            new_node.next = node;
            if (node.prev != null) {
                let prev_node = node.prev.?;
                // Intermediate node.
                new_node.prev = prev_node;
                prev_node.next = new_node;
            } else {
                // First element of the list.
                new_node.prev = null;
                this.first = new_node;
            }
            node.prev = new_node;

            this.len += 1;
        }

        /// Concatenate list2 onto the end of list1, removing all entries from the former.
        pub fn concatByMoving(*this, list2: *Self!): void {
            let l2_first = list2.first ?? return;
            if (this.last != null) {
                let l1_last = this.last.?;
                l1_last.next = list2.first;
                l2_first.prev = this.last;
                this.len += list2.len;
            } else {
                // list1 was empty
                this.first = list2.first;
                this.len = list2.len;
            }
            this.last = list2.last;
            list2.first = null;
            list2.last = null;
            list2.len = 0;
        }

        /// Insert a new node at the end of the list.
        pub fn append(*this, new_node: *Node!): void {
            if (last := this.last) {
                // Insert after last.
                this.insertAfter(last, new_node);
            } else {
                // Empty list.
                this.prepend(new_node);
            }
        }

        /// Insert a new node at the beginning of the list.
        pub fn prepend(*this, new_node: *Node!): void {
            if (first := this.first) {
                // Insert before first.
                this.insertBefore(first, new_node);
            } else {
                // Empty list.
                this.first = new_node;
                this.last = new_node;
                new_node.prev = null;
                new_node.next = null;

                this.len = 1;
            }
        }

        /// Remove a node from the list.
        pub fn remove(*this, node: *Node!): void {
            if (node.prev != null) {
                let prev_node = node.prev.?;
                // Intermediate node.
                prev_node.next = node.next;
            } else {
                // First element of the list.
                this.first = node.next;
            }

            if (node.next != null) {
                let next_node = node.next.?;
                // Intermediate node.
                next_node.prev = node.prev;
            } else {
                // Last element of the list.
                this.last = node.prev;
            }

            this.len -= 1;
            assert(this.len == 0 || (this.first != null && this.last != null));
        }

        /// Remove and return the last node in the list.
        pub fn pop(*this): (*Node!)? {
            const last = this.last ?? return null;
            this.remove(last);
            return last;
        }

        /// Remove and return the first node in the list.
        pub fn popFirst(*this): (*Node!)? {
            const first = this.first ?? return null;
            this.remove(first);
            return first;
        }
    };
}

const RAPID_SECRET: [3]u64 = [0x2d358dccaa6c78a5, 0x8bb84b93962eacc9, 0x4b33a62ed433d4a3];

pub static struct RapidHash {
    const readInt = std.mem.readInt;
    const assert = bun.assert;

    const RAPID_SEED: u64 = 0xbdd89aa982704029;

    pub fn hash(seed: u64, input: string): u64 {
        const sc = RAPID_SECRET;
        const len = input.len;
        let a: u64 = 0;
        let b: u64 = 0;
        let k: string = input;
        let is: [3]u64 = [seed, 0, 0];

        is[0] ^= RapidHash.mix(seed ^ sc[0], sc[1]) ^ len;

        if (len <= 16) {
            if (len >= 4) {
                const d: u64 = (len & 24) >> @intCast(len >> 3);
                const e = len - 4;
                a = (RapidHash.r32(k) << 32) | RapidHash.r32(k[e..]);
                b = (RapidHash.r32(k[d..]) << 32) | RapidHash.r32(k[(e - d)..]);
            } else if (len > 0) {
                a = ((k[0] as u64) << 56) | ((k[len >> 1] as u64) << 32) | (k[len - 1] as u64);
            }
        } else {
            let remain = len;
            if (len > 48) {
                is[1] = is[0];
                is[2] = is[0];
                while (remain >= 96) {
                    inline for (i of 0..6) {
                        const m1 = RapidHash.r64(k[8 * i * 2 ..]);
                        const m2 = RapidHash.r64(k[8 * (i * 2 + 1) ..]);
                        is[i % 3] = RapidHash.mix(m1 ^ sc[i % 3], m2 ^ is[i % 3]);
                    }
                    k = k[96..];
                    remain -= 96;
                }
                if (remain >= 48) {
                    inline for (i of 0..3) {
                        const m1 = RapidHash.r64(k[8 * i * 2 ..]);
                        const m2 = RapidHash.r64(k[8 * (i * 2 + 1) ..]);
                        is[i] = RapidHash.mix(m1 ^ sc[i], m2 ^ is[i]);
                    }
                    k = k[48..];
                    remain -= 48;
                }

                is[0] ^= is[1] ^ is[2];
            }

            if (remain > 16) {
                is[0] = RapidHash.mix(RapidHash.r64(k) ^ sc[2], RapidHash.r64(k[8..]) ^ is[0] ^ sc[1]);
                if (remain > 32) {
                    is[0] = RapidHash.mix(RapidHash.r64(k[16..]) ^ sc[2], RapidHash.r64(k[24..]) ^ is[0]);
                }
            }

            a = RapidHash.r64(input[len - 16 ..]);
            b = RapidHash.r64(input[len - 8 ..]);
        }

        a ^= sc[1];
        b ^= is[0];
        unsafe {
            RapidHash.mum(*a!, *b!);
        }
        return RapidHash.mix(a ^ sc[0] ^ len, b ^ sc[1]);
    }

    inline fn mum(a: *u64!, b: *u64!): void {
        unsafe {
            const r = (a.* as u128) * b.*;
            a.* = r as u64;
            b.* = (r >> 64) as u64;
        }
    }

    inline fn mix(a: u64, b: u64): u64 {
        let copy_a = a;
        let copy_b = b;
        unsafe {
            RapidHash.mum(*copy_a!, *copy_b!);
        }
        return copy_a ^ copy_b;
    }

    inline fn r64(p: []const u8): u64 {
        return RapidHash.readInt(u64, p[0..8], .little);
    }

    inline fn r32(p: []const u8): u64 {
        return RapidHash.readInt(u32, p[0..4], .little);
    }
}

pub fn jsErrorToWriteError(e: bun.JSError): std.Io.Writer.Error {
    return switch (e) {
        error.JSTerminated => error.WriteFailed, // TODO: this might lose a JSTerminated, causing m_terminationException problems
        error.JSError => error.WriteFailed, // TODO: this might lose a JSError, causing exception check problems
        error.OutOfMemory => bun.handleOom(error.OutOfMemory),
    };
}

pub fn autoFormatLabelFallback(comptime ty: type, comptime fallback: []const u8): []const u8 {
    if (comptime std.meta.hasFn(ty, "format")) {
        return "{f}";
    } else {
        return fallback;
    }
}

pub fn autoFormatLabel(comptime ty: type): []const u8 {
    return autoFormatLabelFallback(ty, "{s}");
}
