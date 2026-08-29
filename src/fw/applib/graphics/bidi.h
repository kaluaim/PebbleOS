/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Minimal implementation of the Unicode Bidirectional Algorithm (UAX 9),
//! implicit levels only (no explicit RLE/LRE/RLI/... overrides, which do not
//! occur in watch text). Resolves embedding levels for a single display line,
//! reorders it to visual order, and mirrors paired punctuation. Lets a line
//! that mixes Arabic/Hebrew with Latin words, numbers and parenthesised islands
//! lay out correctly, which the previous per-direction-segment reversal could
//! not do.

//! Maximum codepoints resolved at once - a whole paragraph, so weak, neutral
//! and bracket resolution cross soft wraps exactly as UAX 9 requires. 255
//! keeps every index in a uint8_t; watch notification paragraphs are well
//! under it, and longer ones fall back to logical-order rendering.
#define BIDI_MAX_CODEPOINTS 255

//! Boundary-context hints for a wrapped display line: what the strong context
//! adjacent to this slice of the paragraph is. AUTO derives from the base
//! level (UAX 9 X10, correct for a whole paragraph); L/R/AL carry the strong
//! type across a soft wrap so weak and neutral runs straddling the wrap
//! resolve as they would have in the full paragraph.
#define BIDI_BOUNDARY_AUTO (-1)
#define BIDI_BOUNDARY_L 0
#define BIDI_BOUNDARY_R 1
#define BIDI_BOUNDARY_AL 2

//! Working storage for one paragraph resolution (~3.7 KiB). Task stacks are
//! 2-4 KiB, so the render path must heap-allocate this rather than keep it on
//! the stack; tests may use a static instance. After bidi_resolve_paragraph(),
//! cps and level hold the paragraph state; visual/type/open_pos are free for
//! the caller to reuse as line-building scratch, and bidi_apply_line() uses
//! orig and vis as its own scratch.
typedef struct BidiScratch {
  Codepoint cps[BIDI_MAX_CODEPOINTS];     //!< decoded logical-order codepoints
  Codepoint visual[BIDI_MAX_CODEPOINTS];  //!< reordered visual-order codepoints
  uint8_t type[BIDI_MAX_CODEPOINTS];      //!< resolved bidi class per position
  uint8_t orig[BIDI_MAX_CODEPOINTS];      //!< original bidi class per position
  uint8_t level[BIDI_MAX_CODEPOINTS];     //!< resolved embedding level
  uint8_t vis[BIDI_MAX_CODEPOINTS];       //!< visual-to-logical position map
  uint8_t open_pos[BIDI_MAX_CODEPOINTS];  //!< bracket stack: opening positions
  uint8_t pair_open[BIDI_MAX_CODEPOINTS];   //!< matched bracket pairs: open pos
  uint8_t pair_close[BIDI_MAX_CODEPOINTS];  //!< matched bracket pairs: close pos
} BidiScratch;

//! Paragraph embedding level from the first strong character (UAX 9 P2/P3):
//! 1 if the first strong character is R or AL, otherwise 0.
int bidi_base_level(const Codepoint *cps, size_t n);

//! UTF-8 variant of bidi_base_level() with one deviation from P2/P3: a
//! paragraph with no strong character but at least one Arabic-script number
//! codepoint (Arabic-Indic digits or Arabic number signs, class AN) is RTL,
//! so digit-only text (times, verification codes) in an Arabic notification
//! keeps its right alignment.
int bidi_base_level_utf8(const utf8_t *start, const utf8_t *end);

//! True if the range contains a strong RTL codepoint (bidi class R or AL:
//! Hebrew, Arabic including supplements and presentation forms), using the
//! same class table the reordering itself uses.
//! Distinct from bidi_base_level_utf8(): a digit-only Arabic-Indic paragraph
//! has no strong codepoint, so this returns false, but its base level is 1.
bool bidi_contains_rtl(const utf8_t *start, const utf8_t *end);

//! The bidi layout gate: true when a paragraph reorders, i.e. exactly
//! bidi_contains_rtl() || bidi_base_level_utf8() == 1, folded into a single
//! scan of the range (a strong RTL codepoint returns immediately; otherwise
//! the digit-only deviation applies only when no strong L was seen either).
//! The renderer, measurement pricing and alignment all gate on this, so a
//! suffix on a digit-only Arabic-Indic line still reorders to the visual
//! start and gating can never disagree with reordering.
bool bidi_paragraph_reorders(const utf8_t *start, const utf8_t *end);

//! Reorder a logical-order codepoint array into visual (left-to-right display)
//! order, applying implicit bidi level resolution (weak types, bracket pairs,
//! neutrals) and mirroring glyphs that resolve to an odd (RTL) level.
//! @param cps Logical-order codepoints (already Arabic-shaped)
//! @param n Number of codepoints (clamped to BIDI_MAX_CODEPOINTS)
//! @param base_level 0 for an LTR line, 1 for an RTL line
//! @param out Destination for visual-order codepoints (must hold >= n)
//! @param ws Working storage (cps/visual are the caller's; the rest is internal)
//! @return Number of codepoints written
size_t bidi_reorder_line(const Codepoint *cps, size_t n, int base_level, Codepoint *out,
                         BidiScratch *ws);

//! bidi_reorder_line() with explicit boundary context (BIDI_BOUNDARY_*).
//! Test/conformance surface: the renderer resolves whole paragraphs via
//! bidi_resolve_paragraph()/bidi_apply_line() and does not use these hints. sos_hint is the last strong
//! type before the slice (weak-rule context, W1/W2/W7); sos_n_hint is the
//! resolved direction adjacent to the slice where numbers count (neutral-rule
//! context, N0/N1); eos_hint is the first such direction after the slice.
size_t bidi_reorder_line_ctx(const Codepoint *cps, size_t n, int base_level, int sos_hint,
                             int sos_n_hint, int eos_hint, Codepoint *out, BidiScratch *ws);

//! UTF-8 wrapper around bidi_reorder_line: decode, reorder, re-encode.
//! @return Number of bytes written to dest (excluding any terminator)
size_t bidi_reorder_utf8(const utf8_t *src, size_t src_len, utf8_t *dest, size_t dest_size,
                         int base_level, BidiScratch *ws);

//! bidi_reorder_utf8() with explicit boundary context (BIDI_BOUNDARY_*).
size_t bidi_reorder_utf8_ctx(const utf8_t *src, size_t src_len, utf8_t *dest, size_t dest_size,
                             int base_level, int sos_hint, int sos_n_hint, int eos_hint,
                             BidiScratch *ws);

//! Last strong type (BIDI_BOUNDARY_L/R/AL) in the range, or BIDI_BOUNDARY_AUTO
//! if none. Test/conformance surface, as above.
int bidi_last_strong_utf8(const utf8_t *start, const utf8_t *end);

//! Resolved direction (BIDI_BOUNDARY_L/R) at the end of a paragraph prefix:
//! runs the resolver over the prefix and reads what the last character
//! resolved to, so N0 bracket resolution and W7 number resolution are both
//! represented. Feeds the sos_n (neutral-rule) hint for a wrapped line.
//! Clobbers ws.
int bidi_boundary_ndir_utf8(const utf8_t *start, const utf8_t *end, int base_level,
                            BidiScratch *ws);

//! First direction-determining type in the range for N-rule context: strong
//! L/R/AL (AL reports as R), or a number (AN is R; EN resolves per W2/W7 from
//! prev_strong, the last strong type before this range). BIDI_BOUNDARY_AUTO
//! if none. Feeds the eos hint for a wrapped line.
int bidi_first_strong_utf8(const utf8_t *start, const utf8_t *end, int prev_strong);

//! Resolve embedding levels for a whole paragraph (classes, weak rules,
//! bracket pairs, neutrals, implicit levels - UAX 9 P/W/N/I). Fills ws->cps
//! (a copy of cps), ws->level and ws->orig for n codepoints; no reordering is
//! performed. Lines are then extracted with bidi_apply_line(), which is the
//! UAX 9 Basic Display Algorithm: resolve once per paragraph, reorder per
//! display line.
//! @return n clamped to BIDI_MAX_CODEPOINTS
size_t bidi_resolve_paragraph(const Codepoint *cps, size_t n, int base_level, BidiScratch *ws);

//! Apply the per-line rules (L1 trailing-whitespace reset, L2 reversal, L4
//! mirroring) to one display line's codepoints and their paragraph-resolved
//! levels, writing the visual-order codepoints to out.
//! @param cps The line's codepoints (shaped; ligatures already folded)
//! @param levels The matching resolved level per codepoint
//! @param n Codepoint count (at most BIDI_MAX_CODEPOINTS)
//! @param out Destination for visual order (may not alias cps)
//! @param ws Scratch: uses orig (level working copy) and vis (order map)
size_t bidi_apply_line(const Codepoint *cps, const uint8_t *levels, size_t n, int base_level,
                       Codepoint *out, BidiScratch *ws);

//! Mirror the paired punctuation that occurs in watch text: parentheses,
//! square and curly brackets, angle brackets and guillemets. Other codepoints
//! with the Unicode Bidi_Mirrored property pass through unchanged.
Codepoint bidi_mirror(Codepoint cp);
