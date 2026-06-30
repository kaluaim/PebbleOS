/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/graphics/rtl_support.h"
#include "applib/graphics/utf8.h"

#include "clar.h"

#include <string.h>

///////////////////////////////////////////////////////////
// Stubs

#include "stubs_logging.h"
#include "stubs_passert.h"

///////////////////////////////////////////////////////////
// Helpers

static bool prv_has_rtl(const char *s) {
  return utf8_contains_rtl((const utf8_t *)s, (const utf8_t *)s + strlen(s));
}

void test_rtl_support__initialize(void) {}
void test_rtl_support__cleanup(void) {}

///////////////////////////////////////////////////////////
// Tests

// Detects Arabic and Hebrew; Latin, digits and punctuation do not count. This
// gates the bidi layout path; the reordering itself is tested in test_bidi.c.
void test_rtl_support__contains_rtl(void) {
  cl_assert(prv_has_rtl("\xD8\xA7\xD8\xA8"));  // Arabic Alef Beh
  cl_assert(prv_has_rtl("hi \xD7\x90"));       // Hebrew Alef after Latin
  cl_assert(!prv_has_rtl("hello 123"));
  cl_assert(!prv_has_rtl("(test)"));
  cl_assert(!prv_has_rtl(""));
}
