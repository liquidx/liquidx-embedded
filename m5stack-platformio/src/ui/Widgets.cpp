#include "Widgets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace ui {

namespace {

constexpr int kTitleY = 7;
constexpr int kContentTop = 29;   // first row under the title pill
constexpr int kRightMargin = 9;   // rows stop this far from the card edge
constexpr int kPillPadX = 6;
constexpr int kRowIconInset = 7;
constexpr int kRowIconLabelInset = 32;
constexpr int kRowTextInset = 8;
constexpr int kInfoValueOffset = 66;  // value column, from the label column
constexpr int kInfoPitch = 15;

// Icons, kIconSize wide: the X4's, 1:1.
constexpr const char* kSlidersIcon[kIconH] = {
    "..................", ".....###..........", "....##.##.........", ".####..##########.",
    "....##.##.........", ".....###..........", "..........####....", "..#########..###..",
    "..#########..###..", "..........####....", "...###............", "..##.##...........",
    ".##...###########.", "..##.##...........", "...###............", "..................",
};

constexpr const char* kBluetoothIcon[kIconH] = {
    "........##........", "........###.......", "........####......", "........#####.....",
    "....##..##.###....", "....###.##.##.....", "......######......", ".......####.......",
    ".......####.......", "......######......", "....###.#####.....", "....##..##.###....",
    "........##.##.....", "........####......", "........###.......", "........##........",
};

int contentLeft(const layout::Rect& area) { return area.x + layout::kMarginLeft; }
int contentRight(const layout::Rect& area) { return area.x + area.w - kRightMargin; }

}  // namespace

void drawIcon(Gfx& g, const Icon icon, const int x, const int y, const theme::Color color) {
  const char* const* rows = nullptr;
  switch (icon) {
    case Icon::Sliders:
      rows = kSlidersIcon;
      break;
    case Icon::Bluetooth:
      rows = kBluetoothIcon;
      break;
    case Icon::None:
      return;
  }
  for (int dy = 0; dy < kIconH; dy++) {
    for (int dx = 0; rows[dy][dx] != '\0'; dx++) {
      if (rows[dy][dx] == '#') g.drawPixel(x + dx, y + dy, color);
    }
  }
}

int imageOrigin(const int size, const int avail) { return size > avail ? 0 : (avail - size) / 2; }

int textWidth(Gfx& g, const fonts::Id font, const char* text) {
  g.setFont(fonts::get(font));
  return g.textWidth(text);
}

void drawTextAt(Gfx& g, const fonts::Id font, const int x, const int baseline, const char* text,
                const theme::Color color) {
  g.setFont(fonts::get(font));
  g.setTextDatum(lgfx::textdatum_t::baseline_left);
  g.setTextColor(color);  // no background: glyph edges blend with what's there
  g.drawString(text, x, baseline);
}

void drawTextCentred(Gfx& g, const fonts::Id font, const int centreX, const int baseline, const char* text,
                     const theme::Color color) {
  drawTextAt(g, font, centreX - textWidth(g, font, text) / 2, baseline, text, color);
}

int centredBaseline(const fonts::Id font, const int y, const int h) { return y + (h + fonts::capHeight(font)) / 2; }

void truncate(Gfx& g, const fonts::Id font, const char* text, const int maxWidth, char* out, const size_t size) {
  snprintf(out, size, "%s", text);
  size_t n = strlen(out);
  while (n > 0 && textWidth(g, font, out) > maxWidth) {
    n--;
    snprintf(out + n, size - n, "...");
  }
}

int drawPill(Gfx& g, const fonts::Id font, const int x, const int y, const char* text, const theme::Color fill,
             const theme::Color ink) {
  const int w = textWidth(g, font, text) + kPillPadX * 2;
  g.fillSmoothRoundRect(x, y, w, kPillH, kPillH / 2, fill);
  drawTextAt(g, font, x + kPillPadX, centredBaseline(font, y, kPillH), text, ink);
  return w;
}

int drawTitle(Gfx& g, const layout::Rect& area, const char* title, layout::Rect* pill) {
  if (pill != nullptr) *pill = {0, 0, 0, 0};
  if (title == nullptr) return area.y + kTitleY;
  char upper[64];
  size_t i = 0;
  for (; title[i] != '\0' && i < sizeof(upper) - 1; i++) upper[i] = static_cast<char>(toupper(title[i]));
  upper[i] = '\0';
  char fitted[64];
  // The pill may run closer to the card's edge than rows do.
  truncate(g, fonts::LABEL_11, upper, area.x + area.w - 4 - contentLeft(area) - kPillPadX * 2, fitted, sizeof(fitted));
  const int w = drawPill(g, fonts::LABEL_11, contentLeft(area), area.y + kTitleY, fitted, theme::kSurface,
                         theme::accent());
  if (pill != nullptr) *pill = {contentLeft(area), area.y + kTitleY, w, kPillH};
  return area.y + kContentTop;
}

void drawRow(Gfx& g, const layout::Rect& row, const RowStyle& style, const Icon icon, const char* label,
             const char* value, const bool selected) {
  if (selected) g.fillSmoothRoundRect(row.x, row.y, row.w, row.h, style.radius, theme::accent());
  const theme::Color ink = selected ? theme::kOnAccent : theme::kText;

  int labelX = row.x + kRowTextInset;
  if (icon != Icon::None) {
    drawIcon(g, icon, row.x + kRowIconInset, row.y + (row.h - kIconH) / 2, ink);
    labelX = row.x + kRowIconLabelInset;
  }

  const fonts::Id font = style.font;
  const int baseline = centredBaseline(font, row.y, row.h);
  const int right = row.x + row.w - kRowTextInset;
  int labelW = right - labelX;
  if (value != nullptr && value[0] != '\0') {
    const int valueW = textWidth(g, font, value);
    drawTextAt(g, font, right - valueW, baseline, value, selected ? theme::kOnAccent : theme::kTextDim);
    labelW -= valueW + layout::kPad;
  }
  char text[48];
  truncate(g, font, label, labelW, text, sizeof(text));
  drawTextAt(g, font, labelX, baseline, text, ink);
}

void drawScrollbar(Gfx& g, const layout::Rect& area, const int top, const int visible, const int count) {
  if (count <= visible || visible <= 0) return;
  const int x = contentRight(area) + 4;
  const int trackY = area.y + kContentTop;
  const int trackH = area.h - kContentTop - 9;
  g.fillRect(x, trackY, 2, trackH, theme::kSurface);
  const int thumbH = std::max(10, trackH * visible / count);
  const int thumbY = trackY + (trackH - thumbH) * top / (count - visible);
  g.fillRect(x, thumbY, 2, thumbH, theme::kTextDim);
}

bool ListView::move(const int delta, const int count) {
  if (count < 2) return false;
  selected_ = (selected_ + delta + count) % count;
  return true;
}

void ListView::render(Gfx& g, const layout::Rect& area, const char* title, const int count, TextFn label, TextFn value,
                      const void* ctx) {
  const auto& style = kListRow;
  const int rowsY = drawTitle(g, area, title);
  const int visible = std::max(1, (area.y + area.h - rowsY + (style.pitch - style.height)) / style.pitch);

  selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
  if (selected_ < top_) top_ = selected_;
  if (selected_ >= top_ + visible) top_ = selected_ - visible + 1;
  top_ = std::clamp(top_, 0, std::max(0, count - visible));

  const int x = contentLeft(area);
  const int w = contentRight(area) - x;
  for (int i = top_; i < std::min(count, top_ + visible); i++) {
    const layout::Rect row{x, rowsY + (i - top_) * style.pitch, w, style.height};
    drawRow(g, row, style, Icon::None, label(ctx, i), value ? value(ctx, i) : nullptr, i == selected_);
  }
  drawScrollbar(g, area, top_, visible, count);
}

bool InfoView::scroll(const int delta) {
  if (maxTop_ == 0) return false;
  // One key scrolls (the other is Back), so run off the end back to the top.
  top_ = (top_ + delta + maxTop_ + 1) % (maxTop_ + 1);
  return true;
}

void InfoView::render(Gfx& g, const layout::Rect& area, const char* title, const Row* rows, const int count) {
  const int rowsY = drawTitle(g, area, title);
  const int visible = std::max(1, (area.y + area.h - rowsY) / kInfoPitch);

  maxTop_ = std::max(0, count - visible);
  top_ = std::clamp(top_, 0, maxTop_);

  const int labelX = contentLeft(area) + 2;
  const int valueX = labelX + kInfoValueOffset;
  const int valueW = contentRight(area) - valueX;
  for (int i = top_; i < std::min(count, top_ + visible); i++) {
    const int baseline = centredBaseline(fonts::SMALL_12, rowsY + (i - top_) * kInfoPitch, kInfoPitch);
    drawTextAt(g, fonts::SMALL_12, labelX, baseline, rows[i].label, theme::kTextDim);
    char value[48];
    truncate(g, fonts::SMALL_12, rows[i].value, valueW, value, sizeof(value));
    drawTextAt(g, fonts::SMALL_12, valueX, baseline, value);
  }
  drawScrollbar(g, area, top_, visible, count);
}

}  // namespace ui
