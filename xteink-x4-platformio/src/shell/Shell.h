#pragma once

#include <GfxRenderer.h>
#include <Rtc.h>

#include "../ui/Widgets.h"
#include "App.h"

// Home shows the date, a large clock and a vertical list of apps. Opening an
// app gives it a page card on top of home; the first front key goes back a
// page, and from an app's top level returns home.
class Shell {
 public:
  static constexpr int kMaxApps = 8;

  Shell(GfxRenderer& renderer, Rtc& rtc) : renderer_(renderer), rtc_(rtc) {}

  void addApp(App* app);
  void begin();

  // Apply one action to shell/app state. Drawing is deferred to flush() so a
  // burst of queued presses costs a single refresh.
  void dispatch(Action action);

  // Repaint if anything changed since the last flush. Returns true if it drew.
  bool flush();

  // Call from the main loop: marks home dirty when the clock's minute changes.
  void tick();

  // Mark the screen dirty. `clean` asks for a half refresh.
  void invalidate(bool clean = false);

  // Before auto-sleep: repaint the current page with chrome hidden and a
  // paused badge in the bottom-right corner, to stay on the panel while asleep.
  void showPaused();

 private:
  int depth() const;
  int clockMinute() const;
  void drawHome(const layout::Rect& area);
  void drawCardStack(int depth, int slide) const;
  void drawGutter() const;
  void drawPausedBadge() const;
  void drawScreen(int slide = 0);
  void drawSlideFrame(int left);
  void redraw(bool clean);
  void present(bool clean);

  GfxRenderer& renderer_;
  Rtc& rtc_;
  App* apps_[kMaxApps] = {};
  int appCount_ = 0;
  App* current_ = nullptr;  // nullptr: home
  int selected_ = 0;        // home row
  bool chrome_ = true;
  int fastSinceClean_ = 0;
  bool dirty_ = false;
  bool dirtyClean_ = false;
  int shownMinute_ = -1;  // minute of day on screen, -1 when the clock isn't set
  unsigned long lastClockPollMs_ = 0;
};
