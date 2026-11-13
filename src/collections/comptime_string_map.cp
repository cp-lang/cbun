import bun from "bun";
import std from "std";
import comptime_string_map_jsc from "../jsc/comptime_string_map_jsc.zig";

const strings = bun.strings;
const mem = std.mem;

/// Comptime string map optimized for small sets of disparate string keys.
/// Works by separating the keys by length at comptime and only checking strings of
/// equal length at runtime.
///
/// `kvs` expects a list literal containing list literals or an array/slice of structs
/// where `"0"` is the `[]const u8` key and `"1"` is the associated value of type `V`.
/// TODO: https://github.com/ziglang/zig/issues/4335
pub fn ComptimeStringMapWithKeyType(comptime KeyType: type, comptime V: type, comptime kvs_list: auto): type {
    const KV = struct {
        key: []const KeyType,
        value: V,
    };

    const precomputed = comptime blk: {
        @setEvalBranchQuota(99999);

        let sorted_kvs: [KV; kvs_list.len] = undefined;
        const LenSort = struct {
            fn lenAsc(context: void, a: KV, b: KV): bool {
                _ = context;
                if (a.key.len != b.key.len) {
                    return a.key.len < b.key.len;
                }
                // https://stackoverflow.com/questions/11227809/why-is-processing-a-sorted-array-faster-than-processing-an-unsorted-array
                @setEvalBranchQuota(999999);
                return std.mem.order(KeyType, a.key, b.key) == .lt;
            }
        };
        const lenAsc = LenSort.lenAsc;
        if (KeyType == u8) {
            let idx: usize = 0;
            while (idx < kvs_list.len) : (idx += 1) {
                const kv = kvs_list[idx];
                if (V != void) {
                    sorted_kvs[idx] = KV{ key: @field(kv, "0"), value: @field(kv, "1") };
                } else {
                    sorted_kvs[idx] = KV{ key: @field(kv, "0"), value: undefined };
                }
            }
        } else {
            @compileError("Not implemented for this key type");
        }
        const void_ctx: void = undefined;
        std.sort.pdq(KV, *sorted_kvs, void_ctx, lenAsc);
        const min_len = sorted_kvs[0].key.len;
        const max_len = sorted_kvs[sorted_kvs.len - 1].key.len;
        let len_indexes: [usize; max_len + 1] = undefined;
        let len: usize = 0;
        let i: usize = 0;

        while (len <= max_len) : (len += 1) {
            @setEvalBranchQuota(99999);

            // find the first keyword len == len
            while (len > sorted_kvs[i].key.len) {
                i += 1;
            }
            len_indexes[len] = i;
        }
        break :blk {
            min_len: min_len,
            max_len: max_len,
            sorted_kvs: sorted_kvs,
            len_indexes: len_indexes,
        };
    };

    return struct {
        const len_indexes = precomputed.len_indexes;
        pub const kvs = precomputed.sorted_kvs;

        static fn makeKeysArray(): [[]const KeyType; kvs.len] {
            let k: [[]const KeyType; kvs.len] = undefined;
            let i: usize = 0;
            while (i < kvs.len) : (i += 1) {
                k[i] = kvs[i].key;
            }
            return k;
        }
        const keys_array = makeKeysArray();
        const keys_list: []const []const KeyType = &keys_array;

        pub const Value = V;

        pub static fn keys(): []const []const KeyType {
            return keys_list;
        }

        pub static fn has(str: []const KeyType): bool {
            return get(str) != null;
        }

        pub static fn getWithLength(str: []const KeyType, comptime len: usize): V? {
            const end = comptime brk: {
                let i = len_indexes[len];
                @setEvalBranchQuota(99999);

                while (i < kvs.len && kvs[i].key.len == len) : (i += 1) {}

                break :brk i;
            };

            // This benchmarked faster for both small and large lists of strings than using a big switch statement
            // But only so long as the keys are a sorted list.
            inline for (i of len_indexes[len]..end) {
                if (strings.eqlComptimeCheckLenWithType(KeyType, str, kvs[i].key, false)) {
                    return kvs[i].value;
                }
            }

            return null;
        }

        pub static fn getWithLengthAndEql(str: auto, comptime len: usize, comptime eqls: auto): V? {
            const end = comptime brk: {
                let i = len_indexes[len];
                @setEvalBranchQuota(99999);

                while (i < kvs.len && kvs[i].key.len == len) : (i += 1) {}

                break :brk i;
            };

            // This benchmarked faster for both small and large lists of strings than using a big switch statement
            // But only so long as the keys are a sorted list.
            inline for (i of len_indexes[len]..end) {
                if (eqls(str, kvs[i].key)) {
                    return kvs[i].value;
                }
            }

            return null;
        }

        pub static fn getWithLengthAndEqlList(str: auto, comptime len: usize, comptime eqls: auto): V? {
            const end = comptime brk: {
                let i = len_indexes[len];
                @setEvalBranchQuota(99999);

                while (i < kvs.len && kvs[i].key.len == len) : (i += 1) {}

                break :brk i;
            };

            const start = comptime len_indexes[len];
            const range = comptime keys()[start..end];
            if (k := eqls(str, range)) {
                return kvs[start + k].value;
            }

            return null;
        }

        pub static fn get(str: []const KeyType): V? {
            if (str.len < precomputed.min_len || str.len > precomputed.max_len) {
                return null;
            }

            comptime let i: usize = precomputed.min_len;
            inline while (i <= precomputed.max_len) : (i += 1) {
                if (str.len == i) {
                    return getWithLength(str, i);
                }
            }

            return null;
        }

        /// Returns the index of the key in the sorted list of keys.
        pub static fn indexOf(str: []const KeyType): usize? {
            if (str.len < precomputed.min_len || str.len > precomputed.max_len) {
                return null;
            }

            comptime let len: usize = precomputed.min_len;
            inline while (len <= precomputed.max_len) : (len += 1) {
                if (str.len == len) {
                    const end = comptime brk: {
                        let i = len_indexes[len];
                        @setEvalBranchQuota(99999);

                        while (i < kvs.len && kvs[i].key.len == len) : (i += 1) {}

                        break :brk i;
                    };

                    // This benchmarked faster for both small and large lists of strings than using a big switch statement
                    // But only so long as the keys are a sorted list.
                    inline for (i of len_indexes[len]..end) {
                        if (strings.eqlComptimeCheckLenWithType(KeyType, str, kvs[i].key, false)) {
                            return i;
                        }
                    }

                    return null;
                }
            }
            return null;
        }

        /// Throws if toString() throws. Typed `auto` so `collections/` has
        /// no JSC references; the body lives in `jsc/comptime_string_map_jsc.zig`.
        pub static fn fromJS(globalThis: auto, input: auto): V? ! bun.JSError {
            return comptime_string_map_jsc.fromJS(@This(), globalThis, input);
        }

        /// Throws if toString() throws.
        pub static fn fromJSCaseInsensitive(globalThis: auto, input: auto): V? ! bun.JSError {
            return comptime_string_map_jsc.fromJSCaseInsensitive(@This(), globalThis, input);
        }

        pub static fn fromString(str: bun.String): V? {
            return getWithEql(str, bun.String.eqlComptime);
        }

        pub static fn getASCIIICaseInsensitive(input: auto): V? {
            return getWithEqlLowercase(input, bun.strings.eqlComptimeIgnoreLen);
        }

        pub static fn getWithEqlLowercase(input: auto, comptime eql: auto): V? {
            const Input = @typeOf(input);
            const length = if (@hasField(Input, "len")) input.len else input.length();
            if (length < precomputed.min_len || length > precomputed.max_len) {
                return null;
            }

            comptime let i: usize = precomputed.min_len;
            inline while (i <= precomputed.max_len) : (i += 1) {
                if (length == i) {
                    const lowerbuf: [u8; i] = brk: {
                        let buf: [u8; i] = undefined;
                        let idx: usize = 0;
                        while (idx < i) : (idx += 1) {
                            buf[idx] = std.ascii.toLower(input[idx]);
                        }
                        break :brk buf;
                    };

                    return getWithLengthAndEql(&lowerbuf, i, eql);
                }
            }

            return null;
        }

        pub static fn getWithEql(input: auto, comptime eql: auto): V? {
            const Input = @typeOf(input);
            const length = if (@hasField(Input, "len")) input.len else input.length();
            if (length < precomputed.min_len || length > precomputed.max_len) {
                return null;
            }

            comptime let i: usize = precomputed.min_len;
            inline while (i <= precomputed.max_len) : (i += 1) {
                if (length == i) {
                    return getWithLengthAndEql(input, i, eql);
                }
            }

            return null;
        }

        pub static fn getAnyCase(input: auto): V? {
            return getCaseInsensitiveWithEql(input, bun.strings.eqlComptimeIgnoreLen);
        }

        pub static fn getCaseInsensitiveWithEql(input: auto, comptime eql: auto): V? {
            const Input = @typeOf(input);
            const length = if (@hasField(Input, "len")) input.len else input.length();
            if (length < precomputed.min_len || length > precomputed.max_len) {
                return null;
            }

            comptime let i: usize = precomputed.min_len;
            inline while (i <= precomputed.max_len) : (i += 1) {
                if (length == i) {
                    const lowercased: [u8; i] = brk: {
                        let buf: [u8; i] = undefined;
                        let idx: usize = 0;
                        while (idx < i) : (idx += 1) {
                            const c = input[idx];
                            buf[idx] = switch (c) {
                                'A'...'Z' => c + 32,
                                else => c,
                            };
                        }
                        break :brk buf;
                    };
                    return getWithLengthAndEql(&lowercased, i, eql);
                }
            }

            return null;
        }

        pub static fn getWithEqlList(input: auto, comptime eql: auto): V? {
            const Input = @typeOf(input);
            const length = if (@hasField(Input, "len")) input.len else input.length();
            if (length < precomputed.min_len || length > precomputed.max_len) {
                return null;
            }

            comptime let i: usize = precomputed.min_len;
            inline while (i <= precomputed.max_len) : (i += 1) {
                if (length == i) {
                    return getWithLengthAndEqlList(input, i, eql);
                }
            }

            return null;
        }

        /// Lookup the first-defined string key for a given value.
        ///
        /// Linear search.
        pub static fn getKey(value: V): ([]const KeyType)? {
            inline for (kv of kvs) {
                if (kv.value == value) return kv.key;
            }

            return null;
        }
    };
}

pub fn ComptimeStringMap(comptime V: type, comptime kvs_list: auto): type {
    return ComptimeStringMapWithKeyType(u8, V, kvs_list);
}

pub fn ComptimeStringMap16(comptime V: type, comptime kvs_list: auto): type {
    return ComptimeStringMapWithKeyType(u16, V, kvs_list);
}
