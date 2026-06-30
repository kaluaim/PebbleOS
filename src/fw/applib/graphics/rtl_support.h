/* SPDX-FileCopyrightText: 2026 Ahmed Hussein */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "utf8.h"

#include <stdbool.h>
#include <stddef.h>

//! Check if a UTF-8 string range contains any RTL (right-to-left) characters.
//! This includes Arabic (U+0600-U+06FF) and Hebrew (U+0590-U+05FF) scripts.
//! Used to decide whether a line needs the bidirectional layout path (see
//! bidi.h); the reordering itself lives in the bidi engine.
//! @param start Pointer to start of UTF-8 string
//! @param end Pointer to end of UTF-8 string (exclusive)
//! @return true if the range contains at least one RTL character
bool utf8_contains_rtl(const utf8_t *start, const utf8_t *end);
