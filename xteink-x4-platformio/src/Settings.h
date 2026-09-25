#pragma once

#include <cstdint>

// User settings, persisted to NVS. Each value is picked from a short fixed list
// of options, so the settings UI can show them as a vertical choice.
namespace settings {

struct Option {
  uint16_t value;
  const char* big;   // large display value on the settings page
  const char* unit;  // small unit beside it, or "" for none
  const char* list;  // short form in the settings list
};

struct Choice {
  const char* key;    // NVS key
  const char* title;  // e.g. "Refresh"
  const char* heading;
  const char* description;
  const Option* options;
  uint8_t count;
  uint8_t defaultIndex;
};

// Full refresh after this many fast refreshes; 0 = never.
extern const Choice kRefresh;
// Minutes without a key press before sleeping; 0 = never.
extern const Choice kSleep;
// 24 or 12.
extern const Choice kClock;
// 1 = deep-sleep between Bluetooth frames when the sender says when the next
// is due (docs/ble-cast-protocol.md); 0 = stay connected.
extern const Choice kFrameSleep;

void begin();
int index(const Choice& choice);
uint16_t value(const Choice& choice);
void set(const Choice& choice, int index);

}  // namespace settings
