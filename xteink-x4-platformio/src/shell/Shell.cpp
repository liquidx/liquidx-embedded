#include "Shell.h"

#include <Arduino.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "../Fonts.h"
#include "../Settings.h"

namespace {

// Home screen geometry (docs/design/01-home.png).
constexpr int kHomeRight = 37;  // rows stop this far from the card edge
constexpr int kPillY = 32;
constexpr int kClockBaseline = 242;
constexpr int kRowsY = 320;
constexpr int kRowsVisible = 2;

constexpr unsigned long kClockPollMs = 1000;

// Card transitions are extra frames with the top card shifted left by each
// offset in turn: on entering a page, the new card arriving from the left and
// easing into place; on Back, the leaving card, as if sliding away to the left.
// Each frame costs one fast refresh, but no re-render (see Shell::shiftCard).
constexpr int kSlideIn[] = {64, 16};
constexpr int kSlideOut[] = {10};

constexpr int maxSlide() {
  int m = 0;
  for (const int px : kSlideIn) m = std::max(m, px);
  for (const int px : kSlideOut) m = std::max(m, px);
  return m;
}

// Frames are shifted in the framebuffer, where logical x is panel x (1 bit a
// pixel, MSB first). The card zone left of the gutter is whole bytes, and each
// row's leftmost pixels are kept aside so later frames can shift back right.
constexpr bool kPanelNative = layout::kOrientation == GfxRenderer::LandscapeCounterClockwise;
static_assert(layout::kGutterX % 8 == 0);
constexpr int kCardBytes = layout::kGutterX / 8;
constexpr int kSavedBytes = (maxSlide() + 7) / 8;
uint8_t savedLeft[layout::kScreenH][kSavedBytes];

// dst pixel x = src pixel x + shift over an n-byte row; pixels from outside the
// row are white. dst and src must not overlap.
void shiftRow(uint8_t* dst, const uint8_t* src, const int n, const int shift) {
  const int q = shift >> 3;  // floor, for negative shifts too
  const int r = shift & 7;
  const auto at = [&](const int i) -> unsigned { return i >= 0 && i < n ? src[i] : 0xFF; };
  for (int i = 0; i < n; i++) {
    dst[i] = static_cast<uint8_t>((at(i + q) << r) | (at(i + q + 1) >> (8 - r)));
  }
}

const char* const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

}  // namespace

void Shell::addApp(App* app) {
  if (appCount_ < kMaxApps) apps_[appCount_++] = app;
}

void Shell::begin() {
  rtc_.begin();
  redraw(true);
}

void Shell::invalidate(const bool clean) {
  dirty_ = true;
  dirtyClean_ = dirtyClean_ || clean;
}

bool Shell::flush() {
  if (!dirty_) {
    // The framebuffer still holds what's on the panel: touch up the gutter.
    if (!chrome_ || litKeys() == keysShown_) return false;
    redrawKeys();
    return true;
  }
  const bool clean = dirtyClean_;
  dirty_ = dirtyClean_ = false;
  redraw(clean);
  return true;
}

int Shell::clockMinute() const {
  Rtc::DateTime now;
  if (!rtc_.now(now)) return -1;
  return now.hour * 60 + now.minute;
}

void Shell::tick() {
  if (current_ != nullptr) {
    if (dirty_) return;  // let the page draw first
    const Result result = current_->tick();
    if (result != Result::Ignored) invalidate(result == Result::CleanRedraw);
    return;
  }
  const unsigned long now = millis();
  if (now - lastClockPollMs_ < kClockPollMs) return;
  lastClockPollMs_ = now;
  if (clockMinute() != shownMinute_) invalidate();
}

void Shell::dispatch(const Action action) {
  if (action == Action::None) return;

  // Show the key pressed on the next frame, even if it's released by then.
  const int key = input::keySlot(action);
  if (chrome_ && key >= 0) keysFlash_ |= 1 << key;

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
          slide(kSlideIn, std::size(kSlideIn), true);
        }
        break;
      default:
        break;
    }
    return;
  }

  if (action == Action::Back) slide(kSlideOut, std::size(kSlideOut), false);

  if (action == Action::Back && current_->depth() == 0) {
    current_ = nullptr;
    invalidate();
    return;
  }

  const int before = depth();
  const Result result = current_->handle(action);
  if (result == Result::Ignored) return;
  if (depth() > before) {
    slide(kSlideIn, std::size(kSlideIn), true, result == Result::CleanRedraw);
  } else {
    invalidate(result == Result::CleanRedraw);
  }
}

int Shell::depth() const {
  if (current_ == nullptr) return 0;
  return std::min(layout::kMaxDepth, 1 + current_->depth());
}

uint8_t Shell::litKeys() const { return input::heldKeys() | keysFlash_; }

// Show the current state with the top card shifted left by each of `offsets`
// in turn. With `settle`, then show it in place as a resting page (a half
// refresh if `clean`); otherwise the caller changes state and invalidates.
void Shell::slide(const int* offsets, const int count, const bool settle, const bool clean) {
  // Only with chrome: without it there is no card edge to move.
  if (!chrome_) {
    if (settle) invalidate(clean);
    return;
  }
  uint8_t* fb = renderer_.getFrameBuffer();
  if (!kPanelNative || fb == nullptr) {
    for (int i = 0; i < count; i++) {
      drawScreen(offsets[i]);
      present(Refresh::Frame);
    }
    if (settle) invalidate(clean);
    return;
  }

  // Render the page in place once, unless it's already what's on the panel.
  if (settle || dirty_) drawScreen();
  const int stride = renderer_.getDisplayWidthBytes();
  for (int y = 0; y < layout::kScreenH; y++) memcpy(savedLeft[y], fb + y * stride, kSavedBytes);

  int shown = 0;
  for (int i = 0; i < count; i++) {
    shiftCard(shown, offsets[i]);
    shown = offsets[i];
    present(Refresh::Frame);
  }
  if (!settle) return;

  shiftCard(shown, 0);
  const bool half = clean || dirtyClean_;
  dirty_ = dirtyClean_ = false;
  present(half ? Refresh::Clean : Refresh::Page);
}

// The framebuffer holds the page with the top card `from` pixels left of its
// place, and savedLeft its left edge in place. Redraw it `to` pixels left, the
// same as drawScreen(to) but without rendering the page again.
void Shell::shiftCard(const int from, const int to) {
  uint8_t* fb = renderer_.getFrameBuffer();
  const int stride = renderer_.getDisplayWidthBytes();
  uint8_t page[kCardBytes];
  for (int y = 0; y < layout::kScreenH; y++) {
    uint8_t* row = fb + y * stride;
    shiftRow(page, row, kCardBytes, -from);  // back in place...
    memcpy(page, savedLeft[y], kSavedBytes);  // ...with the left edge `from` lost
    shiftRow(row, page, kCardBytes, to);
  }
  // Clear what the shift dragged in (lower card edges, the old right edge),
  // then redraw the stack and gutter where they belong.
  const int right = layout::cardWidth(depth()) - to;
  renderer_.fillRect(right, 0, layout::kScreenW - right, layout::kScreenH, false);
  drawCardStack(depth(), to);
  drawGutter();
}

void Shell::showPaused() {
  chrome_ = false;
  drawScreen();
  drawPausedBadge();
  renderer_.displayBuffer(HalDisplay::HALF_REFRESH);
}

void Shell::redraw(const bool clean) {
  drawScreen();
  present(clean ? Refresh::Clean : Refresh::Page);
}

// Only the pressed keys changed: repaint the gutter over the last frame.
void Shell::redrawKeys() {
  renderer_.fillRect(layout::kGutterX, 0, layout::kGutterW, layout::kScreenH, false);
  drawGutter();
  present(Refresh::Frame);
}

// `slide` shifts the top card (content and edge) left by that many pixels.
void Shell::drawScreen(const int slide) {
  renderer_.clearScreen();
  auto area = chrome_ ? layout::cardArea(depth()) : layout::kFullScreen;
  area.x -= slide;
  const int clipX = std::max(0, area.x);  // pixels left of the panel aren't drawable
  renderer_.setClipRect(clipX, area.y, area.x + area.w - clipX, area.h);
  if (current_ == nullptr) {
    drawHome(area);
  } else {
    current_->render(renderer_, area, chrome_);
  }
  renderer_.setClipRect(0, 0, layout::kScreenW, layout::kScreenH);
  if (chrome_) {
    drawCardStack(depth(), slide);
    drawGutter();
  }
}

void Shell::drawHome(const layout::Rect& area) {
  Rtc::DateTime now;
  const bool haveTime = rtc_.now(now);
  shownMinute_ = haveTime ? now.hour * 60 + now.minute : -1;
  const int x = area.x + layout::kMarginLeft;

  char date[16];
  if (haveTime) {
    snprintf(date, sizeof(date), "%s %u %s", kWeekdays[now.weekday % 7], now.day, kMonths[(now.month + 11) % 12]);
  } else {
    snprintf(date, sizeof(date), "Clock not set");
  }
  ui::drawPill(renderer_, fonts::SMALL_15, x, area.y + kPillY, date);

  char clock[8];
  if (!haveTime) {
    snprintf(clock, sizeof(clock), "--:--");
  } else if (settings::value(settings::kClock) == 12) {
    snprintf(clock, sizeof(clock), "%u:%02u", now.hour % 12 == 0 ? 12 : now.hour % 12, now.minute);
  } else {
    snprintf(clock, sizeof(clock), "%02u:%02u", now.hour, now.minute);
  }
  ui::drawTextAt(renderer_, fonts::DISPLAY_136, x, area.y + kClockBaseline, clock);

  // App rows, a page of kRowsVisible at a time.
  const auto& style = ui::kHomeRow;
  const int w = area.w - layout::kMarginLeft - kHomeRight;
  const int first = selected_ / kRowsVisible * kRowsVisible;
  for (int i = first; i < std::min(appCount_, first + kRowsVisible); i++) {
    char count[12] = "";
    const int n = apps_[i]->itemCount();
    if (n >= 0) snprintf(count, sizeof(count), "%d", n);
    const layout::Rect row{x, area.y + kRowsY + (i - first) * style.pitch, w, style.height};
    ui::drawRow(renderer_, row, style, apps_[i]->icon(), apps_[i]->name(), count, i == selected_);
  }
}

void Shell::drawCardStack(const int depth, const int slide) const {
  // Cards bleed off the top, bottom and left, so only the right corners show.
  constexpr int R = layout::kCardRadius;
  constexpr int kBleed = R + 2;
  const int topW = layout::cardWidth(depth) - slide;
  // Square off whatever the top page drew outside its rounded corners...
  renderer_.maskRoundedRectOutsideCorners(-kBleed, -1, topW + kBleed, layout::kScreenH + 2, R, Color::White);
  // ...then outline every card in the stack; lower ones peek out to the right.
  for (int d = 0; d <= depth; d++) {
    const int w = d == depth ? topW : layout::cardWidth(d);
    renderer_.drawRoundedRect(-kBleed, -1, w + kBleed, layout::kScreenH + 2, 1, R, true);
  }
}

void Shell::drawGutter() {
  // Battery gauge above the first key.
  constexpr int kBattX = 751, kBattY = 24, kBattW = 26, kBattH = 14;
  renderer_.drawRoundedRect(kBattX, kBattY, kBattW, kBattH, 1, 2, true);
  renderer_.fillRect(kBattX + kBattW + 1, kBattY + 5, 2, 4);
  const int level = std::clamp<int>(powerManager.getBatteryPercentage(), 0, 100);
  renderer_.fillRect(kBattX + 3, kBattY + 3, (kBattW - 6) * level / 100, kBattH - 6);

  // All four keys are always shown, even when inert on this page. A key held
  // down (or pressed since the last frame) shows inverted: an outlined white
  // button with a black glyph.
  const uint8_t lit = litKeys();
  keysShown_ = lit;
  constexpr int R = layout::kKeyRadius;
  const int cx = layout::kKeyCenterX;
  for (int i = 0; i < layout::kKeyCount; i++) {
    const auto& slot = layout::kKeySlots[i];
    const int cy = slot.y + slot.h / 2;
    const bool inverted = lit & (1 << i);
    if (inverted) {
      renderer_.drawRoundedRect(cx - R, cy - R, R * 2, R * 2, 2, R, true);
    } else {
      renderer_.fillRoundedRect(cx - R, cy - R, R * 2, R * 2, R, Color::Black);
    }

    switch (i) {
      case 0: {  // ◀ back
        const int xs[] = {cx + 5, cx + 5, cx - 5};
        const int ys[] = {cy - 5, cy + 6, cy};
        renderer_.fillPolygon(xs, ys, 3, inverted);
        break;
      }
      case 1:  // ● select
        renderer_.fillRoundedRect(cx - 6, cy - 5, 12, 12, 6, inverted ? Color::Black : Color::White);
        break;
      case 2: {  // ▲ up
        const int xs[] = {cx - 5, cx + 5, cx};
        const int ys[] = {cy + 6, cy + 6, cy - 4};
        renderer_.fillPolygon(xs, ys, 3, inverted);
        break;
      }
      default: {  // ▼ down
        const int xs[] = {cx - 5, cx + 5, cx};
        const int ys[] = {cy - 5, cy - 5, cy + 5};
        renderer_.fillPolygon(xs, ys, 3, inverted);
        break;
      }
    }
  }
}

void Shell::drawPausedBadge() const {
  // A key button's size, in the gutter's column, inset from the bottom edge by
  // the same margin the buttons keep from the right edge.
  constexpr int R = layout::kKeyRadius;
  constexpr int cx = layout::kKeyCenterX;
  constexpr int cy = layout::kScreenH - (layout::kScreenW - layout::kKeyCenterX);
  renderer_.fillRoundedRect(cx - R, cy - R, R * 2, R * 2, R, Color::Black);
  // ❚❚
  renderer_.fillRect(cx - 6, cy - 6, 4, 13, false);
  renderer_.fillRect(cx + 2, cy - 6, 4, 13, false);
}

void Shell::present(const Refresh refresh) {
  keysFlash_ = 0;  // the keys pressed are on this frame
  if (refresh == Refresh::Frame) {
    renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  // Force a half refresh every N fast pages to clear ghosting; 0 means never.
  const int every = settings::value(settings::kRefresh);
  if (refresh == Refresh::Clean || (every > 0 && ++fastSinceClean_ >= every)) {
    fastSinceClean_ = 0;
    renderer_.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
