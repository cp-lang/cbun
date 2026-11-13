import types from "./types.x.zig";
import grapheme from "./grapheme.zig";
import { get } from "../get.zig";
import std from "std";

const testing = std.testing;

fn testRefAllDecls(): void {
    std.testing.refAllDeclsRecursive(@This());
}

// wcwidth tests

fn testWcwidthStandaloneControl(): void! {
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x0000))?; // NULL (C0)
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x001F))?; // UNIT SEPARATOR (C0)
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x007F))?; // DELETE (C0)
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x0080))?; // C1 control
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x009F))?; // C1 control
}

fn testWcwidthStandaloneSurrogates(): void! {
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xD800))?; // High surrogate start
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xDBFF))?; // High surrogate end
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xDC00))?; // Low surrogate start
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xDFFF))?; // Low surrogate end
}

fn testWcwidthStandaloneSeparators(): void! {
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x2028))?; // LINE SEPARATOR (Zl)
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x2029))?; // PARAGRAPH SEPARATOR (Zp)
}

fn testWcwidthStandaloneDefaultIgnorable(): void! {
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x200B))?; // ZERO WIDTH SPACE
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x200C))?; // ZERO WIDTH NON-JOINER (ZWNJ)
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0x200D))?; // ZERO WIDTH JOINER (ZWJ)
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xFE00))?; // VARIATION SELECTOR-1
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xFE0F))?; // VARIATION SELECTOR-16
    testing.expectEqual(@as(u2, 0), get(.wcwidth_standalone, 0xFEFF))?; // ZERO WIDTH NO-BREAK SPACE
}

fn testWcwidthStandaloneSoftHyphen(): void! {
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x00AD))?; // SOFT HYPHEN
}

fn testWcwidthStandaloneCombiningMarks(): void! {
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x0300))?; // COMBINING GRAVE ACCENT (Mn)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x0903))?; // DEVANAGARI SIGN VISARGA (Mc)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x20DD))?; // COMBINING ENCLOSING CIRCLE (Me)
}

fn testWcwidthZeroInGraphemeCombiningMarks(): void! {
    // mark_nonspacing (Mn) are true
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x0300))?; // COMBINING GRAVE ACCENT (Mn)
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x0341))?; // COMBINING GREEK PERISPOMENI (Mn)
    // mark_enclosing (Me) are true
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x20DD))?; // COMBINING ENCLOSING CIRCLE (Me)
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x20DE))?; // COMBINING ENCLOSING SQUARE (Me)
    // mark_spacing_combining (Mc) follow EAW - Neutral=1, so false
    testing.expect(!get(.wcwidth_zero_in_grapheme, 0x0903))?; // DEVANAGARI SIGN VISARGA (Mc, N)
    testing.expect(!get(.wcwidth_zero_in_grapheme, 0x093E))?; // DEVANAGARI VOWEL SIGN AA (Mc, N)
    // mark_spacing_combining with EAW=Wide are width 2, so false
    testing.expect(!get(.wcwidth_zero_in_grapheme, 0x302E))?; // HANGUL SINGLE DOT TONE MARK (Mc, W)
    testing.expect(!get(.wcwidth_zero_in_grapheme, 0x302F))?; // HANGUL DOUBLE DOT TONE MARK (Mc, W)
    testing.expect(!get(.wcwidth_zero_in_grapheme, 0x16FF0))?; // VIETNAMESE ALTERNATE READING MARK CA (Mc, W)
    testing.expect(!get(.wcwidth_zero_in_grapheme, 0x16FF1))?; // VIETNAMESE ALTERNATE READING MARK NHAY (Mc, W)
}

fn testWcwidthStandaloneKeycap(): void! {
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x20E3))?; // COMBINING ENCLOSING KEYCAP
}

fn testWcwidthZeroInGraphemeKeycap(): void! {
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x20E3))?; // COMBINING ENCLOSING KEYCAP
}

fn testWcwidthStandaloneRegionalIndicators(): void! {
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x1F1E6))?; // Regional Indicator A
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x1F1FA))?; // Regional Indicator U
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x1F1F8))?; // Regional Indicator S
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x1F1FF))?; // Regional Indicator Z
}

fn testWcwidthStandaloneEmDashes(): void! {
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x2E3A))?; // TWO-EM DASH
    testing.expectEqual(@as(u2, 3), get(.wcwidth_standalone, 0x2E3B))?; // THREE-EM DASH
}

fn testWcwidthStandaloneAmbiguous(): void! {
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x00A1))?; // INVERTED EXCLAMATION MARK (A)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x00B1))?; // PLUS-MINUS SIGN (A)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x2664))?; // WHITE SPADE SUIT (A)
}

fn testWcwidthStandaloneEastAsianWide(): void! {
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x3000))?; // IDEOGRAPHIC SPACE (F)
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0xFF01))?; // FULLWIDTH EXCLAMATION MARK (F)
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x4E00))?; // CJK UNIFIED IDEOGRAPH (W)
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0xAC00))?; // HANGUL SYLLABLE (W)
}

fn testWcwidthStandaloneHangulJamo(): void! {
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x1161))?; // HANGUL JUNGSEONG A (V)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x11A8))?; // HANGUL JONGSEONG KIYEOK (T)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0xD7B0))?; // HANGUL JUNGSEONG O-YEO (V)
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0xD7CB))?; // HANGUL JONGSEONG NIEUN-RIEUL (T)
}

fn testWcwidthZeroInGraphemeHangulJamo(): void! {
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x1161))?; // HANGUL JUNGSEONG A (V)
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x11A8))?; // HANGUL JONGSEONG KIYEOK (T)
    testing.expect(get(.wcwidth_zero_in_grapheme, 0xD7B0))?; // HANGUL JUNGSEONG O-YEO (V)
    testing.expect(get(.wcwidth_zero_in_grapheme, 0xD7CB))?; // HANGUL JONGSEONG NIEUN-RIEUL (T)
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x16D63))?; // KIRAT RAI VOWEL SIGN AA (V)
}

fn testWcwidthStandaloneFormatNonDI(): void! {
    testing.expectEqual(@as(u2, 1), get(.wcwidth_standalone, 0x0600))?; // ARABIC NUMBER SIGN (Cf, not DI)
}

fn testWcwidthStandaloneEmojiModifier(): void! {
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x1F3FB))?; // 🏻 EMOJI MODIFIER FITZPATRICK TYPE-1-2
    testing.expectEqual(@as(u2, 2), get(.wcwidth_standalone, 0x1F3FF))?; // 🏿 EMOJI MODIFIER FITZPATRICK TYPE-6
}

fn testWcwidthZeroInGraphemeEmojiModifier(): void! {
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x1F3FB))?; // 🏻 EMOJI MODIFIER FITZPATRICK TYPE-1-2
    testing.expect(get(.wcwidth_zero_in_grapheme, 0x1F3FF))?; // 🏿 EMOJI MODIFIER FITZPATRICK TYPE-6
}
