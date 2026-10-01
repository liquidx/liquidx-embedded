#pragma once

#include "Action.h"

namespace input {

void begin();

// Read the keys (M5.update()). Call once per main loop pass, then drain next().
void poll();

// Pop the next action. Returns Action::None when there are none left.
// `anyPress` is set if a key was pressed, for inactivity tracking.
Action next(bool& anyPress);

// Gutter button (layout::kKeyA or kKeyB) an action's key is drawn as, or -1.
int keySlot(Action action);

// Keys held down right now: bit layout::kKeyB, bit layout::kKeyA.
uint8_t heldKeys();

}  // namespace input
