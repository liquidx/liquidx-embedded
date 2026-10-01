// The board and the keys, for a computer: a fixed clock and battery.
#include <cstdio>

#include "Sim.h"
#include "hal/Board.h"
#include "shell/Input.h"

namespace sim {
uint8_t heldKeys = 0;
int battery = 82;
bool charging = false;
}  // namespace sim

namespace board {

void begin() {}
void present(Gfx&) {}
uint32_t uptimeMs() {
  static uint32_t ms = 0;
  return ms += 1000;  // each look at the clock is a second later
}
void setBrightness(int) {}

bool now(DateTime& out) {
  out = {2026, 9, 23, 14, 32, 0, 3};  // Wed 23 Sep, 14:32, as the X4's design
  return true;
}
bool setTime(const DateTime&) { return false; }

int batteryPercent() { return sim::battery; }
bool charging() { return sim::charging; }
bool onUsb() { return sim::charging; }
void sleep() {}

void about(About& out) {
  snprintf(out.chip, sizeof(out.chip), "ESP32-S3 rev 2");
  snprintf(out.flash, sizeof(out.flash), "8 MB");
  snprintf(out.mac, sizeof(out.mac), "AA:BB:CC:DD:1A:2B");
  snprintf(out.memory, sizeof(out.memory), "212 KB free");
}

}  // namespace board

namespace input {

void begin() {}
void poll() {}
Action next(bool&) { return Action::None; }
int keySlot(Action) { return -1; }
uint8_t heldKeys() { return sim::heldKeys; }

}  // namespace input
