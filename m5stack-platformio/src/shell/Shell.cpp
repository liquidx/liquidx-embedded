#include "Shell.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "../Fonts.h"
#include "../Settings.h"
#include "../Theme.h"
#include "../hal/Board.h"
#include "Input.h"

namespace {

// Home screen geometry.
constexpr int kHomeRight = 9;  // rows stop this far from the card edge
constexpr int kPillY = 7;
constexpr int kClockBaseline = 67;
constexpr int kRowsY = 77;
constexpr int kBottomMargin = 7;  // below the last row on screen

// Home is one tall page (date, clock, then every app row) that scrolls as a
// whole to keep the selected row on screen.

// How often home checks the clock and the battery.
constexpr uint32_t kPollMs = 1000;

// Card transitions are extra frames with the top card shifted left by each
// offset in turn: on entering a page, the new card arriving from the left and
// easing into place; on Back, the leaving card sliding away to the left. A
// frame takes about 15 ms to reach the LCD, which paces them.
constexpr int kSlideIn[] = {72, 40, 20, 8, 2};
constexpr int kSlideOut[] = {6, 16};

// The top card's rounded right edge, as it was before the page drew over it:
// a strip kCardRadius wide, in the sprite's own 16-bit pixels.
constexpr int kEdgeW = layout::kCardRadius;
uint16_t savedEdge[layout::kScreenH][kEdgeW];

// True if the pixel `fromRight` columns in from a card's right edge, on row
// `y`, is the card's rim or outside its rounded corners: the page mustn't
// keep what it drew there.
bool onCardEdge(const int fromRight, const int y) {
  constexpr int R = layout::kCardRadius;
  if (fromRight == 0) return true;  // the rim
  const int cy = y < R ? R : (y >= layout::kScreenH - R ? layout::kScreenH - R : -1);
  if (cy < 0) return false;
  // Distance of the pixel's centre from the corner's centre, doubled to stay
  // in integers. Keep the page only well inside the arc, so the rim survives.
  const int dx = 2 * (R - fromRight) - 1;
  const int dy = 2 * (y - cy) + 1;
  constexpr int kInner = 2 * R - 3;
  return dx * dx + dy * dy > kInner * kInner;
}

const char* const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

}  // namespace

void Shell::addApp(App* app) {
  if (appCount_ < kMaxApps) apps_[appCount_++] = app;
}

void Shell::begin() {
  drawScreen();
  show(true);
}

bool Shell::flush() {
  if (!dirty_) {
    // The canvas still holds what's on the panel: touch up the gutter.
    if (!chrome_ || input::heldKeys() == keysShown_) return false;
    canvas_.fillRect(layout::kGutterX, 0, layout::kGutterW, layout::kScreenH, theme::kBackdrop);
    drawGutter();
    show(false);
    return true;
  }
  dirty_ = false;
  drawScreen();
  show(true);
  return true;
}

void Shell::tick() {
  if (current_ != nullptr) {
    if (dirty_) return;  // let the page draw first
    if (current_->tick() != Result::Ignored) invalidate();
    return;
  }
  const uint32_t now = board::uptimeMs();
  if (now - lastPollMs_ < kPollMs) return;
  lastPollMs_ = now;
  board::DateTime time;
  const int minute = board::now(time) ? time.hour * 60 + time.minute : -1;
  if (minute != shownMinute_ || board::batteryPercent() != shownBattery_) invalidate();
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
        if (appCount_ > 1) {
          selected_ = (selected_ + (action == Action::Up ? -1 : 1) + appCount_) % appCount_;
          invalidate();
        }
        break;
      case Action::Select:
        if (appCount_ > 0) {
          current_ = apps_[selected_];
          current_->onOpen();
          slide(kSlideIn, std::size(kSlideIn));
          invalidate();
        }
        break;
      default:
        break;
    }
    return;
  }

  if (action == Action::Back) slide(kSlideOut, std::size(kSlideOut));

  if (action == Action::Back && current_->depth() == 0) {
    current_->onClose();
    current_ = nullptr;
    invalidate();
    return;
  }

  const int before = depth();
  if (current_->handle(action) == Result::Ignored) return;
  if (depth() > before) slide(kSlideIn, std::size(kSlideIn));
  invalidate();
}

int Shell::depth() const {
  if (current_ == nullptr) return 0;
  return std::min(layout::kMaxDepth, 1 + current_->depth());
}

// Show the current state with the top card shifted left by each of `offsets`
// in turn. The caller then invalidates, so the page comes to rest in place.
void Shell::slide(const int* offsets, const int count) {
  // Only with chrome: without it there is no card edge to move. And only on a
  // panel: nobody would see the frames.
  if (!chrome_ || present_ == nullptr) return;
  for (int i = 0; i < count; i++) {
    drawScreen(offsets[i]);
    show(false);
  }
}

void Shell::show(const bool resting) {
  if (present_ != nullptr) present_(canvas_);
  if (resting && current_ != nullptr) current_->presented();
}

// `slide` shifts the top card (content and edge) left by that many pixels.
void Shell::drawScreen(const int slide) {
  const int d = depth();
  auto area = chrome_ ? layout::cardArea(d) : layout::kFullScreen;
  area.x -= slide;

  canvas_.clearClipRect();
  canvas_.fillScreen(chrome_ ? theme::kBackdrop : theme::kCard);
  if (chrome_) drawCardStack(d, slide);

  // Keep the card's rim and what shows past its corners, to put back over
  // whatever the page draws there (a Bluetooth frame fills its whole area).
  auto* pixels = static_cast<uint16_t*>(canvas_.getBuffer());
  const int edgeX = area.x + area.w - kEdgeW;
  const bool keepEdge = chrome_ && pixels != nullptr && edgeX >= 0;
  if (keepEdge) {
    for (int y = 0; y < layout::kScreenH; y++) {
      memcpy(savedEdge[y], pixels + y * layout::kScreenW + edgeX, sizeof(savedEdge[y]));
    }
  }

  const int clipX = std::max(0, area.x);  // pixels left of the panel aren't drawable
  canvas_.setClipRect(clipX, area.y, area.x + area.w - clipX, area.h);
  if (current_ == nullptr) {
    drawHome(area);
  } else {
    current_->render(canvas_, area, chrome_);
  }
  canvas_.clearClipRect();

  if (keepEdge) {
    for (int y = 0; y < layout::kScreenH; y++) {
      uint16_t* row = pixels + y * layout::kScreenW + edgeX;
      for (int x = 0; x < kEdgeW; x++) {
        if (onCardEdge(kEdgeW - 1 - x, y)) row[x] = savedEdge[y][x];
      }
    }
  }
  if (chrome_) drawGutter();
}

void Shell::drawHome(const layout::Rect& area) {
  board::DateTime now;
  const bool haveTime = board::now(now);
  shownMinute_ = haveTime ? now.hour * 60 + now.minute : -1;
  const int x = area.x + layout::kMarginLeft;

  // Scroll just enough to show the selected row: down when it would pass the
  // bottom margin, back up when it would rise above where the rows start.
  const auto& style = ui::kHomeRow;
  const int rowTop = kRowsY + selected_ * style.pitch;
  const int viewBottom = area.h - kBottomMargin;
  if (rowTop + style.height - homeScroll_ > viewBottom) homeScroll_ = rowTop + style.height - viewBottom;
  if (rowTop - homeScroll_ < kRowsY) homeScroll_ = rowTop - kRowsY;
  homeScroll_ = std::max(0, homeScroll_);
  const int top = area.y - homeScroll_;

  char date[16];
  if (haveTime) {
    snprintf(date, sizeof(date), "%s %u %s", kWeekdays[now.weekday % 7], now.day, kMonths[(now.month + 11) % 12]);
  } else {
    snprintf(date, sizeof(date), "Clock not set");
  }
  ui::drawPill(canvas_, fonts::SMALL_12, x, top + kPillY, date, theme::kSurface, theme::kText);

  char clock[8];
  if (!haveTime) {
    snprintf(clock, sizeof(clock), "--:--");
  } else if (settings::value(settings::kClock) == 12) {
    snprintf(clock, sizeof(clock), "%u:%02u", now.hour % 12 == 0 ? 12 : now.hour % 12, now.minute);
  } else {
    snprintf(clock, sizeof(clock), "%02u:%02u", now.hour, now.minute);
  }
  ui::drawTextAt(canvas_, fonts::DISPLAY_46, x, top + kClockBaseline, clock);

  // Every row; the clip rect drops the ones scrolled off screen.
  const int w = area.w - layout::kMarginLeft - kHomeRight;
  for (int i = 0; i < appCount_; i++) {
    char count[12] = "";
    const int n = apps_[i]->itemCount();
    if (n >= 0) snprintf(count, sizeof(count), "%d", n);
    const layout::Rect row{x, top + kRowsY + i * style.pitch, w, style.height};
    ui::drawRow(canvas_, row, style, apps_[i]->icon(), apps_[i]->name(), count, i == selected_);
  }
}

void Shell::drawCardStack(const int depth, const int slide) {
  // Cards bleed off the top, bottom and left, so only the right corners show.
  // Bottom of the stack first; lower ones peek out to the right, each with a
  // 1px lighter rim.
  constexpr int R = layout::kCardRadius;
  for (int d = 0; d <= depth; d++) {
    const bool top = d == depth;
    const int w = layout::cardWidth(d) - (top ? slide : 0);
    if (w <= 0) continue;
    canvas_.fillSmoothRoundRect(-R, 0, w + R, layout::kScreenH, R, theme::kCardEdge);
    canvas_.fillSmoothRoundRect(-R, 0, w + R - 1, layout::kScreenH, R, top ? theme::kCard : theme::kCardBelow);
  }
}

void Shell::drawGutter() {
  // Both keys are always shown, even when inert on this page. A key held down
  // shows in the accent colour.
  const uint8_t lit = input::heldKeys();
  keysShown_ = lit;
  constexpr int R = layout::kKeyRadius;
  constexpr int cx = layout::kKeyCenterX;

  // B, the top key: a circle, ◀ back.
  {
    const bool held = lit & (1 << layout::kKeyB);
    constexpr int cy = layout::kKeyBCenterY;
    canvas_.fillSmoothCircle(cx, cy, R, held ? theme::accent() : theme::kSurface);
    canvas_.fillTriangle(cx + 2, cy - 4, cx + 2, cy + 4, cx - 3, cy, held ? theme::kOnAccent : theme::kText);
  }

  // A, the front key: a tall bar. ▼ next on one press, ● select on two.
  {
    const bool held = lit & (1 << layout::kKeyA);
    const theme::Color glyph = held ? theme::kOnAccent : theme::kText;
    constexpr int y = layout::kKeyBarY, h = layout::kKeyBarH;
    canvas_.fillSmoothRoundRect(cx - R, y, R * 2 + 1, h, R, held ? theme::accent() : theme::kSurface);
    const int nextY = y + h / 3, selectY = y + h * 2 / 3 + 2;
    canvas_.fillTriangle(cx - 4, nextY - 3, cx + 4, nextY - 3, cx, nextY + 2, glyph);
    canvas_.fillSmoothCircle(cx, selectY, 3, glyph);
  }

  // Battery gauge at the bottom.
  constexpr int kBattW = 15, kBattH = 8;
  constexpr int kBattX = layout::kKeyCenterX - kBattW / 2 - 1, kBattY = layout::kScreenH - kBattH - 6;
  const int level = std::clamp(board::batteryPercent(), 0, 100);
  shownBattery_ = level;
  theme::Color fill = theme::kText;
  if (board::charging()) {
    fill = theme::kGood;
  } else if (level <= 15) {
    fill = theme::kBad;
  } else if (level <= 30) {
    fill = theme::kWarn;
  }
  canvas_.drawRoundRect(kBattX, kBattY, kBattW, kBattH, 2, theme::kTextDim);
  canvas_.fillRect(kBattX + kBattW, kBattY + 2, 2, kBattH - 4, theme::kTextDim);
  canvas_.fillRect(kBattX + 2, kBattY + 2, std::max(1, (kBattW - 4) * level / 100), kBattH - 4, fill);
}
