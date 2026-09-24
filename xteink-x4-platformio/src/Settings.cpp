#include "Settings.h"

#include <Preferences.h>

namespace settings {

namespace {

constexpr Option kRefreshOptions[] = {
    {6, "6", "pages", "6 pages"},
    {12, "12", "pages", "12 pages"},
    {24, "24", "pages", "24 pages"},
    {0, "Never", "", "Never"},
};
constexpr Option kSleepOptions[] = {
    {1, "1", "min", "1 min"},
    {5, "5", "min", "5 min"},
    {10, "10", "min", "10 min"},
    {30, "30", "min", "30 min"},
    {0, "Never", "", "Never"},
};
constexpr Option kClockOptions[] = {
    {24, "24", "hour", "24h"},
    {12, "12", "hour", "12h"},
};

constexpr int kMaxChoices = 3;
const Choice* const kAll[kMaxChoices] = {&kRefresh, &kSleep, &kClock};
uint8_t indices[kMaxChoices];

Preferences prefs;

int slot(const Choice& choice) {
  for (int i = 0; i < kMaxChoices; i++) {
    if (kAll[i] == &choice) return i;
  }
  return 0;
}

}  // namespace

const Choice kRefresh{"refresh",
                      "Refresh",
                      "Full refresh every",
                      "Clears ghosting left by fast partial refreshes. Higher is faster; lower is cleaner.",
                      kRefreshOptions,
                      4,
                      1};
const Choice kSleep{"sleep",
                    "Sleep after",
                    "Sleep after",
                    "Time without a key press before the device sleeps. Hold Power to sleep at any time.",
                    kSleepOptions,
                    5,
                    2};
const Choice kClock{"clock", "Clock", "Clock shows", "How the home screen shows the time.", kClockOptions, 2, 0};

void begin() {
  prefs.begin("shell", false);
  for (int i = 0; i < kMaxChoices; i++) {
    const uint8_t stored = prefs.getUChar(kAll[i]->key, kAll[i]->defaultIndex);
    indices[i] = stored < kAll[i]->count ? stored : kAll[i]->defaultIndex;
  }
}

int index(const Choice& choice) { return indices[slot(choice)]; }

uint16_t value(const Choice& choice) { return choice.options[index(choice)].value; }

void set(const Choice& choice, const int index) {
  if (index < 0 || index >= choice.count) return;
  indices[slot(choice)] = index;
  prefs.putUChar(choice.key, index);
}

}  // namespace settings
