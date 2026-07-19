/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/graphics/bidi.h"

#include "clar.h"

#include <string.h>

///////////////////////////////////////////////////////////
// Stubs

#include "stubs_logging.h"
#include "stubs_passert.h"

///////////////////////////////////////////////////////////
// Helpers

static BidiScratch s_ws;

// Reorder logical codepoints to visual order at the given base level.
static size_t prv_visual(const Codepoint *in, size_t n, int base, Codepoint *out) {
  return bidi_reorder_line(in, n, base, out, &s_ws);
}

static void prv_assert_eq(const Codepoint *got, const Codepoint *want, size_t n) {
  for (size_t i = 0; i < n; i++) {
    cl_assert_equal_i(got[i], want[i]);
  }
}

static int prv_base_utf8(const char *s) {
  return bidi_base_level_utf8((const utf8_t *)s, (const utf8_t *)s + strlen(s));
}

void test_bidi__initialize(void) {}
void test_bidi__cleanup(void) {}

///////////////////////////////////////////////////////////
// Tests

// Base level comes from the first strong character.
void test_bidi__base_level(void) {
  const Codepoint rtl[] = {0x0627, 'a'};   // Alef first -> RTL
  const Codepoint ltr[] = {'a', 0x0627};   // 'a' first -> LTR
  const Codepoint num[] = {'1', 0x0627};   // number is weak; Alef is first strong
  cl_assert_equal_i(bidi_base_level(rtl, 2), 1);
  cl_assert_equal_i(bidi_base_level(ltr, 2), 0);
  cl_assert_equal_i(bidi_base_level(num, 2), 1);
}

// UTF-8 base level: weak digits are skipped to the first strong character, and
// Arabic-Indic digits with no strong character at all still read RTL.
void test_bidi__base_level_utf8(void) {
  cl_assert_equal_i(prv_base_utf8("123 \xD9\x85\xD8\xB1"), 1);       // 123 Meem Reh -> RTL
  cl_assert_equal_i(prv_base_utf8("abc \xD9\x85"), 0);               // Latin first -> LTR
  cl_assert_equal_i(prv_base_utf8("123"), 0);                        // Western digits -> LTR
  cl_assert_equal_i(prv_base_utf8("\xD9\xA2\xD9\xA0\xD9\xA2\xD9\xA6"), 1);  // ٢٠٢٦ -> RTL
}

// Strong RTL detection spans the engine's full class table: Hebrew, Arabic,
// and Arabic presentation forms (pre-shaped text); Latin and digits do not
// count. This gates the bidi layout path.
void test_bidi__contains_rtl(void) {
  const char heb[] = "hi \xD7\x90";           // Hebrew Alef after Latin
  const char pres[] = "\xEF\xBA\x8D";         // U+FE8D, Alef final form
  const char latin[] = "hello 123 (test)";
  cl_assert(bidi_contains_rtl((const utf8_t *)heb, (const utf8_t *)heb + strlen(heb)));
  cl_assert(bidi_contains_rtl((const utf8_t *)pres, (const utf8_t *)pres + strlen(pres)));
  cl_assert(!bidi_contains_rtl((const utf8_t *)latin, (const utf8_t *)latin + strlen(latin)));
  cl_assert(!bidi_contains_rtl((const utf8_t *)latin, (const utf8_t *)latin));
}

// Hebrew is a strong RTL run like Arabic: it reverses to visual order.
void test_bidi__hebrew_strong_run(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x05D1, 0x05D2};  // Alef Bet Gimel
  const Codepoint want[] = {0x05D2, 0x05D1, 0x05D0};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
  cl_assert_equal_i(bidi_base_level(in, 3), 1);
}

// Pure Arabic reverses to visual order; pure Latin is untouched.
void test_bidi__strong_runs(void) {
  Codepoint out[8];
  const Codepoint ar[] = {0x0627, 0x0628, 0x062C};  // Alef Beh Jeem
  const Codepoint ar_want[] = {0x062C, 0x0628, 0x0627};
  prv_visual(ar, 3, 1, out);
  prv_assert_eq(out, ar_want, 3);

  const Codepoint la[] = {'a', 'b', 'c'};
  const Codepoint la_want[] = {'a', 'b', 'c'};
  prv_visual(la, 3, 0, out);
  prv_assert_eq(out, la_want, 3);
}

// Western digits stay left-to-right inside an RTL run.
void test_bidi__western_digits_ltr(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, '1', '2', '3'};  // Alef 1 2 3
  const Codepoint want[] = {'1', '2', '3', 0x0627};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// Arabic-Indic digits stay left-to-right too: they are in the Arabic block,
// so the engine must type them AN, not strong R.
void test_bidi__arabic_indic_digits_ltr(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x0662, 0x0660};  // Alef ٢ ٠
  const Codepoint want[] = {0x0662, 0x0660, 0x0627};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// A time keeps its colon-separated groups in order inside RTL: ١٢:٣٤.
void test_bidi__time_digits_in_order(void) {
  Codepoint out[12];
  // Alef SP ١ ٢ : ٣ ٤
  const Codepoint in[] = {0x0627, 0x20, 0x0661, 0x0662, ':', 0x0663, 0x0664};
  size_t n = prv_visual(in, 7, 1, out);
  cl_assert_equal_i(n, 7);
  // Collect the digit/colon subsequence in visual order; it must read ١٢:٣٤.
  const Codepoint want_num[] = {0x0661, 0x0662, ':', 0x0663, 0x0664};
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (out[i] == 0x0627 || out[i] == 0x20) continue;
    cl_assert(k < 5);
    cl_assert_equal_i(out[i], want_num[k++]);
  }
  cl_assert_equal_i(k, 5);
}

// A date keeps its slash-separated groups in order (W4 joins a single CS
// between two Arabic numbers): ٢٠٢٦/٠٦/٢٢, not ٢٢/٠٦/٢٠٢٦.
void test_bidi__date_groups_in_order(void) {
  Codepoint out[12];
  const Codepoint in[] = {0x0662, 0x0660, 0x0662, 0x0666, '/',
                          0x0660, 0x0666, '/', 0x0662, 0x0662};  // ٢٠٢٦/٠٦/٢٢
  size_t n = prv_visual(in, 10, 1, out);
  cl_assert_equal_i(n, 10);
  prv_assert_eq(out, in, 10);
}

// A separator only joins a number with a digit on both sides. Here "/" has a
// letter on one side, so it reorders as a neutral: ا/٢ -> visual ٢ / ا.
void test_bidi__separator_needs_two_digits(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, '/', 0x0662};  // Alef / ٢
  const Codepoint want[] = {0x0662, '/', 0x0627};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// Extended Arabic-Indic (Persian) digits are EN per the UCD; after an Arabic
// letter W2 turns them AN, so they stay in order like Arabic-Indic digits.
void test_bidi__persian_digits(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x06F1, 0x06F2};  // Alef ۱ ۲
  const Codepoint want[] = {0x06F1, 0x06F2, 0x0627};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
  // Digit-only Persian text still falls back to RTL for alignment.
  cl_assert_equal_i(prv_base_utf8("\xDB\xB1\xDB\xB2"), 1);  // ۱۲
}

// The Arabic comma is CS, not strong AL: in an LTR paragraph it must not drag
// the following digits into an RTL run. "abc، 123" stays in logical order.
void test_bidi__arabic_comma_is_weak(void) {
  Codepoint out[10];
  const Codepoint in[] = {'a', 'b', 'c', 0x060C, 0x20, '1', '2', '3'};
  prv_visual(in, 8, 0, out);
  prv_assert_eq(out, in, 8);
}

// Directional marks are zero-width but strong: they set the base direction.
void test_bidi__directional_marks(void) {
  const Codepoint rlm[] = {0x200F, '1'};
  const Codepoint lrm[] = {0x200E, 0x0627};
  cl_assert_equal_i(bidi_base_level(rlm, 2), 1);
  cl_assert_equal_i(bidi_base_level(lrm, 2), 0);
}

// NARROW NO-BREAK SPACE is CS: it joins two number groups (W4), so a Hebrew
// line keeps "12 34" ordered instead of swapping the groups.
void test_bidi__nnbsp_joins_numbers(void) {
  Codepoint out[10];
  const Codepoint in[] = {0x05D0, 0x20, '1', '2', 0x202F, '3', '4'};
  const Codepoint want[] = {'1', '2', 0x202F, '3', '4', 0x20, 0x05D0};
  prv_visual(in, 7, 1, out);
  prv_assert_eq(out, want, 7);
}

// Boundary neutrals are transparent to the weak rules (UAX 9 5.2): a ZWNJ
// inside "1.11" must not stop W4 from joining the number, and must not split
// the number's level run.
void test_bidi__zwnj_transparent_in_number(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, '1', '.', 0x200C, '1', '1'};
  const Codepoint want[] = {'1', '.', 0x200C, '1', '1', 0x05D0};
  prv_visual(in, 6, 1, out);
  prv_assert_eq(out, want, 6);
}

// A ZWNJ between two spaces must not split the neutral run: the spaces around
// it take the direction of the surrounding strong text, so the whole RTL
// island reverses as one block even at an LTR base.
void test_bidi__zwnj_between_neutrals(void) {
  Codepoint out[8];
  const Codepoint in[] = {'a', 0x05D0, 0x20, 0x200C, 0x20, 0x05D1, 'b'};
  const Codepoint want[] = {'a', 0x05D1, 0x20, 0x200C, 0x20, 0x05D0, 'b'};
  prv_visual(in, 7, 0, out);
  prv_assert_eq(out, want, 7);
}

// N0 propagates a resolved bracket type to immediately following NSMs: the
// fathatan after ")" takes the brackets' L, so the whole "(b)ً" island stays
// together (Unicode BidiCharacterTest bracket+NSM cases).
void test_bidi__n0_nsm_propagation(void) {
  Codepoint out[10];
  const Codepoint in[] = {0x0627, 0x20, 'a', '(', 'b', ')', 0x064B};
  const Codepoint want[] = {'a', '(', 'b', ')', 0x064B, 0x20, 0x0627};
  prv_visual(in, 7, 1, out);
  prv_assert_eq(out, want, 7);
}

// A wrapped line resolves its boundary neutrals with the strong context of
// the adjacent paragraph text (sos/eos hints), not the base direction: in a
// base-L paragraph "a XA, XB" wrapped after the comma, the comma joins the
// Hebrew run exactly as it does in the unwrapped paragraph.
void test_bidi__softwrap_boundary_context(void) {
  static BidiScratch ws;
  Codepoint out[8];
  const Codepoint slice[] = {'a', 0x20, 0x05D0, ','};
  const Codepoint want[] = {'a', 0x20, ',', 0x05D0};
  size_t n = bidi_reorder_line_ctx(slice, 4, 0, BIDI_BOUNDARY_AUTO, BIDI_BOUNDARY_AUTO,
                                   BIDI_BOUNDARY_R, out, &ws);
  cl_assert_equal_i(n, 4);
  prv_assert_eq(out, want, 4);
}

// The micro sign is a Latin-1 letter (L), not a neutral: it joins the Latin
// island instead of travelling with the Hebrew run.
void test_bidi__micro_sign_is_letter(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x20, 0x00B5, 0x20, 'a', 'b', 'c'};
  const Codepoint want[] = {0x00B5, 0x20, 'a', 'b', 'c', 0x20, 0x05D0};
  prv_visual(in, 7, 1, out);
  prv_assert_eq(out, want, 7);
}

// CJK double angle brackets pair and keep an enclosed Latin island together
// in an RTL paragraph (RLM base): "a 《b.1》" keeps its logical order.
void test_bidi__cjk_double_angle_pairs(void) {
  Codepoint out[10];
  const Codepoint in[] = {0x200F, 'a', 0x20, 0x300A, 'b', '.', '1', 0x300B};
  const Codepoint want[] = {'a', 0x20, 0x300A, 'b', '.', '1', 0x300B, 0x200F};
  prv_visual(in, 8, 1, out);
  prv_assert_eq(out, want, 8);
}

// Fullwidth digits are EN (shipped in the CJK notification fonts): after an
// RLM they keep their order instead of reversing as strong text.
void test_bidi__fullwidth_digits_en(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x200F, 0xFF11, 0xFF12};   // RLM １ ２
  const Codepoint want[] = {0xFF11, 0xFF12, 0x200F};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// Corner brackets pair and mirror like the other CJK brackets.
void test_bidi__corner_brackets_pair(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x300C, 0x0628, 0x300D};   // Alef 「 Beh 」
  const Codepoint want[] = {0x300C, 0x0628, 0x300D, 0x0627};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// Mirror-only codepoints outside the bracket pairs: <= flips to >= on an odd
// level, and the multiplication sign stays a neutral (not a Latin letter).
void test_bidi__le_mirrors_and_times_neutral(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x2264, 0x0628};   // Alef <= Beh
  const Codepoint want[] = {0x0628, 0x2265, 0x0627};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);

  // x is ON between two ANs: per UAX the neutral takes the run direction and
  // the whole expression reverses (٣x٤ displays ٤x٣), unlike a letter would.
  const Codepoint mul[] = {0x0663, 0x00D7, 0x0664};
  const Codepoint mul_want[] = {0x0664, 0x00D7, 0x0663};
  prv_visual(mul, 3, 1, out);
  prv_assert_eq(out, mul_want, 3);
}

// The eos scanner weighs numbers as N-rule context: a digit right after the
// wrap makes the slice-final comma join the Hebrew run, as in the paragraph.
void test_bidi__softwrap_number_context(void) {
  static BidiScratch ws;
  Codepoint out[8];
  const Codepoint slice[] = {'a', 0x20, 0x05D0, ','};
  const Codepoint want[] = {'a', 0x20, ',', 0x05D0};
  const char after[] = " 1";
  int eos = bidi_first_strong_utf8((const utf8_t *)after, (const utf8_t *)after + 2,
                                   BIDI_BOUNDARY_R);
  cl_assert_equal_i(eos, BIDI_BOUNDARY_R);
  size_t n = bidi_reorder_line_ctx(slice, 4, 0, BIDI_BOUNDARY_AUTO, BIDI_BOUNDARY_AUTO,
                                   eos, out, &ws);
  cl_assert_equal_i(n, 4);
  prv_assert_eq(out, want, 4);
}

// A continuation line uses its OWN last strong type as the context for a
// digit after the wrap - not a staler strong from earlier in the paragraph.
// Para "a XA, 1" wrapped as [a]/[XA ,]/[1]: line 2's comma joins the Hebrew
// run because the digit after the wrap resolves R against the line's Hebrew.
void test_bidi__continuation_line_context(void) {
  static BidiScratch ws;
  Codepoint out[4];
  const Codepoint slice[] = {0x05D0, ','};
  const Codepoint want[] = {',', 0x05D0};
  size_t n = bidi_reorder_line_ctx(slice, 2, 0, BIDI_BOUNDARY_L, BIDI_BOUNDARY_R,
                                   BIDI_BOUNDARY_R, out, &ws);
  cl_assert_equal_i(n, 2);
  prv_assert_eq(out, want, 2);
}

// The weak-rule and neutral-rule boundary contexts can differ: after
// "a .. AN |", the wrap's W7 context is the strong L but the adjacent AN
// supplies R to the neutral run, so "[, XA]" keeps the comma with the Hebrew.
void test_bidi__split_sos_contexts(void) {
  static BidiScratch ws;
  Codepoint out[4];
  const Codepoint slice[] = {',', 0x05D0};
  const Codepoint want[] = {0x05D0, ','};
  size_t n = bidi_reorder_line_ctx(slice, 2, 0, BIDI_BOUNDARY_L, BIDI_BOUNDARY_R,
                                   BIDI_BOUNDARY_AUTO, out, &ws);
  cl_assert_equal_i(n, 2);
  prv_assert_eq(out, want, 2);
}

// Fullwidth plus/comma join fullwidth digits (ES/CS like their ASCII forms):
// a CJK expression after Hebrew keeps its order.
void test_bidi__fullwidth_expression(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x20, 0xFF11, 0xFF0B, 0xFF12};  // XA SP １＋２
  const Codepoint want[] = {0xFF11, 0xFF0B, 0xFF12, 0x20, 0x05D0};
  prv_visual(in, 5, 1, out);
  prv_assert_eq(out, want, 5);
}

// The neutral-boundary scan is resolver-backed: a leading EN in a base-L
// paragraph resolves L (W7), so the wrapped continuation keeps its order.
void test_bidi__boundary_en_resolves_by_base(void) {
  static BidiScratch ws;
  const char prefix[] = "1 ";
  int ndir = bidi_boundary_ndir_utf8((const utf8_t *)prefix, (const utf8_t *)prefix + 2, 0, &ws);
  cl_assert_equal_i(ndir, BIDI_BOUNDARY_L);
  Codepoint out[6];
  const Codepoint slice[] = {'!', 0x0661, 'a', 0x05D0};
  size_t n = bidi_reorder_line_ctx(slice, 4, 0, BIDI_BOUNDARY_AUTO, ndir, BIDI_BOUNDARY_AUTO,
                                   out, &ws);
  cl_assert_equal_i(n, 4);
  prv_assert_eq(out, slice, 4);
}

// A raw scan cannot see N0: "a(XA)" resolves its brackets L, so the boundary
// after the closing bracket is L even though a strong R sits inside the pair.
// The wrapped "!XB" line then keeps the bang on the left.
void test_bidi__boundary_sees_bracket_resolution(void) {
  static BidiScratch ws;
  const char prefix[] = "a(\xD7\x90)";  // a ( Hebrew-Alef )
  int ndir = bidi_boundary_ndir_utf8((const utf8_t *)prefix, (const utf8_t *)prefix + 5, 0, &ws);
  cl_assert_equal_i(ndir, BIDI_BOUNDARY_L);
  Codepoint out[4];
  const Codepoint slice[] = {'!', 0x05D1};
  size_t n = bidi_reorder_line_ctx(slice, 2, 0, BIDI_BOUNDARY_R, ndir, BIDI_BOUNDARY_AUTO,
                                   out, &ws);
  cl_assert_equal_i(n, 2);
  prv_assert_eq(out, slice, 2);
}

// Paragraph-wide resolution, per-line application (UAX 9 Basic Display
// Algorithm): the four soft-wrap cases that no scalar boundary hint could
// represent all resolve as in the unwrapped paragraph.

// Wrap space + punctuation are one neutral run between two strong Rs: the
// second line renders reversed even though its own slice starts neutral.
void test_bidi__para_wrap_neutral_run(void) {
  static BidiScratch ws;
  Codepoint out[8];
  const Codepoint para[] = {'a', 0x05D0, 0x20, '!', 0x05D1};
  bidi_resolve_paragraph(para, 5, 0, &ws);
  const Codepoint want2[] = {0x05D1, '!'};
  size_t n = bidi_apply_line(&ws.cps[3], &ws.level[3], 2, 0, out, &ws);
  cl_assert_equal_i(n, 2);
  prv_assert_eq(out, want2, 2);
}

// A long digit run keeps the strong context from before the resolver window
// because the whole paragraph resolves at once (98 codepoints here).
void test_bidi__para_long_digit_run(void) {
  static BidiScratch ws;
  static Codepoint para[98];
  static Codepoint out[96];
  para[0] = 'a';
  para[1] = 0x05D0;
  for (int i = 0; i < 96; i++) para[2 + i] = '1';
  bidi_resolve_paragraph(para, 98, 0, &ws);
  size_t n = bidi_apply_line(&ws.cps[2], &ws.level[2], 96, 0, out, &ws);
  cl_assert_equal_i(n, 96);
  for (int i = 0; i < 96; i++) cl_assert_equal_i(out[i], '1');
}

// The bracket pair after the wrap resolves to L (it encloses an L with L
// before it), so the first line's trailing "!" stays put - a raw scan of the
// suffix would have seen the enclosed R instead.
void test_bidi__para_bracket_after_wrap(void) {
  static BidiScratch ws;
  Codepoint out[8];
  const Codepoint para[] = {'a', 0x05D0, '!', 0x20, '(', 0x05D1, 'c', ')'};
  bidi_resolve_paragraph(para, 8, 0, &ws);
  const Codepoint want1[] = {'a', 0x05D0, '!'};
  size_t n = bidi_apply_line(&ws.cps[0], &ws.level[0], 3, 0, out, &ws);
  cl_assert_equal_i(n, 3);
  prv_assert_eq(out, want1, 3);
}

// W4 joins a comma between two digits even when the wrap splits the number:
// both halves keep logical order because the levels were resolved before the
// split.
void test_bidi__para_w4_across_wrap(void) {
  static BidiScratch ws;
  Codepoint out[8];
  const Codepoint para[] = {'a', '1', ',', '2', 0x05D0};
  bidi_resolve_paragraph(para, 5, 0, &ws);
  const Codepoint want1[] = {'a', '1', ','};
  size_t n = bidi_apply_line(&ws.cps[0], &ws.level[0], 3, 0, out, &ws);
  cl_assert_equal_i(n, 3);
  prv_assert_eq(out, want1, 3);
  const Codepoint want2[] = {'2', 0x05D0};
  n = bidi_apply_line(&ws.cps[3], &ws.level[3], 2, 0, out, &ws);
  cl_assert_equal_i(n, 2);
  prv_assert_eq(out, want2, 2);
}

// Shipped number signs are ET: W5 absorbs the per-mille sign into the
// following number, so the sign stays with its digits in an RTL line.
void test_bidi__per_mille_stays_with_number(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x20, 0x2030, '1'};
  const Codepoint want[] = {0x2030, '1', 0x20, 0x05D0};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// Non-bracket mirrored pairs shipped in the CJK fonts: a fullwidth < after
// Hebrew mirrors to fullwidth > on its odd level.
void test_bidi__fullwidth_angle_mirrors(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x20, 0xFF1C, 0x05D1};
  const Codepoint want[] = {0x05D1, 0xFF1E, 0x20, 0x05D0};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// Parenthesized ideograph markers are L: the CJK list marker stays with its
// following LTR run instead of moving to the far side.
void test_bidi__parenthesized_ideograph_l(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x20, 0x3220, 'a'};
  const Codepoint want[] = {0x3220, 'a', 0x20, 0x05D0};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// The keycap combining mark is NSM: it stays attached to its digit inside an
// RTL line instead of splitting the group as a neutral.
void test_bidi__keycap_mark_attaches(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, '1', 0x20E3};
  const Codepoint want[] = {'1', 0x20E3, 0x0627};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// Cyrillic combining marks are NSM, not letters: after a Hebrew letter the
// titlo inherits R (W1) and travels with it instead of joining the Latin run.
// L3 re-reverses the marks+base cluster, so the base leads and the mark
// follows - matching the reference order, and placing a zero-advance mark
// with negative bearing over its base at draw time.
void test_bidi__cyrillic_combining_nsm(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x0483, 'a'};
  const Codepoint want[] = {'a', 0x05D0, 0x0483};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// L3 looks through a retained ZWNJ between mark and base: the fatha re-joins
// its Beh across the boundary neutral instead of attaching to the next glyph.
void test_bidi__l3_through_boundary_neutral(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0628, 0x200C, 0x064E, 0x062C};  // Beh ZWNJ fatha Jeem
  const Codepoint want[] = {0x062C, 0x0628, 0x200C, 0x064E};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// Thaana vowel marks are NSM: between two Thaana letters the mark travels
// with its preceding base through the reorder (letter, mark, letter -> the
// mark stays beside the first letter).
void test_bidi__thaana_vowel_nsm(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0780, 0x07A6, 0x0781};
  const Codepoint want[] = {0x0781, 0x0780, 0x07A6};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// L3 with no base at the visual tail: a leading mark run (with or without a
// retained ZWNJ) keeps its own logical order instead of staying reversed.
void test_bidi__l3_leading_marks_no_base(void) {
  Codepoint out[8];
  const Codepoint bare[] = {0x064E, 0x0651, 0x0628};  // fatha shadda Beh
  const Codepoint bare_want[] = {0x0628, 0x064E, 0x0651};
  prv_visual(bare, 3, 1, out);
  prv_assert_eq(out, bare_want, 3);
  const Codepoint led[] = {0x200C, 0x064E, 0x0651, 0x0628};
  const Codepoint led_want[] = {0x0628, 0x200C, 0x064E, 0x0651};
  prv_visual(led, 4, 1, out);
  prv_assert_eq(out, led_want, 4);
}

// L3 with Arabic harakat: a fatha on the middle letter of a reversed word
// follows its base in the visual sequence.
void test_bidi__harakat_follow_base(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0628, 0x064E, 0x062C};  // Beh fatha Jeem
  const Codepoint want[] = {0x062C, 0x0628, 0x064E};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// The Cyrillic thousands sign is L per the UCD (not a neutral): it stays with
// the following Latin run.
void test_bidi__cyrillic_thousands_l(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x05D0, 0x20, 0x0482, 'a'};
  const Codepoint want[] = {0x0482, 'a', 0x20, 0x05D0};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// BD16 pairs brackets by canonical equivalence: the deprecated angle bracket
// pairs with the CJK closer, and vice versa, in either mixed combination.
void test_bidi__canonical_bracket_equivalence(void) {
  Codepoint out[10];
  const Codepoint mixed1[] = {0x200F, 'a', 0x20, 0x2329, 'b', '.', '1', 0x3009};
  const Codepoint want1[] = {'a', 0x20, 0x2329, 'b', '.', '1', 0x3009, 0x200F};
  prv_visual(mixed1, 8, 1, out);
  prv_assert_eq(out, want1, 8);
  const Codepoint mixed2[] = {0x200F, 'a', 0x20, 0x3008, 'b', '.', '1', 0x232A};
  const Codepoint want2[] = {'a', 0x20, 0x3008, 'b', '.', '1', 0x232A, 0x200F};
  prv_visual(mixed2, 8, 1, out);
  prv_assert_eq(out, want2, 8);
}

// Nested brackets resolve outer-before-inner (BD16), not on closing order.
// Base LTR: "a<AR>(( 1)b)" - the outer pair sees the trailing 'b' (L=e) and
// resolves to L, and the inner pair then resolves to L from that context, so
// the space keeps its place rather than flipping around the digit.
void test_bidi__nested_brackets_bd16(void) {
  Codepoint out[12];
  //  a    AR      (    (   SP   1    )    b    )
  const Codepoint in[] = {'a', 0x0627, '(', '(', ' ', '1', ')', 'b', ')'};
  const Codepoint want[] = {'a', 0x0627, '(', '(', ' ', '1', ')', 'b', ')'};
  prv_visual(in, 9, 0, out);
  prv_assert_eq(out, want, 9);
}

// The Arabic percent sign is ET (like '%'), not a strong Arabic letter: in an
// LTR line it does not drag the following number into an RTL run.
void test_bidi__arabic_percent_is_et(void) {
  Codepoint out[10];
  const Codepoint in[] = {'a', 'b', 'c', ' ', 0x066A, ' ', '1', '2'};  // abc ٪ 12
  prv_visual(in, 8, 0, out);
  prv_assert_eq(out, in, 8);
}

// CJK angle brackets are paired brackets: they pair (N0) and mirror (L4) like
// ASCII brackets. Around Arabic in an RTL line, "ا〈ب〉" reverses and the
// angles mirror so they keep wrapping the enclosed word.
void test_bidi__cjk_angle_brackets(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x3008, 0x0628, 0x3009};   // Alef 〈 Beh 〉
  const Codepoint want[] = {0x3008, 0x0628, 0x3009, 0x0627};  // 〈 Beh 〉 Alef, mirrored
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// Arabic-block signs that the UCD classes ON (roots, verse marks) are neutral,
// not strong letters: in an LTR line they do not start an RTL run.
void test_bidi__arabic_on_signs(void) {
  Codepoint out[8];
  const Codepoint in[] = {'a', 'b', 0x060E, 'c', 'd'};  // U+060E poetic verse sign
  prv_visual(in, 5, 0, out);
  prv_assert_eq(out, in, 5);
}

// A digit-only RTL line with a trailing suffix: the digits keep their order
// but the ellipsis moves to the visual left (reading end), which the standard
// renderer could not do - this is why the digit-only case enters the bidi path.
void test_bidi__digit_only_suffix_rtl(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0661, 0x0662, 0x2026};  // ١ ٢ …
  const Codepoint want[] = {0x2026, 0x0661, 0x0662};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// A regional-indicator pair is strong L (UCD): it stays adjacent and in order
// through an RTL reorder, so the draw pass can fold it into one flag after.
void test_bidi__ri_pair_survives_rtl(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x20, 0x1F1FA, 0x1F1F8};  // Alef SP RI-U RI-S
  const Codepoint want[] = {0x1F1FA, 0x1F1F8, 0x20, 0x0627};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// A literal flag emoji is a neutral symbol (UCD ON), not a strong character:
// it does not decide the base direction and travels with the surrounding run.
void test_bidi__literal_flag_is_neutral(void) {
  const Codepoint in[] = {0x1F3F3, 0x0627};  // white flag, Alef
  cl_assert_equal_i(bidi_base_level(in, 2), 1);
  Codepoint out[4];
  const Codepoint run[] = {0x0627, 0x1F3F3, 0x0628};  // Alef flag Beh
  const Codepoint want[] = {0x0628, 0x1F3F3, 0x0627};  // reversed with the run
  prv_visual(run, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// A trailing neutral (ellipsis/hyphen suffix) on an RTL line lands at the
// visual left edge - the reading-order end - not the right.
void test_bidi__trailing_neutral_rtl(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0646, 0x0635, 0x2026};  // Noon Sad …
  const Codepoint want[] = {0x2026, 0x0635, 0x0646};
  prv_visual(in, 3, 1, out);
  prv_assert_eq(out, want, 3);
}

// Brackets on a purely-RTL line: mirror and wrap the word. "(نص)" -> '(' ص ن ')'.
void test_bidi__brackets_rtl(void) {
  Codepoint out[8];
  const Codepoint in[] = {'(', 0x0646, 0x0635, ')'};  // ( Noon Sad )
  const Codepoint want[] = {'(', 0x0635, 0x0646, ')'};
  prv_visual(in, 4, 1, out);
  prv_assert_eq(out, want, 4);
}

// The Codex case: an LTR island in RTL keeps its parentheses wrapping it.
// "ا (en)" -> visual "(en) ا".
void test_bidi__ltr_island_in_rtl(void) {
  Codepoint out[8];
  const Codepoint in[] = {0x0627, 0x20, '(', 'e', 'n', ')'};
  const Codepoint want[] = {'(', 'e', 'n', ')', 0x20, 0x0627};
  size_t n = prv_visual(in, 6, 1, out);
  cl_assert_equal_i(n, 6);
  prv_assert_eq(out, want, 6);
}

// An RTL island in LTR: parentheses are NOT mirrored (LTR context). "ab (ج)".
void test_bidi__rtl_island_in_ltr(void) {
  Codepoint out[8];
  const Codepoint in[] = {'a', 'b', 0x20, '(', 0x062C, ')'};
  const Codepoint want[] = {'a', 'b', 0x20, '(', 0x062C, ')'};
  prv_visual(in, 6, 0, out);
  prv_assert_eq(out, want, 6);
}
