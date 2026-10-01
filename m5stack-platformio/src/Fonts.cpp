#include "Fonts.h"

#include "fonts/plexmono_11_label.h"
#include "fonts/plexmono_12_regular.h"
#include "fonts/plexmono_16_medium.h"
#include "fonts/plexmono_30_large.h"
#include "fonts/plexmono_46_display.h"

namespace fonts {

namespace {

struct Loaded {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  int cap = 0;
};
Loaded loaded[kCount];

template <size_t N>
void load(const Id id, const uint8_t (&bytes)[N], const int cap, const int space) {
  Loaded& l = loaded[id];
  l.data.set(bytes, N);
  l.font.loadFont(&l.data);
  // VLW has no space glyph and guesses its width from the font size; these
  // are monospaced, so use the real advance.
  l.font.spaceWidth = space;
  l.cap = cap;
}

}  // namespace

void begin() {
  using namespace fontdata;
  load(SMALL_12, plexmono_12_regular, plexmono_12_regular_cap, plexmono_12_regular_space);
  load(LABEL_11, plexmono_11_label, plexmono_11_label_cap, plexmono_11_label_space);
  load(MEDIUM_16, plexmono_16_medium, plexmono_16_medium_cap, plexmono_16_medium_space);
  load(LARGE_30, plexmono_30_large, plexmono_30_large_cap, plexmono_30_large_space);
  load(DISPLAY_46, plexmono_46_display, plexmono_46_display_cap, plexmono_46_display_space);
}

const lgfx::IFont* get(const Id id) { return &loaded[id].font; }

int capHeight(const Id id) { return loaded[id].cap; }

}  // namespace fonts
