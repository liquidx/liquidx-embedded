#include "Input.h"

#include <M5Unified.h>

#include "Layout.h"

namespace {

// Bits of heldKeys(), as layout::kKeyB and kKeyA.
constexpr int kSlotB = layout::kKeyB, kSlotA = layout::kKeyA;

constexpr int kQueue = 4;
Action queue[kQueue];
int queued = 0;
bool pressed = false;
// A key held through boot (the one that woke the device) mustn't act: wait
// until both have been seen up.
bool armed = false;

void push(const Action action) {
  if (queued < kQueue) queue[queued++] = action;
}

}  // namespace

namespace input {

void begin() {}

void poll() {
  M5.update();
  if (!armed) {
    armed = !M5.BtnA.isPressed() && !M5.BtnB.isPressed();
    return;
  }
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) pressed = true;
  // Presses are counted, so one press acts once the time for a second has
  // passed without it. More than two count as two.
  if (M5.BtnA.wasDecideClickCount()) push(M5.BtnA.getClickCount() >= 2 ? Action::Select : Action::Down);
  if (M5.BtnB.wasDecideClickCount()) push(M5.BtnB.getClickCount() >= 2 ? Action::ToggleChrome : Action::Back);
}

Action next(bool& anyPress) {
  if (pressed) anyPress = true;
  pressed = false;
  if (queued == 0) return Action::None;
  const Action action = queue[0];
  for (int i = 1; i < queued; i++) queue[i - 1] = queue[i];
  queued--;
  return action;
}

int keySlot(const Action action) {
  switch (action) {
    case Action::Select:
    case Action::Down:
      return kSlotA;
    case Action::Back:
      return kSlotB;
    default:
      return -1;
  }
}

uint8_t heldKeys() {
  return (M5.BtnB.isPressed() ? 1 << kSlotB : 0) | (M5.BtnA.isPressed() ? 1 << kSlotA : 0);
}

}  // namespace input
