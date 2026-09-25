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
  // When only the held front keys changed, repaints just the gutter.
  bool flush();

  // Call from the main loop: marks home dirty when the clock's minute changes,
  // and lets the open app do deferred work once its page is on screen.
  void tick();

  // True while the open app has background work; the main loop then runs
  // flat out instead of idling.
  bool busy() const { return current_ != nullptr && current_->busy(); }

  // Mark the screen dirty. `clean` asks for a half refresh.
  void invalidate(bool clean = false);

  // Before auto-sleep: repaint the current page with chrome hidden and a
  // paused badge in the bottom-right corner, to stay on the panel while asleep.
  void showPaused();

 private:
  enum class Refresh : uint8_t {
    Page,   // a resting page: fast, with a half refresh every N (settings)
    Clean,  // a resting page: half refresh
    Frame,  // a transition frame or key change: always fast, never counted
  };

  int depth() const;
  int clockMinute() const;
  uint8_t litKeys() const;
  void drawHome(const layout::Rect& area);
  void drawCardStack(int depth, int slide) const;
  void drawGutter();
  void drawPausedBadge() const;
  void drawScreen(int slide = 0);
  void slide(const int* offsets, int count, bool settle, bool clean = false);
  void shiftCard(int from, int to);
  void redraw(bool clean);
  void redrawKeys();
  void present(Refresh refresh);

  GfxRenderer& renderer_;
  Rtc& rtc_;
  App* apps_[kMaxApps] = {};
  int appCount_ = 0;
  App* current_ = nullptr;  // nullptr: home
  int selected_ = 0;        // home row
  bool chrome_ = true;
  int fastSinceClean_ = 0;
  // Gutter buttons drawn inverted: keys held down, plus keys pressed since the
  // last resting page (a tap shorter than a refresh still shows).
  uint8_t keysFlash_ = 0;
  uint8_t keysShown_ = 0;
  bool dirty_ = false;
  bool dirtyClean_ = false;
  int shownMinute_ = -1;  // minute of day on screen, -1 when the clock isn't set
  unsigned long lastClockPollMs_ = 0;
};
