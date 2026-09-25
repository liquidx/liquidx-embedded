#pragma once

#include <GfxRenderer.h>

#include "../Fonts.h"
#include "../shell/Layout.h"

// Reusable drawing pieces shared by the home screen and apps. Geometry follows
// docs/design/README.md.
namespace ui {

// Small line icons, drawn in a kIconSize square.
enum class Icon : uint8_t { None, Image, Sliders };
constexpr int kIconSize = 18;
void drawIcon(GfxRenderer& r, Icon icon, int x, int y, bool black);

// Draw `text` with its baseline at `baseline` (drawText takes the line top).
void drawTextAt(GfxRenderer& r, int font, int x, int baseline, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR);
// Baseline that vertically centres `font`'s capitals in a band of height h at y.
int centredBaseline(const GfxRenderer& r, int font, int y, int h);

// A fully rounded label in the small font. Black pills have white text; white
// pills black text. Returns the pill's width.
constexpr int kPillH = 27;
int drawPill(GfxRenderer& r, int font, int x, int y, const char* text, bool black = true);

// The page label pill at the top-left of a card, uppercased. Returns the y
// where content below it starts. With title == nullptr (chrome hidden) only
// returns that y.
int drawTitle(GfxRenderer& r, const layout::Rect& area, const char* title);

// One selectable row: optional icon, label, and a value right-aligned. The
// selected row is a black pill with white text. Anything selectable uses the
// medium font; the small one is too thin to read on e-ink except as decoration.
struct RowStyle {
  int height;
  int pitch;  // row-to-row distance
  int radius;
  int font;
};
constexpr RowStyle kHomeRow{60, 68, 16, fonts::MEDIUM_22};
constexpr RowStyle kListRow{56, 62, 14, fonts::MEDIUM_22};
void drawRow(GfxRenderer& r, const layout::Rect& row, const RowStyle& style, Icon icon, const char* label,
             const char* value, bool selected);

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
  void render(GfxRenderer& r, const layout::Rect& area, const char* title, int count, TextFn label, TextFn value,
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
  // Scroll by `delta` rows. Returns true if it moved.
  bool scroll(int delta);

  void render(GfxRenderer& r, const layout::Rect& area, const char* title, const Row* rows, int count);

 private:
  int top_ = 0;
  int maxTop_ = 0;  // from the last render
};

// Thin scroll indicator between the rows and the card's right edge.
void drawScrollbar(GfxRenderer& r, const layout::Rect& area, int top, int visible, int count);

}  // namespace ui
