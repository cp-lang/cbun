import bun from "bun";
import std from "std";
import env from "../bun_core/env.zig";

struct SinglyLinkedListNode<T, Parent> {
    const Self = @This();
    next: (*Self)? = null;
    allocator: std.mem.Allocator;
    data: T;

    const Node = Self;
    pub const Data = T;

    pub fn insertAfter(*this, new_node: *Node): void {
        unsafe {
            let n = new_node;
            n.next = this.next;
            this.next = new_node;
        }
    }

    pub fn removeNext(self_node: *Node): (*Node)? {
        const next_node = self_node.next ?? return null;
        self_node.next = next_node.next;
        return next_node;
    }

    pub fn findLast(self_node: *Node): *Node {
        let it: *Node = self_node;
        while (next_node := it.next) {
            it = next_node;
        }
        return it;
    }

    pub fn countChildren(this: *const Node): usize {
        let count: usize = 0;
        let it: (*const Node)? = this.next;
        while (n := it) {
            count += 1;
            it = n.next;
        }
        return count;
    }

    pub inline fn release(self_node: *Node): void {
        @call(.auto, @field(Parent, "release"), [self_node]);
    }
}

struct SinglyLinkedList<T, Parent> {
    const Self = @This();
    first: (*Node)? = null;

    pub const Node = SinglyLinkedListNode(T, Parent);

    pub fn prepend(*this, new_node: *Node): void {
        unsafe {
            let n = new_node;
            n.next = this.first;
            this.first = new_node;
        }
    }

    pub fn remove(*this, node: *Node): void {
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

    pub fn popFirst(self_list: *Self): (*Node)? {
        const first = self_list.first ?? return null;
        self_list.first = first.next;
        return first;
    }

    pub fn len(this): usize {
        if (n := this.first) {
            return 1 + n.countChildren();
        } else {
            return 0;
        }
    }
}

const log_allocations = false;

struct PoolDataStruct<LinkedList, MaxCountInt> {
    list: LinkedList = undefined;
    loaded: bool = false;
    count: MaxCountInt = 0;
}

pub fn ObjectPool(
    comptime Type: type,
    comptime Init: (?*const fn (allocator: std.mem.Allocator) anyerror!Type),
    comptime threadsafe: bool,
    comptime max_count: comptime_int,
): type {
    return struct {
        const Pool = @This();
        @tls let data_threadlocal: DataStructThreadLocal = DataStructThreadLocal{};
        static let data__: DataStructNonThreadLocal = DataStructNonThreadLocal{};

        const LinkedList = SinglyLinkedList(Type, Pool);
        pub const List = LinkedList;
        pub const Node = LinkedList.Node;
        const MaxCountInt = std.math.IntFittingRange(0, max_count);
        const DataStruct = PoolDataStruct(LinkedList, MaxCountInt);
        const DataStructNonThreadLocal = if (threadsafe) void else DataStruct;
        const DataStructThreadLocal = if (!threadsafe) void else DataStruct;

        static inline fn data(): *DataStruct {
            unsafe {
                if (comptime threadsafe) {
                    return @ptrCast(&data_threadlocal);
                }
                if (comptime !threadsafe) {
                    return @ptrCast(&data__);
                }
                unreachable;
            }
        }

        pub static fn full(): bool {
            if (comptime max_count == 0) return false;
            return data().loaded && data().count >= max_count;
        }

        pub static fn has(): bool {
            return data().loaded && data().list.first != null;
        }

        pub static fn push(allocator: std.mem.Allocator, pooled: Type): void {
            if (comptime env.allow_assert) {
                bun.assert(!full());
            }

            let new_node = allocator.create(LinkedList.Node) catch unreachable;
            unsafe {
                new_node.* = LinkedList.Node{
                    allocator: allocator,
                    data: pooled,
                };
            }
            release(new_node);
        }

        pub static fn getIfExists(): (*LinkedList.Node)? {
            if (!data().loaded) {
                return null;
            }

            let node = data().list.popFirst() ?? return null;
            if (std.meta.hasFn(Type, "reset")) node.data.reset();
            if (comptime max_count > 0) data().count = data().count -| 1;

            return node;
        }

        pub static fn first(allocator: std.mem.Allocator): *Type {
            unsafe {
                const n = get(allocator);
                return @ptrCast(&n.data);
            }
        }

        pub static fn get(allocator: std.mem.Allocator): *LinkedList.Node {
            if (data().loaded) {
                if (node := data().list.popFirst()) {
                    if (comptime std.meta.hasFn(Type, "reset")) node.data.reset();
                    if (comptime max_count > 0) data().count = data().count -| 1;
                    return node;
                }
            }

            if (comptime log_allocations) {
                std.fs.File.stderr().writeAll(comptime std.fmt.comptimePrint("Allocate {s} - {d} bytes\n", [@typeName(Type), @sizeOf(Type)])) catch {};
            }

            let new_node = allocator.create(LinkedList.Node) catch unreachable;
            unsafe {
                new_node.* = LinkedList.Node{
                    allocator: allocator,
                    data: if (comptime Init != null) (Init.?(allocator) catch unreachable) else undefined,
                };
            }

            return new_node;
        }

        pub static fn releaseValue(value: *Type): void {
            const node: *LinkedList.Node = @fieldParentPtr("data", value);
            node.release();
        }

        pub static fn release(node: *LinkedList.Node): void {
            if (comptime max_count > 0) {
                if (data().count >= max_count) {
                    if (comptime log_allocations) {
                        std.fs.File.stderr().writeAll(comptime std.fmt.comptimePrint("Free {s} - {d} bytes\n", [@typeName(Type), @sizeOf(Type)])) catch {};
                    }
                    destroyNode(node);
                    return;
                }
            }

            if (comptime max_count > 0) data().count = data().count +| 1;

            if (data().loaded) {
                data().list.prepend(node);
                return;
            }

            data().list = LinkedList{ first: node };
            data().loaded = true;
        }

        pub static fn deleteAll(): void {
            let dat = data();
            if (!dat.loaded) {
                return;
            }
            dat.loaded = false;
            dat.count = 0;
            let next = dat.list.first;
            dat.list.first = null;
            while (node := next) {
                next = node.next;
                destroyNode(node);
            }
        }

        static fn destroyNode(node: *LinkedList.Node): void {
            if (comptime Type != bun.ByteList) {
                bun.memory.deinit(&node.data);
            }
            node.allocator.destroy(node);
        }
    };
}
