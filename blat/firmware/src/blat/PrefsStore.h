#pragma once

#if defined(ARDUINO)

#include <Preferences.h>

#include "Store.h"

namespace blat {

// A Store in ESP32 NVS, through Arduino's Preferences, in one namespace.
class PrefsStore : public Store {
 public:
  // `ns`: the NVS namespace, up to 15 bytes.
  bool begin(const char* ns) { return prefs_.begin(ns, false); }

  bool getInt(const char* key, int32_t& out) override {
    if (!prefs_.isKey(key)) return false;
    out = prefs_.getInt(key, 0);
    return true;
  }
  void putInt(const char* key, const int32_t value) override { prefs_.putInt(key, value); }
  size_t getBytes(const char* key, void* out, const size_t size) override {
    if (!prefs_.isKey(key)) return 0;
    const size_t length = prefs_.getBytesLength(key);
    if (length == 0 || length > size) return 0;
    return prefs_.getBytes(key, out, size);
  }
  void putBytes(const char* key, const void* data, const size_t length) override {
    prefs_.putBytes(key, data, length);
  }
  void remove(const char* key) override { prefs_.remove(key); }

 private:
  Preferences prefs_;
};

}  // namespace blat

#endif  // ARDUINO
