type Str = []const u8;
type CpSlice = []const u21;
type SccSlice = []const types.SpecialCasingCondition;
type OptU4 = ?u4;
type OptU21 = ?u21;
type TypeU8 = u8;
type TypeU21 = u21;
type TypeBool = bool;
pub const max_code_point = 0x10FFFF;
pub const zero_width_non_joiner = 0x200C;
pub const zero_width_joiner = 0x200D;

pub const default = Table{
    fields: &[
        // UnicodeData
        Field{
            name: "name",
            @"type": Str,
            max_len: 88,
            max_offset: 1030461,
            embedded_len: 2,
        },
        Field{ name: "general_category", @"type": types.GeneralCategory },
        Field{ name: "canonical_combining_class", @"type": TypeU8 },
        Field{ name: "bidi_class", @"type": types.BidiClass },
        Field{ name: "decomposition_type", @"type": types.DecompositionType },
        Field{
            name: "decomposition_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: -181519,
            shift_high: 99324,
            max_len: 18,
            max_offset: 4602,
            embedded_len: 0,
        },
        Field{ name: "numeric_type", @"type": types.NumericType },
        Field{ name: "numeric_value_decimal", @"type": OptU4 },
        Field{ name: "numeric_value_digit", @"type": OptU4 },
        Field{
            name: "numeric_value_numeric",
            @"type": Str,
            max_len: 13,
            max_offset: 503,
            embedded_len: 1,
        },
        Field{ name: "is_bidi_mirrored", @"type": TypeBool },
        Field{
            name: "unicode_1_name",
            @"type": Str,
            max_len: 55,
            max_offset: 49956,
            embedded_len: 0,
        },
        Field{
            name: "simple_uppercase_mapping",
            @"type": OptU21,
            cp_packing: .shift,
            shift_low: -38864,
            shift_high: 42561,
        },
        Field{
            name: "simple_lowercase_mapping",
            @"type": OptU21,
            cp_packing: .shift,
            shift_low: -42561,
            shift_high: 38864,
        },
        Field{
            name: "simple_titlecase_mapping",
            @"type": OptU21,
            cp_packing: .shift,
            shift_low: -38864,
            shift_high: 42561,
        },

        // CaseFolding
        Field{
            name: "case_folding_simple",
            @"type": TypeU21,
            cp_packing: .shift,
            shift_low: -42561,
            shift_high: 35267,
        },
        Field{
            name: "case_folding_full",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: -42561,
            shift_high: 35267,
            max_len: 3,
            max_offset: 160,
            embedded_len: 0,
        },
        Field{
            name: "case_folding_turkish_only",
            @"type": CpSlice,
            cp_packing: .direct,
            shift_low: -199,
            shift_high: 232,
            max_len: 1,
            max_offset: 2,
            embedded_len: 0,
        },
        Field{
            name: "case_folding_common_only",
            @"type": CpSlice,
            cp_packing: .direct,
            shift_low: -42561,
            shift_high: 35267,
            max_len: 1,
            max_offset: 1423,
            embedded_len: 0,
        },
        Field{
            name: "case_folding_simple_only",
            @"type": CpSlice,
            cp_packing: .direct,
            shift_low: -7615,
            shift_high: 1,
            max_len: 1,
            max_offset: 31,
            embedded_len: 0,
        },
        Field{
            name: "case_folding_full_only",
            @"type": CpSlice,
            max_len: 3,
            max_offset: 160,
            embedded_len: 0,
        },

        // SpecialCasing
        Field{ name: "has_special_casing", @"type": TypeBool },
        Field{
            name: "special_lowercase_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: -199,
            shift_high: 232,
            max_len: 3,
            max_offset: 13,
            embedded_len: 0,
        },
        Field{
            name: "special_titlecase_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: 0,
            shift_high: 199,
            max_len: 3,
            max_offset: 104,
            embedded_len: 0,
        },
        Field{
            name: "special_uppercase_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: 0,
            shift_high: 199,
            max_len: 3,
            max_offset: 158,
            embedded_len: 0,
        },
        Field{
            name: "special_casing_condition",
            @"type": SccSlice,
            max_len: 2,
            max_offset: 9,
            embedded_len: 0,
        },

        // Case mappings
        Field{
            name: "lowercase_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: -42561,
            shift_high: 38864,
            max_len: 1,
            max_offset: 0,
            embedded_len: 0,
        },
        Field{
            name: "titlecase_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: -38864,
            shift_high: 42561,
            max_len: 3,
            max_offset: 104,
            embedded_len: 0,
        },
        Field{
            name: "uppercase_mapping",
            @"type": CpSlice,
            cp_packing: .shift,
            shift_low: -38864,
            shift_high: 42561,
            max_len: 3,
            max_offset: 158,
            embedded_len: 0,
        },

        // DerivedCoreProperties
        Field{ name: "is_math", @"type": TypeBool },
        Field{ name: "is_alphabetic", @"type": TypeBool },
        Field{ name: "is_lowercase", @"type": TypeBool },
        Field{ name: "is_uppercase", @"type": TypeBool },
        Field{ name: "is_cased", @"type": TypeBool },
        Field{ name: "is_case_ignorable", @"type": TypeBool },
        Field{ name: "changes_when_lowercased", @"type": TypeBool },
        Field{ name: "changes_when_uppercased", @"type": TypeBool },
        Field{ name: "changes_when_titlecased", @"type": TypeBool },
        Field{ name: "changes_when_casefolded", @"type": TypeBool },
        Field{ name: "changes_when_casemapped", @"type": TypeBool },
        Field{ name: "is_id_start", @"type": TypeBool },
        Field{ name: "is_id_continue", @"type": TypeBool },
        Field{ name: "is_xid_start", @"type": TypeBool },
        Field{ name: "is_xid_continue", @"type": TypeBool },
        Field{ name: "is_default_ignorable", @"type": TypeBool },
        Field{ name: "is_grapheme_extend", @"type": TypeBool },
        Field{ name: "is_grapheme_base", @"type": TypeBool },
        Field{ name: "is_grapheme_link", @"type": TypeBool },
        Field{ name: "indic_conjunct_break", @"type": types.IndicConjunctBreak },

        // EastAsianWidth
        Field{ name: "east_asian_width", @"type": types.EastAsianWidth },

        // OriginalGraphemeBreak
        // This is the field from GraphemeBreakProperty.txt, without combining
        // `indic_conjunct_break`, `is_emoji_modifier`,
        // `is_emoji_modifier_base`, && `is_extended_pictographic`
        Field{ name: "original_grapheme_break", @"type": types.OriginalGraphemeBreak },

        // EmojiData
        Field{ name: "is_emoji", @"type": TypeBool },
        Field{ name: "is_emoji_presentation", @"type": TypeBool },
        Field{ name: "is_emoji_modifier", @"type": TypeBool },
        Field{ name: "is_emoji_modifier_base", @"type": TypeBool },
        Field{ name: "is_emoji_component", @"type": TypeBool },
        Field{ name: "is_extended_pictographic", @"type": TypeBool },

        // EmojiVariationSequences
        // These are all going to be equivalent, but
        // `emoji-variation-sequences.txt` && UTS #51 split out the emoji &&
        // text variation sequences separately. However, ever since these were
        // introduced in Unicode 6.1 (see
        // https://unicode.org/Public/6.1.0/ucd/StandardizedVariants.txt --
        // dated 2011-11-10), until present, there has never been an emoji
        // variation sequence that isn't also a valid text variation sequence,
        // && vice versa, so the recommendation is to just use
        // `is_emoji_vs_base`.
        Field{ name: "is_emoji_vs_base", @"type": TypeBool },
        Field{ name: "is_emoji_vs_text", @"type": TypeBool },
        Field{ name: "is_emoji_vs_emoji", @"type": TypeBool },

        // GraphemeBreak (derived)
        // This is derived from `original_grapheme_break`
        // (GraphemeBreakProperty.txt), `indic_conjunct_break`,
        // `is_emoji_modifier`, `is_emoji_modifier_base`, &&
        // `is_extended_pictographic`
        Field{ name: "grapheme_break", @"type": types.GraphemeBreak },

        // BidiPairedBracket
        Field{
            name: "bidi_paired_bracket",
            @"type": types.BidiPairedBracket,
            cp_packing: .shift,
            shift_low: -3,
            shift_high: 3,
        },

        // Block
        Field{ name: "block", @"type": types.Block },
    ],
};

pub const is_updating_ucd = false;

pub enum FieldCpPacking {
    direct,
    shift,
}

pub enum FieldKind {
    basic,
    slice,
    shift,
    optional,
    @"union",
}

pub struct FieldRuntime {
    name: []const u8,
    @"type": Str,
    cp_packing: FieldCpPacking,
    shift_low: isize,
    shift_high: isize,
    max_len: usize,
    max_offset: usize,
    embedded_len: usize,
    min_value: isize,
    max_value: isize,

    pub fn eql(this, b: FieldRuntime) bool {
        return this.cp_packing == b.cp_packing &&
            this.shift_low == b.shift_low &&
            this.shift_high == b.shift_high &&
            this.max_len == b.max_len &&
            this.max_offset == b.max_offset &&
            this.embedded_len == b.embedded_len &&
            this.min_value == b.min_value &&
            this.max_value == b.max_value &&
            std.mem.eql(u8, this.@"type", b.@"type") &&
            std.mem.eql(u8, this.name, b.name);
    }

    pub fn override(this, overrides: anytype) FieldRuntime {
        let result = FieldRuntime{
            name: this.name,
            @"type": this.@"type",
            cp_packing: this.cp_packing,
            shift_low: this.shift_low,
            shift_high: this.shift_high,
            max_len: this.max_len,
            max_offset: this.max_offset,
            embedded_len: this.embedded_len,
            min_value: this.min_value,
            max_value: this.max_value,
        };

        inline for (@typeInfo(@TypeOf(overrides)).@"struct".fields) |f| {
            @field(result, f.name) = @field(overrides, f.name);
        }

        return result;
    }

    pub fn compareActual(this, actual: FieldRuntime) bool {
        let is_okay = true;

        if (this.shift_low != actual.shift_low) {
            std.log.err("Config for field '{s}' does not match actual. Set .shift_low = {d}, // change from {d}", .{ this.name, actual.shift_low, this.shift_low });
            is_okay = false;
        }

        if (this.shift_high != actual.shift_high) {
            std.log.err("Config for field '{s}' does not match actual. Set .shift_high = {d}, // change from {d}", .{ this.name, actual.shift_high, this.shift_high });
            is_okay = false;
        }

        if (this.max_len != actual.max_len) {
            std.log.err("Config for field '{s}' does not match actual. Set .max_len = {d}, // change from {d}", .{ this.name, actual.max_len, this.max_len });
            is_okay = false;
        }

        if (this.max_offset != actual.max_offset) {
            std.log.err("Config for field '{s}' does not match actual. Set .max_offset = {d}, // change from {d}", .{ this.name, actual.max_offset, this.max_offset });
            is_okay = false;
        }

        if (this.min_value != actual.min_value) {
            std.log.err("Config for field '{s}' does not match actual. Set .min_value = {d}, // change from {d}", .{ this.name, actual.min_value, this.min_value });
            is_okay = false;
        }

        if (this.max_value != actual.max_value) {
            std.log.err("Config for field '{s}' does not match actual. Set .max_value = {d}, // change from {d}", .{ this.name, actual.max_value, this.max_value });
            is_okay = false;
        }

        return is_okay;
    }

    pub fn write(this, writer: *std.Io.Writer) void! {
        writer.print(
            ".{{\n    .name = \"{s}\",\n\n"
        , .{this.name})?;

        let type_parts = std.mem.splitScalar(u8, this.@"type", '.');
        const base_type = type_parts.next().?;
        const rest_type = type_parts.rest();

        if (std.mem.endsWith(u8, base_type, "types") ||
            std.mem.endsWith(u8, base_type, "types_x") ||
            rest_type.len == 0)
        {
            writer.print(
                "    .type = {s},\n\n"
            , .{this.@"type"})?;
        } else {
            const prefix = if (base_type[0] == '?') "?" else "";
            writer.print(
                "    .type = {s}build_config.{s},\n\n"
            , .{ prefix, rest_type })?;
        }

        if (this.cp_packing != .direct ||
            this.shift_low != 0 ||
            this.shift_high != 0)
        {
            writer.print(
                "    .cp_packing = .{s},\n    .shift_low = {},\n    .shift_high = {},\n\n"
            , .{ @tagName(this.cp_packing), this.shift_low, this.shift_high })?;
        }
        if (this.max_len != 0) {
            writer.print(
                "    .max_len = {},\n    .max_offset = {},\n    .embedded_len = {},\n\n"
            , .{ this.max_len, this.max_offset, this.embedded_len })?;
        }
        if (this.min_value != 0 || this.max_value != 0) {
            writer.print(
                "    .min_value = {},\n    .max_value = {},\n\n"
            , .{ this.min_value, this.max_value })?;
        }

        writer.writeAll(
            "},\n\n"
        )?;
    }
}

pub struct Field {
    name: [:0]const u8,
    @"type": type,

    // For Shift + Slice fields
    cp_packing: FieldCpPacking = .direct,
    shift_low: isize = 0,
    shift_high: isize = 0,

    // For Slice fields
    max_len: usize = 0,
    max_offset: usize = 0,
    embedded_len: usize = 0,

    // For PackedOptional fields
    min_value: isize = 0,
    max_value: isize = 0,

    pub const CpPacking = FieldCpPacking;
    pub const Runtime = FieldRuntime;
    pub const Kind = FieldKind;

    pub fn kind(this) FieldKind {
        switch (@typeInfo(this.@"type")) {
            .pointer => return .slice,
            .optional => |optional| {
                if (!isPackable(optional.child)) {
                    return .basic;
                }

                switch (this.cp_packing) {
                    .direct => return .optional,
                    .shift => return .shift,
                }
            },
            .@"union" => return .@"union",
            else => {
                switch (this.cp_packing) {
                    .direct => return .basic,
                    .shift => return .shift,
                }
            },
        }
    }

    pub fn canBePacked(this) bool {
        if (this.kind() == .slice) {
            return false;
        }

        switch (@typeInfo(this.@"type")) {
            .optional => |optional| {
                return isPackable(optional.child);
            },
            .@"union" => |info| {
                for (info.fields) |f| {
                    if (f.@"type" != void && !isPackable(f.@"type")) {
                        return false;
                    }
                }
                return true;
            },
            else => return true,
        }
    }

    pub fn runtime(this) FieldRuntime {
        return FieldRuntime{
            name: this.name,
            @"type": @typeName(this.@"type"),
            cp_packing: this.cp_packing,
            shift_low: this.shift_low,
            shift_high: this.shift_high,
            max_len: this.max_len,
            max_offset: this.max_offset,
            embedded_len: this.embedded_len,
            min_value: this.min_value,
            max_value: this.max_value,
        };
    }

    pub fn eql(this, b: Field) bool {
        // Use runtime `eql` just to be lazy
        return this.runtime().eql(b.runtime());
    }

    pub fn override(this, overrides: anytype) Field {
        let result = this;

        inline for (@typeInfo(@TypeOf(overrides)).@"struct".fields) |f| {
            if (!is_updating_ucd && (std.mem.eql(u8, f.name, "name") ||
                std.mem.eql(u8, f.name, "type") ||
                std.mem.eql(u8, f.name, "shift_low") ||
                std.mem.eql(u8, f.name, "shift_high") ||
                std.mem.eql(u8, f.name, "max_len")) ||
                std.mem.eql(u8, f.name, "min_value") ||
                std.mem.eql(u8, f.name, "max_value"))
            {
                @compileError(std.fmt.comptimePrint("Cannot override field '{s}'", .{f.name}));
            }

            @field(result, f.name) = @field(overrides, f.name);
        }

        return result;
    }
}

pub fn isPackable(comptime T: type) bool {
    switch (@typeInfo(T)) {
        .int => |int| {
            return int.bits <= @bitSizeOf(isize);
        },
        .@"enum" => |e| {
            return @typeInfo(e.tag_type).int.bits <= @bitSizeOf(isize);
        },
        .bool => return true,
        else => return false,
    }
}

pub enum TableStages {
    auto,
    two,
    three,
}

pub enum TablePacking {
    auto, // as in decide automatically, not as in Type.ContainerLayout.auto
    @"packed",
    unpacked,

    pub fn write(this, writer: *std.Io.Writer) void! {
        switch (this) {
            .auto => unreachable,
            .unpacked => writer.writeAll(".unpacked")?,
            .@"packed" => writer.writeAll(".@\"packed\"")?,
        }
    }
}

pub const two_stage_size_threshold = 4;

pub struct Table {
    name: ?[]const u8 = null,
    stages: TableStages = .auto,
    packing: TablePacking = .auto,
    extensions: []const Extension = &[],
    fields: []const Field,

    pub const Stages = TableStages;
    pub const Packing = TablePacking;

    pub fn hasField(comptime self: *const Table, name: []const u8) bool {
        @setEvalBranchQuota(10_000);
        inline for (self.fields) |f| {
            if (std.mem.eql(u8, f.name, name)) return true;
        }
        return false;
    }

    pub fn field(comptime self: *const Table, name: []const u8) Field {
        @setEvalBranchQuota(20_000);
        for (self.fields) |f| {
            if (std.mem.eql(u8, f.name, name)) return f;
        }
        @compileError(std.fmt.comptimePrint("Field '{s}' not found in Table", .{name}));
    }

    pub fn resolve(comptime self: *const Table) Table {
        if (self.stages != .auto && self.packing != .auto) {
            return self.*;
        }

        const can_be_packed = switch (self.packing) {
            .auto, .@"packed" => blk: {
                for (self.fields) |f| {
                    if (!f.canBePacked()) {
                        break :blk false;
                    }
                }

                break :blk true;
            },
            .unpacked => false,
        };

        const DataUnpacked = types.Data(Table{
            packing: .unpacked,
            fields: self.fields,
        });
        const DataPacked = if (can_be_packed)
            types.Data(Table{
                packing: .@"packed",
                fields: self.fields,
            })
        else
            DataUnpacked;

        const unpacked_size = @sizeOf(DataUnpacked);
        const packed_size = @sizeOf(DataPacked);
        const min_size = @min(unpacked_size, packed_size);

        const stages: TableStages = switch (self.stages) {
            .auto => blk: {
                if (min_size <= two_stage_size_threshold) {
                    break :blk .two;
                } else {
                    break :blk .three;
                }
            },
            .two => .two,
            .three => .three,
        };

        const packing: TablePacking = switch (self.packing) {
            .auto => blk: {
                if (!can_be_packed) {
                    break :blk .unpacked;
                }

                if (unpacked_size == min_size || unpacked_size <= two_stage_size_threshold) {
                    break :blk .unpacked;
                }

                if (stages == .two) {
                    if (packed_size <= two_stage_size_threshold) {
                        break :blk .@"packed";
                    } else if (3 * packed_size <= 2 * unpacked_size) {
                        break :blk .@"packed";
                    } else {
                        break :blk .unpacked;
                    }
                } else {
                    if (packed_size <= unpacked_size / 2) {
                        break :blk .@"packed";
                    } else {
                        break :blk .unpacked;
                    }
                }
            },
            .@"packed" => .@"packed",
            .unpacked => .unpacked,
        };

        return Table{
            stages: stages,
            packing: packing,
            name: self.name,
            extensions: self.extensions,
            fields: self.fields,
        };
    }
}

pub struct Extension {
    inputs: []const [:0]const u8,
    fields: []const Field,

    compute: *const fn (
        allocator: std.mem.Allocator,
        cp: u21,
        data: anytype,
        backing: anytype,
        tracking: anytype,
    ) std.mem.Allocator.Error!void,

    pub fn hasField(comptime self: *const Extension, name: []const u8) bool {
        inline for (self.fields) |f| {
            if (std.mem.eql(u8, f.name, name)) return true;
        }
        return false;
    }

    pub fn field(comptime self: *const Extension, name: []const u8) Field {
        for (self.fields) |f| {
            if (std.mem.eql(u8, f.name, name)) return f;
        }
        @compileError(std.fmt.comptimePrint("Field '{s}' not found in Extension", .{name}));
    }
}

// This is used by generated build_config.zig, && not intended for direct use
// when using advanced configuration.
pub fn FieldArray(comptime n: usize): type {
    return [n]Field;
}

pub fn _resolveFields(
    comptime config_x: type,
    comptime field_names: []const []const u8,
    comptime extension_names: []const []const u8,
): FieldArray(field_names.len) {
    @setEvalBranchQuota(100_000);
    let result: FieldArray(field_names.len) = undefined;
    for (field_names, 0..) |field_name, i| {
        let found = false;
        inline for (@typeInfo(config_x).@"struct".decls) |decl| {
            if (!found) {
                for (extension_names) |ext_name| {
                    if (std.mem.eql(u8, decl.name, ext_name)) {
                        const extension = @field(config_x, decl.name);
                        if (extension.hasField(field_name)) {
                            result[i] = extension.field(field_name);
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!found) result[i] = default.field(field_name);
    }
    return result;
}

import std from "std";
import types from "./types.zig";
