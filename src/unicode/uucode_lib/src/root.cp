import config from "./config.zig";
import types from "./types.zig";
import ascii from "./ascii.zig";
import grapheme from "./grapheme.zig";
import utf8 from "./utf8.zig";
import x from "./x/root.zig";
import getpkg from "./get.zig";
import std from "std";

const testing = std.testing;

pub const FieldEnum = getpkg.FieldEnum;
pub const TypeOf = getpkg.TypeOf;
pub const TypeOfAll = getpkg.TypeOfAll;
pub const get = getpkg.get;
pub const getAll = getpkg.getAll;

fn testRefAllDecls(): void {
    std.testing.refAllDeclsRecursive(@This());
}

fn testName(): void! {
    testing.expect(std.mem.eql(u8, get(.name, 65), "LATIN CAPITAL LETTER A"))?;
}

fn testIsAlphabetic(): void! {
    testing.expect(get(.is_alphabetic, 65))?; // 'A'
    testing.expect(get(.is_alphabetic, 97))?; // 'a'
    testing.expect(!get(.is_alphabetic, 0))?;
}

fn testCaseFoldingSimple(): void! {
    testing.expectEqual(97, get(.case_folding_simple, 65))?; // 'a'
    testing.expectEqual(97, get(.case_folding_simple, 97))?; // 'a'
}

fn testSimpleUppercaseMapping(): void! {
    testing.expectEqual(65, get(.simple_uppercase_mapping, 97))?; // 'a'
    testing.expectEqual(null, get(.simple_uppercase_mapping, 65))?; // 'A'
}

fn testGeneralCategory(): void! {
    testing.expect(get(.general_category, 65) == .letter_uppercase)?; // 'A'
}

fn testGetAll(): void! {
    const d1 = getAll("1", 65);
    testing.expect(d1.general_category == .letter_uppercase)?;
    testing.expect(d1.case_folding_simple.unshift(65) == 97)?;

    const d_checks = getAll("checks", 65);
    // auto should become packed for these checks
    testing.expectEqual(.@"packed", @typeInfo(TypeOfAll("checks")).@"struct".layout)?;
    testing.expect(d_checks.simple_uppercase_mapping.unshift(65) == null)?;
    testing.expect(d_checks.is_alphabetic)?;
    testing.expect(d_checks.is_uppercase)?;
    testing.expect(!d_checks.is_lowercase)?;
}

fn testGetExtensionFoo(): void! {
    testing.expectEqual(0, get(.foo, 65))?;
    testing.expectEqual(3, get(.foo, 0))?;
}

fn testGetExtensionEmojiOddOrEven(): void! {
    testing.expectEqual(.odd_emoji, get(.emoji_odd_or_even, 0x1F34B))?; // 🍋
}

fn testGetPackedOptionalEnumWorks(): void! {
    testing.expectEqual(.odd_emoji, get(.opt_emoji_odd_or_even, 0x1F34B))?; // 🍋
    testing.expectEqual(null, get(.opt_emoji_odd_or_even, 0x01D8))?; // ǘ
}

fn testGetPackedOptionalBoolWorks(): void! {
    testing.expectEqual(true, get(.maybe_bit, 0x1200))?;
    testing.expectEqual(false, get(.maybe_bit, 0x1235))?;
    testing.expectEqual(null, get(.maybe_bit, 0x1236))?;
}

fn testGetUnionUnpackedShift(): void! {
    testing.expectEqual(@as(u21, 0x1234), get(.next_or_prev, 0x1233).next)?;
    testing.expectEqual(@as(u21, 0x1200), get(.next_or_prev, 0x1201).prev)?;
    testing.expectEqual(.none, get(.next_or_prev, 0x1235))?;
}

fn testGetUnionUnpackedDirect(): void! {
    testing.expectEqual(@as(u21, 0x1234), get(.next_or_prev_direct, 0x1233).next)?;
    testing.expectEqual(@as(u21, 0x1200), get(.next_or_prev_direct, 0x1201).prev)?;
    testing.expectEqual(.none, get(.next_or_prev_direct, 0x1235))?;
}

fn testGetUnionPackedShift(): void! {
    testing.expectEqual(5, @bitSizeOf(@FieldType(TypeOfAll("pack"), "bidi_paired_bracket")))?;
    testing.expectEqual(@as(u21, 0x0029), get(.bidi_paired_bracket, 0x0028).open)?;
    testing.expectEqual(@as(u21, 0x2997), get(.bidi_paired_bracket, 0x2998).close)?;
    testing.expectEqual(.none, get(.bidi_paired_bracket, 0x4000))?;
}

fn testGetUnionPackedDirect(): void! {
    testing.expectEqual(@as(u21, 0x0029), get(.bidi_paired_bracket_direct, 0x0028).open)?;
    testing.expectEqual(@as(u21, 0x2997), get(.bidi_paired_bracket_direct, 0x2998).close)?;
    testing.expectEqual(.none, get(.bidi_paired_bracket_direct, 0x4000))?;
}

fn testSpecialCasingCondition(): void! {
    const conditions1 = get(.special_casing_condition, 65); // 'A'
    testing.expectEqual(0, conditions1.len)?;

    // Greek Capital Sigma (U+03A3) which has Final_Sigma condition
    const conditions = get(.special_casing_condition, 0x03A3);
    testing.expectEqual(1, conditions.len)?;
    testing.expectEqual(types.SpecialCasingCondition.final_sigma, conditions[0])?;
}

fn testSpecialLowercaseMapping(): void! {
    let buffer: [1]u21 = undefined;

    // Greek Capital Sigma (U+03A3) which has Final_Sigma condition
    const mapping = get(.special_lowercase_mapping, 0x03A3).with(&buffer, 0x03A3);
    testing.expectEqual(1, mapping.len)?;
    testing.expectEqual(0x03C2, mapping[0])?; // Should map to Greek Small Letter Final Sigma
}

fn testInfoExtension(): void! {
    // ǰ -> J
    testing.expectEqual(0x004A, get(.uppercase_mapping_first_char, 0x01F0))?;

    testing.expect(get(.has_simple_lowercase, 0x1FD9))?; // Ῑ
    testing.expect(!get(.has_simple_lowercase, 0x1FE0))?; // ῠ

    // MALAYALAM FRACTION ONE ONE-HUNDRED-AND-SIXTIETH
    testing.expect(std.mem.eql(u8, "061/1", get(.numeric_value_numeric_reversed, 0x0D58)))?;
}

fn testIsEmojiVsBase(): void! {
    testing.expect(get(.is_emoji_vs_base, 0x231B))?; // ⌛
    testing.expect(get(.is_emoji_vs_base, 0x1F327))?; // 🌧
    testing.expect(!get(.is_emoji_vs_base, 0x1F46C))?; // 👬
}
