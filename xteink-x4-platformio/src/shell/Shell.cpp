#include "Shell.h"

#include <HalPowerManager.h>

#include <cstdio>

#include "../Fonts.h"

namespace {

// Fast refreshes leave ghosting; force a half refresh after this many.
constexpr int kMaxFastRefreshes = 12;

const char* appName(const void* ctx, const int index) { return static_cast<App* const*>(ctx)[index]->name(); }

}  // namespace

void Shell::addApp(App* app) {
  if (appCount_ < kMaxApps) apps_[appCount_++] = app;
}

void Shell::begin() { redraw(true); }

void Shell::invalidate(const bool clean) {
  dirty_ = true;
  dirtyClean_ = dirtyClean_ || clean;
}

bool Shell::flush() {
  if (!dirty_) return false;
  const bool clean = dirtyClean_;
  dirty_ = dirtyClean_ = false;
  redraw(clean);
  return true;
}

void Shell::dispatch(const Action action) {
  if (action == Action::None) return;

  if (action == Action::ToggleChrome) {
    chrome_ = !chrome_;
    invalidate();
    return;
  }

  if (current_ == nullptr) {
    switch (action) {
      case Action::Up:
      case Action::Down:
        if (home_.move(action == Action::Up ? -1 : 1, appCount_)) invalidate();
        break;
      case Action::Select:
        if (appCount_ > 0) {
          current_ = apps_[home_.selected()];
          current_->onOpen();
          invalidate();
        }
        break;
      default:
        break;
    }
    return;
  }

  if (action == Action::Back && !current_->canGoBack()) {
    current_ = nullptr;
    invalidate();
    return;
  }

  const Result result = current_->handle(action);
  if (result != Result::Ignored) invalidate(result == Result::CleanRedraw);
}

layout::Rect Shell::contentArea() const { return chrome_ ? layout::kContentWithChrome : layout::kFullScreen; }

KeyHints Shell::currentHints() const {
  if (current_ == nullptr) {
    const bool canMove = appCount_ > 1;
    return {nullptr, appCount_ > 0 ? "Open" : nullptr, canMove ? "Up" : nullptr, canMove ? "Down" : nullptr};
  }
  KeyHints hints = current_->hints();
  hints.back = current_->canGoBack() ? "Back" : "Home";
  return hints;
}

void Shell::redraw(const bool clean) {
  renderer_.clearScreen();
  const auto area = contentArea();
  renderer_.setClipRect(area.x, area.y, area.w, area.h);
  if (current_ == nullptr) {
    home_.render(renderer_, area, chrome_ ? "Home" : nullptr, appCount_, appName, apps_);
  } else {
    current_->render(renderer_, area, chrome_);
  }
  renderer_.setClipRect(0, 0, layout::kScreenW, layout::kScreenH);
  if (chrome_) drawGutter();
  present(clean);
}

void Shell::drawGutter() const {
  // Battery above the first key.
  char battery[16];
  snprintf(battery, sizeof(battery), "%u%%", powerManager.getBatteryPercentage());
  const int batteryW = renderer_.getTextWidth(fonts::UI_10, battery);
  renderer_.drawText(fonts::UI_10, layout::kGutterX + (layout::kGutterW - batteryW) / 2, 22, battery);

  const KeyHints hints = currentHints();
  const char* labels[layout::kKeyCount] = {hints.back, hints.select, hints.up, hints.down};
  const bool homeIcon = current_ != nullptr && !current_->canGoBack();

  // Tabs run from just inside the gutter to the bezel edge, rounded on the side
  // facing the screen so they read as pointing at the key.
  const int tabX = layout::kGutterX + 4;
  const int tabW = layout::kScreenW - layout::kBezelRight - tabX;
  constexpr int kRadius = 10;
  constexpr int kIcon = 7;  // half-size of the glyph drawn in each tab

  for (int i = 0; i < layout::kKeyCount; i++) {
    const auto& slot = layout::kKeySlots[i];
    if (labels[i] == nullptr || labels[i][0] == '\0') {
      // Inactive key: a faint stub so the key position is still visible.
      renderer_.fillRectDither(layout::kScreenW - layout::kBezelRight - 6, slot.y + 20, 6, slot.h - 40,
                               Color::LightGray);
      continue;
    }

    renderer_.fillRoundedRect(tabX, slot.y, tabW, slot.h, kRadius, true, false, true, false, Color::Black);

    // Glyph per key, label per context. All glyphs are drawn white on the tab.
    const int cx = tabX + tabW / 2;
    const int cy = slot.y + 24;
    switch (i) {
      case 0:
        if (homeIcon) {
          // House: roof triangle over a square body.
          const int xs[] = {cx - kIcon - 1, cx + kIcon + 1, cx};
          const int ys[] = {cy - 1, cy - 1, cy - kIcon - 1};
          renderer_.fillPolygon(xs, ys, 3, false);
          renderer_.fillRect(cx - kIcon + 2, cy - 1, kIcon * 2 - 4, kIcon, false);
        } else {
          const int xs[] = {cx + kIcon, cx + kIcon, cx - kIcon};
          const int ys[] = {cy - kIcon, cy + kIcon, cy};
          renderer_.fillPolygon(xs, ys, 3, false);
        }
        break;
      case 1:
        renderer_.fillRoundedRect(cx - kIcon, cy - kIcon, kIcon * 2, kIcon * 2, kIcon, Color::White);
        break;
      case 2: {
        const int xs[] = {cx - kIcon, cx + kIcon, cx};
        const int ys[] = {cy + kIcon / 2, cy + kIcon / 2, cy - kIcon};
        renderer_.fillPolygon(xs, ys, 3, false);
        break;
      }
      default: {
        const int xs[] = {cx - kIcon, cx + kIcon, cx};
        const int ys[] = {cy - kIcon / 2, cy - kIcon / 2, cy + kIcon};
        renderer_.fillPolygon(xs, ys, 3, false);
        break;
      }
    }

    const auto text = renderer_.truncatedText(fonts::UI_10, labels[i], tabW - 6);
    const int textW = renderer_.getTextWidth(fonts::UI_10, text.c_str());
    renderer_.drawText(fonts::UI_10, cx - textW / 2, slot.y + 44, text.c_str(), false);
  }
}

void Shell::present(const bool clean) {
  if (clean || ++fastSinceClean_ >= kMaxFastRefreshes) {
    fastSinceClean_ = 0;
    renderer_.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
