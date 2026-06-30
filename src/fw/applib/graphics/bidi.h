/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Minimal implementation of the Unicode Bidirectional Algorithm (UAX #9),
//! implicit levels only (no explicit RLE/LRE/RLI/... overrides, which do not
//! occur in watch text). Resolves embedding levels for a single display line,
//! reorders it to visual order, and mirrors paired punctuation. Lets a line
//! that mixes Arabic/Hebrew with Latin words, numbers and parenthesised islands
//! lay out correctly, which the previous per-direction-segment reversal could
//! not do.

//! Maximum codepoints resolved in one line. Matches the line buffers in the
//! text layout (a ~260 px line holds far fewer glyphs than this).
#define BIDI_MAX_CODEPOINTS 96

//! Paragraph embedding level from the first strong character (UAX #9 P2/P3):
//! 1 if the first strong character is R or AL, otherwise 0.
int bidi_base_level(const Codepoint *cps, size_t n);

//! Reorder a logical-order codepoint array into visual (left-to-right display)
//! order, applying implicit bidi level resolution (weak types, bracket pairs,
//! neutrals) and mirroring glyphs that resolve to an odd (RTL) level.
//! @param cps Logical-order codepoints (already Arabic-shaped)
//! @param n Number of codepoints (clamped to BIDI_MAX_CODEPOINTS)
//! @param base_level 0 for an LTR line, 1 for an RTL line
//! @param out Destination for visual-order codepoints (must hold >= n)
//! @return Number of codepoints written
size_t bidi_reorder_line(const Codepoint *cps, size_t n, int base_level, Codepoint *out);

//! UTF-8 wrapper around bidi_reorder_line: decode, reorder, re-encode.
//! @return Number of bytes written to dest (excluding any terminator)
size_t bidi_reorder_utf8(const utf8_t *src, size_t src_len, utf8_t *dest, size_t dest_size,
                         int base_level);

//! Mirror a codepoint with the Bidi_Mirrored property (parentheses, brackets,
//! braces, angle brackets, guillemets); returns the input unchanged otherwise.
Codepoint bidi_mirror(Codepoint cp);
