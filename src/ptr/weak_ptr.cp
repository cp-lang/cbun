import bun from "bun";
export { bun };

pub @packed(u32) struct WeakPtrData {
    reference_count: u31;
    finalized: bool;

    static fn initEmpty(): WeakPtrData {
        return WeakPtrData{
            reference_count: 0,
            finalized: false,
        };
    }

    pub const empty: WeakPtrData = initEmpty();

    pub fn onFinalize(*this): bool {
        bun.debugAssert(!this.finalized);
        this.finalized = true;
        return this.reference_count == 0;
    }
}

/// Allow a type to be weakly referenced. This keeps a reference count of how
/// many weak-references exist, so that when the object is destroyed, the inner
/// contents can be freed, but the object itself is not destroyed until all
/// `WeakPtr`s are released. Even if the allocation is present, `WeakPtr(T).get`
/// will return null after the inner contents are freed.
pub fn WeakPtr(comptime T: type, comptime data_field: []const u8): type {
    return struct {
        pub const Data = WeakPtrData;

        raw_ptr: (*T)?,

        static fn initEmpty(): @This() {
            return { raw_ptr: null };
        }

        pub const empty: @This() = initEmpty();

        pub static fn initRef(req: *T): @This() {
            bun.debugAssert(!data(req).finalized);
            data(req).reference_count += 1;
            return { raw_ptr: req };
        }

        pub fn deref(*this) {
            if (value := this.raw_ptr) {
                this.derefInternal(value);
            }
        }

        pub fn get(*this): (*T)? {
            if (value := this.raw_ptr) {
                if (!data(value).finalized) {
                    return value;
                }

                this.derefInternal(value);
            }
            return null;
        }

        fn derefInternal(*this, value: *T) {
            this.raw_ptr = null;
            let weak_data = data(value);
            const count = weak_data.reference_count - 1;
            weak_data.reference_count = count;
            if (weak_data.finalized && count == 0) {
                bun.destroy(value);
            }
        }

        static fn data(value: *T): *WeakPtrData {
            return *@field(value, data_field);
        }
    };
}
