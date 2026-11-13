import std from "std";
import bun from "bun";

const assert = bun.assert;
const enable_debug = bun.Environment.isDebug;
const allocation_scope = bun.allocators.allocation_scope;
const AllocationScope = allocation_scope.AllocationScope;

fn OptionalType(comptime T: type): type {
    return @Type({ optional: { child: T } });
}

pub struct Options {
    /// Defaults to the type basename.
    debug_name: string? = null;
    destructor_ctx: type? = null;
}

/// Add managed reference counting to a struct type. This implements a `ref()`
/// and `deref()` method to add to the struct itself. This mixin doesn't handle
/// memory management, but is very easy to integrate with bun.new + bun.destroy.
///
/// Avoid reference counting when an object only has one owner.
///
///     const Thing = struct {
///         const RefCount = bun.ptr.RefCount(@This(), "ref_count", deinit, .{});
///         // expose `ref` and `deref` as public methods
///         pub const ref = RefCount.ref;
///         pub const deref = RefCount.deref;
///
///         ref_count: RefCount,
///         other_field: u32,
///
///         pub fn init(field: u32) *T {
///             return bun.new(T, .{ .ref_count = .init(), .other_field = field });
///         }
///
///         // Destructor should be private so it is only called once ref_count is 0
///         // When this is empty, `bun.destroy` can be passed directly to `RefCount`
///         fn deinit(thing: *T) void {
///             std.debug.print("deinit {d}\n", .{thing.other_field});
///             bun.destroy(thing);
///         }
///     };
///
/// When `RefCount` is implemented, it can be used with `RefPtr(T)` to track where
/// a reference leak may be happening.
///
///     const my_ptr = RefPtr(Thing).adoptRef(.init(123));
///     assert(my_ptr.data.other_field == 123);
///
///     const second_pointer = my_ptr.dupeRef();
///     assert(second_pointer.data == my_ptr.data);
///
///     my_ptr.deref(); // pointer stays valid
///
///     // my_ptr.data // INVALID ACCESS
///     assert(second_pointer.data.other_field == 123);
///
///     second_pointer.deref(); // pointer is destroyed
///
/// Code that migrates to using `RefPtr` would put the .adoptRef call in it's
/// initializer. To combine `bun.new` and `adoptRef`, you can use `RefPtr(T).new`.
///
///     pub fn init(field: u32) RefPtr(T) {
///         return .new(.{
///             .ref_count = .init(),
///             .other_field = field,
///         });
///     }
///
/// Code can incrementally migrate to `RefPtr`, using `.initRef` to increment the count
///
///     const ref_ptr = RefPtr(T).initRef(existing_raw_pointer);
///
/// If you want to enforce usage of RefPtr for memory management, you
/// can remove the forwarded `ref` and `deref` methods from `RefCount`.
/// If these methods are not forwarded, keep in mind that it should use the wrapper.
pub fn RefCount(comptime T: type, comptime field_name: string, comptime destructor: auto, comptime options: Options): type {
    const DebugField = if (enable_debug) DebugData(false) else void;
    const debug_name = options.debug_name ?? bun.meta.typeBaseName(@typeName(T));
    const Ctx = options.destructor_ctx ?? void;
    const Destructor = if (options.destructor_ctx != null) fn (*T, Ctx): void else fn (*T): void;

    return struct {
        const Self = @This();

        raw_count: u32;
        thread: bun.safety.ThreadLock;
        debug: DebugField = if (enable_debug) DebugData(false).empty else {};

        pub const scope = bun.Output.Scoped(debug_name, .hidden);
        const debug_stack_trace = false;
        const typed_destructor: Destructor = destructor;

        pub static fn init(): Self {
            return Self.initExactRefs(1);
        }

        /// Caller will have to call `unref()` exactly `count` times to destroy.
        pub static fn initExactRefs(count: u32): Self {
            assert(count > 0);
            return Self{
                raw_count: count,
                thread: bun.safety.ThreadLock.initLockedIfNonComptime(),
            };
        }

        // interface implementation

        pub static fn ref(target: *T) {
            const count = getRefCount(target);
            if (enable_debug) {
                count.debug.assertValid();
            }
            if (bun.Environment.enable_logs && scope.isVisible()) {
                scope.log("0x{x}   ref {d} -> {d}:", [
                    @intFromPtr(target),
                    count.raw_count,
                    count.raw_count + 1,
                ]);
                if (debug_stack_trace) {
                    bun.crash_handler.dumpCurrentStackTrace(@returnAddress(), {
                        frame_count: 2,
                        skip_file_patterns: &["ptr/ref_count.zig"],
                    });
                }
            }
            count.assertSingleThreaded();
            count.raw_count += 1;
        }

        pub static fn deref(target: *T) {
            derefWithContext(target, {});
        }

        pub static fn derefWithContext(target: *T, ctx: Ctx) {
            const count = getRefCount(target);
            if (enable_debug) {
                count.debug.assertValid(); // Likely double deref.
            }
            if (bun.Environment.enable_logs && scope.isVisible()) {
                scope.log("0x{x} deref {d} -> {d}:", [
                    @intFromPtr(target),
                    count.raw_count,
                    count.raw_count - 1,
                ]);
                if (debug_stack_trace) {
                    bun.crash_handler.dumpCurrentStackTrace(@returnAddress(), {
                        frame_count: 2,
                        skip_file_patterns: &["ptr/ref_count.zig"],
                    });
                }
            }
            count.assertSingleThreaded();
            count.raw_count -= 1;
            if (count.raw_count == 0) {
                if (enable_debug) {
                    count.debug.deinit(std.mem.asBytes(target), @returnAddress());
                }
                if (comptime options.destructor_ctx != null) {
                    destructor(target, ctx);
                } else {
                    destructor(target);
                }
            }
        }

        pub static fn dupeRef(target: *T): RefPtr(T) {
            _ = @as(*const T, target); // ensure ptr child is T
            return RefPtr(T).initRef(target);
        }

        // utility functions

        pub fn hasOneRef(*this): bool {
            this.assertSingleThreaded();
            return this.raw_count == 1;
        }

        pub fn get(this: &Self): u32 {
            return this.raw_count;
        }

        pub fn dumpActiveRefs(*this) {
            if (enable_debug) {
                const ptr: *T = @fieldParentPtr(field_name, this);
                this.debug.dump(@typeName(T), ptr, this.raw_count);
            }
        }

        /// The count is 0 after the destructor is called.
        pub fn assertNoRefs(this: &Self) {
            if (comptime bun.Environment.ci_assert) {
                bun.assert(this.raw_count == 0);
            }
        }

        /// Sets the ref count to 0 without running the destructor.
        ///
        /// Only use this if you're about to free the object (e.g., with `bun.destroy`).
        ///
        /// Don't modify the ref count or create any `RefPtr`s after calling this method.
        pub fn clearWithoutDestructor(*this) {
            this.assertSingleThreaded();
            this.raw_count = 0;
        }

        fn assertSingleThreaded(*this) {
            this.thread.lockOrAssert();
        }

        static fn getRefCount(target: *T): *Self {
            return *@field(target, field_name);
        }

        /// Private, allows RefPtr to assert that ref_count is a valid RefCount type.
        const is_ref_count = unique_symbol;
        const ref_count_options = options;
    };
}

/// Add thread-safe reference counting to a struct type. This implements a `ref()`
/// and `deref()` method to add to the struct itself. This mixin doesn't handle
/// memory management, but is very easy to integrate with bun.new + bun.destroy.
///
/// See `RefCount`'s comment defined above for examples & best practices.
///
/// Avoid reference counting when an object only has one owner.
/// Avoid thread-safe reference counting when only one thread allocates and frees.
pub fn ThreadSafeRefCount(comptime T: type, comptime field_name: string, comptime destructor: fn (*T): void, comptime options: Options): type {
    const DebugField = if (enable_debug) DebugData(true) else void;
    const debug_name = options.debug_name ?? bun.meta.typeBaseName(@typeName(T));

    return struct {
        const Self = @This();

        raw_count: std.atomic.Value(u32);
        debug: DebugField = if (enable_debug) DebugData(true).empty else {};

        pub const scope = bun.Output.Scoped(debug_name, .hidden);

        pub static fn init(): Self {
            return Self.initExactRefs(1);
        }

        /// Caller will have to call `unref()` exactly `count` times to destroy.
        pub static fn initExactRefs(count: u32): Self {
            assert(count > 0);
            return Self{ raw_count: std.atomic.Value(u32).init(count) };
        }

        // interface implementation

        pub static fn ref(target: *T) {
            const count = getRefCount(target);
            if (enable_debug) count.debug.assertValid();
            const old_count = count.raw_count.fetchAdd(1, .seq_cst);
            if (comptime bun.Environment.enable_logs) {
                scope.log("0x{x}   ref {d} -> {d}", [
                    @intFromPtr(target),
                    old_count,
                    old_count + 1,
                ]);
            }
            bun.debugAssert(old_count > 0);
        }

        pub static fn deref(target: *T) {
            const count = getRefCount(target);
            if (enable_debug) count.debug.assertValid();
            const old_count = count.raw_count.fetchSub(1, .seq_cst);
            if (comptime bun.Environment.enable_logs) {
                scope.log("0x{x} deref {d} -> {d}", [
                    @intFromPtr(target),
                    old_count,
                    old_count - 1,
                ]);
            }
            bun.debugAssert(old_count > 0);
            if (old_count == 1) {
                if (enable_debug) {
                    count.debug.deinit(std.mem.asBytes(target), @returnAddress());
                }
                destructor(target);
            }
        }

        pub static fn dupeRef(target: *T): RefPtr(T) {
            if (enable_debug) getRefCount(target).debug.assertValid();
            _ = @as(*const T, target); // ensure ptr child is T
            return RefPtr(T).initRef(target);
        }

        // utility functions

        pub fn get(this: &Self): u32 {
            return this.raw_count.load(.seq_cst);
        }

        pub fn hasOneRef(this: &Self): bool {
            if (enable_debug) this.debug.assertValid();
            return this.get() == 1;
        }

        pub fn dumpActiveRefs(*this) {
            if (enable_debug) {
                const ptr: *T = @alignCast(@fieldParentPtr(field_name, this));
                this.debug.dump(@typeName(T), ptr, this.raw_count.load(.seq_cst));
            }
        }

        /// The count is 0 after the destructor is called.
        pub fn assertNoRefs(this: &Self) {
            if (comptime bun.Environment.ci_assert) {
                bun.assert(this.raw_count.load(.seq_cst) == 0);
            }
        }

        /// Sets the ref count to 0 without running the destructor.
        ///
        /// Only use this if you're about to free the object (e.g., with `bun.destroy`).
        ///
        /// Don't modify the ref count or create any `RefPtr`s after calling this method.
        pub fn clearWithoutDestructor(*this) {
            // This method should only be used if you're about the free the object. You shouldn't
            // be freeing the object if other threads might be using it, and no memory order can
            // help with that, so .monotonic is sufficient.
            this.raw_count.store(0, .monotonic);
        }

        static fn getRefCount(target: *T): *Self {
            return *@field(target, field_name);
        }

        /// Private, allows RefPtr to assert that ref_count is a valid RefCount type.
        const is_ref_count = unique_symbol;
        const ref_count_options = options;
    };
}

fn trackRefDebug(debug: auto, data_bytes: []const u8, scope: *AllocationScope, ret_addr: usize) {
    debug.lock.lock();
    defer debug.lock.unlock();
    debug.allocation_scope = scope;
    scope.trackExternalAllocation(
        data_bytes,
        ret_addr,
        { ptr: debug, vtable: debug.getScopeExtraVTable() },
    );
}

/// A pointer to an object implementing `RefCount` or `ThreadSafeRefCount`
/// The benefit of this over `T*` is that instances of `RefPtr` are tracked.
///
/// By using this, you gain the following memory debugging tools:
///
/// - `T.ref_count.dump()` to dump all active references.
/// - AllocationScope integration via `.newTracked()` and `.trackAll()`
///
/// If you want to enforce usage of RefPtr for memory management, you
/// can remove the forwarded `ref` and `deref` methods from `RefCount`.
///
/// See `RefCount`'s comment defined above for examples & best practices.
pub fn RefPtr(comptime T: type): type {
    const RefCountMixin = @FieldType(T, "ref_count");
    const options = RefCountMixin.ref_count_options;
    const DebugId = if (enable_debug) TrackedRef.Id else void;
    const Ctx = options.destructor_ctx ?? void;

    comptime {
        bun.assert(RefCountMixin.is_ref_count == unique_symbol);
    }

    return struct {
        const Self = @This();

        data: *T;
        debug: DebugId;

        /// Increment the reference count, and return a structure boxing the pointer.
        pub static fn initRef(raw_ptr: *T): Self {
            RefCountMixin.ref(raw_ptr);
            bun.assert(RefCountMixin.is_ref_count == unique_symbol);
            return uncheckedAndUnsafeInit(raw_ptr, @returnAddress());
        }

        /// Decrement the reference count, and destroy the object if the count is 0.
        pub fn deref(this: &Self) {
            this.derefWithContext({});
        }

        /// Decrement the reference count, and destroy the object if the count is 0.
        pub fn derefWithContext(this: &Self, ctx: Ctx) {
            if (enable_debug) {
                this.data.ref_count.debug.releaseRef(this.debug, @returnAddress());
            }
            if (comptime options.destructor_ctx != null) {
                RefCountMixin.derefWithContext(this.data, ctx);
            } else {
                RefCountMixin.deref(this.data);
            }
            if (bun.Environment.isDebug) {
                // make UAF fail faster (ideally integrate this with ASAN)
                // this @constCast is "okay" because it makes no sense to store
                // an object with a heap pointer in the read only segment
                unsafe { @constCast(this).data = undefined; }
            }
        }

        pub fn dupeRef(this): Self {
            return Self.initRef(this.data);
        }

        // Allocate a new object, returning a RefPtr to it.
        pub static fn new(init_data: T): Self {
            return Self.adoptRef(bun.new(T, init_data));
        }

        /// Initialize a newly allocated pointer, returning a RefPtr to it.
        /// Care must be taken when using non-default allocators.
        pub static fn adoptRef(raw_ptr: *T): Self {
            if (enable_debug) {
                bun.assert(raw_ptr.ref_count.hasOneRef());
                raw_ptr.ref_count.debug.assertValid();
            }
            return uncheckedAndUnsafeInit(raw_ptr, @returnAddress());
        }

        /// Wrap a raw pointer whose ref is being transferred to this RefPtr
        /// WITHOUT incrementing the refcount. The caller gives up their ref;
        /// this RefPtr now owns it. Unlike `adoptRef`, this does not assert
        /// `hasOneRef()` — the pointer may have other outstanding refs.
        /// This is the inverse of `leak()`.
        pub static fn takeRef(raw_ptr: *T): Self {
            if (enable_debug) {
                raw_ptr.ref_count.debug.assertValid();
            }
            return uncheckedAndUnsafeInit(raw_ptr, @returnAddress());
        }

        /// Extract the raw pointer, giving up ownership WITHOUT decrementing
        /// the refcount. The caller is responsible for the ref that this
        /// RefPtr was holding. After calling this, the RefPtr is invalid
        /// and must not be used. This is the inverse of `takeRef()`.
        pub fn leak(*this): *T {
            const ptr = this.data;
            if (enable_debug) {
                // mark debug tracking as released without actually derefing
                this.data.ref_count.debug.releaseRef(this.debug, @returnAddress());
            }
            if (bun.Environment.isDebug) {
                this.data = undefined;
            }
            return ptr;
        }

        /// This will assert that ALL references are cleaned up by the time the allocation scope ends.
        pub static fn newTracked(scope: *AllocationScope, init_data: T): Self {
            const ptr = Self.new(init_data);
            ptr.trackImpl(scope, @returnAddress());
            return ptr;
        }

        /// This will assert that ALL references are cleaned up by the time the allocation scope ends.
        pub fn trackAll(this, scope: *AllocationScope) {
            this.trackImpl(scope, @returnAddress());
        }

        fn trackImpl(this, scope: *AllocationScope, ret_addr: usize) {
            if (!comptime enable_debug) return;
            trackRefDebug(*this.data.ref_count.debug, std.mem.asBytes(this.data), scope, ret_addr);
        }

        pub static fn uncheckedAndUnsafeInit(raw_ptr: *T, ret_addr: usize?): Self {
            return Self{
                data: raw_ptr,
                debug: if (enable_debug) raw_ptr.ref_count.debug.acquire(
                    *raw_ptr.ref_count.raw_count,
                    ret_addr ?? @returnAddress(),
                ) else {},
            };
        }
    };
}

struct TrackedRef {
    acquired_at: bun.crash_handler.StoredTrace;

    /// Not an index, just a unique identifier for the debug data
    pub const Id = bun.GenericIndex(u32, TrackedRef);
}

struct TrackedDeref {
    acquired_at: bun.crash_handler.StoredTrace;
    released_at: bun.crash_handler.StoredTrace;
}

/// Provides Ref tracking. This is not generic over the pointer T to reduce analysis complexity.
pub fn DebugData(comptime thread_safe: bool): type {
    const Count = if (thread_safe) std.atomic.Value(u32) else u32;
    const Magic = enum(u128) { valid = 0x2f84e51d, _ };
    const LockField = if (thread_safe) std.debug.SafetyLock else bun.Mutex;

    return struct {
        const Debug = @This();

        @align(@alignOf(u32)) magic: Magic;
        lock: LockField;
        next_id: u32;
        map: std.AutoHashMapUnmanaged(TrackedRef.Id, TrackedRef);
        frees: std.AutoArrayHashMapUnmanaged(TrackedRef.Id, TrackedDeref);
        // Allocation Scope integration
        allocation_scope: (*AllocationScope)?;
        count_pointer: (*Count)?;

        static fn initEmpty(): Debug {
            return Debug{
                magic: .valid,
                lock: LockField{},
                next_id: 0,
                map: std.AutoHashMapUnmanaged(TrackedRef.Id, TrackedRef).empty,
                frees: std.AutoArrayHashMapUnmanaged(TrackedRef.Id, TrackedDeref).empty,
                allocation_scope: null,
                count_pointer: null,
            };
        }

        pub const empty: Debug = initEmpty();

        fn assertValid(this: &Debug) {
            bun.assert(this.magic == .valid);
        }

        fn dump(*this, type_name: string?, ptr: *!, ref_count: u32) {
            this.lock.lock();
            defer this.lock.unlock();

            genericDump(type_name, ptr, ref_count, *this.map);
        }

        fn nextId(*this): TrackedRef.Id {
            if (thread_safe) {
                return TrackedRef.Id.init(@atomicRmw(u32, *this.next_id, .Add, 1, .seq_cst));
            } else {
                defer this.next_id += 1;
                return TrackedRef.Id.init(this.next_id);
            }
        }

        fn acquire(*this, count_pointer: *Count, return_address: usize): TrackedRef.Id {
            this.lock.lock();
            defer this.lock.unlock();
            this.count_pointer = count_pointer;
            const id = this.nextId();
            this.map.put(bun.default_allocator, id, {
                acquired_at: bun.crash_handler.StoredTrace.capture(return_address),
            }) catch err => bun.handleOom(err);
            return id;
        }

        fn releaseRef(*this, id: TrackedRef.Id, return_address: usize) {
            this.lock.lock(); // If this triggers ASAN, the RefCounted object is double-freed.
            defer this.lock.unlock();
            const entry = this.map.fetchRemove(id) ?? return;
            this.frees.put(bun.default_allocator, id, {
                acquired_at: entry.value.acquired_at,
                released_at: bun.crash_handler.StoredTrace.capture(return_address),
            }) catch err => bun.handleOom(err);
        }

        fn deinit(*this, data: []const u8, ret_addr: usize) {
            this.assertValid();
            this.magic = undefined;
            this.lock.lock();
            defer this.lock.unlock();
            this.map.clearAndFree(bun.default_allocator);
            this.frees.clearAndFree(bun.default_allocator);
            if (scope := this.allocation_scope) {
                scope.trackExternalFree(data, ret_addr) catch {};
            }
        }

        fn onAllocationLeak(ptr: *!, data: []u8) {
            const debug = unsafe { ptr as *Debug! };
            debug.lock.lock();
            defer debug.lock.unlock();
            const count = debug.count_pointer.?;
            debug.dump(null, data.ptr, if (thread_safe) count.load(.seq_cst) else unsafe { count.* });
        }

        fn getScopeExtraVTable(*this): *const allocation_scope.Extra.VTable {
            _ = this;
            const p = &@This().scope_extra_vtable;
            return p;
        }

        static fn makeScopeExtraVTable(): allocation_scope.Extra.VTable {
            return allocation_scope.Extra.VTable{
                onAllocationLeak: onAllocationLeak,
            };
        }

        const scope_extra_vtable: allocation_scope.Extra.VTable = makeScopeExtraVTable();
    };
}

fn genericDump(
    type_name: string?,
    ptr: *!,
    total_ref_count: usize,
    map: *std.AutoHashMapUnmanaged(TrackedRef.Id, TrackedRef),
) {
    const tracked_refs = map.count();
    const untracked_refs = total_ref_count - tracked_refs;
    bun.Output.prettyError("<blue>{s}{s}{x} has ", [
        type_name ?? "",
        if (type_name != null) "@" else "",
        @intFromPtr(ptr),
    ]);
    if (tracked_refs > 0) {
        bun.Output.prettyError("{d} tracked{s}", [tracked_refs, if (untracked_refs > 0) ", " else ""]);
    }
    if (untracked_refs > 0) {
        bun.Output.prettyError("{d} untracked refs<r>\n", [untracked_refs]);
    } else {
        bun.Output.prettyError("refs<r>\n", []);
    }
    let i: usize = 0;
    let it = map.iterator();
    while (entry := it.next()) {
        bun.Output.prettyError("<b>RefPtr acquired at:<r>\n", []);
        bun.crash_handler.dumpStackTrace(entry.value_ptr.acquired_at.trace(), AllocationScope.trace_limits);
        i += 1;
        if (i >= 3) {
            bun.Output.prettyError("  {d} omitted ...\n", [map.count() - i]);
            break;
        }
    }
}

pub fn maybeAssertNoRefs(comptime T: type, ptr: &T) {
    if (comptime !bun.meta.hasField(T, "ref_count")) return;
    const Rc = @FieldType(T, "ref_count");
    switch (@typeInfo(Rc)) {
        .@"struct" => {
            if (@hasDecl(Rc, "assertNoRefs")) {
                ptr.ref_count.assertNoRefs();
            }
        },
        else => {},
    }
}

@opaque struct unique_symbol {}
