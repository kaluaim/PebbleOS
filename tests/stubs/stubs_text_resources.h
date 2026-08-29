/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once 

#include "applib/fonts/fonts_private.h"
#include "applib/graphics/text_resources.h"
#include "applib/fonts/codepoint.h"

#include <inttypes.h>
#include <stdbool.h>

#define HORIZ_ADVANCE_PX (2)

static int8_t (*s_text_resources_horiz_advance_override)(Codepoint codepoint);

bool text_resources_setup_font(FontCache* font_cache, FontInfo* fontinfo) {
  return true;
}

int8_t text_resources_get_glyph_horiz_advance(FontCache* font_cache, Codepoint codepoint, FontInfo* fontinfo) {
  if (codepoint_is_zero_width(codepoint)) {
    return 0;
  }
  if (s_text_resources_horiz_advance_override) {
    const int8_t advance = s_text_resources_horiz_advance_override(codepoint);
    if (advance >= 0) {
      return advance;
    }
  }
  // Real fonts have some weird values here, give something totally bogus for testing.
  if (codepoint == '\n') {
    return 5;
  }
  return HORIZ_ADVANCE_PX;
}

int8_t text_resources_get_glyph_height(FontCache* font_cache, Codepoint codepoint, FontInfo* fontinfo) {
  return 10;
}

const GlyphData *text_resources_get_glyph(FontCache* font_cache, Codepoint codepoint,
                                          FontInfo* fontinfo, int16_t *baseline_adjust_out) {
  if (baseline_adjust_out) {
    *baseline_adjust_out = 0;
  }
  return NULL;
}
