/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//!  Overview:
//!    - Summary of text layout and rendering:
//!      - A line iterator is created to iterate over the lines in a text-box
//!      - The line iterator creates a word iterator to advance through the text
//!      - The word iterator creates a character iterator to advance through
//!        codepoints. This allows reserved codepoints to be used for in-line text
//!        formatting.
//!      - The character iterator uses a UTF-8 iterator to advance through the
//!        UTF-8 encoded unicode codepoints.

#include "text.h"
#include "text_layout_private.h"

#include "arabic_shaping.h"
#include "bidi.h"
#include "graphics.h"
#include "graphics_private.h"
#include "gtypes.h"
#include "text_render.h"
#include "text_resources.h"
#include "utf8.h"

#include "applib/fonts/codepoint.h"
#include "applib/fonts/fonts.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/kernel_ui.h"
#include "process_state/app_state/app_state.h"
#include "applib/applib_malloc.auto.h"
#include "process_state/app_state/app_state.h"
#include <pbl/logging/logging.h>
#include "system/passert.h"
#include "pbl/util/hash.h"
#include "pbl/util/iterator.h"
#include "pbl/util/math.h"

#include "process_management/process_manager.h"

#include <stdint.h>
#include <string.h>
#include <limits.h>

static bool prv_char_iter_next_start_of_word(Iterator* char_iter);
static int prv_mirror_aware_advance(GContext *ctx, const TextBoxParams *text_box_params,
                                    Codepoint cp, int advance);
static bool prv_paragraph_may_reorder(const TextBoxParams *text_box_params, const utf8_t *pos,
                                      bool nl_as_space);

//! Lazily-evaluated prv_paragraph_may_reorder() answer for one word or line.
//! Mirror-aware pricing only matters for a codepoint with a bidi mirror, so
//! the paragraph scan the gate needs is deferred until such a codepoint
//! actually appears - most words and lines have none and never scan.
typedef struct {
  bool known;
  bool reorders;
} ReorderGate;

static int prv_priced_advance(GContext *ctx, const TextBoxParams *text_box_params, Codepoint cp,
                              int advance, const utf8_t *pos, ReorderGate *gate);

//! Check if a codepoint is invisible: formatting indicator or should-skip (no direction, no width).
static bool prv_codepoint_is_invisible(Codepoint cp) {
  return codepoint_is_formatting_indicator(cp) || codepoint_should_skip(cp);
}

//! Last non-transparent codepoint in the raw bytes [from, to), or 0 if none.
//! The character iterator filters formatting codepoints (ZWNJ, directional
//! marks) out of the stream, but they still break Arabic joining, so any
//! shaping context that tracks the previous codepoint from the iterator must
//! check the raw gap it skipped.
static Codepoint prv_last_raw_cp_between(const utf8_t *from, const utf8_t *to) {
  Codepoint found = 0;
  utf8_t *q = (utf8_t *)from;
  while (q != NULL && to != NULL && q < to && *q != '\0') {
    utf8_t *qnext = NULL;
    Codepoint qcp = utf8_peek_codepoint(q, &qnext);
    if (qcp == 0 || qnext == NULL) break;
    if (!arabic_is_transparent(qcp)) found = qcp;
    q = qnext;
  }
  return found;
}

//! Arabic joining context for text starting at pos: the last non-transparent
//! codepoint before it in the box, or 0 at the very start. A newline or any
//! non-Arabic codepoint breaks the join naturally, so this is safe across
//! paragraphs. Measurement, the bidi fit scan and the bidi line builder must
//! all use this same context, or a hyphenated intraword wrap measures a
//! different form (and width) than it draws - losing or doubling a glyph at
//! the wrap.
static Codepoint prv_joining_context_before(const TextBoxParams *const text_box_params,
                                            const utf8_t *pos) {
  Codepoint prev = 0;
  if (text_box_params->utf8_bounds == NULL || pos == NULL) return 0;
  utf8_t *q = (utf8_t *)text_box_params->utf8_bounds->start;
  // A join never survives a newline, so start at the last one before pos.
  for (utf8_t *r = (utf8_t *)pos; r > q; r--) {
    if (r[-1] == '\n') {
      q = r;
      break;
    }
  }
  while (q != NULL && q < pos && *q != '\0') {
    utf8_t *qnext = NULL;
    Codepoint qcp = utf8_peek_codepoint(q, &qnext);
    if (qcp == 0 || qnext == NULL) break;
    if (!arabic_is_transparent(qcp)) prev = qcp;
    q = qnext;
  }
  return prev;
}


//! Bounds of the paragraph containing pos: from just past the previous newline
//! (or the box start) to the next newline (or the box end). The bidi base
//! direction and the RTL gate are paragraph properties (UAX 9 P2/P3), so both
//! must look at exactly this range - not the whole text box.
//! The backward scan is per line, so a paragraph of length N costs O(N) per
//! wrapped line. Deliberate: watch text boxes are at most a few hundred bytes
//! and multi-paragraph text exits at the nearest newline, so caching paragraph
//! bounds across lines is not worth the state it would add.
static void prv_paragraph_bounds(const utf8_t *box_start, const utf8_t *box_end,
                                 const utf8_t *pos, bool nl_as_space,
                                 utf8_t **para_start, utf8_t **para_end) {
  // In Fill mode a newline is rendered as a space, not a paragraph break, so
  // the whole box is one paragraph for base-direction purposes.
  if (nl_as_space) {
    *para_start = (utf8_t *)box_start;
    *para_end = (utf8_t *)box_end;
    return;
  }
  utf8_t *start = (utf8_t *)box_start;
  for (utf8_t *q = (utf8_t *)pos; q > (utf8_t *)box_start; q--) {
    if (q[-1] == '\n') {
      start = q;
      break;
    }
  }
  utf8_t *end = memchr(pos, '\n', (size_t)(box_end - pos));
  *para_start = start;
  *para_end = (end != NULL) ? end : (utf8_t *)box_end;
}


// PBL-23045 Eventually remove perimeter debugging
void graphics_text_perimeter_debugging_enable(bool enable) {
  app_state_set_text_perimeter_debugging_enabled(enable);
}

// Return the advance width for a Unicode space codepoint, computed from the font's em size
// (max_height) and the reference glyph widths. Widths follow the Unicode standard, see
// https://jkorpela.fi/chars/spaces.html
// Returns -1 if the codepoint is not a Unicode space.
static int8_t prv_unicode_space_advance(FontCache *font_cache, const GFont font,
                                        const Codepoint codepoint) {
  if (!codepoint_is_unicode_space(codepoint)) {
    return -1;
  }

  const int em = font->max_height;

  switch (codepoint) {
    case EM_QUAD_CODEPOINT:
    case EM_SPACE_CODEPOINT:
    case IDEOGRAPHIC_SPACE_CODEPOINT:
      return em;
    case EN_QUAD_CODEPOINT:
    case EN_SPACE_CODEPOINT:
      return em / 2;
    case THREE_PER_EM_SPACE_CODEPOINT:
      return em / 3;
    case FOUR_PER_EM_SPACE_CODEPOINT:
      return em / 4;
    case SIX_PER_EM_SPACE_CODEPOINT:
      return em / 6;
    case THIN_SPACE_CODEPOINT:
    case NARROW_NO_BREAK_SPACE_CODEPOINT:
      return em / 5;
    case HAIR_SPACE_CODEPOINT:
      return MAX(em / 12, 1);
    case MEDIUM_MATHEMATICAL_SPACE_CODEPOINT:
      return (em * 4) / 18;
    case FIGURE_SPACE_CODEPOINT:
      return text_resources_get_glyph_horiz_advance(font_cache, '0', font);
    case PUNCTUATION_SPACE_CODEPOINT:
      return text_resources_get_glyph_horiz_advance(font_cache, '.', font);
    case NO_BREAK_SPACE_CODEPOINT:
      return text_resources_get_glyph_horiz_advance(font_cache, ' ', font);
    default:
      return -1;
  }
}

// [CTX] processing individual codepoints doesn't work for contextual writing systems.
static int8_t prv_codepoint_get_horizontal_advance(FontCache* const font_cache,
                                                   const GFont font,
                                                   const Codepoint codepoint) {
  PBL_ASSERTN(font_cache);
  int8_t horiz_advance = 0;
  if (codepoint_is_zero_width(codepoint)) {
    return 0;
  }
  const int8_t space_advance = prv_unicode_space_advance(font_cache, font, codepoint);
  if (space_advance >= 0) {
    return space_advance;
  }
  horiz_advance = text_resources_get_glyph_horiz_advance(font_cache, codepoint, font);
  return MAX(horiz_advance, 0);
}

////////////////////////////////////////////////////////////
// Init functions

//! @note can be init to a null-termination character
void char_iter_init(Iterator* char_iter, CharIterState* char_iter_state, const TextBoxParams* const text_box_params, utf8_t* start) {
  Iterator* utf8_iter = &char_iter_state->utf8_iter;
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state->utf8_iter_state;

  utf8_iter_init(utf8_iter, utf8_iter_state, text_box_params->utf8_bounds, start);

  char_iter_state->text_box_params = text_box_params;

  iter_init(char_iter, (IteratorCallback) char_iter_next, char_iter_prev, (IteratorState) char_iter_state);
}

typedef enum {
  WordStateStart,
  WordStateIdeograph,
  WordStateGrowing,
  WordStateJoining,
  WordStateEnd,
} WordState;

WordState word_state_update(WordState state, Codepoint codepoint) {
  WordState new_state = state;

  switch (state) {
    case WordStateStart:
      if (codepoint == NEWLINE_CODEPOINT) {
        new_state = WordStateEnd;
      } else if (codepoint_is_ideograph(codepoint)) {
        new_state = WordStateIdeograph;
      } else {
        new_state = WordStateGrowing;
      }
      break;
    case WordStateIdeograph:
      if (codepoint == WORD_JOINER_CODEPOINT) {
        new_state = WordStateJoining;
      } else {
        new_state = WordStateEnd;
      }
      break;
    case WordStateGrowing:
      if (codepoint == WORD_JOINER_CODEPOINT) {
        new_state = WordStateJoining;
      } else if (codepoint_is_ideograph(codepoint) || codepoint_is_end_of_word(codepoint)) {
        new_state = WordStateEnd;
      } else {
        new_state = WordStateGrowing;
      }
      break;
    case WordStateJoining:
      if (codepoint == NEWLINE_CODEPOINT) {
        new_state = WordStateEnd;
      } else if (codepoint_is_ideograph(codepoint)) {
        new_state = WordStateIdeograph;
      } else if (codepoint == WORD_JOINER_CODEPOINT) {
        new_state = WordStateJoining;
      } else {
        new_state = WordStateGrowing;
      }
      break;
    case WordStateEnd:
      new_state = WordStateEnd;
      break;
  }

  return new_state;
}

//! @return true if init to new word, false otherwise (ie end of text)
//! @note assumes 'start' is not NULL, but does not assume 'start' is valid start of word
static Codepoint prv_peek_next_letter(utf8_t *pos, const utf8_t *end);

// Pair shaping shared by the measuring and rendering passes; emoji pairs fold
// first, then the existing script shaping runs.
static Codepoint prv_shape_pair(Codepoint prev_cp, Codepoint curr_cp, Codepoint next_cp,
                                bool *consumed_next) {
  Codepoint shaped_cp = emoji_shape_pair(curr_cp, next_cp, consumed_next);
  if (shaped_cp != curr_cp || *consumed_next) {
    return shaped_cp;
  }
  return arabic_shape_pair(prev_cp, curr_cp, next_cp, consumed_next);
}

bool word_init(GContext* ctx, Word* word, const TextBoxParams* const text_box_params, utf8_t* start) {
  word->width_px = 0;

  if (*start == NULL_CODEPOINT) {
    word->start = start;
    word->end = start;
    return false;
  }

  // Set up iterator
  Iterator char_iter;
  CharIterState char_iter_state;
  char_iter_init(&char_iter, &char_iter_state, text_box_params, start);
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state.utf8_iter_state;

  bool success = prv_char_iter_next_start_of_word(&char_iter);
  if (!success) {
    // We couldn't find the next start of the word, just initialize to nothing
    word->start = start;
    word->end = start;
    return false;
  }

  // Init the word & state
  word->start = utf8_iter_state->current;
  // Track previous and current codepoints so we can resolve Arabic
  // contextual presentation forms while measuring. The renderer shapes
  // letters before drawing (see walk_line()), so unshaped widths would
  // over- or under-estimate the word and cause spurious wraps.
  Codepoint prev_cp = 0;
  Codepoint curr_cp = utf8_iter_state->codepoint;
  WordState state = WordStateStart;
  state = word_state_update(state, curr_cp);
  // prv_shape_pair() can fold this codepoint and the next into one glyph and
  // report the next as consumed; its advance is then already counted, so the
  // folded codepoint is skipped later. Marks (harakat) are transparent: they
  // keep their own width but are stepped over when picking joining context,
  // like the renderer.
  bool skip_ligature_member = false;
  const utf8_t *bounds_end =
      (text_box_params->utf8_bounds != NULL) ? text_box_params->utf8_bounds->end : NULL;
  ReorderGate reorder_gate = {0};

  do {
    utf8_t *curr_pos = utf8_iter_state->current;
    iter_next(&char_iter);
    Codepoint next_cp = utf8_iter_state->codepoint;

    if (state == WordStateGrowing || state == WordStateIdeograph) {
      if (skip_ligature_member && !arabic_is_transparent(curr_cp)) {
        // Folded into the preceding pair: already counted.
        skip_ligature_member = false;
      } else if (arabic_is_transparent(curr_cp)) {
        // A mark keeps its own width but is not reshaped.
        word->width_px += prv_codepoint_get_horizontal_advance(&ctx->font_cache,
            text_box_params->font, curr_cp);
      } else {
        // Raw peek, not the iterator's next: a filtered-out formatting
        // codepoint between two letters must break the join here just as it
        // does in the render fit scan and line builder. ZWNJ breaks the join
        // per Unicode; ZWJ is join-causing there but breaks it here too, a
        // deliberate simplification kept identical across all three stages so
        // measurement and rendering never disagree.
        Codepoint shape_next = prv_peek_next_letter(curr_pos, bounds_end);
        bool consumed_next = false;
        Codepoint width_cp = prv_shape_pair(prev_cp, curr_cp, shape_next, &consumed_next);
        int adv = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
            text_box_params->font, width_cp);
        adv = prv_priced_advance(ctx, text_box_params, width_cp, adv, word->start, &reorder_gate);
        word->width_px += adv;
        skip_ligature_member = consumed_next;
      }
    }

    if (!arabic_is_transparent(curr_cp)) {
      prev_cp = curr_cp;
    }
    {
      // If the iterator skipped formatting codepoints, the last of them is
      // the real joining predecessor.
      utf8_t *after_curr = NULL;
      utf8_peek_codepoint(curr_pos, &after_curr);
      Codepoint gap_cp = prv_last_raw_cp_between(after_curr, utf8_iter_state->current);
      if (gap_cp != 0) {
        prev_cp = gap_cp;
      }
    }
    curr_cp = next_cp;
    state = word_state_update(state, curr_cp);
  } while (state != WordStateEnd);

  word->end = utf8_iter_state->current;

  return true;
}

void word_iter_init(Iterator* word_iter, WordIterState* word_iter_state, GContext* ctx,
                    const TextBoxParams* const text_box_params, utf8_t* start) {
  *word_iter_state = (WordIterState) {
    .ctx = ctx,
    .text_box_params = text_box_params
  };

  word_init(ctx, &word_iter_state->current, text_box_params, start);

  iter_init(word_iter, (IteratorCallback) word_iter_next, NULL, (IteratorState) word_iter_state);
}

void line_iter_init(Iterator* line_iter, LineIterState* line_iter_state, GContext* ctx) {
  *line_iter_state = (LineIterState) {
    .ctx = ctx,
    .current = &ctx->text_draw_state.line
  };

  WordIterState* word_iter_state = &line_iter_state->word_iter_state;
  word_iter_init(&line_iter_state->word_iter, word_iter_state, ctx,
                 &ctx->text_draw_state.text_box, ctx->text_draw_state.text_box.utf8_bounds->start);

  iter_init(line_iter, (IteratorCallback) line_iter_next, NULL, (IteratorState) line_iter_state);
}

////////////////////////////////////////////////////////////
// Private helper functions

static int16_t prv_get_line_height(const TextBoxParams *text_box_params) {
  return fonts_get_font_height(text_box_params->font) + text_box_params->line_spacing_delta;
}

static int16_t prv_layout_get_line_spacing_delta(GTextLayoutCacheRef layout) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    return 0;
  }

  return (layout ? ((TextLayoutExtended *)layout)->line_spacing_delta: 0);
}

////////////////////////////////////////////////////////////
// Iterator advance functions

//! Advance the char iterator to the start of the next word. Used by word_init
//! to find the start of the next word.
//! @return is_success
static bool prv_char_iter_next_start_of_word(Iterator* char_iter) {
  CharIterState* char_iter_state = (CharIterState*) char_iter->state;
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state->utf8_iter_state;

  // the first codepoint could be invalid, iter_next takes care of the others
  Codepoint codepoint = utf8_iter_state->codepoint;
  if (codepoint_should_skip(codepoint) || codepoint_is_formatting_indicator(codepoint)) {
    if (!iter_next(char_iter)) {
      return false;
    }
  }

  while (codepoint_is_zero_width(utf8_iter_state->codepoint)) {
    if (utf8_iter_state->codepoint == 0) {
      PBL_ASSERTN(utf8_iter_state->current == utf8_iter_state->bounds->end);
      return false;
    }

    if (!iter_next(char_iter)) {
      break;
    }
  }

  return true;
}

static bool prv_line_iter_is_vertical_overflow(const LineIterState* const line_iter_state,
                                               const TextBoxParams* const text_box_params) {
  int16_t next_line_y_extent;
  // Normally, we lay out the text one line below the regular cutoff so that it may be rendered,
  // albeit clipped.  But, if we're rendering in truncation mode (e.g. GTextOverflowModeFill or
  // GTextOverflowModeTrailingEllipsis), we can immediately cut the text off below the box height
  // if we're not rendering the first line.
  //    - This, because the user does not expect to see more text drawn below, after the '...'.
  //    - The first-line exception means that text, and therefore the telltale
  //      ellipsis, will always be visisble.
  if ((text_box_params->overflow_mode == GTextOverflowModeTrailingEllipsis ||
       text_box_params->overflow_mode == GTextOverflowModeFill) &&
      line_iter_state->current->origin.y != text_box_params->box.origin.y) {
    // We're in a truncation mode AND not on the first line.
    // So, include the full height of the current line in next_line_y_extent, so text will stop
    // being layed out immediately after it exceeds the height of the container.
    next_line_y_extent = line_iter_state->current->origin.y + prv_get_line_height(text_box_params);
  } else {
    // We're either in a non-truncating mode, or on the first line of a truncating mode.
    // So, only include the extent of the previous line in next_line_y_extent (making it more of
    // a "last_line_y_extent").
    // Putting aside the misleading variable name, this will cause us to lay out one more line than
    // will completely fit in the container - so that it may still be displayed, even if partially
    // or completely clipped.
    next_line_y_extent = line_iter_state->current->origin.y;
  }
  return (next_line_y_extent > (text_box_params->box.origin.y + text_box_params->box.size.h));
}

//! @return is_advanced
bool line_iter_next(IteratorState state) {
  LineIterState* line_iter_state = (LineIterState*) state;
  const TextBoxParams* const text_box_params = &line_iter_state->ctx->text_draw_state.text_box;

  if (prv_line_iter_is_vertical_overflow(line_iter_state, text_box_params)) {
    return false;
  }

  line_iter_state->current->origin.x = text_box_params->box.origin.x;
  line_iter_state->current->origin.y += prv_get_line_height(text_box_params);
  line_iter_state->current->width_px = 0;  // needs to be reset per line
  line_iter_state->current->max_width_px = text_box_params->box.size.w;
  line_iter_state->current->suffix_codepoint = 0;
  line_iter_state->current->bidi_text_end = NULL;
  line_iter_state->current->start = NULL;

  return true;
}

//! @return is_advanced
bool word_iter_next(IteratorState state) {
  WordIterState* word_iter_state = (WordIterState*) state;

  Word* current_word = &word_iter_state->current;
  const TextBoxParams* const text_box_params = word_iter_state->text_box_params;
  GContext* ctx = word_iter_state->ctx;

  if (*current_word->end == NULL_CODEPOINT) {
    return false;
  }

  return word_init(ctx, current_word, text_box_params, current_word->end);
}

//! @return is_advanced
bool char_iter_next(IteratorState state) {
  CharIterState* char_iter_state = (CharIterState*) state;

  Codepoint codepoint;
  Iterator* utf8_iter = &char_iter_state->utf8_iter;
  Utf8IterState* utf8_iter_state = &char_iter_state->utf8_iter_state;

  while (true) {
    if (utf8_iter_state->current >= utf8_iter_state->bounds->end) {
      // EOS while searching for valid codepoint
      return false;
    }

    bool is_utf8_advanced = iter_next(utf8_iter);
    codepoint = utf8_iter_state->codepoint;

    if (!is_utf8_advanced) {
      return is_utf8_advanced;
    }

    PBL_ASSERTN(codepoint != 0);

    if (codepoint_is_formatting_indicator(codepoint)) {
      continue;
    }

    if (codepoint_should_skip(codepoint)) {
      continue;
    };

    return true;
  }
}

bool char_iter_prev(IteratorState state) {
  CharIterState* char_iter_state = (CharIterState*) state;

  Codepoint codepoint;
  Iterator* utf8_iter = &char_iter_state->utf8_iter;
  Utf8IterState* utf8_iter_state = &char_iter_state->utf8_iter_state;

  while (true) {
    if (utf8_iter_state->current <= utf8_iter_state->bounds->start) {
      // EOS while searching for valid codepoint
      return false;
    }

    bool is_utf8_advanced = iter_prev(utf8_iter);
    codepoint = utf8_iter_state->codepoint;

    if (!is_utf8_advanced) {
      return is_utf8_advanced;
    }

    PBL_ASSERTN(codepoint != 0);

    if (codepoint_is_formatting_indicator(codepoint)) {
      continue;
    }

    if (codepoint_should_skip(codepoint)) {
      continue;
    };

    return true;
  }
}

////////////////////////////////////////////////////////////
// Helper functions

//! Trim given codepoint from the start of the word
//! Used to remove whitespace and newlines
//! @return is_trimmed
bool word_trim_preceeding_codepoint(GContext* ctx, Word* word, const Codepoint codepoint,
                                    const TextBoxParams* const text_box_params) {
  Iterator char_iter;
  CharIterState char_iter_state;
  char_iter_init(&char_iter, &char_iter_state, text_box_params, word->start);

  Utf8IterState* utf8_iter_state = &char_iter_state.utf8_iter_state;

  if (utf8_iter_state->codepoint != codepoint) {
    return false;
  }

  bool is_advanced = iter_next(&char_iter);

  if (!is_advanced) {
    PBL_ASSERTN(*word->end == NULL_CODEPOINT);
    word->start = NULL;
    return false;
  }

  if (word->end == char_iter_state.utf8_iter_state.current) {
    // Word has been completely trimmed; init a new word
    bool is_end_of_text = (*word->end == NULL_CODEPOINT ||
        char_iter_state.utf8_iter_state.current >= text_box_params->utf8_bounds->end);

    if (!is_end_of_text) {
      word_init(ctx, word, text_box_params, word->end);
    }
    return false;
  }

  // Trim
  int advance = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
      text_box_params->font, codepoint);
  PBL_ASSERTN(advance <= word->width_px); // Negative-length word not allowed

  word->width_px -= advance;
  word->start = utf8_iter_state->current;
  return true;
}

// [INTL] whitespace is more than just the space character.
void word_trim_preceeding_whitespace(GContext* ctx, Word* word, const TextBoxParams* const text_box_params) {
  while (word_trim_preceeding_codepoint(ctx, word, SPACE_CODEPOINT, text_box_params));
}

////////////////////////////////////////////////////////////
// Walk Line

typedef void (*CharVisitorCallback)(GContext* ctx, const TextBoxParams* const text_box_params,
                                    Line* line, GRect cursor, const Codepoint codepoint);

void render_chars_char_visitor_cb(GContext* ctx, const TextBoxParams* const text_box_params,
                                  Line* line, GRect cursor, const Codepoint codepoint) {
  if (codepoint_is_zero_width(codepoint) || codepoint_is_unicode_space(codepoint)) {
    return;
  }

  render_glyph(ctx, codepoint, text_box_params->font, cursor);
}

void update_dimensions_char_visitor_cb(GContext* ctx, const TextBoxParams* const text_box_params,
                                       Line* line, GRect cursor, const Codepoint codepoint) {
  (void) ctx;
  (void) codepoint;
  PBL_ASSERT(cursor.origin.x >= line->origin.x, "Text cursor x=<%u> ahead of line origin x=<%u>",
      cursor.origin.x, line->origin.x);

  // Use the advance the caller already computed (the cursor width). walk_line
  // makes it pair-aware -- a folded pair is one glyph -- so recomputing it per
  // codepoint here would double-count the folded member on the measurement path.
  const int glyph_width_px = cursor.size.w;

  line->width_px = (cursor.origin.x + glyph_width_px) - line->origin.x;

  PBL_ASSERT(line->width_px <= text_box_params->box.size.w,
      "Line <%p>: max extent=<%" PRId16 "> exceeds text_box_params width=<%" PRId16 ">",
      line, line->width_px + line->origin.x, text_box_params->box.size.w);
}

// Peek the next letter after the one at `pos`, skipping transparent Arabic marks
// and bounded by `end`. Returns 0 at a line/text boundary. Lets the width pass
// fold a Lam-Alef (even across an intervening harakat) into one ligature advance
// and pick the same joining context the renderer shapes with.
static Codepoint prv_peek_next_letter(utf8_t *pos, const utf8_t *end) {
  if (pos == NULL) {
    return 0;
  }
  utf8_t *after = NULL;
  utf8_peek_codepoint(pos, &after);
  while (after != NULL && (end == NULL || after < end) && *after != '\0' && *after != '\n') {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(after, &next);
    if (cp == 0 || next == NULL) {
      break;
    }
    if (!arabic_is_transparent(cp)) {
      return cp;
    }
    after = next;
  }
  return 0;
}

// Pair-aware glyph advance: a pair folded by prv_shape_pair() measures as the
// single glyph it draws as, matching word_init and the render path.
// *consumed_next is set when the next codepoint was folded in and must
// therefore contribute zero width. *shaped_cp receives the folded codepoint so
// the draw pass renders the glyph that was measured.
static int prv_shaped_glyph_advance(GContext *ctx, const TextBoxParams *text_box_params,
                                    Codepoint prev_cp, Codepoint curr_cp, Codepoint next_cp,
                                    bool *consumed_next, Codepoint *shaped_cp) {
  Codepoint width_cp = prv_shape_pair(prev_cp, curr_cp, next_cp, consumed_next);
  *shaped_cp = width_cp;
  return prv_codepoint_get_horizontal_advance(&ctx->font_cache, text_box_params->font, width_cp);
}

//! Advance for measurement in a paragraph that may reorder: a Bidi_Mirrored
//! codepoint can be drawn as its mirror, whose advance may differ in a
//! proportional font (e.g. '<' 9 px vs '>' 10 px in Roboto Condensed 21).
//! Price the wider of the two so the measured width can never be exceeded by
//! the drawn width - the residue is at most a hairline gap, never overdraw.
//! Callers gate this on the paragraph containing RTL, so LTR-only text
//! prices exactly as before.
static int prv_mirror_aware_advance(GContext *ctx, const TextBoxParams *text_box_params,
                                    Codepoint cp, int advance) {
  Codepoint mirrored = bidi_mirror(cp);
  if (mirrored == cp) return advance;
  int m_adv = prv_codepoint_get_horizontal_advance(&ctx->font_cache, text_box_params->font,
                                                   mirrored);
  return (m_adv > advance) ? m_adv : advance;
}

//! True when the paragraph containing pos can reorder (same gate as the bidi
//! render path), meaning measurement must price mirrored advances.
static bool prv_paragraph_may_reorder(const TextBoxParams *text_box_params, const utf8_t *pos,
                                      bool nl_as_space) {
  if (text_box_params->utf8_bounds == NULL || text_box_params->utf8_bounds->end == NULL ||
      pos == NULL || text_box_params->utf8_bounds->end <= pos) {
    return false;
  }
  utf8_t *para_start = NULL;
  utf8_t *para_end = NULL;
  prv_paragraph_bounds(text_box_params->utf8_bounds->start, text_box_params->utf8_bounds->end,
                       pos, nl_as_space, &para_start, &para_end);
  return bidi_paragraph_reorders(para_start, para_end);
}

//! Advance for the measurement paths: price cp mirror-aware only when the
//! paragraph containing pos reorders, resolving that gate lazily through
//! *gate. A codepoint with no mirror never consults the gate, so a word or
//! line without mirrored punctuation skips the gate's paragraph scan.
static int prv_priced_advance(GContext *ctx, const TextBoxParams *text_box_params, Codepoint cp,
                              int advance, const utf8_t *pos, ReorderGate *gate) {
  if (bidi_mirror(cp) == cp) {
    return advance;
  }
  if (!gate->known) {
    gate->reorders = prv_paragraph_may_reorder(text_box_params, pos,
        text_box_params->overflow_mode == GTextOverflowModeFill);
    gate->known = true;
  }
  if (!gate->reorders) {
    return advance;
  }
  return prv_mirror_aware_advance(ctx, text_box_params, cp, advance);
}

typedef struct {
  size_t count;
  int width_px;
  int base_level;
  bool suffix_in_visual;
} BidiVisualLine;

// Resolve the paragraph and build one shaped, reordered display-line slice.
static bool prv_build_bidi_visual_line(GContext *ctx, const TextBoxParams *text_box_params,
                                       utf8_t *para_start, utf8_t *para_end, utf8_t *line_start,
                                       utf8_t *line_end, Codepoint suffix_codepoint,
                                       BidiScratch *ws, BidiVisualLine *result) {
  if (line_start == NULL || line_end == NULL || line_start > line_end || line_start < para_start ||
      line_end > para_end) {
    return false;
  }

  size_t para_n = 0;
  size_t lo = 0;
  size_t hi = 0;
  bool found_lo = false;
  bool found_hi = false;
  const bool nl_as_space = (text_box_params->overflow_mode == GTextOverflowModeFill);
  utf8_t *dp = para_start;
  while (dp < para_end && *dp != '\0') {
    if (dp == line_start) {
      lo = para_n;
      found_lo = true;
    }
    if (dp == line_end) {
      hi = para_n;
      found_hi = true;
    }

    utf8_t *dnext = NULL;
    Codepoint dcp = utf8_peek_codepoint(dp, &dnext);
    if (dcp == 0 || dnext == NULL || para_n >= BIDI_MAX_CODEPOINTS) {
      return false;
    }
    if (nl_as_space && dcp == '\n') {
      dcp = ' ';
    }
    ws->cps[para_n++] = dcp;
    dp = dnext;
  }
  if (dp == line_start) {
    lo = para_n;
    found_lo = true;
  }
  if (dp == line_end) {
    hi = para_n;
    found_hi = true;
  }
  if (!found_lo || !found_hi || hi < lo ||
      (hi - lo) + (suffix_codepoint ? 1u : 0u) > BIDI_MAX_CODEPOINTS) {
    return false;
  }

  const int base_level = bidi_base_level_utf8(para_start, para_end);
  bidi_resolve_paragraph(ws->cps, para_n, base_level, ws);

  Codepoint *const line_cps = ws->visual;
  uint8_t *const line_lvl = ws->type;
  size_t m = 0;
  Codepoint prev_sh = 0;
  for (size_t k = lo; k > 0; k--) {
    if (!arabic_is_transparent(ws->cps[k - 1])) {
      prev_sh = ws->cps[k - 1];
      break;
    }
  }

  size_t k = lo;
  while (k < hi) {
    const Codepoint cur = ws->cps[k];
    Codepoint nxt = 0;
    size_t nxt_idx = para_n;
    for (size_t j = k + 1; j < para_n; j++) {
      if (!arabic_is_transparent(ws->cps[j])) {
        nxt = ws->cps[j];
        nxt_idx = j;
        break;
      }
    }

    bool consumed = false;
    Codepoint shaped;
    if (codepoint_is_regional_indicator(cur)) {
      shaped = FLAG_CODEPOINT;
      consumed = (nxt_idx < hi) && codepoint_is_regional_indicator(nxt);
    } else if (arabic_is_transparent(cur)) {
      shaped = cur;
    } else {
      shaped = arabic_shape_pair(prev_sh, cur, nxt, &consumed);
      if (consumed && nxt_idx >= hi) {
        consumed = false;
        shaped = arabic_shape_codepoint(prev_sh, cur, nxt);
      }
    }

    line_cps[m] = shaped;
    line_lvl[m] = ws->level[k];
    m++;
    if (consumed) {
      for (size_t j = k + 1; j < nxt_idx; j++) {
        line_cps[m] = ws->cps[j];
        line_lvl[m] = ws->level[j];
        m++;
      }
      prev_sh = nxt;
      k = nxt_idx + 1;
    } else {
      if (!arabic_is_transparent(cur)) {
        prev_sh = cur;
      }
      k++;
    }
  }

  if (suffix_codepoint) {
    line_cps[m] = suffix_codepoint;
    line_lvl[m] = (uint8_t)base_level;
    m++;
  }

  const size_t count = bidi_apply_line(line_cps, line_lvl, m, base_level, ws->cps, ws);
  int width_px = 0;
  for (size_t i = 0; i < count; i++) {
    if (!prv_codepoint_is_invisible(ws->cps[i])) {
      width_px +=
          prv_codepoint_get_horizontal_advance(&ctx->font_cache, text_box_params->font, ws->cps[i]);
    }
  }

  *result = (BidiVisualLine){
      .count = count,
      .width_px = width_px,
      .base_level = base_level,
      .suffix_in_visual = (suffix_codepoint != 0),
  };
  return true;
}

static bool prv_get_exact_bidi_width(GContext *ctx, const TextBoxParams *text_box_params,
                                     utf8_t *line_start, utf8_t *line_end,
                                     Codepoint suffix_codepoint, int *width_px) {
  if (text_box_params->utf8_bounds == NULL || text_box_params->utf8_bounds->end == NULL ||
      line_start == NULL || line_end == NULL) {
    return false;
  }

  const bool nl_as_space = (text_box_params->overflow_mode == GTextOverflowModeFill);
  utf8_t *para_start = NULL;
  utf8_t *para_end = NULL;
  prv_paragraph_bounds(text_box_params->utf8_bounds->start, text_box_params->utf8_bounds->end,
                       line_start, nl_as_space, &para_start, &para_end);
  if (!bidi_paragraph_reorders(para_start, para_end)) {
    return false;
  }

  BidiScratch *ws = task_malloc(sizeof(BidiScratch));
  if (ws == NULL) {
    return false;
  }
  BidiVisualLine visual;
  const bool success =
      prv_build_bidi_visual_line(ctx, text_box_params, para_start, para_end, line_start, line_end,
                                 suffix_codepoint, ws, &visual);
  if (success) {
    *width_px = visual.width_px;
  }
  task_free(ws);
  return success;
}

//! Call char_visitor_cb on each character in the line
//! Used to update line dimensions and render characters
//! Traverse until end of line->width_px if rendering chars, else text_box_params width
//! if updating line dimensions
//! @return utf8_t* pointer to last visited character
utf8_t* walk_line(GContext* ctx, Line* line, const TextBoxParams* const text_box_params,
                  CharVisitorCallback char_visitor_cb) {
  PBL_ASSERTN(char_visitor_cb);

  // Render pre-loads glyph bitmaps below; measure must not, or the deep flash load overflows.
  const bool is_render = (char_visitor_cb != update_dimensions_char_visitor_cb);

  // We used to check that the line height was <= the container height here - no longer required,
  // as the vertical overflow is handled during layout.

  int available_horiz_px;
  if (char_visitor_cb == update_dimensions_char_visitor_cb) {
    // Line dimensions not yet set; use all available line space
    available_horiz_px = line->max_width_px;
  } else {
    available_horiz_px = line->width_px;
  }

  PBL_ASSERT(line->width_px <= text_box_params->box.size.w,
      "Line <%p>: max extent=<%" PRId16 "> exceeds text_box_params width=<%" PRId16 ">", line,
      line->width_px + line->origin.x, text_box_params->box.size.w);

  int suffix_width_px = 0;

  if (line->suffix_codepoint) {
    suffix_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
        text_box_params->font, line->suffix_codepoint);
  }

  if (available_horiz_px < suffix_width_px) {
    return NULL;
  }

  // RTL support: lines whose paragraph contains RTL characters take the bidi
  // path during the render pass (measurement shares the standard path below).
  bool is_rendering = (char_visitor_cb == render_chars_char_visitor_cb);
  bool take_bidi_path = false;
  utf8_t *para_start = NULL;
  utf8_t *para_end = NULL;
  if (is_rendering && line->start != NULL && text_box_params->utf8_bounds != NULL &&
      text_box_params->utf8_bounds->end != NULL &&
      text_box_params->utf8_bounds->end > line->start) {
    // Gate on the line's whole paragraph: a display line never crosses a
    // newline, so RTL text in another paragraph must not pull this line
    // through the (allocating) bidi path - but every line of a paragraph that
    // contains RTL anywhere must take it, including LTR-only continuation
    // lines, so their alignment and edge punctuation follow the paragraph
    // base direction.
    const bool nl_as_space = (text_box_params->overflow_mode == GTextOverflowModeFill);
    prv_paragraph_bounds(text_box_params->utf8_bounds->start, text_box_params->utf8_bounds->end,
                         line->start, nl_as_space, &para_start, &para_end);
    // A paragraph with strong RTL, or a digit-only paragraph that resolves RTL
    // (Arabic-Indic numbers): the latter reorders to the identity, but a
    // truncation suffix still has to land at the visual start.
    take_bidi_path = bidi_paragraph_reorders(para_start, para_end);
  }
  if (take_bidi_path) {
    const bool nl_as_space = (text_box_params->overflow_mode == GTextOverflowModeFill);

    // Render the line with the bidirectional algorithm: find the logical range
    // that fits, Arabic-shape it, reorder to visual order and draw left to right.
    utf8_t *line_end = (utf8_t *)text_box_params->utf8_bounds->end;

    // Walk the logical text accumulating shaped (ligature-folded, mark-aware)
    // advances - the same arithmetic the measurement pass uses - to find where
    // this line ends within the available width. Mirrored codepoints are
    // priced at the wider of the pair (see prv_mirror_aware_advance), so the
    // drawn line can never exceed the fitted width. Also cap the codepoint count:
    // the reorder handles at most BIDI_MAX_CODEPOINTS, and zero-width marks
    // consume codepoints without consuming width, so width alone cannot bound
    // it; without the cap the tail past the limit would be consumed but never
    // drawn.
    int content_width_px = 0;
    // Reserve one codepoint of reorder capacity for the suffix, which joins
    // the sequence below so it lands on the correct visual side.
    const size_t fit_cap = BIDI_MAX_CODEPOINTS - (line->suffix_codepoint ? 1 : 0);
    size_t fit_cps = 0;
    // Joining context shared with the standard measurement path and the line
    // builder: an intraword wrap must measure the same (medial) form it draws.
    Codepoint prev_cp = prv_joining_context_before(text_box_params, line->start);
    bool skip_ligature_member = false;
    utf8_t *fit_end = line->start;
    utf8_t *last_cp_start = NULL;
    utf8_t *p = (utf8_t *)line->start;
    bool cap_truncated = false;
    if (line->bidi_text_end != NULL) {
      fit_end = line->bidi_text_end;
      while (p < fit_end && p < line_end && *p != '\0' && fit_cps < fit_cap) {
        utf8_t *pnext = NULL;
        const Codepoint cp = utf8_peek_codepoint(p, &pnext);
        if (cp == 0 || pnext == NULL || pnext > fit_end) {
          break;
        }
        fit_cps++;
        last_cp_start = p;
        p = pnext;
      }
      cap_truncated = (p != fit_end);
    } else {
      while (p < line_end && *p != '\0' && (nl_as_space || *p != '\n') && fit_cps < fit_cap) {
        utf8_t *pnext = NULL;
        Codepoint cp = utf8_peek_codepoint(p, &pnext);
        if (cp == 0 || pnext == NULL) {
          break;
        }
        if (nl_as_space && cp == '\n') {
          cp = ' ';
        }
        fit_cps++;
        if (prv_codepoint_is_invisible(cp)) {
          prev_cp = cp;
          last_cp_start = p;
          p = pnext;
          fit_end = p;
          continue;
        }
        if (skip_ligature_member && !arabic_is_transparent(cp)) {
          skip_ligature_member = false;
        } else {
          int glyph_width;
          if (arabic_is_transparent(cp)) {
            glyph_width =
                prv_codepoint_get_horizontal_advance(&ctx->font_cache, text_box_params->font, cp);
          } else {
            Codepoint next_cp = prv_peek_next_letter(p, line_end);
            Codepoint shaped_cp;
            glyph_width = prv_shaped_glyph_advance(ctx, text_box_params, prev_cp, cp, next_cp,
                                                   &skip_ligature_member, &shaped_cp);
            glyph_width = prv_mirror_aware_advance(ctx, text_box_params, shaped_cp, glyph_width);
          }
          if (content_width_px + glyph_width + suffix_width_px > available_horiz_px) {
            break;
          }
          content_width_px += glyph_width;
        }
        if (!arabic_is_transparent(cp)) {
          prev_cp = cp;
        }
        last_cp_start = p;
        p = pnext;
        fit_end = p;
      }

      // If the cap stopped the scan before the width or paragraph boundary,
      // fall back instead of dropping the tail.
      cap_truncated =
          (fit_cps >= fit_cap) && p < line_end && *p != '\0' && (nl_as_space || *p != '\n');
    }

    // Trim trailing spaces like the standard path: they must not consume
    // width or sit between the content and its suffix. In Fill mode a trailing
    // newline is a space too. (last_cp_start may point at a trimmed space
    // afterwards - the trimmed codepoints were visited, and the render-pass
    // caller discards the return value.)
    while (fit_end > line->start &&
           (fit_end[-1] == ' ' || (nl_as_space && fit_end[-1] == '\n'))) {
      fit_end--;
    }

    // UAX 9 Basic Display Algorithm: resolve the whole paragraph once, then
    // apply the per-line rules (L1/L2/L4) to this display line's slice. Weak,
    // neutral and bracket resolution therefore cross soft wraps exactly as
    // they would in an unwrapped paragraph. On allocation failure, paragraph
    // overflow or the codepoint cap, fall through to the standard path so the
    // line degrades to complete logical-order rendering. The scratch uses
    // task_malloc(), not applib_malloc(): the latter croaks on a failed
    // privileged allocation, which would take down a system task instead of
    // falling back.
    int walked_width_px = 0;
    bool bidi_rendered = false;
    bool suffix_in_visual = false;
    size_t fit_len = (size_t)(fit_end - line->start);
    BidiScratch *bidi_ws = NULL;
    if (!cap_truncated && fit_len > 0) {
      bidi_ws = task_malloc(sizeof(BidiScratch));
    }
    if (bidi_ws) {
      BidiVisualLine visual;
      if (prv_build_bidi_visual_line(ctx, text_box_params, para_start, para_end, line->start,
                                     fit_end, line->suffix_codepoint, bidi_ws, &visual)) {
        bidi_rendered = true;
        suffix_in_visual = visual.suffix_in_visual;

        GTextAlignment eff_align = text_box_params->alignment;
        if (eff_align == GTextAlignmentLeft && visual.base_level == 1) {
          eff_align = GTextAlignmentRight;
        }
        int pen_shift = 0;
        const int width_delta = (int)line->width_px - visual.width_px;
        if (width_delta > 0) {
          if (eff_align == GTextAlignmentRight) {
            pen_shift = width_delta;
          } else if (eff_align == GTextAlignmentCenter) {
            pen_shift = width_delta / 2;
          }
        }
        walked_width_px = pen_shift;

        int base_x = pen_shift;
        int base_adv = 0;
        for (size_t v = 0; v < visual.count; v++) {
          const Codepoint vcp = bidi_ws->cps[v];
          if (prv_codepoint_is_invisible(vcp)) {
            continue;
          }
          const int glyph_width =
              prv_codepoint_get_horizontal_advance(&ctx->font_cache, text_box_params->font, vcp);
          GRect cursor = {
              .origin = line->origin,
              .size.w = glyph_width,
              .size.h = fonts_get_font_height(text_box_params->font),
          };
          cursor.origin.x += walked_width_px;
          if (!codepoint_is_zero_width(vcp) && !codepoint_is_unicode_space(vcp)) {
            const GlyphData *glyph =
                text_resources_get_glyph(&ctx->font_cache, vcp, text_box_params->font, NULL);
            if (glyph_width == 0 && glyph != NULL) {
              const int m_left = glyph->header.left_offset_px;
              const int m_w = glyph->header.width_px;
              int best_pen = base_x;
              int best_overlap = -1;
              const int pens[2] = {base_x, base_x + base_adv};
              for (int pi = 0; pi < 2; pi++) {
                const int span_lo = pens[pi] + m_left;
                const int span_hi = span_lo + m_w;
                const int o_lo = (span_lo > base_x) ? span_lo : base_x;
                const int o_hi = (span_hi < base_x + base_adv) ? span_hi : base_x + base_adv;
                const int overlap = (o_hi > o_lo) ? (o_hi - o_lo) : 0;
                if (overlap > best_overlap) {
                  best_overlap = overlap;
                  best_pen = pens[pi];
                }
              }
              cursor.origin.x = line->origin.x + best_pen;
            }
            render_glyph(ctx, vcp, text_box_params->font, cursor);
          }
          if (glyph_width > 0) {
            base_x = walked_width_px;
            base_adv = glyph_width;
          }
          walked_width_px += glyph_width;
        }
      }
      task_free(bidi_ws);
    } else if (!cap_truncated && fit_len == 0) {
      bidi_rendered = true;  // nothing fits: nothing to draw beyond the suffix
    }
    // Any other way here (cap, paragraph overflow, allocation failure) falls
    // through to the standard logical-order path below.

    if (bidi_rendered) {
      // Suffix not reordered with the content (empty line): draw it after.
      if (line->suffix_codepoint && !suffix_in_visual) {
        GRect cursor = {
          .origin = line->origin,
          .size.w = suffix_width_px,
          .size.h = fonts_get_font_height(text_box_params->font),
        };
        cursor.origin.x += walked_width_px;
        text_resources_get_glyph(&ctx->font_cache, line->suffix_codepoint, text_box_params->font,
                                 NULL);
        render_glyph(ctx, line->suffix_codepoint, text_box_params->font, cursor);
      }

      // Per the function contract: the start of the last visited codepoint,
      // or NULL when nothing fit on the line.
      return last_cp_start;
    }
    // Bidi render did not run: continue into the standard path (which draws
    // the suffix itself).
    if (line->bidi_text_end != NULL) {
      // Preserve the accepted logical slice if bidi scratch allocation fails.
      available_horiz_px = INT_MAX;
    }
  }

  // Standard rendering path (no RTL or not rendering)
  Iterator char_iter;
  CharIterState char_iter_state;
  char_iter_init(&char_iter, &char_iter_state, text_box_params, line->start);
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state.utf8_iter_state;

  bool is_newline_as_space = text_box_params->overflow_mode == GTextOverflowModeFill;
  Codepoint current_codepoint = utf8_iter_state->codepoint;
  if (current_codepoint == NEWLINE_CODEPOINT) {
    if (is_newline_as_space) {
      current_codepoint = SPACE_CODEPOINT;
    } else {
      return utf8_iter_state->current;
    }
  }

  const utf8_t *text_end =
      (text_box_params->utf8_bounds != NULL) ? text_box_params->utf8_bounds->end : NULL;

  int walked_width_px = 0;
  // Arabic joining context from the text before this line, matching the bidi
  // fit scan and line builder, so layout and render always agree on forms.
  Codepoint prev_shaped_cp = prv_joining_context_before(text_box_params, line->start);
  // One advance policy everywhere: in a paragraph that can reorder, price
  // mirrored codepoints at the wider advance here too, or the hyphenation
  // split consumes a codepoint the render fit cannot fit (it is then drawn on
  // neither line).
  ReorderGate std_gate = {0};
  bool skip_ligature_member = false;  // current codepoint was folded into a preceding pair
  bool consumed_next = false;
  bool current_folded = false;        // current codepoint was folded into the preceding pair
  Codepoint current_draw_cp = current_codepoint;  // shaped codepoint whose advance was measured
  Codepoint peek_cp = prv_peek_next_letter(utf8_iter_state->current, text_end);
  int next_glyph_width_px = prv_shaped_glyph_advance(ctx, text_box_params, prev_shaped_cp,
      current_codepoint, peek_cp, &consumed_next, &current_draw_cp);
  next_glyph_width_px = prv_priced_advance(ctx, text_box_params, current_draw_cp,
                                           next_glyph_width_px, line->start, &std_gate);

  utf8_t* last_visited_char = NULL;

  while ((line->bidi_text_end == NULL || utf8_iter_state->current < line->bidi_text_end) &&
         walked_width_px + next_glyph_width_px + suffix_width_px <= available_horiz_px) {
    GRect cursor = {
      .origin = line->origin,
      .size.w = next_glyph_width_px,
      .size.h = fonts_get_font_height(text_box_params->font)
    };
    cursor.origin.x += walked_width_px;

    // A codepoint folded into the preceding pair has zero width and must not
    // draw over the pair's glyph; everything else draws the shaped codepoint
    // whose advance was measured.
    if (!current_folded) {
      if (is_render && !codepoint_is_zero_width(current_draw_cp) &&
          !codepoint_is_unicode_space(current_draw_cp)) {
        // Pre-load here so the deeper render_glyph() is a cache hit, not a deep flash read.
        text_resources_get_glyph(&ctx->font_cache, current_draw_cp, text_box_params->font, NULL);
      }

      char_visitor_cb(ctx, text_box_params, line, cursor, current_draw_cp);
    }

    walked_width_px += next_glyph_width_px;

    last_visited_char = utf8_iter_state->current;

    // Carry the ligature decision before advancing. Marks (harakat) keep their
    // width but do not break joining context, so they leave prev/skip untouched.
    if (consumed_next) {
      skip_ligature_member = true;
    }
    if (!arabic_is_transparent(current_codepoint)) {
      prev_shaped_cp = current_codepoint;
    }

    utf8_t *pos_before_advance = utf8_iter_state->current;
    if (!iter_next(&char_iter)) {
      break;
    }
    {
      // Formatting codepoints the iterator skipped still break the join.
      utf8_t *after_prev = NULL;
      utf8_peek_codepoint(pos_before_advance, &after_prev);
      Codepoint gap_cp = prv_last_raw_cp_between(after_prev, utf8_iter_state->current);
      if (gap_cp != 0) {
        prev_shaped_cp = gap_cp;
      }
    }

    current_codepoint = utf8_iter_state->codepoint;
    if (current_codepoint == NEWLINE_CODEPOINT) {
      if (is_newline_as_space) {
        current_codepoint = SPACE_CODEPOINT;
      } else {
        break;
      }
    }

    consumed_next = false;
    current_folded = false;
    if (skip_ligature_member && !arabic_is_transparent(current_codepoint)) {
      // Folded into the preceding pair: adds no width.
      next_glyph_width_px = 0;
      skip_ligature_member = false;
      current_folded = true;
    } else if (arabic_is_transparent(current_codepoint)) {
      // A mark keeps its own width but is not reshaped.
      current_draw_cp = current_codepoint;
      next_glyph_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
          text_box_params->font, current_codepoint);
    } else {
      peek_cp = prv_peek_next_letter(utf8_iter_state->current, text_end);
      next_glyph_width_px = prv_shaped_glyph_advance(ctx, text_box_params, prev_shaped_cp,
          current_codepoint, peek_cp, &consumed_next, &current_draw_cp);
      next_glyph_width_px = prv_priced_advance(ctx, text_box_params, current_draw_cp,
                                               next_glyph_width_px, line->start, &std_gate);
    }
  }

  // Trim trailing whitespace
  if (last_visited_char) {
    while ((current_codepoint == NEWLINE_CODEPOINT || current_codepoint == SPACE_CODEPOINT)) {
      // Newlines should not adjust the width
      if (current_codepoint == NEWLINE_CODEPOINT) {
        next_glyph_width_px = 0;
      } else {
        next_glyph_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
                                                               text_box_params->font,
                                                               current_codepoint);
      }

      // Safety check
      if (walked_width_px < next_glyph_width_px) {
        break;
      }
      walked_width_px -= next_glyph_width_px;

      if (!iter_prev(&char_iter)) {
        break;
      }
      current_codepoint = utf8_iter_state->codepoint;
    }
  }

  if (line->suffix_codepoint) {
    GRect cursor = {
      .origin = line->origin,
      .size.w = suffix_width_px,
      .size.h = fonts_get_font_height(text_box_params->font)
    };
    cursor.origin.x += walked_width_px;
    if (char_visitor_cb) {
      if (is_render) {
        text_resources_get_glyph(&ctx->font_cache, line->suffix_codepoint, text_box_params->font,
                                 NULL);
      }
      char_visitor_cb(ctx, text_box_params, line, cursor, line->suffix_codepoint);
    }
  }

  return last_visited_char;
}


////////////////////////////////////////////////////////////
// Walk Lines

void set_ellipsis_on_overflow_last_line_cb(GContext* ctx, Line* line,
                                           const TextBoxParams* const text_box_params,
                                           const bool is_text_remaining) {
  // Only set a trailing ellipsis if there is text remaining
  if (!is_text_remaining) {
    return;
  }

  // Check if outputting two lines extend beyond the text box height - then display the ellipsis
  // on the current line
  bool is_last_line = ((line->origin.y + (2 * prv_get_line_height(text_box_params))) >
                       (text_box_params->box.origin.y + text_box_params->box.size.h));
  // Check if this is the last line
  if (!is_last_line) {
    return;
  }

  line->suffix_codepoint = ELLIPSIS_CODEPOINT;
  line->bidi_text_end = NULL;

  // update the line dimensions
  walk_line(ctx, line, text_box_params, update_dimensions_char_visitor_cb);
}

void render_all_render_line_cb(GContext* ctx, Line* line, const TextBoxParams* const text_box_params) {
  walk_line(ctx, line, text_box_params, (CharVisitorCallback) render_chars_char_visitor_cb);
}

void update_all_layout_update_cb(TextLayout* layout, Line* line,
                                 const TextBoxParams* const text_box_params) {
  PBL_ASSERTN(line);
  if (layout) {
    layout->max_used_size.h = (line->origin.y - layout->box.origin.y) + line->height_px +
                              text_box_params->line_spacing_delta;
    layout->max_used_size.w = MAX(line->width_px, layout->max_used_size.w);
  }
}

//! @return is_overflow
bool is_clip_box_overflow_top_stop_condition_cb(GContext* ctx, Line* line,
                                                const TextBoxParams* const text_box_params) {
  int next_line_max_y = line->origin.y;
  int clip_box_min_y = ctx->draw_state.clip_box.origin.y;
  return (next_line_max_y < clip_box_min_y);
}

//! @return is_overflow
bool is_clip_box_overflow_bottom_stop_condition_cb(GContext* ctx, Line* line,
                                                   const TextBoxParams* const text_box_params) {
  int next_line_min_y = line->origin.y + line->height_px + text_box_params->line_spacing_delta;
  int clip_box_max_y = ctx->draw_state.clip_box.origin.y + ctx->draw_state.clip_box.size.h;
  return (next_line_min_y > clip_box_max_y);
}

//! @return is_overflow
bool is_clip_box_overflow_stop_condition_cb(GContext* ctx, Line* line,
                                            const TextBoxParams* const text_box_params) {
  return (is_clip_box_overflow_bottom_stop_condition_cb(ctx, line, text_box_params) ||
          is_clip_box_overflow_top_stop_condition_cb(ctx, line, text_box_params));
}

#define TEXT_LINE_BASE_LINE(line) ((line)->height_px)
#define TEXT_LINE_CAP_LINE(line) ((line)->height_px * 1 / 2)
// Based on Gothic fonts, DESCENDER is approx 1/5 of height (ascender + descender)
// Gothic 24 Bold ascent = 840, descent 168
// Gothic 18 Bold ascent = 840, descent 168
// Gothic 14 ascent = 864, descent 144
// Bitham ascent = 800, descend = 200
// DroidSerif Bold ascent = 1638, descent = 410
#define TEXT_LINE_DESCENDER_LINE(line) DIVIDE_CEIL((line)->height_px, 5)  // 1/5th rounded up

T_STATIC NOINLINE MOCKABLE void prv_debug_perimeter(GContext *ctx, const GRangeHorizontal *h_range,
                                                   const Line *line) {
  // PBL-23045 Eventually remove perimeter debugging
  // Draw a red horizontal line to show the range of the current lines perimeter
  if (app_state_get_text_perimeter_debugging_enabled()) {
#if !defined(UNITTEST)
    const Fixed_S16_3 fixed_x1 = (Fixed_S16_3) {
      .integer = h_range->origin_x,
    };
    const Fixed_S16_3 fixed_x2 = (Fixed_S16_3) {
      .integer = h_range->origin_x + h_range->size_w,
    };
    graphics_private_draw_horizontal_line_prepared(ctx, &ctx->dest_bitmap,
                                                   &ctx->dest_bitmap.bounds,
                                                   line->origin.y + TEXT_LINE_CAP_LINE(line),
                                                   fixed_x1, fixed_x2, GColorRed);
    graphics_private_draw_horizontal_line_prepared(ctx, &ctx->dest_bitmap,
                                                   &ctx->dest_bitmap.bounds,
                                                   line->origin.y + TEXT_LINE_BASE_LINE(line),
                                                   fixed_x1, fixed_x2, GColorRed);
#endif
  }
}

typedef struct {
  int16_t origin_x;
  int16_t width_px;
  utf8_t *bidi_text_end;
} OrphanLineState;

static OrphanLineState prv_capture_orphan_state(Line const* line) {
  return (OrphanLineState){
      .origin_x = line->origin.x,
      .width_px = line->width_px,
      .bidi_text_end = line->bidi_text_end,
  };
}

static void prv_apply_orphan_state(const OrphanLineState *state, Line *line) {
  line->origin.x = state->origin_x;
  line->width_px = state->width_px;
  line->bidi_text_end = state->bidi_text_end;
}

//! Iterate over lines in the text box
static inline void prv_walk_lines_down(Iterator* const line_iter, TextLayout* const layout,
                                       WalkLinesCallbacks* const callbacks) {
  LineIterState* line_iter_state = (LineIterState*) line_iter->state;
  GContext* ctx = line_iter_state->ctx;
  const GSize ctx_size = graphics_context_get_framebuffer_size(ctx);
  const TextBoxParams* const text_box_params = &ctx->text_draw_state.text_box;
  Line* line = line_iter_state->current;

  const TextLayoutFlowData *flow_data = graphics_text_layout_get_flow_data(layout);
  const bool uses_paging = flow_data->paging.page_on_screen.size_h != 0;
  const bool uses_perimeter = flow_data->perimeter.impl != NULL;
  const GPoint perimeter_paging_offset =
    uses_paging ? gpoint_sub(flow_data->paging.origin_on_screen, line->origin) : GPointZero;
  Word prev_line_word = WORD_EMPTY;
  while (!prv_line_iter_is_vertical_overflow(line_iter_state, text_box_params)) {
    GPoint line_in_perimeter_space = gpoint_add(line->origin, perimeter_paging_offset);

    if (uses_paging) {
      const int16_t page_max_y = flow_data->paging.page_on_screen.origin_y +
                                 flow_data->paging.page_on_screen.size_h;

      // TODO: optimize
      while (line_in_perimeter_space.y < flow_data->paging.page_on_screen.origin_y) {
        line_in_perimeter_space.y += flow_data->paging.page_on_screen.size_h;
      }
      while (line_in_perimeter_space.y >= page_max_y) {
        line_in_perimeter_space.y -= flow_data->paging.page_on_screen.size_h;
      }

      const int16_t distance_to_page_end = page_max_y - line_in_perimeter_space.y;

      if (distance_to_page_end < line->height_px + TEXT_LINE_DESCENDER_LINE(line)) {
        // If this line would exceed the page_height, shift the line origin to the next page
        line->origin.y += distance_to_page_end;
        continue;  // skip rendering this round, bypasses iter_next (no reset necessary)
      }
    }

    // If we are restricting the perimeter of the draw box, restrict per line region here
    if (uses_perimeter) {
      GRangeHorizontal text_horizontal_range = {.origin_x = line_in_perimeter_space.x,
                                                .size_w = line->max_width_px};
      const GRangeVertical vertical_range = {
        .origin_y = line_in_perimeter_space.y + TEXT_LINE_CAP_LINE(line),
        .size_h = TEXT_LINE_BASE_LINE(line) - TEXT_LINE_CAP_LINE(line)
      };
      GRangeHorizontal perimeter_horizontal_range =
        flow_data->perimeter.impl->callback(flow_data->perimeter.impl, &ctx_size, vertical_range,
                                            flow_data->perimeter.inset);

      prv_debug_perimeter(ctx, &perimeter_horizontal_range, line);

      // protect against range expanding: clip perimeter to the original text range
      grange_clip((GRange*)&perimeter_horizontal_range, (GRange*)&text_horizontal_range);
      text_horizontal_range = perimeter_horizontal_range;

      // convert range back to screen space
      text_horizontal_range.origin_x -= perimeter_paging_offset.x;

      // Update line parameters for restricted horizontal range
      line->origin.x = text_horizontal_range.origin_x;
      line->max_width_px = text_horizontal_range.size_w;
    }

    // reference into the iterator's current word to easily access this attribute here and
    // later without the complicated cast
    Word *const current_word_ref = &(((WordIterState*)line_iter_state->word_iter.state)->current);
    // state that needs to be captured so we can restore it in case of an orphan
    const Word word_before_rendering = *current_word_ref;
    const OrphanLineState orphan_state = prv_capture_orphan_state(line);

    // When repeating text to prevent orhpans we could run into the situation where repeating text
    // pushes down the remaining text far enough so it ends up on yet another page. This would
    // enter an infinite loop.
    // To avoid that, we only apply this strategy, when it's "safe" to do so (in theory, there's
    // still the propability to run into this scenario if the perimeter isn't vertically symmetric).
    // The chosen number should be large enough for the previous line, the orphan line plus some
    // buffer.
    const int num_safe_lines = 3;
    const bool page_contains_enough_lines =
      (flow_data->paging.page_on_screen.size_h >= num_safe_lines * line->height_px);
    bool avoiding_orphans = uses_paging && ctx->draw_state.avoid_text_orphans &&
                            page_contains_enough_lines;

render_line: {} // this {} is just an empty statement that both C and our linter accepts
    const bool is_text_remaining = line_add_words(
        line, &line_iter_state->word_iter, callbacks->last_line_cb);
    // NOTE: Account for descender - assume descender is no more than half the line height
    const int16_t line_spacing_delta = prv_layout_get_line_spacing_delta(layout);
    const int32_t line_max_y = line->origin.y + line->height_px +
                               TEXT_LINE_DESCENDER_LINE(line) + line_spacing_delta;
    const int32_t clip_box_min_y = ctx->draw_state.clip_box.origin.y;

    if (line_max_y > clip_box_min_y) {
      if (avoiding_orphans) {
        const bool line_is_first_line_page =
          (line_in_perimeter_space.y == flow_data->paging.page_on_screen.origin_y);
        const bool is_orphan =
          (line_is_first_line_page && prev_line_word.start && !is_text_remaining);

        if (is_orphan) {
          *current_word_ref = prev_line_word;
          prv_apply_orphan_state(&orphan_state, line);
          avoiding_orphans = false; // prevent infinte loops
          goto render_line;
        }
      }
      if (callbacks->render_line_cb) {
        callbacks->render_line_cb(ctx, line, text_box_params);
      }
    }
    prev_line_word = word_before_rendering;

    if (callbacks->layout_update_cb) {
      callbacks->layout_update_cb(layout, line, text_box_params);
    }

    if (callbacks->stop_condition_cb) {
      if (callbacks->stop_condition_cb(ctx, line, text_box_params)) {
        break;
      }
    }

    if (!is_text_remaining) {
      break;
    }

    // Shouldn't have rendered the line if there was insufficient space
    PBL_ASSERTN(iter_next(line_iter));
  }
}

////////////////////////////////////////////////////////////
// Text layout

//! @return is_success
bool line_add_word(GContext* ctx, Line* line, Word* word, const TextBoxParams* const text_box_params) {
  // Horizontal overflow
  if (line->width_px > line->max_width_px) {
    return false;
  }

  // Don't set the line height if there is a vertical overflow
  const int line_height = fonts_get_font_height(text_box_params->font);

  // We used to re-check for vertical overflow here
  // but this is protected by a call to prv_line_iter_is_vertical_overflow,
  // which will handle the truncation/clipping logic.

  PBL_ASSERTN(word->start);

  bool is_newline_first_codepoint = (*word->start == NEWLINE_CODEPOINT);

  line->height_px = line_height;

  if (is_newline_first_codepoint) {
    // This trims off leading \n's from word. If we reach the end of the text while doing this, it sets
    //  word->start to NULL. 
    word_trim_preceeding_codepoint(ctx, word, NEWLINE_CODEPOINT, text_box_params);
    if (text_box_params->overflow_mode != GTextOverflowModeFill) {
      return false;
    }
    // If there is word text left (we have \n's at the end of the text), we're done
    if (word->start == NULL) {
      return false;
    }
  }

  bool is_overflow = (line->width_px + word->width_px > line->max_width_px);
  bool is_start_of_line = (line->width_px == 0);

  if (is_start_of_line) {
    line->start = word->start;
  }

  // Conservative mirror pricing can reject a slice whose reordered glyphs
  // fit. Once that happens, measure each following candidate through the same
  // shaping and reordering path used by the renderer.
  if (is_overflow || line->bidi_text_end != NULL) {
    int exact_width_px = 0;
    if (prv_get_exact_bidi_width(ctx, text_box_params, line->start, word->end, 0,
                                 &exact_width_px)) {
      if (exact_width_px <= line->max_width_px) {
        PBL_ASSERTN(line->suffix_codepoint == 0);
        line->width_px = exact_width_px;
        line->bidi_text_end = word->end;
        return true;
      }
      is_overflow = true;
    } else if (line->bidi_text_end != NULL) {
      is_overflow = true;
    }
  }

  const bool should_hyphenate = (is_overflow && is_start_of_line);
  if (should_hyphenate) {
    // Set suffix character
    // [CJK] - when breaking a Katakana word, you probably don't want to add a hyphen. And to
    // a Japanese user, a hyphen with Katakana looks like a long (chou-on) sound mark.
    line->suffix_codepoint = HYPHEN_CODEPOINT;
    utf8_t* last_visited = walk_line(ctx, line, text_box_params,
        (CharVisitorCallback) update_dimensions_char_visitor_cb);
    last_visited = (last_visited == NULL) ? (word->start) : last_visited;

    // Trim the word
    int suffix_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
        text_box_params->font, HYPHEN_CODEPOINT);
    int truncated_word_length_px = (line->width_px - suffix_width_px);
    PBL_ASSERTN(word->width_px >= truncated_word_length_px);
    word->width_px -= truncated_word_length_px;
    word->start = utf8_get_next(last_visited);
    // Never start a continuation line on a filtered formatting codepoint
    // (ZWNJ, directional marks): the iterator never yields them, but the
    // standard measurement path takes a line's first codepoint raw and would
    // price it as a visible wildcard glyph, splitting layout and render
    // accounting. They remain in the raw text, so the joining-context scans
    // still see them break the join.
    while (word->start != NULL) {
      utf8_t *skip_next = NULL;
      Codepoint skip_cp = utf8_peek_codepoint(word->start, &skip_next);
      if (skip_cp == 0 || skip_next == NULL || !prv_codepoint_is_invisible(skip_cp)) {
        break;
      }
      word->start = skip_next;
    }

    return false;
  }

  if (!is_overflow) {
    // Add entire word
    PBL_ASSERTN(line->suffix_codepoint == 0);
    line->width_px += word->width_px;
    return true;
  }

  // Word-wrap
  word_trim_preceeding_whitespace(ctx, word, text_box_params);
  return false;
}

static void prv_line_justify(Line* line, const TextBoxParams* const text_box_params) {
  PBL_ASSERTN(line->max_width_px >= line->width_px);

  int horiz_px_remaining = (line->max_width_px - line->width_px);

  // Determine effective alignment - RTL text defaults to right alignment
  GTextAlignment effective_alignment = text_box_params->alignment;

  // If alignment is left (default) and the paragraph base direction is RTL,
  // switch to right. Scan the line's whole paragraph with the same base-level
  // rule as the bidi render path, so every wrapped line of an RTL paragraph
  // aligns the same way (including a digit-only Arabic-Indic paragraph, which
  // the renderer also gates into the bidi path so a suffix reorders left).
  if (effective_alignment == GTextAlignmentLeft && line->start != NULL &&
      text_box_params->utf8_bounds != NULL && text_box_params->utf8_bounds->end != NULL &&
      text_box_params->utf8_bounds->end > line->start) {
    utf8_t *para_start = NULL;
    utf8_t *para_end = NULL;
    const bool nl_as_space = (text_box_params->overflow_mode == GTextOverflowModeFill);
    prv_paragraph_bounds(text_box_params->utf8_bounds->start, text_box_params->utf8_bounds->end,
                         line->start, nl_as_space, &para_start, &para_end);
    if (bidi_base_level_utf8(para_start, para_end) == 1) {
      effective_alignment = GTextAlignmentRight;
    }
  }

  switch (effective_alignment) {
    case GTextAlignmentCenter:
      line->origin.x = line->origin.x + (horiz_px_remaining / 2);
      break;
    case GTextAlignmentRight:
      line->origin.x = line->origin.x + horiz_px_remaining;
      break;
    case GTextAlignmentLeft:
      break;
  }
}

//! @return is_text_remaining
bool line_add_words(Line* line, Iterator* word_iter, LastLineCallback last_line_cb) {

  WordIterState* word_iter_state = (WordIterState*) word_iter->state;

  line->start = word_iter_state->current.start;

  bool is_text_remaining = (line->start != NULL);

  // PBL-22083 : max_width_px == 0 eats a character that should appear on next line
  while (is_text_remaining && line->max_width_px > 0) {
    Word next_word = word_iter_state->current;

    bool is_added = line_add_word(word_iter_state->ctx, line, &next_word,
                                  word_iter_state->text_box_params);

    if (!is_added) {
      word_iter_state->current = next_word;
      // Check if word was trimmed until the null termination
      if (next_word.start == NULL) {
        is_text_remaining = false;
      } else {
        is_text_remaining = true;
      }
      break;
    }

    is_text_remaining = iter_next(word_iter);
  }

  if (last_line_cb) {
    last_line_cb(word_iter_state->ctx, line, word_iter_state->text_box_params, is_text_remaining);
  }

  prv_line_justify(line, word_iter_state->text_box_params);

  return is_text_remaining;
}

static bool prv_text_layout_is_fresh(TextLayout* layout, GFont const font, const GRect box,
                                     const GTextOverflowMode overflow_mode,
                                     const GTextAlignment alignment, Codepoint text_hash) {
  PBL_ASSERTN(layout);

  if (text_hash != layout->hash) {
    return false;
  }

  if (!grect_equal(&box, &layout->box)) {
    return false;
  }

  if (overflow_mode != layout->overflow_mode) {
    return false;
  }

  if (alignment != layout->alignment) {
    return false;
  }

  if (font != layout->font) {
    return false;
  }

  return true;
}

static inline void prv_text_walk_lines(GContext* ctx, TextLayout* const layout,
                                       WalkLinesCallbacks* callbacks) {

  TextBoxParams *text_box = &ctx->text_draw_state.text_box;

  // Degenerate boxes draw nothing; negative sizes must not reach line layout
  // (grect_is_empty only catches sizes that are exactly zero)
  if (text_box->box.size.w <= 0 || text_box->box.size.h <= 0) {
    return;
  }

  const Utf8Bounds *utf8_bounds = text_box->utf8_bounds;

  bool is_string_empty = (utf8_bounds->start == utf8_bounds->end);
  if (is_string_empty) {
    return;
  }

  const GTextOverflowMode overflow_mode = text_box->overflow_mode;
  bool is_ellipsis_on_overflow = (overflow_mode == GTextOverflowModeTrailingEllipsis ||
                                  overflow_mode == GTextOverflowModeFill);
  if (is_ellipsis_on_overflow) {
    callbacks->last_line_cb = set_ellipsis_on_overflow_last_line_cb;
  } else {
    callbacks->last_line_cb = NULL;
  }

  ctx->text_draw_state.line = (Line) {
    .start = utf8_bounds->start,
    // set initial bounding values for line
    .origin = text_box->box.origin, //<! Needs to be in global co-ords!
    .max_width_px = text_box->box.size.w,
    .height_px = fonts_get_font_height(text_box->font)
  };

  Iterator line_iter;
  line_iter_init(&line_iter, &ctx->text_draw_state.line_iter_state, ctx);

  prv_walk_lines_down(&line_iter, layout, callbacks);
}

static void prv_graphics_text_layout_update(GContext* ctx, const char* text, GFont const font,
                                            const GRect box, const GTextOverflowMode overflow_mode,
                                            const GTextAlignment alignment,
                                            TextLayout* const layout) {
  PBL_ASSERTN(layout);

  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, text);
  if (!success) {
    layout->max_used_size = GSizeZero;
    PBL_LOG_DBG("Invalid UTF8");
    return;
  }

  int str_len_bytes = (utf8_bounds.end - utf8_bounds.start);
  Codepoint text_hash = hash((const uint8_t*) utf8_bounds.start, str_len_bytes);

  if (prv_text_layout_is_fresh(layout, font, box, overflow_mode, alignment, text_hash)) {
    return;
  }

  layout->max_used_size = GSizeZero;
  layout->hash = text_hash;
  layout->box = box;
  layout->overflow_mode = overflow_mode;
  layout->alignment = alignment;
  layout->font = font;

  WalkLinesCallbacks callbacks = {
    .layout_update_cb = update_all_layout_update_cb
  };

  int16_t line_spacing_delta = prv_layout_get_line_spacing_delta(layout);
  ctx->text_draw_state.text_box = (TextBoxParams) {
    .utf8_bounds = &utf8_bounds,
    .box = box,
    .font = font,
    .overflow_mode = overflow_mode,
    .alignment = alignment,
    .line_spacing_delta = line_spacing_delta,
  };

  prv_text_walk_lines(ctx, layout, &callbacks);
}

// helper macro to avoid source code duplication
// we call this instead of a true function to keep the stack as low as possible as this is
// on a critical path.
#define APP_TEXT_GET_CONTENT_SIZE(text, font, box, overflow_mode, alignment, text_attributes) \
  do { \
    GContext* ctx = app_state_get_graphics_context(); \
    return graphics_text_layout_get_max_used_size( \
      ctx, text, font, box, overflow_mode, alignment, text_attributes); \
  } while (0)

GSize app_graphics_text_layout_get_content_size_with_attributes(
  const char *text, GFont const font, const GRect box, const GTextOverflowMode overflow_mode,
  const GTextAlignment alignment, GTextAttributes *text_attributes) {
  APP_TEXT_GET_CONTENT_SIZE(text, font, box, overflow_mode, alignment, text_attributes);
}


GSize app_graphics_text_layout_get_content_size(const char *text, GFont const font, const GRect box,
                                                const GTextOverflowMode overflow_mode,
                                                const GTextAlignment alignment) {
  APP_TEXT_GET_CONTENT_SIZE(text, font, box, overflow_mode, alignment, NULL);
}

uint16_t graphics_text_layout_get_text_height(GContext *ctx, const char *text, GFont const font,
                                              uint16_t bounds_width,
                                              const GTextOverflowMode overflow_mode,
                                              const GTextAlignment alignment) {
  const int16_t LAYOUT_HEIGHT_IGNORE = SHRT_MAX;
  GRect box = {
        .origin = (GPoint) { .x = 0, .y = 0 },
        .size = (GSize) { .w = bounds_width, .h = LAYOUT_HEIGHT_IGNORE }
      };
  GSize size = graphics_text_layout_get_max_used_size(ctx, text, font,
      box, overflow_mode, alignment, NULL);
  return size.h;
}

GSize graphics_text_layout_get_max_used_size(GContext *ctx, const char *text, GFont const font,
                                             const GRect box, const GTextOverflowMode overflow_mode,
                                             const GTextAlignment alignment,
                                             GTextLayoutCacheRef const layout) {
  TextLayoutExtended stack_layout = { 0 }; // Default use extended layout
  TextLayout* text_layout = layout ? (TextLayout*) layout : (TextLayout*) &stack_layout;
  prv_graphics_text_layout_update(ctx, text, font, box, overflow_mode, alignment, text_layout);
  return text_layout->max_used_size;
}

void graphics_draw_text(GContext* ctx, const char* text, GFont const font,
                        GRect box, const GTextOverflowMode overflow_mode,
                        const GTextAlignment alignment, GTextLayoutCacheRef const layout) {
  if (ctx->lock) {
    return;
  }

  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, text);
  if (!success) {
    PBL_LOG_DBG("Invalid UTF8");
    return;
  }

  GRect global_box = grect_to_global_coordinates(box, ctx);

  GRect temp_box = global_box;
  grect_clip(&temp_box, &ctx->draw_state.clip_box);
  if (temp_box.size.h <= 0) {
    // the text is not ever going to make it on screen. Bail early.
    return;
  }


  if (layout) {
    layout->box.origin = global_box.origin;
  }

  WalkLinesCallbacks callbacks = {
    .render_line_cb = render_all_render_line_cb,
    .layout_update_cb = update_all_layout_update_cb,
    .stop_condition_cb = is_clip_box_overflow_bottom_stop_condition_cb
  };

  int16_t line_spacing_delta = prv_layout_get_line_spacing_delta(layout);
  ctx->text_draw_state.text_box = (TextBoxParams) {
    .utf8_bounds = &utf8_bounds,
    .box = global_box,
    .font = font,
    .overflow_mode = overflow_mode,
    .alignment = alignment,
    .line_spacing_delta = line_spacing_delta,
  };

  prv_text_walk_lines(ctx, layout, &callbacks);
}

void graphics_text_layout_cache_init(GTextLayoutCacheRef* layout) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    *layout = applib_type_malloc(TextLayout);
    *((TextLayout*) *layout) = (TextLayout) { 0 };
  } else {
    *layout = applib_type_malloc(TextLayoutExtended);
    *((TextLayoutExtended*) *layout) = (TextLayoutExtended) { 0 };
  }
}

void graphics_text_layout_cache_deinit(GTextLayoutCacheRef* layout) {
  TextLayout* text_layout = (TextLayout*) *layout;
  applib_free(text_layout);
  *layout = NULL;
}

GTextAttributes *graphics_text_attributes_create(void) {
  GTextAttributes *result;
  graphics_text_layout_cache_init(&result);
  return result;
}

void graphics_text_attributes_destroy(GTextAttributes *text_attributes) {
  if (!text_attributes) {
    return;
  }

  graphics_text_layout_cache_deinit(&text_attributes);
}


static TextLayoutExtended* prv_get_writable_extended_layout(GTextLayoutCacheRef layout) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk()); // should not get here if 2.X
  PBL_ASSERTN(layout);
  // Invalidate the hash to ensure the layout gets updated when prv_graphics_text_layout_update is
  // called on the layout
  layout->hash = 0;
  return (TextLayoutExtended *)layout;
}

static TextLayoutExtended* prv_get_readable_extended_layout(GTextLayoutCacheRef layout) {
  if (!layout || process_manager_compiled_with_legacy2_sdk()) {
    return NULL;
  }
  return (TextLayoutExtended*)layout;
}


void graphics_text_layout_set_line_spacing_delta(GTextLayoutCacheRef layout, int16_t delta) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (extended) {
    extended->line_spacing_delta = delta;
  }
}

int16_t graphics_text_layout_get_line_spacing_delta(const GTextLayoutCacheRef layout) {
  return prv_layout_get_line_spacing_delta(layout);
}

void graphics_text_attributes_restore_default_text_flow(GTextLayoutCacheRef layout) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (!extended) {
    return;
  }
  extended->flow_data.perimeter.impl = NULL;
}

// this way, we don't need to pull in all the dependencies when doing unit-tests
// if you want to test this aspect, just define the symbol below in your test_*.c file
#if !defined(UNITTEST)
#define USE_DISPLAY_PERIMETER_ON_FONT_LAYOUT
#endif

void graphics_text_attributes_enable_screen_text_flow(GTextLayoutCacheRef layout, uint8_t inset) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (!extended) {
    return;
  }

#if defined(USE_DISPLAY_PERIMETER_ON_FONT_LAYOUT)
  // on rectangular screens, we can just leave the perimeter blank when we don't need an inset
  const GPerimeter *shortcut_perimeter = PBL_IF_ROUND_ELSE(g_perimeter_for_display, NULL);
  const GPerimeter *perimeter = inset > 0 ? g_perimeter_for_display : shortcut_perimeter;
#else
  const GPerimeter *perimeter = NULL;
#endif

  extended->flow_data.perimeter = (TextLayoutFlowDataPerimeter) {
    .impl = perimeter,
    .inset = inset,
  };
}

void graphics_text_attributes_restore_default_paging(GTextLayoutCacheRef layout) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (!extended) {
    return;
  }
  extended->flow_data.paging.page_on_screen.size_h = 0;
}

void graphics_text_attributes_enable_paging(
  GTextLayoutCacheRef layout, GPoint content_origin_on_screen, GRect paging_on_screen) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (extended) {
    extended->flow_data.paging = (TextLayoutFlowDataPaging) {
      .origin_on_screen = content_origin_on_screen,
      .page_on_screen.origin_y = paging_on_screen.origin.y,
      .page_on_screen.size_h = paging_on_screen.size.h,
    };
  }
}

const TextLayoutFlowData *graphics_text_layout_get_flow_data(GTextLayoutCacheRef layout) {
  TextLayoutExtended *extended_layout = prv_get_readable_extended_layout(layout);
  if (extended_layout) {
    return &extended_layout->flow_data;
  } else {
    static const TextLayoutFlowData s_default_data = {
      // yes, this is basically just an empty struct but I want to be explicit here:
      .perimeter.impl = NULL, // no perimeter/inset configured
      .paging.page_on_screen.size_h = 0, // no paging or origin
    };
    return &s_default_data;
  }
}
