#pragma once

#include <cstddef>
#include <cstdint>

#include "../Gfx.h"

// The M5StickS3 itself: panel, power chip, clock. Board.cpp is the device;
// sim/ has a stand-in so the UI renders on a computer.
namespace board {

// M5.begin(), the panel's rotation, the clock.
void begin();
// Put a finished frame on the LCD.
void present(Gfx& canvas);
// Milliseconds since boot.
uint32_t uptimeMs();
// Backlight, 0–100.
void setBrightness(int percent);

struct DateTime {
  uint16_t year;
  uint8_t month, day;  // from 1
  uint8_t hour, minute, second;
  uint8_t weekday;  // 0 = Sunday
};
// Local time. False until the clock has been set: the StickS3 has no
// battery-backed clock chip, so that's after every power-off (it keeps time
// through sleep and restarts).
bool now(DateTime& out);
bool setTime(const DateTime& time);

// The battery, 0–100. Read from the power chip at most every few seconds.
int batteryPercent();
bool charging();
bool onUsb();

// Screen off and deep sleep. The front key (or the power key) wakes it, as a
// restart. Doesn't return.
void sleep();

// For Settings → About.
struct About {
  char chip[32];
  char flash[16];
  char mac[24];
  char memory[24];
};
void about(About& out);

}  // namespace board
