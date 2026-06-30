/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bidi.h"

#include <string.h>

// Bidi character classes (UAX #9). Only the classes that occur in watch text
// are distinguished; everything else falls back to ON (other neutral).
typedef enum {
  L, R, AL,             // strong
  EN, ES, ET, AN, CS,   // weak (numbers and number punctuation)
  NSM, BN,              // marks and boundary neutrals
  ON, WS, B, S,         // neutrals and separators
} BidiClass;

static bool prv_in(Codepoint cp, Codepoint lo, Codepoint hi) {
  return cp >= lo && cp <= hi;
}

// Assign a bidi class to a codepoint (the subset relevant to watch text).
static BidiClass prv_class(Codepoint cp) {
  if (cp == 0x000A || cp == 0x000D || cp == 0x0085 || cp == 0x2029) return B;
  if (cp == 0x0009 || cp == 0x000B || cp == 0x001F) return S;
  if (cp == 0x0020) return WS;

  // European numbers and number punctuation.
  if (prv_in(cp, 0x0030, 0x0039)) return EN;
  if (cp == 0x002B || cp == 0x002D) return ES;
  if (cp == 0x0023 || cp == 0x0024 || cp == 0x0025 ||
      prv_in(cp, 0x00A2, 0x00A5) || cp == 0x00B0 || cp == 0x00B1) return ET;
  if (cp == 0x002C || cp == 0x002E || cp == 0x002F || cp == 0x003A) return CS;

  // Arabic numbers and number signs.
  if (prv_in(cp, 0x0660, 0x0669) || prv_in(cp, 0x06F0, 0x06F9) ||
      prv_in(cp, 0x066B, 0x066C) || prv_in(cp, 0x0600, 0x0605) || cp == 0x06DD) return AN;

  // Non-spacing marks (Arabic harakat, Hebrew points, generic combining).
  if (prv_in(cp, 0x0610, 0x061A) || prv_in(cp, 0x064B, 0x065F) || cp == 0x0670 ||
      prv_in(cp, 0x06D6, 0x06DC) || prv_in(cp, 0x06DF, 0x06E4) ||
      prv_in(cp, 0x06E7, 0x06E8) || prv_in(cp, 0x06EA, 0x06ED) ||
      prv_in(cp, 0x0591, 0x05BD) || cp == 0x05BF || cp == 0x05C1 || cp == 0x05C2 ||
      cp == 0x05C4 || cp == 0x05C5 || cp == 0x05C7 || prv_in(cp, 0x0300, 0x036F)) return NSM;

  // Zero-width and other boundary neutrals.
  if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
      prv_in(cp, 0x0000, 0x0008) || prv_in(cp, 0x000E, 0x001B)) return BN;

  // Strong RTL: Hebrew (R) and Arabic (AL).
  if (prv_in(cp, 0x0590, 0x05FF) || prv_in(cp, 0x07C0, 0x07FF)) return R;
  if (prv_in(cp, 0x0600, 0x06FF) || prv_in(cp, 0x0750, 0x077F) ||
      prv_in(cp, 0x08A0, 0x08FF) || prv_in(cp, 0xFB50, 0xFDFF) ||
      prv_in(cp, 0xFE70, 0xFEFF)) return AL;

  // Strong LTR: Latin, Greek, Cyrillic, and the major LTR CJK/Kana/Hangul blocks.
  if (prv_in(cp, 0x0041, 0x005A) || prv_in(cp, 0x0061, 0x007A) ||
      prv_in(cp, 0x00C0, 0x02AF) || prv_in(cp, 0x0370, 0x04FF) ||
      prv_in(cp, 0x3040, 0x30FF) || prv_in(cp, 0x3400, 0x9FFF) ||
      prv_in(cp, 0xAC00, 0xD7AF) || prv_in(cp, 0xF900, 0xFAFF)) return L;

  // Brackets, punctuation and symbols are neutral.
  return ON;
}

Codepoint bidi_mirror(Codepoint cp) {
  switch (cp) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB;  // « -> »
    case 0x00BB: return 0x00AB;  // » -> «
    case 0x2039: return 0x203A;  // ‹ -> ›
    case 0x203A: return 0x2039;  // › -> ‹
    default: return cp;
  }
}

// Canonical closing bracket for an opening one, or 0 if not a paired bracket.
// Only the Bidi_Paired_Bracket set (round, square, curly) takes part in N0;
// angle brackets and guillemets are mirrored (L4) but are not paired brackets.
static Codepoint prv_paired_close(Codepoint cp) {
  switch (cp) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    default: return 0;
  }
}

int bidi_base_level(const Codepoint *cps, size_t n) {
  for (size_t i = 0; i < n; i++) {
    BidiClass c = prv_class(cps[i]);
    if (c == L) return 0;
    if (c == R || c == AL) return 1;
  }
  return 0;
}

// Map a resolved type to its strong direction for neutral/bracket resolution,
// where European and Arabic numbers count as R.
static BidiClass prv_dir_of(BidiClass t) {
  return (t == R || t == EN || t == AN) ? R : L;
}

size_t bidi_reorder_line(const Codepoint *cps, size_t n, int base_level, Codepoint *out) {
  if (n == 0) return 0;
  if (n > BIDI_MAX_CODEPOINTS) n = BIDI_MAX_CODEPOINTS;

  BidiClass type[BIDI_MAX_CODEPOINTS];
  BidiClass orig[BIDI_MAX_CODEPOINTS];
  uint8_t level[BIDI_MAX_CODEPOINTS];

  const BidiClass sos = (base_level & 1) ? R : L;  // boundary strong type
  const BidiClass e = sos;                         // embedding direction

  for (size_t i = 0; i < n; i++) {
    orig[i] = type[i] = prv_class(cps[i]);
    level[i] = (uint8_t)base_level;
  }

  // W1: a non-spacing mark (and boundary neutral) takes the type of the
  // preceding character, or the boundary type at the start.
  BidiClass prev = sos;
  for (size_t i = 0; i < n; i++) {
    if (type[i] == NSM || type[i] == BN) {
      type[i] = prev;
    } else {
      prev = type[i];
    }
  }

  // W2: European number becomes Arabic number if the last strong type is AL.
  BidiClass strong = sos;
  for (size_t i = 0; i < n; i++) {
    if (type[i] == L || type[i] == R || type[i] == AL) strong = type[i];
    else if (type[i] == EN && strong == AL) type[i] = AN;
  }

  // W3: AL becomes R.
  for (size_t i = 0; i < n; i++) {
    if (type[i] == AL) type[i] = R;
  }

  // W4: a single ES between two EN, or a single CS between two numbers of the
  // same kind, joins them.
  for (size_t i = 1; i + 1 < n; i++) {
    if (type[i] == ES && type[i - 1] == EN && type[i + 1] == EN) type[i] = EN;
    else if (type[i] == CS && type[i - 1] == EN && type[i + 1] == EN) type[i] = EN;
    else if (type[i] == CS && type[i - 1] == AN && type[i + 1] == AN) type[i] = AN;
  }

  // W5: a run of ET adjacent to EN becomes EN.
  for (size_t i = 0; i < n; i++) {
    if (type[i] != ET) continue;
    size_t j = i;
    while (j < n && type[j] == ET) j++;
    bool en_before = (i > 0 && type[i - 1] == EN);
    bool en_after = (j < n && type[j] == EN);
    if (en_before || en_after) {
      for (size_t k = i; k < j; k++) type[k] = EN;
    }
    i = j - 1;
  }

  // W6: remaining number punctuation becomes neutral.
  for (size_t i = 0; i < n; i++) {
    if (type[i] == ES || type[i] == ET || type[i] == CS) type[i] = ON;
  }

  // W7: European number becomes L if the last strong type is L.
  strong = sos;
  for (size_t i = 0; i < n; i++) {
    if (type[i] == L || type[i] == R) strong = type[i];
    else if (type[i] == EN && strong == L) type[i] = L;
  }

  // N0: resolve paired brackets to a single direction so a parenthesised island
  // (e.g. "(english)" inside Arabic) stays together. Stack of open positions.
  {
    size_t open_pos[BIDI_MAX_CODEPOINTS];
    Codepoint open_close[BIDI_MAX_CODEPOINTS];
    int sp = 0;
    for (size_t i = 0; i < n; i++) {
      if (type[i] != ON) continue;
      Codepoint close = prv_paired_close(cps[i]);
      if (close != 0) {
        if (sp < (int)BIDI_MAX_CODEPOINTS) {
          open_pos[sp] = i;
          open_close[sp] = close;
          sp++;
        }
      } else if (cps[i] == ')' || cps[i] == ']' || cps[i] == '}') {
        for (int s = sp - 1; s >= 0; s--) {
          if (open_close[s] == cps[i]) {
            size_t o = open_pos[s];
            sp = s;  // pop this and everything above (BD16)
            // Strong directions strictly inside the pair.
            bool found_e = false, found_o = false;
            for (size_t k = o + 1; k < i; k++) {
              if (type[k] == L || type[k] == R || type[k] == EN || type[k] == AN) {
                if (prv_dir_of(type[k]) == e) found_e = true;
                else found_o = true;
              }
            }
            BidiClass set = ON;
            if (found_e) {
              set = e;
            } else if (found_o) {
              // Opposite direction inside: use it only if the context before the
              // opening bracket is also opposite, else the embedding direction.
              BidiClass before = sos;
              for (size_t k = o; k > 0; k--) {
                BidiClass t = type[k - 1];
                if (t == L || t == R || t == EN || t == AN) { before = prv_dir_of(t); break; }
              }
              set = (before != e) ? before : e;
            }
            if (set != ON) {
              type[o] = set;
              type[i] = set;
            }
            break;
          }
        }
      }
    }
  }

  // N1/N2: a run of neutrals takes the surrounding direction if both sides
  // agree, otherwise the embedding direction.
  for (size_t i = 0; i < n; i++) {
    if (!(type[i] == ON || type[i] == WS || type[i] == B || type[i] == S)) continue;
    size_t j = i;
    while (j < n && (type[j] == ON || type[j] == WS || type[j] == B || type[j] == S)) j++;
    BidiClass left = (i > 0) ? prv_dir_of(type[i - 1]) : sos;
    BidiClass right = (j < n) ? prv_dir_of(type[j]) : sos;
    BidiClass set = (left == right) ? left : e;
    for (size_t k = i; k < j; k++) type[k] = set;
    i = j - 1;
  }

  // I1/I2: implicit levels.
  for (size_t i = 0; i < n; i++) {
    uint8_t lv = (uint8_t)base_level;
    if ((base_level & 1) == 0) {  // even (LTR) base
      if (type[i] == R) lv = base_level + 1;
      else if (type[i] == EN || type[i] == AN) lv = base_level + 2;
    } else {  // odd (RTL) base
      if (type[i] == L || type[i] == EN || type[i] == AN) lv = base_level + 1;
    }
    level[i] = lv;
  }

  // L1: reset separators and trailing whitespace to the base level.
  for (size_t i = 0; i < n; i++) {
    if (orig[i] == B || orig[i] == S) {
      level[i] = (uint8_t)base_level;
      for (size_t k = i; k > 0; k--) {
        if (orig[k - 1] == WS || orig[k - 1] == BN) level[k - 1] = (uint8_t)base_level;
        else break;
      }
    }
  }
  for (size_t k = n; k > 0; k--) {
    if (orig[k - 1] == WS || orig[k - 1] == BN) level[k - 1] = (uint8_t)base_level;
    else break;
  }

  // L2: from the highest level down to the lowest odd level, reverse any
  // contiguous run of positions at that level or higher.
  uint16_t vis[BIDI_MAX_CODEPOINTS];
  for (size_t i = 0; i < n; i++) vis[i] = (uint16_t)i;
  uint8_t max_level = 0, min_odd = 255;
  for (size_t i = 0; i < n; i++) {
    if (level[i] > max_level) max_level = level[i];
    if ((level[i] & 1) && level[i] < min_odd) min_odd = level[i];
  }
  for (int lv = max_level; lv >= (int)min_odd && lv >= 1; lv--) {
    size_t v = 0;
    while (v < n) {
      if (level[vis[v]] >= lv) {
        size_t v2 = v;
        while (v2 < n && level[vis[v2]] >= lv) v2++;
        for (size_t a = v, b = v2 - 1; a < b; a++, b--) {
          uint16_t tmp = vis[a]; vis[a] = vis[b]; vis[b] = tmp;
        }
        v = v2;
      } else {
        v++;
      }
    }
  }

  // L4: emit visual order, mirroring glyphs that resolved to an odd level.
  for (size_t v = 0; v < n; v++) {
    uint16_t li = vis[v];
    Codepoint c = cps[li];
    if (level[li] & 1) c = bidi_mirror(c);
    out[v] = c;
  }
  return n;
}

size_t bidi_reorder_utf8(const utf8_t *src, size_t src_len, utf8_t *dest, size_t dest_size,
                         int base_level) {
  if (src == NULL || dest == NULL || src_len == 0 || dest_size == 0) return 0;

  Codepoint cps[BIDI_MAX_CODEPOINTS];
  size_t n = 0;
  utf8_t *ptr = (utf8_t *)src;
  const utf8_t *end = src + src_len;
  while (ptr < end && *ptr != '\0' && n < BIDI_MAX_CODEPOINTS) {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) break;
    cps[n++] = cp;
    ptr = next;
  }

  Codepoint visual[BIDI_MAX_CODEPOINTS];
  size_t vn = bidi_reorder_line(cps, n, base_level, visual);

  size_t off = 0;
  for (size_t i = 0; i < vn; i++) {
    if (off + 4 >= dest_size) break;
    size_t w = utf8_encode_codepoint(visual[i], dest + off);
    if (w == 0) continue;
    off += w;
  }
  if (off < dest_size) dest[off] = '\0';
  return off;
}
