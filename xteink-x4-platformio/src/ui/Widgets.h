#pragma once

#include <GfxRenderer.h>

#include "../shell/Layout.h"

// Reusable drawing pieces shared by the home screen and apps.
namespace ui {

// Draw a screen title; returns the y where content below it should start.
// With title == nullptr (chrome hidden) nothing is drawn.
int drawTitle(GfxRenderer& r, const layout::Rect& area, const char* title);

// A vertical, scrolling, single-selection list.
class ListView {
 public:
  using LabelFn = const char* (*)(const void* ctx, int index);

  void reset() { selected_ = top_ = 0; }
  int selected() const { return selected_; }
  void select(int index) { selected_ = index; }

  // Move the selection by `delta`, wrapping. Returns true if it changed.
  bool move(int delta, int count);

  void render(GfxRenderer& r, const layout::Rect& area, const char* title, int count, LabelFn label, const void* ctx);

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
  bool canScrollUp() const { return top_ > 0; }
  bool canScrollDown() const { return top_ < maxTop_; }

  void render(GfxRenderer& r, const layout::Rect& area, const char* title, const Row* rows, int count);

 private:
  int top_ = 0;
  int maxTop_ = 0;  // from the last render
};

// Thin scroll indicator along the right edge of `area`.
void drawScrollbar(GfxRenderer& r, const layout::Rect& area, int top, int visible, int count);

}  // namespace ui
