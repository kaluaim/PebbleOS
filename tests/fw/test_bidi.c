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

// Reorder logical codepoints to visual order at the given base level.
static size_t prv_visual(const Codepoint *in, size_t n, int base, Codepoint *out) {
  return bidi_reorder_line(in, n, base, out);
}

static void prv_assert_eq(const Codepoint *got, const Codepoint *want, size_t n) {
  for (size_t i = 0; i < n; i++) {
    cl_assert_equal_i(got[i], want[i]);
  }
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

// Arabic-Indic digits stay left-to-right too (codepoint_is_rtl is true for them,
// so the engine must type them AN, not R).
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
