#pragma once

#include <GfxRenderer.h>

#include "../ui/Widgets.h"
#include "Input.h"
#include "Layout.h"

// What an app wants the shell to do after handling an action.
enum class Result : uint8_t {
  Ignored,      // nothing changed
  Redraw,       // repaint with a fast refresh (menus, focus moves)
  CleanRedraw,  // repaint with a half refresh to clear ghosting (new image, new page)
};

// An app owns its page card while open. The shell owns home, the gutter, the
// card stack, chrome visibility and refresh.
class App {
 public:
  virtual ~App() = default;

  virtual const char* name() const = 0;

  // Glyph and item count shown on the app's home row. A negative count hides it.
  virtual ui::Icon icon() const { return ui::Icon::None; }
  virtual int itemCount() { return -1; }

  // Called each time the app is opened from home. Reset to the top level.
  virtual void onOpen() {}

  // Draw into `area` (already cleared to white and clipped): the top card, or
  // the whole screen with `chrome` off. With `chrome` off, skip titles,
  // captions and other decoration: show just the content.
  virtual void render(GfxRenderer& r, const layout::Rect& area, bool chrome) = 0;

  // Select / Up / Down, plus Back while depth() > 0. Back at the top level
  // never reaches the app: the shell takes it as Home.
  virtual Result handle(Action action) = 0;

  // How many pages the user has drilled in below the app's top level. Each one
  // adds a card to the visible back stack.
  virtual int depth() const { return 0; }

  // Called from the main loop while the app is open and its page is on
  // screen. For slow work that shouldn't hold up showing the page: do a
  // bounded slice per call, and report busy() until it's finished.
  virtual Result tick() { return Result::Ignored; }
  virtual bool busy() const { return false; }
};
