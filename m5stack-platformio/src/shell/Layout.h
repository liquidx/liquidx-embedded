#pragma once

// Landscape 240x135, held with the front key on the right of the screen.
// The X4's layout (../xteink-x4-platformio/docs/design) at 0.3 scale, in
// colour.
//
//   chrome shown (depth 2)                  chrome hidden
//   ┌──────────────────────────╮││ ┌───┐   ┌───────────────────────────────┐
//   │ (SETTINGS / ACCENT)      │││ │   │   │                               │
//   │                          │││ │ ◀ │   │           content             │
//   │  Orange      [ Orange ]  │││ │ ▼ │   │           240 × 135           │
//   │                          │││ │ ● │   │                               │
//   │                          │││ │ ▭ │   │                               │
//   └──────────────────────────╯││ └───┘   └───────────────────────────────┘
//      card 216 − 5·depth wide       gutter 24
//
// Every page is a card with rounded right corners. Cards for the pages below
// it stay drawn underneath, so each level of the back stack shows as an edge.
namespace layout {

// M5GFX rotation. 1 puts the front key (A) on the right and the other key (B)
// on the top edge. If the screen comes up upside down, use 3.
constexpr int kRotation = 1;

constexpr int kScreenW = 240;
constexpr int kScreenH = 135;

constexpr int kGutterW = 24;
constexpr int kGutterX = kScreenW - kGutterW;

struct Rect {
  int x, y, w, h;
};

constexpr Rect kFullScreen{0, 0, kScreenW, kScreenH};

// Page cards: depth 0 (home) spans the canvas; each level down is kCardStep
// narrower. Corners on the right only; the card bleeds off the other edges.
constexpr int kCardStep = 5;
constexpr int kCardRadius = 10;
constexpr int kMaxDepth = 4;
constexpr int cardWidth(const int depth) { return kGutterX - kCardStep * depth; }
constexpr Rect cardArea(const int depth) { return {0, 0, cardWidth(depth), kScreenH}; }

// The two keys, drawn in the gutter as a group centred top to bottom: the top
// key (B, Back) as a circle, and under it the front key (A, Next / Select) as
// a tall rounded bar.
constexpr int kKeyB = 0, kKeyA = 1;  // bits of input::heldKeys()
constexpr int kKeyCenterX = kGutterX + kGutterW / 2;
constexpr int kKeyRadius = 9;   // B's circle; A's bar is as wide
constexpr int kKeyBarH = 46;
constexpr int kKeyGap = 8;
constexpr int kKeysH = kKeyRadius * 2 + kKeyGap + kKeyBarH;
constexpr int kKeysY = (kScreenH - kKeysH) / 2;
constexpr int kKeyBCenterY = kKeysY + kKeyRadius;
constexpr int kKeyBarY = kKeysY + kKeyRadius * 2 + kKeyGap;

constexpr int kMarginLeft = 10;  // content inset from a card's left edge
constexpr int kPad = 6;

}  // namespace layout
