#pragma once

#include "../Gfx.h"
#include "../ui/Widgets.h"
#include "App.h"

// Home shows the date, a large clock and a vertical list of apps. Opening an
// app gives it a page card on top of home; Back goes back a page, and from an
// app's top level returns home.
class Shell {
 public:
  static constexpr int kMaxApps = 8;
  // Puts the canvas on the panel.
  using Present = void (*)(Gfx& canvas);

  // `present` may be null (sim/): frames stay in the canvas.
  Shell(Gfx& canvas, Present present) : canvas_(canvas), present_(present) {}

  void addApp(App* app);
  // Draw the first screen: home.
  void begin();

  // Apply one action to shell/app state. Drawing is deferred to flush() so a
  // burst of queued presses costs a single repaint (transitions aside).
  void dispatch(Action action);

  // Repaint if anything changed since the last flush. Returns true if it drew.
  // When only the held keys changed, repaints just the gutter.
  bool flush();

  // Call from the main loop: marks home dirty when the clock's minute changes,
  // and lets the open app do deferred work once its page is on screen.
  void tick();

  // True while the open app has background work; the main loop then runs
  // flat out instead of idling.
  bool busy() const { return current_ != nullptr && current_->busy(); }

  // Pass-through to the open app (see App).
  bool takeAppActivity() { return current_ != nullptr && current_->takeActivity(); }
  App* currentApp() const { return current_; }

  // Mark the screen dirty.
  void invalidate() { dirty_ = true; }

 private:
  int depth() const;
  void drawHome(const layout::Rect& area);
  void drawCardStack(int depth, int slide);
  void drawGutter();
  void drawScreen(int slide = 0);
  void slide(const int* offsets, int count);
  void show(bool resting);

  Gfx& canvas_;
  Present present_;
  App* apps_[kMaxApps] = {};
  int appCount_ = 0;
  App* current_ = nullptr;  // nullptr: home
  int selected_ = 0;        // home row
  int homeScroll_ = 0;      // pixels the home page is scrolled up
  bool chrome_ = true;
  uint8_t keysShown_ = 0;  // gutter buttons drawn lit: the keys held down
  bool dirty_ = false;
  int shownMinute_ = -1;  // minute of day on screen, -1 when the clock isn't set
  int shownBattery_ = -1;
  unsigned long lastPollMs_ = 0;
};
