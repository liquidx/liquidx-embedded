#include "Settings.h"

#include <Logging.h>
#include <Preferences.h>

namespace settings {

namespace {

constexpr const char* kNamespace = "settings";

blat::PrefsStore prefsStore;
blat::Values currentValues;

// Before blat, settings were saved in the "shell" namespace as an index into
// each setting's option list. Move them over once, then drop the old keys.
struct Legacy {
  const char* key;
  blat::Id id;
  uint16_t values[5];  // the old option order
  uint8_t count;
};
constexpr Legacy kLegacy[] = {
    {"refresh", kRefresh, {6, 12, 24, 0}, 4},
    {"sleep", kSleep, {1, 5, 10, 30, 0}, 5},
    {"clock", kClock, {24, 12}, 2},
    {"framesleep", kFrameSleep, {0, 1}, 2},
};

void migrateLegacy() {
  Preferences old;
  if (!old.begin("shell", false)) return;
  for (const auto& l : kLegacy) {
    if (!old.isKey(l.key)) continue;
    const uint8_t index = old.getUChar(l.key, 0xFF);
    if (index < l.count) {
      currentValues.set(l.id, l.values[index]);
      LOG_INF("SET", "Moved %s = %u to blat", l.key, l.values[index]);
    }
    old.remove(l.key);
  }
  old.end();
}

}  // namespace

void begin() {
  if (!prefsStore.begin(kNamespace)) LOG_ERR("SET", "Couldn't open NVS namespace %s", kNamespace);
  currentValues.begin(blat::table(controls::kControls), &prefsStore);
  migrateLegacy();
  // Changes made while nobody was connected aren't news to the next host.
  while (currentValues.takeDeviceChange() != 0) {
  }
}

blat::Values& values() { return currentValues; }
blat::Store& store() { return prefsStore; }

}  // namespace settings
