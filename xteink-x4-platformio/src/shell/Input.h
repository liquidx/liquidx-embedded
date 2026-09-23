#pragma once

#include <HalGPIO.h>

// Semantic actions. Apps only ever see these, never raw buttons, so the
// physical mapping lives in exactly one place (Input.cpp).
enum class Action : uint8_t {
  None,
  Back,          // front key 1: Home at an app's top level, Back once drilled in
  Select,        // front key 2
  Up,            // front key 3
  Down,          // front key 4
  ToggleChrome,  // top-edge side key: show/hide gutter, titles, captions
};

namespace input {

// Start background sampling. Buttons are polled on their own task so presses
// made while the main loop is blocked on a display refresh are queued, not lost.
void begin();

// Pop the next queued action. Returns Action::None when the queue is empty.
// `anyPress` is set if a button (including ones with no action, e.g. Power)
// was popped, for inactivity tracking.
Action next(bool& anyPress);

}  // namespace input
