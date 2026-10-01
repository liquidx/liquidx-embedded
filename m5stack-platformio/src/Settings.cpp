#include "Settings.h"

#include "Log.h"

namespace settings {

namespace {

blat::Values currentValues;

#if defined(ARDUINO)
constexpr const char* kNamespace = "settings";
blat::PrefsStore prefsStore;
#endif

}  // namespace

void begin() {
#if defined(ARDUINO)
  if (!prefsStore.begin(kNamespace)) LOG_ERR("SET", "Couldn't open NVS namespace %s", kNamespace);
#endif
  currentValues.begin(blat::table(controls::kControls), store());
  // Changes made while nobody was connected aren't news to the next host.
  while (currentValues.takeDeviceChange() != 0) {
  }
}

blat::Values& values() { return currentValues; }

blat::Store* store() {
#if defined(ARDUINO)
  return &prefsStore;
#else
  return nullptr;
#endif
}

}  // namespace settings
