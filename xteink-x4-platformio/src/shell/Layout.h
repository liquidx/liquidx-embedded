#pragma once

#include <GfxRenderer.h>

// Landscape 800x480, held with the four front keys on the right edge.
//
//   chrome shown                        chrome hidden
//   ┌─────────────────────────┬─────┐   ┌───────────────────────────────┐
//   │                         │ 87% │   │                               │
//   │        content          │⌂Home│   │           content             │
//   │        728 × 480        │●Sel │   │           800 × 480           │
//   │                         │▲Up  │   │                               │
//   │                         │▼Down│   │                               │
//   └─────────────────────────┴─────┘   └───────────────────────────────┘
namespace layout {

// Panel-native orientation on X4/X4C: logical (x, y) == framebuffer (x, y).
constexpr GfxRenderer::Orientation kOrientation = GfxRenderer::LandscapeCounterClockwise;

constexpr int kScreenW = 800;
constexpr int kScreenH = 480;

// Pixels hidden under the X4C bezel in this orientation (BoardConfig
// viewableInsets {top 9, right 7, bottom 3, left 7} in portrait, rotated).
constexpr int kBezelTop = 7;
constexpr int kBezelRight = 3;
constexpr int kBezelBottom = 7;
constexpr int kBezelLeft = 9;

constexpr int kGutterW = 72;
constexpr int kGutterX = kScreenW - kGutterW;

struct Rect {
  int x, y, w, h;
};

constexpr Rect kFullScreen{0, 0, kScreenW, kScreenH};
constexpr Rect kContentWithChrome{0, 0, kGutterX, kScreenH};

// Vertical span of each front key, top to bottom. From CrossPoint's portrait
// hint positions (x = 58, 146, 254, 342, width 80) rotated: y = 479 - x.
constexpr int kKeyCount = 4;
constexpr Rect kKeySlots[kKeyCount] = {
    {kGutterX, 57, kGutterW, 80},   // Back / Home
    {kGutterX, 145, kGutterW, 80},  // Select
    {kGutterX, 253, kGutterW, 80},  // Up
    {kGutterX, 341, kGutterW, 80},  // Down
};

constexpr int kPad = 16;

}  // namespace layout
