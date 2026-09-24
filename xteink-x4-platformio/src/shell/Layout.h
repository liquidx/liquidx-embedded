#pragma once

#include <GfxRenderer.h>

// Landscape 800x480, held with the four front keys on the right edge.
// docs/design/README.md is the visual spec.
//
//   chrome shown (depth 2)                  chrome hidden
//   ┌──────────────────────────╮││ ┌────┐   ┌───────────────────────────────┐
//   │ (SETTINGS / REFRESH)     │││ │ ▭  │   │                               │
//   │                          │││ │ ◀  │   │           content             │
//   │  12 pages       [ 12 ]   │││ │ ●  │   │           800 × 480           │
//   │                          │││ │ ▲  │   │                               │
//   │                          │││ │ ▼  │   │                               │
//   └──────────────────────────╯││ └────┘   └───────────────────────────────┘
//      card 728 − 12·depth wide      gutter 72
//
// Every page is a card with rounded right corners. Cards for the pages below
// it stay drawn underneath, so each level of the back stack shows as an edge.
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

// Page cards: depth 0 (home) spans the canvas; each level down is kCardStep
// narrower. Corners on the right only; the card bleeds off the other edges.
constexpr int kCardStep = 12;
constexpr int kCardRadius = 22;
constexpr int kMaxDepth = 4;
constexpr int cardWidth(const int depth) { return kGutterX - kCardStep * depth; }
constexpr Rect cardArea(const int depth) { return {0, 0, cardWidth(depth), kScreenH}; }

// Vertical span of each front key, top to bottom. From CrossPoint's portrait
// hint positions (x = 58, 146, 254, 342, width 80) rotated: y = 479 - x.
constexpr int kKeyCount = 4;
constexpr Rect kKeySlots[kKeyCount] = {
    {kGutterX, 57, kGutterW, 80},   // Back / Home
    {kGutterX, 145, kGutterW, 80},  // Select
    {kGutterX, 253, kGutterW, 80},  // Up
    {kGutterX, 341, kGutterW, 80},  // Down
};

// Round key buttons in the gutter, one centred on each key slot.
constexpr int kKeyRadius = 24;
constexpr int kKeyCenterX = kScreenW - kBezelRight - 33;

constexpr int kMarginLeft = 40;  // content inset from a card's left edge
constexpr int kPad = 16;

}  // namespace layout
