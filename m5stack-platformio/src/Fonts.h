#pragma once

#include "Gfx.h"

// IBM Plex Mono, as on the X4, antialiased. Sizes are in src/fonts/ (generated
// by scripts/fonts.sh).
namespace fonts {

enum Id : uint8_t {
  SMALL_12,    // non-selectable text: dates, captions, About
  LABEL_11,    // uppercase title pills (tracked)
  MEDIUM_16,   // anything selectable, headings
  LARGE_30,    // a setting's current value
  DISPLAY_46,  // clock, pairing code: digits, ':' and '-' only
  kCount,
};

// Load them all. Before any drawing.
void begin();
const lgfx::IFont* get(Id id);
// Height of a capital letter (or a digit), for centring text in a band.
int capHeight(Id id);

}  // namespace fonts
