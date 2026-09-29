#pragma once

#include <cstddef>
#include <cstdint>

#include "Controls.h"
#include "Store.h"
#include "Wire.h"

namespace blat {

// The current value of every control in a table: loaded from the store at
// begin(), checked against the declaration on every change, and saved when
// the control has saveAs(). Not thread-safe: use it from one task (the
// firmware's main loop), which is also where Device runs.
class Values {
 public:
  Values() = default;
  ~Values();
  Values(const Values&) = delete;
  Values& operator=(const Values&) = delete;

  // Load saved values (anything missing or no longer valid starts at its
  // default). `store` may be null: nothing is saved.
  bool begin(const Table& table, Store* store);
  const Table& table() const { return table_; }
  const Control& control(Id id) const { return table_[id]; }

  // Bool, int and enum values. 0 for anything else.
  int32_t get(Id id) const;
  // Text values ("" for anything else, and for secrets: see isSet()).
  const char* text(Id id) const;
  // A secret or text has a non-empty value.
  bool isSet(Id id) const;

  // Would this value be accepted? Checks type, range, options and length;
  // not access, which is the caller's business.
  Status check(Id id, int32_t value) const;
  Status checkText(Id id, const char* s, size_t length) const;

  // Change a value, from the firmware (the device's own UI, a sensor). Read-
  // only controls can be set this way; hosts can't. Invalid values are
  // refused. Unchanged values do nothing.
  Status set(Id id, int32_t value) { return apply(id, value, kFromDevice); }
  Status setText(Id id, const char* s) { return applyText(id, s, strlen0(s), kFromDevice); }
  // Back to the declared default.
  void reset(Id id);
  void resetAll();

  // Changes made by a host (Device calls these after checking access).
  Status setFromHost(Id id, int32_t value) { return apply(id, value, kFromHost); }
  Status setTextFromHost(Id id, const char* s, size_t length) { return applyText(id, s, length, kFromHost); }

  // True (once) when a host has changed something since the last call: the
  // firmware's cue to re-read values or redraw.
  bool takeHostChanges();
  // Next control changed by the firmware since Device last looked, or 0.
  // Device uses this for `changed` events.
  Id takeDeviceChange();

 private:
  static constexpr uint8_t kFromDevice = 1;  // a host should hear about it
  static constexpr uint8_t kFromHost = 2;    // the firmware should hear about it

  static size_t strlen0(const char* s);
  Status apply(Id id, int32_t value, uint8_t from);
  Status applyText(Id id, const char* s, size_t length, uint8_t from);
  void load(Id id);
  void changed(Id id, uint8_t from);

  Table table_;
  Store* store_ = nullptr;
  int32_t* ints_ = nullptr;      // per control
  uint16_t* textAt_ = nullptr;   // per control: offset into text_
  char* text_ = nullptr;         // every text value, each maxLength + 1 bytes
  uint8_t* pending_ = nullptr;   // per control: kFromDevice | kFromHost
  bool hostChanged_ = false;
  Id scan_ = 1;                  // takeDeviceChange() resumes here
};

}  // namespace blat
