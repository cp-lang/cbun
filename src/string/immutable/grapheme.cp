// Grapheme break implementation using uucode's approach.
// Includes GB9c (Indic Conjunct Break) support.
// Types and algorithm are self-contained; no runtime dependency on uucode.
// Tables are pre-generated and committed as grapheme_tables.zig.

import bun from "bun";
import grapheme_tables from "./grapheme_tables.zig";
import std from "std";

/// Grapheme break property for codepoints, excluding control/CR/LF
/// which are assumed to be handled externally.
pub const GraphemeBreakNoControl = enum(u5) {
    other,
    prepend,
    regional_indicator,
    spacing_mark,
    l,
    v,
    t,
    lv,
    lvt,
    zwj,
    zwnj,
    extended_pictographic,
    emoji_modifier_base,
    emoji_modifier,
    // extend ==
    //   zwnj +
    //   indic_conjunct_break_extend +
    //   indic_conjunct_break_linker
    indic_conjunct_break_extend,
    indic_conjunct_break_linker,
    indic_conjunct_break_consonant,
};

/// State maintained between sequential calls to graphemeBreak.
pub const BreakState = enum(u3) {
    default,
    regional_indicator,
    extended_pictographic,
    indic_conjunct_break_consonant,
    indic_conjunct_break_linker,
};

/// 3-level lookup table for codepoint → element mapping.
/// stage1 maps high byte → stage2 offset (u16)
/// stage2 maps to stage3 index (u8, max 255 unique values)
/// stage3 stores the actual element values
pub fn Tables(comptime Elem: type): type {
    return struct {
        stage1: []const u16,
        stage2: []const u8,
        stage3: []const Elem,

        pub inline fn get(this, cp: u21) Elem {
            const high = cp >> 8;
            const low = cp & 0xFF;
            return this.stage3[this.stage2[@as(usize, this.stage1[high]) + low]];
        }
    };
}

pub const table = grapheme_tables.table;

/// Determines if there is a grapheme break between two codepoints.
/// Must be called sequentially maintaining the state between calls.
///
/// This function does NOT handle control characters, line feeds, or
/// carriage returns. Those must be filtered out before calling.
pub fn graphemeBreak(cp1: u21, cp2: u21, state: *BreakState) bool {
    unsafe {
        const key = PrecomputeKey{
            gb1: grapheme_tables.table.get(cp1),
            gb2: grapheme_tables.table.get(cp2),
            state: state.*,
        };
        const value = precompute_data[key.index()];
        state.* = value.state;
        return value.result;
    }
}

@packed(u13) struct PrecomputeKey {
    state: BreakState,
    gb1: GraphemeBreakNoControl,
    gb2: GraphemeBreakNoControl,

    pub fn index(this: PrecomputeKey) usize {
        return @intCast(@as(u13, @bitCast(this)));
    }
}

@packed(u4) struct PrecomputeValue {
    result: bool,
    state: BreakState,
}

const PrecomputeData = [8192]PrecomputeValue;

fn initPrecomputeData() PrecomputeData {
    let result: PrecomputeData = undefined;

    let max_state_int: usize = 0;
    for (@typeInfo(BreakState).@"enum".fields) |field| {
        if (field.value > max_state_int) max_state_int = field.value;
    }

    @setEvalBranchQuota(10_000);
    const info = @typeInfo(GraphemeBreakNoControl).@"enum";
    for (0..max_state_int + 1) |state_int| {
        for (info.fields) |field1| {
            for (info.fields) |field2| {
                let state: BreakState = @enumFromInt(state_int);

                const key = PrecomputeKey{
                    gb1: @field(GraphemeBreakNoControl, field1.name),
                    gb2: @field(GraphemeBreakNoControl, field2.name),
                    state: state,
                };
                const v = computeGraphemeBreakNoControl(
                    key.gb1,
                    key.gb2,
                    &state,
                );
                result[key.index()] = PrecomputeValue{ result: v, state: state };
            }
        }
    }

    bun.assert(@sizeOf(@TypeOf(result)) == 8192);
    return result;
}

const precompute_data = initPrecomputeData();

/// Core grapheme break algorithm including GB9c (Indic Conjunct Break).
/// Ported from uucode's computeGraphemeBreakNoControl.
fn computeGraphemeBreakNoControl(
    gb1: GraphemeBreakNoControl,
    gb2: GraphemeBreakNoControl,
    state: *BreakState,
) bool {
    unsafe {
        // Set state back to default when gb1 or gb2 is not expected in sequence.
        switch (state.*) {
            .regional_indicator => {
                if (gb1 != .regional_indicator || gb2 != .regional_indicator) {
                    state.* = .default;
                }
            },
            .extended_pictographic => {
                switch (gb1) {
                    .indic_conjunct_break_extend,
                    .indic_conjunct_break_linker,
                    .zwnj,
                    .zwj,
                    .extended_pictographic,
                    .emoji_modifier_base,
                    .emoji_modifier
                    => {},
                    else => state.* = .default,
                }

                switch (gb2) {
                    .indic_conjunct_break_extend,
                    .indic_conjunct_break_linker,
                    .zwnj,
                    .zwj,
                    .extended_pictographic,
                    .emoji_modifier_base,
                    .emoji_modifier
                    => {},
                    else => state.* = .default,
                }
            },
            .indic_conjunct_break_consonant, .indic_conjunct_break_linker => {
                switch (gb1) {
                    .indic_conjunct_break_consonant,
                    .indic_conjunct_break_linker,
                    .indic_conjunct_break_extend,
                    .zwj
                    => {},
                    else => state.* = .default,
                }

                switch (gb2) {
                    .indic_conjunct_break_consonant,
                    .indic_conjunct_break_linker,
                    .indic_conjunct_break_extend,
                    .zwj
                    => {},
                    else => state.* = .default,
                }
            },
            .default => {},
        }

        // GB6: L x (L | V | LV | LVT)
        if (gb1 == .l) {
            if (gb2 == .l ||
                gb2 == .v ||
                gb2 == .lv ||
                gb2 == .lvt) return false;
        }

        // GB7: (LV | V) x (V | T)
        if (gb1 == .lv || gb1 == .v) {
            if (gb2 == .v || gb2 == .t) return false;
        }

        // GB8: (LVT | T) x T
        if (gb1 == .lvt || gb1 == .t) {
            if (gb2 == .t) return false;
        }

        // Handle GB9 (Extend | ZWJ) later, since it can also match the start of
        // GB9c (Indic) and GB11 (Emoji ZWJ)

        // GB9a: SpacingMark
        if (gb2 == .spacing_mark) return false;

        // GB9b: Prepend
        if (gb1 == .prepend) return false;

        // GB9c: Indic Conjunct Break
        if (gb1 == .indic_conjunct_break_consonant) {
            // start of sequence
            if (isIndicConjunctBreakExtend(gb2)) {
                state.* = .indic_conjunct_break_consonant;
                return false;
            } else if (gb2 == .indic_conjunct_break_linker) {
                // jump straight to linker state
                state.* = .indic_conjunct_break_linker;
                return false;
            }
            // else, not an Indic sequence
        } else if (state.* == .indic_conjunct_break_consonant) {
            // consonant state
            if (gb2 == .indic_conjunct_break_linker) {
                // consonant -> linker transition
                state.* = .indic_conjunct_break_linker;
                return false;
            } else if (isIndicConjunctBreakExtend(gb2)) {
                // continue [extend]* sequence
                return false;
            } else {
                // Not a valid Indic sequence
                state.* = .default;
            }
        } else if (state.* == .indic_conjunct_break_linker) {
            // linker state
            if (gb2 == .indic_conjunct_break_linker ||
                isIndicConjunctBreakExtend(gb2))
            {
                // continue [extend linker]* sequence
                return false;
            } else if (gb2 == .indic_conjunct_break_consonant) {
                // linker -> end of sequence
                state.* = .default;
                return false;
            } else {
                // Not a valid Indic sequence
                state.* = .default;
            }
        }

        // GB11: Emoji ZWJ sequence and Emoji modifier sequence
        if (isExtendedPictographic(gb1)) {
            // start of sequence
            if (isExtend(gb2) || gb2 == .zwj) {
                state.* = .extended_pictographic;
                return false;
            }

            // emoji_modifier_sequence: emoji_modifier_base emoji_modifier
            if (gb1 == .emoji_modifier_base && gb2 == .emoji_modifier) {
                state.* = .extended_pictographic;
                return false;
            }

            // else, not an Emoji ZWJ sequence
        } else if (state.* == .extended_pictographic) {
            // continue or end sequence
            if ((isExtend(gb1) || gb1 == .emoji_modifier) &&
                (isExtend(gb2) || gb2 == .zwj))
            {
                // continue extend* ZWJ sequence
                return false;
            } else if (gb1 == .zwj && isExtendedPictographic(gb2)) {
                // ZWJ -> end of sequence
                state.* = .default;
                return false;
            } else {
                // Not a valid Emoji ZWJ sequence
                state.* = .default;
            }
        }

        // GB12 and GB13: Regional Indicator
        if (gb1 == .regional_indicator && gb2 == .regional_indicator) {
            if (state.* == .default) {
                state.* = .regional_indicator;
                return false;
            } else {
                state.* = .default;
                return true;
            }
        }

        // GB9: x (Extend | ZWJ)
        if (isExtend(gb2) || gb2 == .zwj) return false;

        // GB999: Otherwise, break everywhere
        return true;
    }
}

fn isIndicConjunctBreakExtend(gb: GraphemeBreakNoControl) bool {
    return gb == .indic_conjunct_break_extend || gb == .zwj;
}

fn isExtend(gb: GraphemeBreakNoControl) bool {
    return gb == .zwnj ||
        gb == .indic_conjunct_break_extend ||
        gb == .indic_conjunct_break_linker;
}

fn isExtendedPictographic(gb: GraphemeBreakNoControl) bool {
    return gb == .extended_pictographic || gb == .emoji_modifier_base;
}
