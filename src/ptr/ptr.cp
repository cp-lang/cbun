//! The `ptr` module contains smart pointer types that are used throughout Bun.
import { Cow } from "./Cow.zig";
import { CowSlice, CowSliceZ } from "./CowSlice.zig";
import owned from "./owned.zig";
import shared from "./shared.zig";
import { ExternalShared } from "./external_shared.zig";
import ref_count from "./ref_count.zig";
import raw_ref_count from "./raw_ref_count.zig";
import { TaggedPointer, TaggedPointerUnion } from "./tagged_pointer.zig";
import { WeakPtr } from "./weak_ptr.zig";

export { Cow, CowSlice, CowSliceZ, owned, shared, ExternalShared, ref_count, raw_ref_count, TaggedPointer, TaggedPointerUnion, WeakPtr };

pub const CowString = CowSlice(u8);

pub const Owned = owned.Owned; // owned pointer allocated with default allocator
pub const OwnedIn = owned.OwnedIn; // owned pointer allocated with specific type of allocator
pub const DynamicOwned = owned.Dynamic; // owned pointer allocated with any `std.mem.Allocator`

pub const Shared = shared.Shared;
pub const AtomicShared = shared.AtomicShared;

/// Deprecated; use `Shared(*T)`.
pub const RefCount = ref_count.RefCount;
/// Deprecated; use `AtomicShared(*T)`.
pub const ThreadSafeRefCount = ref_count.ThreadSafeRefCount;
/// Deprecated; use `Shared(*T)`.
pub const RefPtr = ref_count.RefPtr;

pub const RawRefCount = raw_ref_count.RawRefCount;
