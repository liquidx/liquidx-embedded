#include "Widgets.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ui {

namespace {

constexpr int kTitleY = 28;
constexpr int kContentTop = 73;       // first row under the title pill
constexpr int kRightMargin = 33;      // rows stop this far from the card edge
constexpr int kPillPadX = 10;
constexpr int kRowIconInset = 22;
constexpr int kRowIconLabelInset = 57;
constexpr int kRowTextInset = 20;
constexpr int kInfoValueOffset = 170;  // value column, from the label column
constexpr int kInfoPitch = 34;

// Icons, kIconSize wide, drawn from the design at 1:1.
constexpr int kIconH = 16;
constexpr const char* kImageIcon[kIconH] = {
    ".################.", "##..............##", "#................#", "#................#",
    "#...###..........#", "#..#...#.........#", "#..#...#.........#", "#..#...#.........#",
    "#...###.....##...#", "#..........####..#", "###.......##..##.#", "#.##.....##....###",
    "#..##...##......##", "#...##.##.......##", "##...###.......###", ".################.",
};
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

int capHeight(const GfxRenderer& r, const int font) {
  const auto it = r.getFontMap().find(font);
  if (it == r.getFontMap().end()) return 0;
  const EpdGlyph* glyph = it->second.getGlyph('H');
  return glyph ? glyph->height : 0;
}

}  // namespace

void drawIcon(GfxRenderer& r, const Icon icon, const int x, const int y, const bool black) {
  const char* const* rows = nullptr;
  switch (icon) {
    case Icon::Image:
      rows = kImageIcon;
      break;
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
      if (rows[dy][dx] == '#') r.drawPixel(x + dx, y + dy, black);
    }
  }
}

int imageOrigin(const int size, const int avail) { return size > avail ? 0 : (avail - size) / 2; }

bool drawBitmapFile(GfxRenderer& r, const char* path, const layout::Rect& area) {
  HalFile file;
  if (!Storage.openFileForRead("IMG", path, file)) return false;
  Bitmap bitmap(file, true);
  bool drawn = false;
  const auto err = bitmap.parseHeaders();
  if (err == BmpReaderError::Ok) {
    const int x = area.x + imageOrigin(bitmap.getWidth(), area.w);
    const int y = area.y + imageOrigin(bitmap.getHeight(), area.h);
    drawn = r.drawBitmap(bitmap, x, y, 0, 0);  // no max size: no scaling
  } else {
    LOG_ERR("IMG", "%s: %s", path, Bitmap::errorToString(err));
  }
  file.close();
  return drawn;
}

void drawTextAt(GfxRenderer& r, const int font, const int x, const int baseline, const char* text, const bool black,
                const EpdFontFamily::Style style) {
  r.drawText(font, x, baseline - r.getFontAscenderSize(font), text, black, style);
}

int centredBaseline(const GfxRenderer& r, const int font, const int y, const int h) {
  return y + (h + capHeight(r, font)) / 2;
}

int drawPill(GfxRenderer& r, const int font, const int x, const int y, const char* text, const bool black) {
  const int w = r.getTextWidth(font, text) + kPillPadX * 2;
  r.fillRoundedRect(x, y, w, kPillH, kPillH / 2, black ? Color::Black : Color::White);
  drawTextAt(r, font, x + kPillPadX, centredBaseline(r, font, y, kPillH), text, !black);
  return w;
}

int drawTitle(GfxRenderer& r, const layout::Rect& area, const char* title) {
  if (title == nullptr) return area.y + kTitleY;
  char upper[64];
  size_t i = 0;
  for (; title[i] != '\0' && i < sizeof(upper) - 1; i++) upper[i] = static_cast<char>(toupper(title[i]));
  upper[i] = '\0';
  drawPill(r, fonts::LABEL_15, contentLeft(area), area.y + kTitleY, upper);
  return area.y + kContentTop;
}

void drawRow(GfxRenderer& r, const layout::Rect& row, const RowStyle& style, const Icon icon, const char* label,
             const char* value, const bool selected) {
  if (selected) r.fillRoundedRect(row.x, row.y, row.w, row.h, style.radius, Color::Black);
  const bool ink = !selected;

  int labelX = row.x + kRowTextInset;
  if (icon != Icon::None) {
    drawIcon(r, icon, row.x + kRowIconInset, row.y + (row.h - kIconH) / 2, ink);
    labelX = row.x + kRowIconLabelInset;
  }

  const int font = style.font;
  const int baseline = centredBaseline(r, font, row.y, row.h);
  const int right = row.x + row.w - kRowTextInset;
  int labelW = right - labelX;
  if (value != nullptr && value[0] != '\0') {
    const int valueW = r.getTextWidth(font, value);
    drawTextAt(r, font, right - valueW, baseline, value, ink);
    labelW -= valueW + layout::kPad;
  }
  const auto text = r.truncatedText(font, label, labelW);
  drawTextAt(r, font, labelX, baseline, text.c_str(), ink);
}

void drawScrollbar(GfxRenderer& r, const layout::Rect& area, const int top, const int visible, const int count) {
  if (count <= visible || visible <= 0) return;
  const int x = contentRight(area) + 12;
  const int trackY = area.y + kContentTop;
  const int trackH = area.h - kContentTop - kContentTop / 2;
  r.fillRectDither(x + 1, trackY, 2, trackH, Color::LightGray);
  const int thumbH = std::max(24, trackH * visible / count);
  const int thumbY = trackY + (trackH - thumbH) * top / (count - visible);
  r.fillRoundedRect(x, thumbY, 4, thumbH, 2, Color::Black);
}

bool ListView::move(const int delta, const int count) {
  if (count < 2) return false;
  selected_ = (selected_ + delta + count) % count;
  return true;
}

void ListView::render(GfxRenderer& r, const layout::Rect& area, const char* title, const int count, TextFn label,
                      TextFn value, const void* ctx) {
  const auto& style = kListRow;
  const int rowsY = drawTitle(r, area, title);
  const int visible = std::max(1, (area.y + area.h - rowsY + (style.pitch - style.height)) / style.pitch);

  selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
  if (selected_ < top_) top_ = selected_;
  if (selected_ >= top_ + visible) top_ = selected_ - visible + 1;
  top_ = std::clamp(top_, 0, std::max(0, count - visible));

  const int x = contentLeft(area);
  const int w = contentRight(area) - x;
  for (int i = top_; i < std::min(count, top_ + visible); i++) {
    const layout::Rect row{x, rowsY + (i - top_) * style.pitch, w, style.height};
    drawRow(r, row, style, Icon::None, label(ctx, i), value ? value(ctx, i) : nullptr, i == selected_);
  }
  drawScrollbar(r, area, top_, visible, count);
}

bool InfoView::scroll(const int delta) {
  const int next = std::clamp(top_ + delta, 0, maxTop_);
  if (next == top_) return false;
  top_ = next;
  return true;
}

void InfoView::render(GfxRenderer& r, const layout::Rect& area, const char* title, const Row* rows, const int count) {
  const int rowsY = drawTitle(r, area, title);
  const int visible = std::max(1, (area.y + area.h - rowsY) / kInfoPitch);

  maxTop_ = std::max(0, count - visible);
  top_ = std::clamp(top_, 0, maxTop_);

  const int labelX = contentLeft(area) + kRowTextInset;
  const int valueX = labelX + kInfoValueOffset;
  const int valueW = contentRight(area) - valueX;
  for (int i = top_; i < std::min(count, top_ + visible); i++) {
    const int baseline = centredBaseline(r, fonts::SMALL_15, rowsY + (i - top_) * kInfoPitch, kInfoPitch);
    drawTextAt(r, fonts::SMALL_15, labelX, baseline, rows[i].label);
    const auto value = r.truncatedText(fonts::SMALL_15, rows[i].value, valueW);
    drawTextAt(r, fonts::SMALL_15, valueX, baseline, value.c_str());
  }
  drawScrollbar(r, area, top_, visible, count);
}

}  // namespace ui
