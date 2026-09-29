#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace blat {

// Where saved values and remembered hosts live. PrefsStore puts them in
// ESP32 NVS; MemoryStore keeps them in RAM (tests, or a device that
// shouldn't remember anything). Keys are at most 15 bytes. The library's own
// keys start with '_'.
class Store {
 public:
  virtual ~Store() = default;
  virtual bool getInt(const char* key, int32_t& out) = 0;
  virtual void putInt(const char* key, int32_t value) = 0;
  // Returns the stored length, or 0 if there's nothing stored (or it doesn't
  // fit in `size`).
  virtual size_t getBytes(const char* key, void* out, size_t size) = 0;
  virtual void putBytes(const char* key, const void* data, size_t length) = 0;
  virtual void remove(const char* key) = 0;
};

// A fixed-size store in RAM.
class MemoryStore : public Store {
 public:
  bool getInt(const char* key, int32_t& out) override {
    const Entry* e = find(key);
    if (e == nullptr || e->length != sizeof(int32_t)) return false;
    memcpy(&out, e->data, sizeof(out));
    return true;
  }
  void putInt(const char* key, const int32_t value) override { putBytes(key, &value, sizeof(value)); }
  size_t getBytes(const char* key, void* out, const size_t size) override {
    const Entry* e = find(key);
    if (e == nullptr || e->length > size) return 0;
    memcpy(out, e->data, e->length);
    return e->length;
  }
  void putBytes(const char* key, const void* data, const size_t length) override {
    Entry* e = find(key);
    for (size_t i = 0; e == nullptr && i < kEntries; i++) {
      if (entries_[i].key[0] == '\0') e = &entries_[i];
    }
    if (e == nullptr || length > sizeof(e->data) || strlen(key) >= sizeof(e->key)) return;
    strcpy(e->key, key);
    memcpy(e->data, data, length);
    e->length = length;
  }
  void remove(const char* key) override {
    if (Entry* e = find(key)) e->key[0] = '\0';
  }

 private:
  struct Entry {
    char key[16] = "";
    uint8_t data[640];
    size_t length = 0;
  };
  static constexpr size_t kEntries = 32;

  Entry* find(const char* key) {
    for (auto& e : entries_) {
      if (e.key[0] != '\0' && strcmp(e.key, key) == 0) return &e;
    }
    return nullptr;
  }

  Entry entries_[kEntries];
};

}  // namespace blat
