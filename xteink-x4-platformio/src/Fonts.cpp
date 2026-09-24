#include "Fonts.h"

#include <GfxRenderer.h>
#include <builtinFonts/all.h>

namespace {

EpdFont small15(&plexmono_15_regular);
EpdFont label15(&plexmono_15_label);
EpdFont medium22(&plexmono_22_medium);
EpdFont display136(&plexmono_136_display);

}  // namespace

namespace fonts {

void registerAll(GfxRenderer& renderer) {
  renderer.insertFont(SMALL_15, EpdFontFamily(&small15));
  renderer.insertFont(LABEL_15, EpdFontFamily(&label15));
  renderer.insertFont(MEDIUM_22, EpdFontFamily(&medium22));
  renderer.insertFont(DISPLAY_136, EpdFontFamily(&display136));
}

}  // namespace fonts
