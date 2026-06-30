/* SPDX-FileCopyrightText: 2026 Ahmed Hussein */
/* SPDX-License-Identifier: Apache-2.0 */

#include "rtl_support.h"

#include "applib/fonts/codepoint.h"
#include "utf8.h"

bool utf8_contains_rtl(const utf8_t *start, const utf8_t *end) {
  if (start == NULL || end == NULL || start >= end) {
    return false;
  }

  utf8_t *ptr = (utf8_t *)start;
  while (ptr < end && *ptr != '\0') {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) {
      break;
    }
    if (codepoint_is_rtl(cp)) {
      return true;
    }
    ptr = next;
  }

  return false;
}
