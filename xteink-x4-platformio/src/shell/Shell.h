#pragma once

#include <GfxRenderer.h>

#include "../ui/Widgets.h"
#include "App.h"

// Home is a full-screen list of apps. Opening one gives it the whole content
// area; the first front key returns home from an app's top level.
class Shell {
 public:
  static constexpr int kMaxApps = 8;

  explicit Shell(GfxRenderer& renderer) : renderer_(renderer) {}

  void addApp(App* app);
  void begin();

  // Apply one action to shell/app state. Drawing is deferred to flush() so a
  // burst of queued presses costs a single refresh.
  void dispatch(Action action);

  // Repaint if anything changed since the last flush. Returns true if it drew.
  bool flush();

  // Mark the screen dirty. `clean` asks for a half refresh.
  void invalidate(bool clean = false);

 private:
  layout::Rect contentArea() const;
  KeyHints currentHints() const;
  void drawGutter() const;
  void redraw(bool clean);
  void present(bool clean);

  GfxRenderer& renderer_;
  App* apps_[kMaxApps] = {};
  int appCount_ = 0;
  App* current_ = nullptr;  // nullptr: home list
  ui::ListView home_;
  bool chrome_ = true;
  int fastSinceClean_ = 0;
  bool dirty_ = false;
  bool dirtyClean_ = false;
};
