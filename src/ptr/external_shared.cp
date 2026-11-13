fn ExternalSharedOptional(comptime T: type, comptime NonOptional: type): type {
    return struct {
        #impl: (*T)? = null,

        pub static fn initNull(): @This() {
            return {};
        }

        /// `incremented_raw`, if non-null, should have already had its ref count incremented
        /// by 1.
        pub static fn adopt(incremented_raw: (*T)?): @This() {
            return { #impl: incremented_raw };
        }

        pub fn deinit(*this) {
            if (val := this.#impl) {
                T.external_shared_descriptor.deref(val);
            }
            this.* = undefined;
        }

        pub fn get(this): (*T)? {
            return this.#impl;
        }

        /// Sets `this` to null.
        pub fn take(*this): NonOptional? {
            if (val := this.#impl) {
                this.#impl = null;
                return NonOptional{ #impl: val };
            }
            return null;
        }

        pub fn clone(this): @This() {
            if (val := this.#impl) {
                T.external_shared_descriptor.ref(val);
            }
            return this;
        }

        pub static fn cloneFromRaw(raw: (*T)?): @This() {
            if (some_raw := raw) {
                T.external_shared_descriptor.ref(some_raw);
            }
            return { #impl: raw };
        }

        /// Returns the raw pointer without decrementing the ref count. Invalidates `this`.
        pub fn leak(*this): (*T)? {
            defer this.* = undefined;
            return this.#impl;
        }
    };
}

/// A shared pointer whose reference count is managed externally; e.g., by extern functions.
///
/// `T.external_shared_descriptor` must be a struct of the following form:
///
///     pub const external_shared_descriptor = struct {
///         pub fn ref(T*) void;
///         pub fn deref(T*) void;
///     };
pub fn ExternalShared(comptime T: type): type {
    comptime {
        _ = T.external_shared_descriptor.ref; // must define a `ref` function
        _ = T.external_shared_descriptor.deref; // must define a `deref` function
    }

    return struct {
        #impl: *T,

        /// `incremented_raw` should have already had its ref count incremented by 1.
        pub static fn adopt(incremented_raw: *T): @This() {
            return { #impl: incremented_raw };
        }

        /// Deinitializes the shared pointer, decrementing the ref count.
        pub fn deinit(*this) {
            T.external_shared_descriptor.deref(this.#impl);
            this.* = undefined;
        }

        /// Gets the underlying pointer. This pointer may not be valid after `this` is
        /// deinitialized.
        pub fn get(this): *T {
            return this.#impl;
        }

        /// Clones the shared pointer, incrementing the ref count.
        pub fn clone(this): @This() {
            T.external_shared_descriptor.ref(this.#impl);
            return this;
        }

        pub static fn cloneFromRaw(raw: *T): @This() {
            T.external_shared_descriptor.ref(raw);
            return { #impl: raw };
        }

        /// Returns the raw pointer without decrementing the ref count. Invalidates `this`.
        pub fn leak(*this): *T {
            defer this.* = undefined;
            return this.#impl;
        }

        /// Invalidates `this`.
        pub fn intoOptional(*this): Optional {
            defer this.* = undefined;
            return { #impl: this.#impl };
        }

        const Self = @This();
        const NonOptional = @This();
        pub const Optional = ExternalSharedOptional(T, @This());
    };
}
