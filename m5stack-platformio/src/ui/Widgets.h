#pragma once

#include <cstddef>

#include "../Fonts.h"
#include "../Gfx.h"
#include "../Theme.h"
#include "../shell/Layout.h"

// Reusable drawing pieces shared by the home screen and apps.
namespace ui {

// Small line icons, drawn in a kIconSize square.
enum class Icon : uint8_t { None, Sliders, Bluetooth };
constexpr int kIconSize = 18;
constexpr int kIconH = 16;
void drawIcon(Gfx& g, Icon icon, int x, int y, theme::Color color);

// Where an image of `size` pixels starts along an axis of `avail`: centred when
// it fits, otherwise pinned to the start (the clip rect crops the rest).
int imageOrigin(int size, int avail);

int textWidth(Gfx& g, fonts::Id font, const char* text);
// Draw `text` with its baseline at `baseline`.
void drawTextAt(Gfx& g, fonts::Id font, int x, int baseline, const char* text, theme::Color color = theme::kText);
// The same, centred on `centreX`.
void drawTextCentred(Gfx& g, fonts::Id font, int centreX, int baseline, const char* text,
                     theme::Color color = theme::kText);
// Baseline that vertically centres `font`'s capitals in a band of height h at y.
int centredBaseline(fonts::Id font, int y, int h);
// `text`, cut short with "..." if it's wider than `maxWidth`.
void truncate(Gfx& g, fonts::Id font, const char* text, int maxWidth, char* out, size_t size);

// A fully rounded label. Returns the pill's width.
constexpr int kPillH = 16;
int drawPill(Gfx& g, fonts::Id font, int x, int y, const char* text, theme::Color fill, theme::Color ink);

// The page label pill at the top-left of a card, uppercased, in the accent
// colour. Returns the y where content below it starts. With title == nullptr
// (chrome hidden) only returns that y. `pill`, if given, is set to the pill's
// bounds (empty when there's no title).
int drawTitle(Gfx& g, const layout::Rect& area, const char* title, layout::Rect* pill = nullptr);

// One selectable row: optional icon, label, and a value right-aligned. The
// selected row is an accent pill with black text.
struct RowStyle {
  int height;
  int pitch;  // row-to-row distance
  int radius;
  fonts::Id font;
};
constexpr RowStyle kHomeRow{24, 27, 8, fonts::MEDIUM_16};
constexpr RowStyle kListRow{22, 25, 7, fonts::MEDIUM_16};
void drawRow(Gfx& g, const layout::Rect& row, const RowStyle& style, Icon icon, const char* label, const char* value,
             bool selected);

// A vertical, scrolling, single-selection list of rows under a title pill.
class ListView {
 public:
  using TextFn = const char* (*)(const void* ctx, int index);

  void reset() { selected_ = top_ = 0; }
  int selected() const { return selected_; }
  void select(int index) { selected_ = index; }

  // Move the selection by `delta`, wrapping. Returns true if it changed.
  bool move(int delta, int count);

  // `value` may be nullptr for label-only rows.
  void render(Gfx& g, const layout::Rect& area, const char* title, int count, TextFn label, TextFn value,
              const void* ctx);

 private:
  int selected_ = 0;
  int top_ = 0;  // first visible row
};

// Label/value rows that scroll a row at a time when they don't fit.
class InfoView {
 public:
  struct Row {
    const char* label;
    const char* value;
  };

  void reset() { top_ = 0; }
  // Scroll by `delta` rows, wrapping from the end back to the top. Returns
  // true if it moved.
  bool scroll(int delta);

  void render(Gfx& g, const layout::Rect& area, const char* title, const Row* rows, int count);

 private:
  int top_ = 0;
  int maxTop_ = 0;  // from the last render
};

// Thin scroll indicator between the rows and the card's right edge.
void drawScrollbar(Gfx& g, const layout::Rect& area, int top, int visible, int count);

}  // namespace ui
