/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bidi.h"

#include <string.h>

// Bidi character classes (UAX 9). Only the classes that occur in watch text
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
  if (cp == 0x000A || cp == 0x000D || cp == 0x0085 || cp == 0x2029 ||
      prv_in(cp, 0x001C, 0x001E)) return B;
  if (cp == 0x0009 || cp == 0x000B || cp == 0x001F) return S;
  if (cp == 0x0020 || cp == 0x000C || cp == 0x2028 || cp == 0x1680 ||
      prv_in(cp, 0x2000, 0x200A) || cp == 0x205F || cp == 0x3000) return WS;

  // Directional marks: zero-width but strong.
  if (cp == 0x200E) return L;   // LEFT-TO-RIGHT MARK
  if (cp == 0x200F) return R;   // RIGHT-TO-LEFT MARK

  // Latin-1 letters outside the contiguous letter ranges, and superscript
  // digits (EN per the UCD).
  if (cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA) return L;  // ª µ º
  if (cp == 0x00B2 || cp == 0x00B3 || cp == 0x00B9) return EN;  // ² ³ ¹

  // Regional indicators are strong L per the UCD. They pass through the
  // reorder as a same-level pair (L never lands on an odd level alone between
  // its partner), so the draw pass can fold a pair into one flag glyph after
  // reordering. The literal flag emoji itself stays ON like other symbols.
  if (prv_in(cp, 0x1F1E6, 0x1F1FF)) return L;

  // European numbers and number punctuation. Extended Arabic-Indic (Persian)
  // digits and fullwidth digits are EN per the UCD; inside Arabic text W2
  // turns them into AN.
  if (prv_in(cp, 0x0030, 0x0039) || prv_in(cp, 0x06F0, 0x06F9) ||
      prv_in(cp, 0xFF10, 0xFF19)) return EN;
  if (cp == 0x002B || cp == 0x002D || cp == 0x2212 ||
      cp == 0xFF0B || cp == 0xFF0D) return ES;
  if (cp == 0x0023 || cp == 0x0024 || cp == 0x0025 ||
      prv_in(cp, 0x00A2, 0x00A5) || cp == 0x00B0 || cp == 0x00B1 ||
      cp == 0x0609 || cp == 0x060A || cp == 0x066A ||
      cp == 0x20AA || cp == 0x20AC || prv_in(cp, 0x2030, 0x2034) ||
      prv_in(cp, 0xFF03, 0xFF05) || prv_in(cp, 0xFFE0, 0xFFE1) ||
      prv_in(cp, 0xFFE5, 0xFFE6)) return ET;
  if (cp == 0x002C || cp == 0x002E || cp == 0x002F || cp == 0x003A ||
      cp == 0x00A0 || cp == 0x060C || cp == 0x202F ||
      cp == 0xFF0C || cp == 0xFF0E || cp == 0xFF0F || cp == 0xFF1A) return CS;

  // Arabic numbers and number signs.
  if (prv_in(cp, 0x0660, 0x0669) || cp == 0x08E2 ||
      prv_in(cp, 0x066B, 0x066C) || prv_in(cp, 0x0600, 0x0605) || cp == 0x06DD) return AN;

  // Non-spacing marks (Arabic harakat, Hebrew points, generic combining).
  if (prv_in(cp, 0x0610, 0x061A) || prv_in(cp, 0x064B, 0x065F) || cp == 0x0670 ||
      prv_in(cp, 0x06D6, 0x06DC) || prv_in(cp, 0x06DF, 0x06E4) ||
      prv_in(cp, 0x06E7, 0x06E8) || prv_in(cp, 0x06EA, 0x06ED) ||
      prv_in(cp, 0x0591, 0x05BD) || cp == 0x05BF || cp == 0x05C1 || cp == 0x05C2 ||
      cp == 0x05C4 || cp == 0x05C5 || cp == 0x05C7 || cp == 0xFB1E ||
      prv_in(cp, 0x08D3, 0x08FF) || prv_in(cp, 0xFE00, 0xFE0F) ||
      prv_in(cp, 0x20D0, 0x20FF) ||  // combining marks for symbols (incl. keycap)
      prv_in(cp, 0x0483, 0x0489) ||  // Cyrillic combining marks (before the L range)
      prv_in(cp, 0x07A6, 0x07B0) ||  // Thaana vowel marks (before the AL range)
      prv_in(cp, 0x0300, 0x036F)) return NSM;

  // Arabic-block signs that are neutral (ON) in the UCD, not letters: roots,
  // poetic/verse marks, rub-el-hizb and place-of-sajdah. Listed before the
  // broad Arabic AL range below so they resolve as neutrals.
  if (cp == 0x0606 || cp == 0x0607 || cp == 0x060E || cp == 0x060F ||
      cp == 0x06DE || cp == 0x06E9 || cp == 0xFD3E || cp == 0xFD3F) return ON;

  // Zero-width and other boundary neutrals. The isolate controls (FSI/RLI/
  // LRI/PDI) are unimplemented; treating them as transparent BN is closer to
  // ignoring them than letting them join neutral runs as ON.
  if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF || prv_in(cp, 0x2060, 0x206F) ||
      prv_in(cp, 0x202A, 0x202E) || prv_in(cp, 0x0000, 0x0008) || prv_in(cp, 0x000E, 0x001B) ||
      cp == 0x007F || prv_in(cp, 0x0080, 0x0084) || prv_in(cp, 0x0086, 0x009F))
    return BN;

  // Strong RTL: Hebrew including presentation forms, and NKo (R); Arabic
  // including supplements and presentation forms (AL).
  if (prv_in(cp, 0x0590, 0x05FF) || prv_in(cp, 0x07C0, 0x07FF) ||
      prv_in(cp, 0xFB1D, 0xFB4F)) return R;
  if (prv_in(cp, 0x0600, 0x06FF) || prv_in(cp, 0x0750, 0x077F) ||
      prv_in(cp, 0x0780, 0x07BF) ||  // Thaana
      prv_in(cp, 0x08A0, 0x08FF) || prv_in(cp, 0xFB50, 0xFDFF) ||
      prv_in(cp, 0xFE70, 0xFEFF)) return AL;

  // Signs inside the letter ranges that the UCD classes ON: multiplication
  // and division, the Greek numeral/accent signs, the Greek question mark and
  // ano teleia.
  if (cp == 0x00D7 || cp == 0x00F7 || cp == 0x0374 || cp == 0x037E ||
      cp == 0x0384 || cp == 0x0385 || cp == 0x0387) return ON;

  // Strong LTR: Latin (including fullwidth forms), Greek, Cyrillic, and the
  // major LTR CJK/Kana/Hangul blocks.
  if (prv_in(cp, 0x0041, 0x005A) || prv_in(cp, 0x0061, 0x007A) ||
      prv_in(cp, 0x00C0, 0x02AF) || prv_in(cp, 0x0370, 0x04FF) ||
      prv_in(cp, 0x3040, 0x30FF) || prv_in(cp, 0x3400, 0x9FFF) ||
      prv_in(cp, 0xAC00, 0xD7AF) || prv_in(cp, 0xF900, 0xFAFF) ||
      prv_in(cp, 0x3220, 0x3229) ||  // parenthesized ideograph list markers
      prv_in(cp, 0xFF21, 0xFF3A) || prv_in(cp, 0xFF41, 0xFF5A)) return L;

  // Brackets, punctuation and symbols are neutral.
  return ON;
}

static Codepoint prv_paired_close(Codepoint cp);
static Codepoint prv_paired_open(Codepoint cp);

Codepoint bidi_mirror(Codepoint cp) {
  // Every paired bracket mirrors to its partner, derived from the pair tables
  // so the pairing and mirroring sets cannot drift apart.
  Codepoint other = prv_paired_close(cp);
  if (other != 0) return other;
  other = prv_paired_open(cp);
  if (other != 0) return other;
  // Mirrored codepoints that are not paired brackets.
  switch (cp) {
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB;  // « -> »
    case 0x00BB: return 0x00AB;  // » -> «
    case 0x2039: return 0x203A;  // ‹ -> ›
    case 0x203A: return 0x2039;  // › -> ‹
    case 0x2264: return 0x2265;  // <= -> >=
    case 0x2265: return 0x2264;
    case 0x2329: return 0x232A;  // deprecated angle brackets (literal mirror)
    case 0x232A: return 0x2329;
    case 0x226E: return 0x226F;  // not-less-than <-> not-greater-than
    case 0x226F: return 0x226E;
    case 0xFF1C: return 0xFF1E;  // fullwidth < -> >
    case 0xFF1E: return 0xFF1C;
    default: return cp;
  }
}

// BD16 matches bracket pairs by canonical equivalence: the deprecated angle
// brackets pair with their CJK canonical equivalents in either combination.
// Only pairing uses this; L4 mirrors the literal codepoint.
static Codepoint prv_bracket_canonical(Codepoint cp) {
  if (cp == 0x2329) return 0x3008;
  if (cp == 0x232A) return 0x3009;
  return cp;
}

// Canonical closing bracket for an opening one, or 0 if not a paired bracket.
// The Bidi_Paired_Bracket set that occurs in watch text: round, square and
// curly brackets, plus the CJK angle brackets (in CJK notification fonts).
// ASCII '<'/'>' and the guillemets are mirrored (L4) but are not paired
// brackets, so they are not listed here.
static Codepoint prv_paired_close(Codepoint cp) {
  switch (cp) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case 0x3008: return 0x3009;  // 〈 〉 (and canonical U+2329 via canonicalization)
    case 0x300A: return 0x300B;  // 《 》
    case 0x300C: return 0x300D;  // 「 」
    case 0x3010: return 0x3011;  // 【 】
    case 0x3014: return 0x3015;  // 〔 〕
    case 0x3016: return 0x3017;  // 〖 〗
    case 0xFF08: return 0xFF09;  // fullwidth ( )
    case 0xFF3B: return 0xFF3D;  // fullwidth [ ]
    case 0xFF5B: return 0xFF5D;  // fullwidth { }
    case 0xFF62: return 0xFF63;  // halfwidth corner brackets
    default: return 0;
  }
}

// Opening bracket for a closing one - the inverse of prv_paired_close().
static Codepoint prv_paired_open(Codepoint cp) {
  switch (cp) {
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
    case 0x3009: return 0x3008;
    case 0x300B: return 0x300A;
    case 0x300D: return 0x300C;
    case 0x3011: return 0x3010;
    case 0x3015: return 0x3014;
    case 0x3017: return 0x3016;
    case 0xFF09: return 0xFF08;
    case 0xFF3D: return 0xFF3B;
    case 0xFF5D: return 0xFF5B;
    case 0xFF63: return 0xFF62;
    default: return 0;
  }
}

// True for the closing half of any pair prv_paired_close() knows.
static bool prv_is_paired_close(Codepoint cp) {
  return prv_paired_open(cp) != 0;
}

int bidi_base_level(const Codepoint *cps, size_t n) {
  for (size_t i = 0; i < n; i++) {
    BidiClass c = prv_class(cps[i]);
    if (c == L) return 0;
    if (c == R || c == AL) return 1;
  }
  return 0;
}

int bidi_base_level_utf8(const utf8_t *start, const utf8_t *end) {
  if (start == NULL || end == NULL || start >= end) {
    return 0;
  }
  bool saw_arabic_number = false;
  utf8_t *ptr = (utf8_t *)start;
  while (ptr < end && *ptr != '\0') {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) {
      break;
    }
    BidiClass c = prv_class(cp);
    if (c == L) return 0;
    if (c == R || c == AL) return 1;
    if (c == AN || prv_in(cp, 0x06F0, 0x06F9)) saw_arabic_number = true;
    ptr = next;
  }
  // Deviation from P3: Arabic-script numbers (Arabic-Indic or Extended
  // Arabic-Indic digits, Arabic number signs) with no strong character read
  // RTL, so digit-only text in an Arabic notification stays right-aligned.
  return saw_arabic_number ? 1 : 0;
}

bool bidi_contains_rtl(const utf8_t *start, const utf8_t *end) {
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
    BidiClass c = prv_class(cp);
    if (c == R || c == AL) {
      return true;
    }
    ptr = next;
  }
  return false;
}

int bidi_last_strong_utf8(const utf8_t *start, const utf8_t *end) {
  int found = BIDI_BOUNDARY_AUTO;
  if (start == NULL || end == NULL || start >= end) return found;
  utf8_t *ptr = (utf8_t *)start;
  while (ptr < end && *ptr != '\0') {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) break;
    BidiClass c = prv_class(cp);
    if (c == L) found = BIDI_BOUNDARY_L;
    else if (c == R) found = BIDI_BOUNDARY_R;
    else if (c == AL) found = BIDI_BOUNDARY_AL;
    ptr = next;
  }
  return found;
}

int bidi_boundary_ndir_utf8(const utf8_t *start, const utf8_t *end, int base_level,
                            BidiScratch *ws) {
  if (start == NULL || end == NULL || start >= end || ws == NULL) return BIDI_BOUNDARY_AUTO;
  // A raw class scan cannot represent N0: a bracket pair resolved to L leaves
  // L at the boundary even though a strong R sits inside it. Run the resolver
  // over the paragraph prefix (its own sos IS the base direction, since the
  // prefix starts the paragraph) and read the direction the last character
  // resolved to. A prefix past the resolver's capacity has no answer here:
  // W2/W7 and N0 can depend on a strong type or an opening bracket
  // arbitrarily far back, so a trailing window would only guess. Report AUTO
  // and let the caller fall back.
  size_t total = 0;
  utf8_t *ptr = (utf8_t *)start;
  while (ptr < end && *ptr != '\0') {
    utf8_t *next = NULL;
    if (utf8_peek_codepoint(ptr, &next) == 0 || next == NULL) break;
    total++;
    ptr = next;
  }
  if (total == 0 || total > BIDI_MAX_CODEPOINTS) return BIDI_BOUNDARY_AUTO;
  size_t n = 0;
  ptr = (utf8_t *)start;
  while (ptr < end && *ptr != '\0' && n < BIDI_MAX_CODEPOINTS) {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) break;
    ws->cps[n++] = cp;
    ptr = next;
  }
  if (bidi_reorder_line_ctx(ws->cps, n, base_level, BIDI_BOUNDARY_AUTO, BIDI_BOUNDARY_AUTO,
                            BIDI_BOUNDARY_AUTO, ws->visual, ws) == 0) {
    return BIDI_BOUNDARY_AUTO;
  }
  for (size_t k = n; k > 0; k--) {
    uint8_t t = ws->type[k - 1];
    if (t == BN) continue;
    return (t == R || t == EN || t == AN) ? BIDI_BOUNDARY_R : BIDI_BOUNDARY_L;
  }
  return BIDI_BOUNDARY_AUTO;
}

int bidi_first_strong_utf8(const utf8_t *start, const utf8_t *end, int prev_strong) {
  if (start == NULL || end == NULL || start >= end) return BIDI_BOUNDARY_AUTO;
  utf8_t *ptr = (utf8_t *)start;
  while (ptr < end && *ptr != '\0') {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) break;
    BidiClass c = prv_class(cp);
    if (c == L) return BIDI_BOUNDARY_L;
    if (c == R || c == AL) return BIDI_BOUNDARY_R;
    // Numbers count for the N rules: AN is always R context; EN resolves per
    // W2/W7 from the strong type before it (the caller's last strong, since
    // no strong type precedes it within this range).
    if (c == AN) return BIDI_BOUNDARY_R;
    if (c == EN) {
      if (prev_strong == BIDI_BOUNDARY_AL) return BIDI_BOUNDARY_R;   // W2: AN
      if (prev_strong == BIDI_BOUNDARY_L) return BIDI_BOUNDARY_L;    // W7: L
      return BIDI_BOUNDARY_R;
    }
    ptr = next;
  }
  return BIDI_BOUNDARY_AUTO;
}

// Map a resolved type to its strong direction for neutral/bracket resolution,
// where European and Arabic numbers count as R.
static BidiClass prv_dir_of(BidiClass t) {
  return (t == R || t == EN || t == AN) ? R : L;
}

size_t bidi_reorder_line(const Codepoint *cps, size_t n, int base_level, Codepoint *out,
                         BidiScratch *ws) {
  return bidi_reorder_line_ctx(cps, n, base_level, BIDI_BOUNDARY_AUTO, BIDI_BOUNDARY_AUTO,
                               BIDI_BOUNDARY_AUTO, out, ws);
}

// Resolution phase (classes, W1-W7, N0-N2, I1/I2, BN levels) - fills
// ws->orig, ws->type and ws->level for cps[0..n).
static void prv_resolve(const Codepoint *cps, size_t n, int base_level, int sos_hint,
                        int sos_n_hint, int eos_hint, BidiScratch *ws) {
  uint8_t *const type = ws->type;
  uint8_t *const orig = ws->orig;
  uint8_t *const level = ws->level;

  // Boundary strong types: what the text just outside this line resolved to.
  // For a full paragraph both default to the base direction (UAX 9 X10); for a
  // wrapped display line the caller passes the adjacent strong context, so
  // weak and neutral runs straddling the soft wrap resolve as they would have
  // in the whole paragraph.
  const BidiClass auto_b = (base_level & 1) ? R : L;
  const BidiClass sos =
      (sos_hint == BIDI_BOUNDARY_L) ? L :
      (sos_hint == BIDI_BOUNDARY_R) ? R :
      (sos_hint == BIDI_BOUNDARY_AL) ? AL : auto_b;
  const BidiClass eos =
      (eos_hint == BIDI_BOUNDARY_L) ? L :
      (eos_hint == BIDI_BOUNDARY_R) ? R : auto_b;
  // The neutral-rule left boundary: the resolved direction adjacent to this
  // slice (numbers count as R/L per W2/W7), which can differ from the last
  // strong type the weak rules need (e.g. "a .. 1 | , X": W7 context is L,
  // but the digit supplies R to the neutral run across the wrap).
  const BidiClass sos_n =
      (sos_n_hint == BIDI_BOUNDARY_L) ? L :
      (sos_n_hint == BIDI_BOUNDARY_R) ? R : ((sos == AL) ? R : sos);
  const BidiClass e = auto_b;                      // embedding direction

  for (size_t i = 0; i < n; i++) {
    orig[i] = type[i] = (uint8_t)prv_class(cps[i]);
    level[i] = (uint8_t)base_level;
  }

  // W1: a non-spacing mark takes the type of the preceding character (looking
  // through boundary neutrals), or the boundary type at the start. BN itself
  // stays BN: X9-retained characters are transparent to the weak rules
  // (UAX 9 5.2) and get their level from the preceding character later.
  BidiClass prev = sos;
  for (size_t i = 0; i < n; i++) {
    if (type[i] == NSM) {
      type[i] = (uint8_t)prev;
    } else if (type[i] != BN) {
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
  // same kind, joins them. Neighbors are the nearest non-BN characters.
  for (size_t i = 1; i + 1 < n; i++) {
    if (type[i] != ES && type[i] != CS) continue;
    size_t a = i;
    while (a > 0 && type[a - 1] == BN) a--;
    if (a == 0) continue;
    size_t b = i + 1;
    while (b < n && type[b] == BN) b++;
    if (b == n) continue;
    BidiClass before = type[a - 1], after = type[b];
    if (type[i] == ES && before == EN && after == EN) type[i] = EN;
    else if (type[i] == CS && before == EN && after == EN) type[i] = EN;
    else if (type[i] == CS && before == AN && after == AN) type[i] = AN;
  }

  // W5: a run of ET adjacent to EN becomes EN, treating BN as transparent on
  // both sides and within the run.
  for (size_t i = 0; i < n; i++) {
    if (type[i] != ET) continue;
    size_t j = i;
    while (j < n && (type[j] == ET || type[j] == BN)) j++;
    size_t a = i;
    while (a > 0 && type[a - 1] == BN) a--;
    bool en_before = (a > 0 && type[a - 1] == EN);
    bool en_after = (j < n && type[j] == EN);
    if (en_before || en_after) {
      for (size_t k = i; k < j; k++) {
        if (type[k] == ET) type[k] = EN;
      }
    }
    i = j - 1;
  }

  // W6: remaining number punctuation becomes neutral.
  for (size_t i = 0; i < n; i++) {
    if (type[i] == ES || type[i] == ET || type[i] == CS) type[i] = ON;
  }

  // W7: European number becomes L if the last strong type is L.
  strong = (sos == AL) ? R : sos;
  for (size_t i = 0; i < n; i++) {
    if (type[i] == L || type[i] == R) strong = type[i];
    else if (type[i] == EN && strong == L) type[i] = L;
  }

  // N0: resolve paired brackets (UAX 9 BD16/N0). Match pairs with a stack, then
  // resolve them in order of their opening bracket - outer before inner - so a
  // parenthesised island stays together and nested pairs resolve outward. (The
  // stack alone would resolve on the closing bracket, i.e. inner first, which
  // disagrees with BD16 for nested pairs.)
  {
    // BD16 specifies a 63-entry stack with overflow aborting bracket
    // processing; this stack holds every possible opener instead and simply
    // stops pushing when full, which can only differ on lines of 60+ nested
    // brackets - unreachable in watch text.
    int sp = 0;   // bracket stack depth
    int np = 0;   // matched pairs collected
    for (size_t i = 0; i < n; i++) {
      if (type[i] != ON) continue;
      if (prv_paired_close(prv_bracket_canonical(cps[i])) != 0) {
        if (sp < (int)BIDI_MAX_CODEPOINTS) {
          ws->open_pos[sp] = (uint8_t)i;
          sp++;
        }
      } else if (prv_is_paired_close(prv_bracket_canonical(cps[i]))) {
        for (int s = sp - 1; s >= 0; s--) {
          // Match by deriving the opener's canonical closer, so a bracket
          // codepoint above 0xFF is compared in full and the deprecated angle
          // brackets pair with their canonical equivalents.
          if (prv_paired_close(prv_bracket_canonical(cps[ws->open_pos[s]])) ==
              prv_bracket_canonical(cps[i])) {
            ws->pair_open[np] = ws->open_pos[s];
            ws->pair_close[np] = (uint8_t)i;
            np++;
            sp = s;  // pop this and everything above (BD16)
            break;
          }
        }
      }
    }
    // Insertion-sort the pairs by opening position (n is tiny).
    for (int a = 1; a < np; a++) {
      uint8_t po = ws->pair_open[a], pc = ws->pair_close[a];
      int b = a - 1;
      while (b >= 0 && ws->pair_open[b] > po) {
        ws->pair_open[b + 1] = ws->pair_open[b];
        ws->pair_close[b + 1] = ws->pair_close[b];
        b--;
      }
      ws->pair_open[b + 1] = po;
      ws->pair_close[b + 1] = pc;
    }
    // Resolve each pair outer-first.
    for (int pidx = 0; pidx < np; pidx++) {
      size_t o = ws->pair_open[pidx];
      size_t i = ws->pair_close[pidx];
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
        BidiClass before = sos_n;
        for (size_t k = o; k > 0; k--) {
          BidiClass t = type[k - 1];
          if (t == L || t == R || t == EN || t == AN) { before = prv_dir_of(t); break; }
        }
        set = (before != e) ? before : e;
      }
      if (set != ON) {
        type[o] = (uint8_t)set;
        type[i] = (uint8_t)set;
        // Characters that were NSM before W1 and immediately follow a bracket
        // that changed type take that bracket's type too (N0, looking through
        // retained boundary neutrals).
        for (size_t k = o + 1; k < n; k++) {
          if (orig[k] == NSM) type[k] = (uint8_t)set;
          else if (orig[k] != BN) break;
        }
        for (size_t k = i + 1; k < n; k++) {
          if (orig[k] == NSM) type[k] = (uint8_t)set;
          else if (orig[k] != BN) break;
        }
      }
    }
  }

  // N1/N2: a run of neutrals takes the surrounding direction if both sides
  // agree, otherwise the embedding direction. BN is transparent: it joins the
  // run for contiguity (a ZWNJ between two spaces must not split them into
  // runs that then read a neutral as their context), keeps its BN type, and
  // the left context scans past any BN just before the run.
  for (size_t i = 0; i < n; i++) {
    if (!(type[i] == ON || type[i] == WS || type[i] == B || type[i] == S)) continue;
    size_t j = i;
    while (j < n && (type[j] == ON || type[j] == WS || type[j] == B || type[j] == S ||
                     type[j] == BN)) j++;
    size_t a = i;
    while (a > 0 && type[a - 1] == BN) a--;
    BidiClass left = (a > 0) ? prv_dir_of(type[a - 1]) : sos_n;
    BidiClass right = (j < n) ? prv_dir_of(type[j]) : eos;
    BidiClass set = (left == right) ? left : e;
    for (size_t k = i; k < j; k++) {
      if (type[k] != BN) type[k] = (uint8_t)set;
    }
    i = j - 1;
  }

  // I1/I2: implicit levels. BN is skipped, then takes the level of the
  // preceding character (UAX 9 5.2), so a zero-width joiner inside a number
  // does not split the number's level run.
  for (size_t i = 0; i < n; i++) {
    if (type[i] == BN) continue;
    uint8_t lv = (uint8_t)base_level;
    if ((base_level & 1) == 0) {  // even (LTR) base
      if (type[i] == R) lv = base_level + 1;
      else if (type[i] == EN || type[i] == AN) lv = base_level + 2;
    } else {  // odd (RTL) base
      if (type[i] == L || type[i] == EN || type[i] == AN) lv = base_level + 1;
    }
    level[i] = lv;
  }
  for (size_t i = 0; i < n; i++) {
    if (type[i] == BN) {
      level[i] = (i > 0) ? level[i - 1] : (uint8_t)base_level;
    }
  }
}

// Per-line phase (L1, L2, L4) over one display line's codepoints and levels.
// L1 needs only "is this a separator or whitespace", which the codepoint
// itself answers, so folded/shaped lines need no original-class array.
static size_t prv_apply(const Codepoint *cps, const uint8_t *levels, size_t n, int base_level,
                        Codepoint *out, uint8_t *level_scratch, uint8_t *vis_scratch) {
  // L1 mutates levels, so work on the caller's scratch copy - not 255 bytes
  // of render-path stack (task stacks are 2-4 KiB).
  memcpy(level_scratch, levels, n);
  uint8_t *const level = level_scratch;

  // L1: reset separators and trailing whitespace to the base level.
  for (size_t i = 0; i < n; i++) {
    BidiClass ci = prv_class(cps[i]);
    if (ci == B || ci == S) {
      level[i] = (uint8_t)base_level;
      for (size_t k = i; k > 0; k--) {
        BidiClass ck = prv_class(cps[k - 1]);
        if (ck == WS || ck == BN) level[k - 1] = (uint8_t)base_level;
        else break;
      }
    }
  }
  for (size_t k = n; k > 0; k--) {
    BidiClass ck = prv_class(cps[k - 1]);
    if (ck == WS || ck == BN) level[k - 1] = (uint8_t)base_level;
    else break;
  }

  // L2: from the highest level down to the lowest odd level, reverse any
  // contiguous run of positions at that level or higher.
  uint8_t *const vis = vis_scratch;
  for (size_t i = 0; i < n; i++) vis[i] = (uint8_t)i;
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
          uint8_t tmp = vis[a]; vis[a] = vis[b]; vis[b] = tmp;
        }
        v = v2;
      } else {
        v++;
      }
    }
  }

  // L3: on odd (reversed) levels a combining mark now precedes its base;
  // the renderer draws marks after their base (zero advance, bearings over
  // the previous pen position), so re-reverse each marks+base cluster. This
  // also makes the visual sequence match the reference base-then-marks order.
  for (size_t v = 0; v < n; v++) {
    if (!(level[vis[v]] & 1) || prv_class(cps[vis[v]]) != NSM) continue;
    // Extend across marks and retained boundary neutrals (a ZWNJ between a
    // mark and its base is transparent, UAX 9 5.2): the cluster's base is the
    // next odd-level codepoint that is neither.
    size_t w = v;
    while (w < n && (level[vis[w]] & 1)) {
      BidiClass cw = prv_class(cps[vis[w]]);
      if (cw != NSM && cw != BN) break;
      w++;
    }
    // Reverse the cluster so the base (when present) leads and the marks and
    // retained neutrals follow in logical order. With no base at the visual
    // tail, still restore the mark run's own logical order.
    size_t last = (w < n && (level[vis[w]] & 1)) ? w : w - 1;
    for (size_t a2 = v, b2 = last; a2 < b2; a2++, b2--) {
      uint8_t tmp = vis[a2]; vis[a2] = vis[b2]; vis[b2] = tmp;
    }
    v = w;
  }

  // L4: emit visual order, mirroring glyphs that resolved to an odd level.
  for (size_t v = 0; v < n; v++) {
    uint8_t li = vis[v];
    Codepoint c = cps[li];
    if (level[li] & 1) c = bidi_mirror(c);
    out[v] = c;
  }
  return n;
}

size_t bidi_reorder_line_ctx(const Codepoint *cps, size_t n, int base_level, int sos_hint,
                             int sos_n_hint, int eos_hint, Codepoint *out, BidiScratch *ws) {
  if (cps == NULL || out == NULL || ws == NULL || n == 0) return 0;
  if (n > BIDI_MAX_CODEPOINTS) n = BIDI_MAX_CODEPOINTS;
  prv_resolve(cps, n, base_level, sos_hint, sos_n_hint, eos_hint, ws);
  return prv_apply(cps, ws->level, n, base_level, out, ws->orig, ws->vis);
}

size_t bidi_resolve_paragraph(const Codepoint *cps, size_t n, int base_level, BidiScratch *ws) {
  if (cps == NULL || ws == NULL || n == 0) return 0;
  if (n > BIDI_MAX_CODEPOINTS) n = BIDI_MAX_CODEPOINTS;
  if (ws->cps != cps) {
    memcpy(ws->cps, cps, n * sizeof(Codepoint));
  }
  prv_resolve(ws->cps, n, base_level, BIDI_BOUNDARY_AUTO, BIDI_BOUNDARY_AUTO,
              BIDI_BOUNDARY_AUTO, ws);
  return n;
}

size_t bidi_apply_line(const Codepoint *cps, const uint8_t *levels, size_t n, int base_level,
                       Codepoint *out, BidiScratch *ws) {
  if (cps == NULL || levels == NULL || out == NULL || ws == NULL || n == 0) return 0;
  if (n > BIDI_MAX_CODEPOINTS) n = BIDI_MAX_CODEPOINTS;
  // ws->orig is free by apply time (L1 derives classes from the codepoints).
  return prv_apply(cps, levels, n, base_level, out, ws->orig, ws->vis);
}

size_t bidi_reorder_utf8(const utf8_t *src, size_t src_len, utf8_t *dest, size_t dest_size,
                         int base_level, BidiScratch *ws) {
  return bidi_reorder_utf8_ctx(src, src_len, dest, dest_size, base_level, BIDI_BOUNDARY_AUTO,
                               BIDI_BOUNDARY_AUTO, BIDI_BOUNDARY_AUTO, ws);
}

size_t bidi_reorder_utf8_ctx(const utf8_t *src, size_t src_len, utf8_t *dest, size_t dest_size,
                             int base_level, int sos_hint, int sos_n_hint, int eos_hint,
                             BidiScratch *ws) {
  if (src == NULL || dest == NULL || ws == NULL || src_len == 0 || dest_size == 0) return 0;

  size_t n = 0;
  utf8_t *ptr = (utf8_t *)src;
  const utf8_t *end = src + src_len;
  while (ptr < end && *ptr != '\0' && n < BIDI_MAX_CODEPOINTS) {
    utf8_t *next = NULL;
    Codepoint cp = utf8_peek_codepoint(ptr, &next);
    if (cp == 0 || next == NULL) break;
    ws->cps[n++] = cp;
    ptr = next;
  }

  size_t vn = bidi_reorder_line_ctx(ws->cps, n, base_level, sos_hint, sos_n_hint, eos_hint,
                                    ws->visual, ws);

  size_t off = 0;
  for (size_t i = 0; i < vn; i++) {
    if (off + 4 >= dest_size) break;
    size_t w = utf8_encode_codepoint(ws->visual[i], dest + off);
    if (w == 0) continue;
    off += w;
  }
  if (off < dest_size) dest[off] = '\0';
  return off;
}
