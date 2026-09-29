#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Declaring a device's controls: the one table that is both its settings
// (values, defaults, storage) and its exposure over blat (what exists, who
// can read and write it). See ../../README.md and ../../../PROTOCOL.md.
//
//   constexpr blat::Option kSleepOptions[] = {{1, "1 min"}, {5, "5 min"}, {0, "Never"}};
//
//   constexpr blat::Control kControls[] = {
//       blat::group("power", "Power"),
//       blat::choice("power.sleep", kSleepOptions)
//           .in("power")
//           .label("Sleep after")
//           .help("Time without a key press before the device sleeps.")
//           .initial(5)
//           .saveAs("sleep")
//           .live(),
//   };
//   static_assert(blat::check(kControls));
//   constexpr blat::Id kSleep = blat::idOf(kControls, "power.sleep");
namespace blat {

// Control types (PROTOCOL.md#control-types).
enum class Type : uint8_t {
  Bool = 1,
  Int = 2,
  Enum = 3,
  Text = 4,
  Secret = 5,
  Bytes = 6,
  Action = 7,
  File = 8,
  Dir = 9,
  Group = 10,
};

// Flags (PROTOCOL.md#flags).
namespace flag {
constexpr uint16_t kReadOnly = 1 << 0;
constexpr uint16_t kLive = 1 << 1;
constexpr uint16_t kDynamic = 1 << 2;
constexpr uint16_t kRestart = 1 << 3;
constexpr uint16_t kConfirm = 1 << 4;
constexpr uint16_t kAdvanced = 1 << 5;
constexpr uint16_t kRequired = 1 << 6;
constexpr uint16_t kHidden = 1 << 7;
}  // namespace flag

// Input hints (PROTOCOL.md#attributes, tag `hint`).
enum class Hint : uint8_t {
  None = 0,
  Multiline = 1,
  Email = 2,
  Url = 3,
  Hostname = 4,
  Ip = 5,
  Mac = 6,
  Color = 7,
  TimeOfDay = 8,
  DateTime = 9,
};

// Access levels (PROTOCOL.md#access-levels).
constexpr uint8_t kLevelAnyone = 0;
constexpr uint8_t kLevelPaired = 1;
constexpr uint8_t kLevelPresent = 2;

// A control's id on the wire: its position in the table, from 1. 0 = none.
using Id = uint16_t;

// One choice of an enum control: the value stored and sent, and its label.
struct Option {
  uint16_t value;
  const char* label;
};

// One control. Start from one of the functions below (group, toggle, number,
// choice, text, secret, action), then chain setters. Every setter returns a
// copy, so a whole declaration is a constant expression and the table lives
// in flash.
//
// Fields end in `_` and are for the library to read; declarations use the
// setters.
struct Control {
  const char* key_ = nullptr;  // stable, dotted: "display.refresh"
  Type type_ = Type::Group;
  const char* label_ = nullptr;
  const char* help_ = nullptr;
  const char* unit_ = nullptr;
  const char* parent_ = nullptr;  // key of the group or action it belongs to
  Hint hint_ = Hint::None;
  uint8_t read_ = kLevelAnyone;
  uint8_t write_ = kLevelPaired;
  uint16_t flags_ = 0;
  int32_t min_ = INT32_MIN;
  int32_t max_ = INT32_MAX;
  int32_t step_ = 1;
  uint8_t scale_ = 0;
  const Option* options_ = nullptr;
  uint8_t optionCount_ = 0;
  uint8_t minLength_ = 0;
  uint8_t maxLength_ = 32;
  int32_t initial_ = 0;             // bool, int, enum
  const char* initialText_ = "";   // text
  const char* saveAs_ = nullptr;   // key in the settings store; nullptr = not saved

  // --- Presentation ---
  // Shown to people. Up to 32 bytes.
  constexpr Control label(const char* s) const { return with([&](Control& c) { c.label_ = s; }); }
  // One or two sentences. Up to 160 bytes.
  constexpr Control help(const char* s) const { return with([&](Control& c) { c.help_ = s; }); }
  // For numbers: "min", "%", "°C". Up to 8 bytes.
  constexpr Control unit(const char* s) const { return with([&](Control& c) { c.unit_ = s; }); }
  // The group (or, for a param, the action) this control belongs to. It must
  // come earlier in the table.
  constexpr Control in(const char* parentKey) const { return with([&](Control& c) { c.parent_ = parentKey; }); }
  constexpr Control hint(Hint h) const { return with([&](Control& c) { c.hint_ = h; }); }

  // --- Access ---
  // The level needed to read (default: anyone) and to write or invoke
  // (default: a paired host). Writes always need at least kLevelPaired.
  constexpr Control read(uint8_t level) const { return with([&](Control& c) { c.read_ = level; }); }
  constexpr Control write(uint8_t level) const { return with([&](Control& c) { c.write_ = level; }); }
  // Hosts may read it but never write it. The firmware still can.
  constexpr Control readOnly() const { return flagged(flag::kReadOnly); }

  // --- Behaviour ---
  // Hosts are told when it changes, however it changed.
  constexpr Control live() const { return flagged(flag::kLive); }
  constexpr Control restart() const { return flagged(flag::kRestart); }
  constexpr Control confirm() const { return flagged(flag::kConfirm); }
  constexpr Control advanced() const { return flagged(flag::kAdvanced); }
  constexpr Control required() const { return flagged(flag::kRequired); }
  constexpr Control hidden() const { return flagged(flag::kHidden); }

  // --- Limits ---
  // For numbers. `scale` is decimal places: range(0, 300).scale(1) is 0.0 to 30.0.
  constexpr Control range(int32_t min, int32_t max, int32_t step = 1) const {
    return with([&](Control& c) {
      c.min_ = min;
      c.max_ = max;
      c.step_ = step;
    });
  }
  constexpr Control scale(uint8_t decimals) const { return with([&](Control& c) { c.scale_ = decimals; }); }
  // For text and secrets: length in bytes.
  constexpr Control length(uint8_t min, uint8_t max) const {
    return with([&](Control& c) {
      c.minLength_ = min;
      c.maxLength_ = max;
    });
  }

  // --- Default ---
  // The value before anything is saved, and after a reset. For an enum, an
  // option's value (default: the first option).
  // One template for all three, so initial(12) isn't ambiguous between int,
  // bool and whatever int32_t is on this toolchain.
  template <typename T>
  constexpr Control initial(T v) const {
    return with([&](Control& c) {
      if constexpr (std::is_convertible_v<T, const char*>) {
        c.initialText_ = v;
      } else if constexpr (std::is_same_v<T, bool>) {
        c.initial_ = v ? 1 : 0;
      } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "initial() takes a number, bool or string");
        c.initial_ = static_cast<int32_t>(v);
      }
    });
  }

  // --- Storage ---
  // Save the value across restarts under this key (up to 15 bytes: NVS's
  // limit). Without it the value lives in RAM and starts at its default.
  constexpr Control saveAs(const char* storeKey) const { return with([&](Control& c) { c.saveAs_ = storeKey; }); }

  // --- For the library ---
  constexpr bool is(uint16_t f) const { return (flags_ & f) != 0; }
  // Holds a value (not a group or action).
  constexpr bool hasValue() const { return type_ != Type::Group && type_ != Type::Action; }
  constexpr bool isText() const { return type_ == Type::Text || type_ == Type::Secret; }

 private:
  template <typename F>
  constexpr Control with(F set) const {
    Control c = *this;
    set(c);
    return c;
  }
  constexpr Control flagged(uint16_t f) const { return with([&](Control& c) { c.flags_ |= f; }); }
};

// --- Starting points, one per type ---

// A section heading. No value.
constexpr Control group(const char* key, const char* label) {
  Control c;
  c.key_ = key;
  c.type_ = Type::Group;
  c.label_ = label;
  return c;
}

// On or off. Default off.
constexpr Control toggle(const char* key) {
  Control c;
  c.key_ = key;
  c.type_ = Type::Bool;
  c.min_ = 0;
  c.max_ = 1;
  return c;
}

// A whole number (fixed point with scale()). Default 0; give it a range.
constexpr Control number(const char* key) {
  Control c;
  c.key_ = key;
  c.type_ = Type::Int;
  return c;
}

// One of a fixed list. Default: the first option.
template <size_t N>
constexpr Control choice(const char* key, const Option (&options)[N]) {
  static_assert(N > 0 && N <= 255, "an enum needs 1 to 255 options");
  Control c;
  c.key_ = key;
  c.type_ = Type::Enum;
  c.options_ = options;
  c.optionCount_ = N;
  c.initial_ = options[0].value;
  return c;
}

// A string. Default "", up to 32 bytes unless length() says otherwise.
constexpr Control text(const char* key) {
  Control c;
  c.key_ = key;
  c.type_ = Type::Text;
  return c;
}

// A write-only string, e.g. a password. Hosts only learn whether it's set.
constexpr Control secret(const char* key) {
  Control c;
  c.key_ = key;
  c.type_ = Type::Secret;
  c.maxLength_ = 64;
  return c;
}

// Something a host can do: a button. Its params are controls declared
// .in() it. Needs a paired host by default; raise write() for more.
constexpr Control action(const char* key) {
  Control c;
  c.key_ = key;
  c.type_ = Type::Action;
  return c;
}

// --- Tables ---

// A table of controls, as the library sees it.
struct Table {
  const Control* controls = nullptr;
  uint16_t count = 0;

  const Control& operator[](Id id) const { return controls[id - 1]; }
  bool valid(Id id) const { return id >= 1 && id <= count; }
};

template <size_t N>
constexpr Table table(const Control (&controls)[N]) {
  return Table{controls, static_cast<uint16_t>(N)};
}

namespace detail {

constexpr size_t length(const char* s) {
  size_t n = 0;
  while (s != nullptr && s[n] != '\0') n++;
  return n;
}

constexpr bool equal(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return a == b;
  size_t i = 0;
  for (; a[i] != '\0' && a[i] == b[i]; i++) {
  }
  return a[i] == b[i];
}

constexpr bool isKeyChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
}

// Called only when a check fails. They aren't constexpr, so the compiler
// stops at the call and names the function: that name is the error message.
inline void error_missing_key() {}
inline void error_bad_key_characters() {}
inline void error_key_too_long() {}
inline void error_duplicate_key() {}
inline void error_label_too_long() {}
inline void error_help_too_long() {}
inline void error_unit_too_long() {}
inline void error_parent_must_come_first_and_be_a_group_or_action() {}
inline void error_level_must_be_0_1_or_2() {}
inline void error_writable_control_needs_write_level_1_or_more() {}
inline void error_initial_value_out_of_range() {}
inline void error_initial_value_is_not_an_option() {}
inline void error_duplicate_option_value() {}
inline void error_bad_range() {}
inline void error_bad_length() {}
inline void error_initial_text_too_long() {}
inline void error_secrets_cannot_have_a_default_or_be_live() {}
inline void error_save_key_too_long() {}
inline void error_duplicate_save_key() {}
inline void error_nothing_to_save() {}
inline void error_type_not_supported_yet() {}
inline void error_well_known_key_has_wrong_type() {}
inline void error_no_control_with_that_key() {}

struct WellKnown {
  const char* key;
  Type type;
};
// PROTOCOL.md#well-known-keys: hosts rely on these types.
constexpr WellKnown kWellKnown[] = {
    {"device.name", Type::Text},     {"wifi.password", Type::Secret}, {"wifi.scan", Type::Action},
    {"wifi.connect", Type::Action},  {"wifi.state", Type::Enum},      {"wifi.ip", Type::Text},
    {"power.battery", Type::Int},    {"time.now", Type::Int},         {"system.restart", Type::Action},
    {"system.factoryReset", Type::Action}, {"system.firmware", Type::File}, {"auth.hosts", Type::Enum},
    {"auth.forget", Type::Action},
};

constexpr bool checkOne(const Control* controls, const size_t count, const size_t i) {
  const Control& c = controls[i];
  if (c.key_ == nullptr || c.key_[0] == '\0') error_missing_key();
  for (size_t k = 0; c.key_[k] != '\0'; k++) {
    if (!isKeyChar(c.key_[k])) error_bad_key_characters();
  }
  if (length(c.key_) > 32) error_key_too_long();
  for (size_t j = 0; j < i; j++) {
    if (equal(controls[j].key_, c.key_)) error_duplicate_key();
    if (c.saveAs_ != nullptr && equal(controls[j].saveAs_, c.saveAs_)) error_duplicate_save_key();
  }
  if (length(c.label_) > 32) error_label_too_long();
  if (length(c.help_) > 160) error_help_too_long();
  if (length(c.unit_) > 8) error_unit_too_long();

  if (c.parent_ != nullptr) {
    bool found = false;
    for (size_t j = 0; j < i; j++) {
      if (equal(controls[j].key_, c.parent_) &&
          (controls[j].type_ == Type::Group || controls[j].type_ == Type::Action)) {
        found = true;
      }
    }
    if (!found) error_parent_must_come_first_and_be_a_group_or_action();
  }

  if (c.read_ > kLevelPresent || c.write_ > kLevelPresent) error_level_must_be_0_1_or_2();
  const bool writable = c.type_ == Type::Action || (c.hasValue() && !c.is(flag::kReadOnly));
  if (writable && c.write_ < kLevelPaired) error_writable_control_needs_write_level_1_or_more();

  switch (c.type_) {
    case Type::Bool:
      if (c.initial_ != 0 && c.initial_ != 1) error_initial_value_out_of_range();
      break;
    case Type::Int:
      if (c.min_ > c.max_ || c.step_ < 1) error_bad_range();
      if (c.initial_ < c.min_ || c.initial_ > c.max_) error_initial_value_out_of_range();
      break;
    case Type::Enum: {
      bool found = false;
      for (size_t o = 0; o < c.optionCount_; o++) {
        if (c.options_[o].value == c.initial_) found = true;
        if (length(c.options_[o].label) > 32) error_label_too_long();
        for (size_t p = 0; p < o; p++) {
          if (c.options_[p].value == c.options_[o].value) error_duplicate_option_value();
        }
      }
      if (!found) error_initial_value_is_not_an_option();
      break;
    }
    case Type::Text:
    case Type::Secret:
      if (c.minLength_ > c.maxLength_ || c.maxLength_ == 0) error_bad_length();
      if (length(c.initialText_) > c.maxLength_) error_initial_text_too_long();
      if (c.type_ == Type::Secret && (length(c.initialText_) > 0 || c.is(flag::kLive))) {
        error_secrets_cannot_have_a_default_or_be_live();
      }
      break;
    case Type::Group:
    case Type::Action:
      if (c.saveAs_ != nullptr) error_nothing_to_save();
      break;
    case Type::Bytes:
    case Type::File:
    case Type::Dir:
      error_type_not_supported_yet();
      break;
  }
  if (c.saveAs_ != nullptr && (length(c.saveAs_) == 0 || length(c.saveAs_) > 15)) error_save_key_too_long();

  for (const auto& w : kWellKnown) {
    if (equal(w.key, c.key_) && w.type != c.type_) error_well_known_key_has_wrong_type();
  }
  (void)count;
  return true;
}

}  // namespace detail

// Checks a table at compile time: static_assert(blat::check(kControls)).
// A mistake stops the build at a call to one of the detail::error_ functions,
// whose name says what's wrong.
template <size_t N>
constexpr bool check(const Control (&controls)[N]) {
  static_assert(N < 0xFFFF, "too many controls");
  for (size_t i = 0; i < N; i++) detail::checkOne(controls, N, i);
  return true;
}

// A control's id, looked up at compile time. A key that isn't in the table
// stops the build.
template <size_t N>
constexpr Id idOf(const Control (&controls)[N], const char* key) {
  for (size_t i = 0; i < N; i++) {
    if (detail::equal(controls[i].key_, key)) return static_cast<Id>(i + 1);
  }
  detail::error_no_control_with_that_key();
  return 0;
}

}  // namespace blat
