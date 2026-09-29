#include "Values.h"

#include <cstring>

namespace blat {

Values::~Values() {
  delete[] ints_;
  delete[] textAt_;
  delete[] text_;
  delete[] pending_;
}

size_t Values::strlen0(const char* s) { return s == nullptr ? 0 : strlen(s); }

bool Values::begin(const Table& table, Store* store) {
  table_ = table;
  store_ = store;
  const uint16_t n = table.count;
  ints_ = new int32_t[n + 1]();
  textAt_ = new uint16_t[n + 1]();
  pending_ = new uint8_t[n + 1]();
  size_t textBytes = 0;
  for (Id id = 1; id <= n; id++) {
    if (table[id].isText()) {
      textAt_[id] = textBytes;
      textBytes += table[id].maxLength_ + 1;
    }
  }
  text_ = new char[textBytes + 1]();
  if (ints_ == nullptr || textAt_ == nullptr || pending_ == nullptr || text_ == nullptr) return false;
  for (Id id = 1; id <= n; id++) load(id);
  return true;
}

// The saved value if it's still valid (a firmware update may have changed
// the options or range), else the default.
void Values::load(const Id id) {
  const Control& c = table_[id];
  if (!c.hasValue()) return;
  if (c.isText()) {
    char* dst = text_ + textAt_[id];
    size_t length = 0;
    bool loaded = false;
    if (store_ != nullptr && c.saveAs_ != nullptr) {
      length = store_->getBytes(c.saveAs_, dst, c.maxLength_);
      if (length == 1 && dst[0] == '\0') {
        length = 0;  // saved as empty (see applyText)
        loaded = true;
      } else {
        loaded = length > 0 && checkText(id, dst, length) == Status::Ok;
      }
    }
    if (!loaded) {
      length = strlen0(c.initialText_);
      memcpy(dst, c.initialText_, length);
    }
    dst[length] = '\0';
    return;
  }
  int32_t saved = 0;
  if (store_ != nullptr && c.saveAs_ != nullptr && store_->getInt(c.saveAs_, saved) && check(id, saved) == Status::Ok) {
    ints_[id] = saved;
  } else {
    ints_[id] = c.initial_;
  }
}

int32_t Values::get(const Id id) const { return table_.valid(id) ? ints_[id] : 0; }

const char* Values::text(const Id id) const {
  if (!table_.valid(id) || table_[id].type_ != Type::Text) return "";
  return text_ + textAt_[id];
}

bool Values::isSet(const Id id) const {
  if (!table_.valid(id) || !table_[id].isText()) return false;
  return text_[textAt_[id]] != '\0';
}

Status Values::check(const Id id, const int32_t value) const {
  if (!table_.valid(id)) return Status::UnknownId;
  const Control& c = table_[id];
  switch (c.type_) {
    case Type::Bool:
      return value == 0 || value == 1 ? Status::Ok : Status::InvalidValue;
    case Type::Int:
      if (value < c.min_ || value > c.max_) return Status::InvalidValue;
      return (static_cast<int64_t>(value) - c.min_) % c.step_ == 0 ? Status::Ok : Status::InvalidValue;
    case Type::Enum:
      for (uint8_t i = 0; i < c.optionCount_; i++) {
        if (c.options_[i].value == value) return Status::Ok;
      }
      return Status::InvalidValue;
    default:
      return Status::InvalidValue;
  }
}

Status Values::checkText(const Id id, const char* s, const size_t length) const {
  if (!table_.valid(id)) return Status::UnknownId;
  const Control& c = table_[id];
  if (!c.isText()) return Status::InvalidValue;
  // A secret can always be cleared ("not set"); a text only if it may be empty.
  if (length == 0) return c.type_ == Type::Secret || c.minLength_ == 0 ? Status::Ok : Status::InvalidValue;
  if (length < c.minLength_ || length > c.maxLength_) return Status::InvalidValue;
  for (size_t i = 0; i < length; i++) {
    if (s[i] == '\0') return Status::InvalidValue;  // no embedded NULs
  }
  return Status::Ok;
}

Status Values::apply(const Id id, const int32_t value, const uint8_t from) {
  const Status status = check(id, value);
  if (status != Status::Ok) return status;
  if (ints_[id] == value) return Status::Ok;
  ints_[id] = value;
  const Control& c = table_[id];
  if (store_ != nullptr && c.saveAs_ != nullptr) store_->putInt(c.saveAs_, value);
  changed(id, from);
  return Status::Ok;
}

Status Values::applyText(const Id id, const char* s, const size_t length, const uint8_t from) {
  const Status status = checkText(id, s, length);
  if (status != Status::Ok) return status;
  char* dst = text_ + textAt_[id];
  if (strlen(dst) == length && memcmp(dst, s, length) == 0) return Status::Ok;
  memcpy(dst, s, length);
  dst[length] = '\0';
  const Control& c = table_[id];
  if (store_ != nullptr && c.saveAs_ != nullptr) {
    // Empty is saved as one NUL byte, so it isn't mistaken for "never set"
    // (which loads the default).
    if (length == 0) {
      store_->putBytes(c.saveAs_, "", 1);
    } else {
      store_->putBytes(c.saveAs_, s, length);
    }
  }
  changed(id, from);
  return Status::Ok;
}

void Values::reset(const Id id) {
  if (!table_.valid(id)) return;
  const Control& c = table_[id];
  if (!c.hasValue()) return;
  if (c.isText()) {
    applyText(id, c.initialText_, strlen0(c.initialText_), kFromDevice);
  } else {
    apply(id, c.initial_, kFromDevice);
  }
  if (store_ != nullptr && c.saveAs_ != nullptr) store_->remove(c.saveAs_);
}

void Values::resetAll() {
  for (Id id = 1; id <= table_.count; id++) reset(id);
}

void Values::changed(const Id id, const uint8_t from) {
  // A change from one side is news to the other.
  pending_[id] |= from == kFromHost ? kFromHost : kFromDevice;
  if (from == kFromHost) hostChanged_ = true;
}

bool Values::takeHostChanges() {
  if (!hostChanged_) return false;
  hostChanged_ = false;
  for (Id id = 1; id <= table_.count; id++) pending_[id] &= ~kFromHost;
  return true;
}

Id Values::takeDeviceChange() {
  for (uint16_t n = 0; n < table_.count; n++) {
    const Id id = scan_;
    scan_ = scan_ >= table_.count ? 1 : scan_ + 1;
    if (pending_[id] & kFromDevice) {
      pending_[id] &= ~kFromDevice;
      return id;
    }
  }
  return 0;
}

}  // namespace blat
