#include "Input.h"

#include <Logging.h>

namespace {

struct Binding {
  uint8_t button;
  Action action;
};

// Held in LandscapeCounterClockwise the four front keys run down the right
// edge. Portrait left-to-right order is Back, Confirm, Left, Right, so
// top-to-bottom it is Right, Left, Confirm, Back. The X4C side keys land on the
// top edge (BTN_DOWN, next to power) and bottom edge (BTN_UP, unassigned).
constexpr Binding kBindings[] = {
    {HalGPIO::BTN_RIGHT, Action::Back},    {HalGPIO::BTN_LEFT, Action::Select},
    {HalGPIO::BTN_CONFIRM, Action::Up},    {HalGPIO::BTN_BACK, Action::Down},
    {HalGPIO::BTN_DOWN, Action::ToggleChrome},
};

}  // namespace

namespace input {

void begin() { gpio.beginAsyncInput(); }

Action next(bool& anyPress) {
  uint8_t button;
  while (gpio.popPress(button)) {
    anyPress = true;
    for (const auto& b : kBindings) {
      if (b.button == button) {
        LOG_DBG("INPUT", "button %u -> action %u", button, static_cast<unsigned>(b.action));
        return b.action;
      }
    }
  }
  return Action::None;
}

int keySlot(const Action action) {
  switch (action) {
    case Action::Back:
      return 0;
    case Action::Select:
      return 1;
    case Action::Up:
      return 2;
    case Action::Down:
      return 3;
    default:
      return -1;
  }
}

uint8_t heldKeys() {
  uint8_t mask = 0;
  for (const auto& b : kBindings) {
    const int slot = keySlot(b.action);
    if (slot >= 0 && gpio.isPressed(b.button)) mask |= 1 << slot;
  }
  return mask;
}

}  // namespace input
