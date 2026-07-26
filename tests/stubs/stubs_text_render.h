/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/fonts/fonts_private.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text_resources.h"

#include <stdbool.h>
#include <inttypes.h>

static void (*s_render_glyph_callback)(uint32_t codepoint, GRect cursor);

void render_glyph(GContext* ctx, uint32_t codepoint, FontInfo* font, GRect cursor) {
  if (s_render_glyph_callback) {
    s_render_glyph_callback(codepoint, cursor);
  }
}
