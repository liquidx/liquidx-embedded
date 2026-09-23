#include "Widgets.h"

#include <algorithm>

#include "../Fonts.h"

namespace ui {

namespace {

constexpr int kTitleTop = 20;
constexpr int kTitleGap = 14;
constexpr int kRowH = 52;
constexpr int kRowRadius = 8;

int contentLeft(const layout::Rect& area) { return area.x + std::max(layout::kPad, layout::kBezelLeft + 8); }
int contentRight(const layout::Rect& area) { return area.x + area.w - layout::kPad; }

}  // namespace

int drawTitle(GfxRenderer& r, const layout::Rect& area, const char* title) {
  if (title == nullptr) return area.y + layout::kBezelTop + layout::kPad;
  const int y = area.y + kTitleTop;
  r.drawText(fonts::TITLE_18, contentLeft(area) + layout::kPad, y, title, true, EpdFontFamily::BOLD);
  return y + r.getLineHeight(fonts::TITLE_18) + kTitleGap;
}

void drawScrollbar(GfxRenderer& r, const layout::Rect& area, const int top, const int visible, const int count) {
  if (count <= visible || visible <= 0) return;
  const int x = contentRight(area) + 6;
  const int trackY = area.y + layout::kBezelTop + 8;
  const int trackH = area.h - layout::kBezelTop - layout::kBezelBottom - 16;
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

void ListView::render(GfxRenderer& r, const layout::Rect& area, const char* title, const int count, LabelFn label,
                      const void* ctx) {
  const int rowsY = drawTitle(r, area, title);
  const int visible = std::max(1, (area.y + area.h - layout::kBezelBottom - rowsY) / kRowH);

  selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
  if (selected_ < top_) top_ = selected_;
  if (selected_ >= top_ + visible) top_ = selected_ - visible + 1;
  top_ = std::clamp(top_, 0, std::max(0, count - visible));

  const int x = contentLeft(area);
  const int w = contentRight(area) - x;
  const int textInset = layout::kPad;
  for (int i = top_; i < std::min(count, top_ + visible); i++) {
    const int y = rowsY + (i - top_) * kRowH;
    const int textY = y + (kRowH - r.getLineHeight(fonts::BODY_14)) / 2;
    const bool isSelected = i == selected_;
    if (isSelected) r.fillRoundedRect(x, y, w, kRowH, kRowRadius, Color::Black);
    const auto text = r.truncatedText(fonts::BODY_14, label(ctx, i), w - textInset * 2,
                                      isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    r.drawText(fonts::BODY_14, x + textInset, textY, text.c_str(), !isSelected,
               isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
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
  const int lineH = r.getLineHeight(fonts::BODY_14) + 8;
  const int visible = std::max(1, (area.y + area.h - layout::kBezelBottom - rowsY) / lineH);

  maxTop_ = std::max(0, count - visible);
  top_ = std::clamp(top_, 0, maxTop_);

  const int labelX = contentLeft(area) + layout::kPad;
  const int valueX = labelX + 170;
  const int valueW = contentRight(area) - valueX;
  for (int i = top_; i < std::min(count, top_ + visible); i++) {
    const int y = rowsY + (i - top_) * lineH;
    r.drawText(fonts::BODY_14, labelX, y, rows[i].label, true, EpdFontFamily::BOLD);
    const auto value = r.truncatedText(fonts::BODY_14, rows[i].value, valueW);
    r.drawText(fonts::BODY_14, valueX, y, value.c_str());
  }
  drawScrollbar(r, area, top_, visible, count);
}

}  // namespace ui
