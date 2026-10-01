#pragma once

#include <cstdint>

#include "Settings.h"

// Colours, as 0xRRGGBB. A dark UI: the LCD is its own light, so black costs
// nothing and the accent colour does the work e-paper's black fill did on the
// X4 (the selected row, lit keys).
namespace theme {

using Color = uint32_t;

constexpr Color kBackdrop = 0x000000;   // behind the cards: the key gutter
constexpr Color kCard = 0x16181D;       // the page on top
constexpr Color kCardBelow = 0x0F1114;  // pages under it, showing as edges
constexpr Color kCardEdge = 0x444A56;   // the 1px rim of every card
constexpr Color kSurface = 0x2A2E38;    // pills, key buttons, scrollbar track
constexpr Color kText = 0xF4F3EE;
constexpr Color kTextDim = 0x8D94A1;
constexpr Color kOnAccent = 0x000000;   // text on an accent fill

constexpr Color kPaper = 0xFFFFFF;  // blit's grey formats count ink on white
constexpr Color kGood = 0x4ADE80;   // battery
constexpr Color kWarn = 0xFBBF24;
constexpr Color kBad = 0xF87171;

// Settings → Accent: the control's value is an index into this.
constexpr Color kAccents[] = {0xFF8A1F, 0x4C9DFF, 0x4ADE80, 0xFF6FAE, 0xF4F3EE};

inline Color accent() {
  constexpr int32_t kCount = sizeof(kAccents) / sizeof(kAccents[0]);
  const int32_t index = settings::value(settings::kAccent);
  return kAccents[index >= 0 && index < kCount ? index : 0];
}

}  // namespace theme
