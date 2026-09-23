#include "Fonts.h"

#include <GfxRenderer.h>
#include <builtinFonts/all.h>

namespace {

EpdFont ui10Regular(&ubuntu_10_regular);
EpdFont ui10Bold(&ubuntu_10_bold);
EpdFont ui12Regular(&ubuntu_12_regular);
EpdFont ui12Bold(&ubuntu_12_bold);
EpdFont body14Regular(&notosans_14_regular);
EpdFont body14Bold(&notosans_14_bold);
EpdFont title18Regular(&notosans_18_regular);
EpdFont title18Bold(&notosans_18_bold);

}  // namespace

namespace fonts {

void registerAll(GfxRenderer& renderer) {
  renderer.insertFont(UI_10, EpdFontFamily(&ui10Regular, &ui10Bold));
  renderer.insertFont(UI_12, EpdFontFamily(&ui12Regular, &ui12Bold));
  renderer.insertFont(BODY_14, EpdFontFamily(&body14Regular, &body14Bold));
  renderer.insertFont(TITLE_18, EpdFontFamily(&title18Regular, &title18Bold));
}

}  // namespace fonts
