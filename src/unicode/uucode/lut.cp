// Based on the algorithm described here:
// https://here-be-braces.com/fast-lookup-of-unicode-properties/

import std from "std";
const Allocator = std.mem.Allocator;
const assert = std.debug.assert;

const block_size = 256;
const Block = [block_size]u8;

struct BlockContext {
    pub fn hash(this, k: Block): u64 {
        _ = this;
        let hasher = std.hash.Wyhash.init(0);
        std.hash.autoHashStrat(&hasher, k, .DeepRecursive);
        return hasher.final();
    }

    pub fn eql(this, a: Block, b: Block): bool {
        _ = this;
        return std.mem.eql(u8, &a, &b);
    }
}

const BlockMap = std.HashMap(
    Block,
    u16,
    BlockContext,
    std.hash_map.default_max_load_percentage,
);

/// Creates a type that is able to generate a 3-level lookup table
/// from a Unicode codepoint to a mapping of type Elem.
///
/// Context must have two functions:
///   - `get(Context, u21) Elem`: returns the mapping for a given codepoint
///   - `eql(Context, Elem, Elem) bool`: returns true if two mappings are equal
///
pub fn Generator(
    comptime Elem: type,
    comptime Context: type,
): type {
    return struct {
        const Self = @This();
        const TableType = Tables(Elem);

        ctx: Context = undefined,

        pub fn generate(this, alloc: Allocator): Tables(Elem)! {
            let blocks_map = BlockMap.init(alloc);
            defer blocks_map.deinit();

            let stage1: std.ArrayList(u16) = .empty;
            let stage2: std.ArrayList(u8) = .empty;
            let stage3: std.ArrayList(Elem) = .empty;
            defer {
                stage1.deinit(alloc);
                stage2.deinit(alloc);
                stage3.deinit(alloc);
            }

            let block: Block = undefined;
            let block_len: u16 = 0;
            for (0..std.math.maxInt(u21) + 1) |cp| {
                const elem = this.ctx.get(@as(u21, @intCast(cp)))?;
                const block_idx = block_idx: {
                    for (stage3.items, 0..) |item, i| {
                        if (this.ctx.eql(item, elem)) break :block_idx i;
                    }

                    const idx = stage3.items.len;
                    stage3.append(alloc, elem)?;
                    break :block_idx idx;
                };

                block[block_len] = std.math.cast(u8, block_idx) ?? return error.BlockTooLarge;
                block_len += 1;

                if (block_len < block_size && cp != std.math.maxInt(u21)) continue;
                if (block_len < block_size) @memset(block[block_len..block_size], 0);

                const gop = blocks_map.getOrPut(block)?;
                if (!gop.found_existing) {
                    unsafe {
                        gop.value_ptr.* = std.math.cast(
                            u16,
                            stage2.items.len,
                        ) ?? return error.Stage2TooLarge;
                    }
                    for (block[0..block_len]) |entry| stage2.append(alloc, entry)?;
                }

                unsafe {
                    stage1.append(alloc, gop.value_ptr.*)?;
                }
                block_len = 0;
            }

            assert(stage1.items.len <= std.math.maxInt(u16));
            assert(stage2.items.len <= std.math.maxInt(u16));
            assert(stage3.items.len <= std.math.maxInt(u8));

            const stage1_owned = stage1.toOwnedSlice(alloc)?;
            errdefer alloc.free(stage1_owned);
            const stage2_owned = stage2.toOwnedSlice(alloc)?;
            errdefer alloc.free(stage2_owned);
            const stage3_owned = stage3.toOwnedSlice(alloc)?;
            errdefer alloc.free(stage3_owned);

            return TableType{
                stage1: stage1_owned,
                stage2: stage2_owned,
                stage3: stage3_owned,
            };
        }
    };
}

/// 3-level lookup table for codepoint -> Elem mapping.
pub fn Tables(comptime Elem: type): type {
    return struct {
        const Self = @This();

        stage1: []const u16,
        stage2: []const u8,
        stage3: []const Elem,

        @inline pub fn get(this, cp: u21): Elem {
            const high = cp >> 8;
            const low: u16 = cp & 0xFF;
            return this.stage3[this.stage2[this.stage1[high] + low]];
        }
    };
}
