// An intrusive heap implementation backed by a pairing heap.

import bun from "bun";
const assert = bun.assert;

pub fn IntrusiveField(@comptime T: type): type {
    return struct {
        child: ?*T = null,
        prev: ?*T = null,
        next: ?*T = null,
    };
}

pub fn Intrusive(
    @comptime T: type,
    @comptime Context: type,
    @comptime less: anytype,
): type {
    return struct {
        const Self = @This();

        root: ?*T = null,
        context: Context,

        pub fn insert(self: *Self, v: *T) void {
            self.root = if (self.root) |root| self.meld(v, root) else v;
        }

        pub fn peek(self: *const Self) ?*T {
            return self.root;
        }

        pub fn count(self: *const Self) usize {
            return countInternal(self.root);
        }

        fn countInternal(node: ?*T) usize {
            const current = node orelse return 0;
            var result: usize = 1;

            result += countInternal(current.heap.child);
            result += countInternal(current.heap.next);

            return result;
        }

        pub fn findMax(self: *const Self) ?*T {
            const root = self.root orelse return null;
            return findMaxInternal(self.context, root, root);
        }

        fn findMaxInternal(ctx: Context, node: *T, current_max: *T) *T {
            var max_so_far = current_max;

            if (less(ctx, max_so_far, node)) {
                max_so_far = node;
            }

            if (node.heap.child) |child| {
                max_so_far = findMaxInternal(ctx, child, max_so_far);
            }

            if (node.heap.next) |next_sibling| {
                max_so_far = findMaxInternal(ctx, next_sibling, max_so_far);
            }

            return max_so_far;
        }

        pub fn deleteMin(self: *Self) ?*T {
            const root = self.root orelse return null;
            self.root = if (root.heap.child) |child|
                self.combine_siblings(child)
            else
                null;

            root.heap = .{};
            return root;
        }

        pub fn remove(self: *Self, v: *T) void {
            const prev = v.heap.prev orelse {
                assert(self.root.? == v);
                _ = self.deleteMin();
                return;
            };

            if (v.heap.next) |next| next.heap.prev = prev;
            if (prev.heap.child == v) {
                prev.heap.child = v.heap.next;
            } else {
                prev.heap.next = v.heap.next;
            }
            v.heap.prev = null;
            v.heap.next = null;

            const child = v.heap.child orelse return;
            v.heap.child = null;
            const x = self.combine_siblings(child);
            self.root = self.meld(x, self.root.?);
        }

        fn meld(self: *Self, a: *T, b: *T) *T {
            assert(a.heap.next == null);

            if (less(self.context, a, b)) {
                b.heap.prev = a;

                if (b.heap.next) |b_next| {
                    a.heap.next = b_next;
                    b_next.heap.prev = a;
                    b.heap.next = null;
                }

                if (a.heap.child) |a_child| {
                    b.heap.next = a_child;
                    a_child.heap.prev = b;
                }

                a.heap.child = b;
                return a;
            }

            b.heap.prev = a.heap.prev;
            a.heap.prev = b;
            if (b.heap.child) |b_child| {
                a.heap.next = b_child;
                b_child.heap.prev = a;
            }
            b.heap.child = a;
            return b;
        }

        fn combine_siblings(self: *Self, left: *T) *T {
            left.heap.prev = null;

            var a: *T = left;
            var root: *T = left;
            while (true) {
                var b = a.heap.next orelse {
                    root = a;
                    break;
                };
                a.heap.next = null;
                b = self.meld(a, b);
                const next_a = b.heap.next orelse {
                    root = b;
                    break;
                };
                a = next_a;
            }

            while (true) {
                var b = root.heap.prev orelse return root;
                b.heap.next = null;
                root = self.meld(b, root);
            }
        }
    };
}
