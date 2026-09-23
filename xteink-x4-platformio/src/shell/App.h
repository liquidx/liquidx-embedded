#pragma once

#include <GfxRenderer.h>

#include "Input.h"
#include "Layout.h"

// What an app wants the shell to do after handling an action.
enum class Result : uint8_t {
  Ignored,      // nothing changed
  Redraw,       // repaint with a fast refresh (menus, focus moves)
  CleanRedraw,  // repaint with a half refresh to clear ghosting (new image, new page)
};

// Labels for the front keys, top to bottom. nullptr means the key does nothing
// right now (drawn as a faint stub). The shell fills in `back` itself.
struct KeyHints {
  const char* back;
  const char* select;
  const char* up;
  const char* down;
};

// An app owns the whole content area while open. The shell owns the home
// list, the key-hint gutter, chrome visibility and refresh.
class App {
 public:
  virtual ~App() = default;

  virtual const char* name() const = 0;

  // Called each time the app is opened from home. Reset to the top level.
  virtual void onOpen() {}

  // Draw into `area` (already cleared to white). With `chrome` off, skip
  // titles, captions and other decoration: show just the content.
  virtual void render(GfxRenderer& r, const layout::Rect& area, bool chrome) = 0;

  // Select / Up / Down, plus Back while canGoBack() is true. Back at the top
  // level never reaches the app: the shell takes it as Home.
  virtual Result handle(Action action) = 0;

  // True once the user has drilled into a sub-screen; the first key becomes Back.
  virtual bool canGoBack() const { return false; }

  // Labels for Select / Up / Down in the current state (`back` is ignored).
  virtual KeyHints hints() const = 0;
};
