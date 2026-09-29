#include "Schema.h"

#include <cstring>

#include "Wire.h"

namespace blat {

namespace {

// Writes into a buffer, or only counts when there's no buffer. Stops writing
// (but keeps counting) once full, so one pass both measures and encodes.
class Writer {
 public:
  Writer(uint8_t* out, size_t size) : out_(out), size_(size) {}

  void u8(const uint8_t v) { bytes(&v, 1); }
  void u16(const uint16_t v) {
    uint8_t b[2];
    put16(b, v);
    bytes(b, 2);
  }
  void u32(const uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    bytes(b, 4);
  }
  void bytes(const void* data, const size_t n) {
    if (out_ != nullptr && at_ + n <= size_) memcpy(out_ + at_, data, n);
    at_ += n;
  }
  // A TLV attribute with a string value, if there is one. Strings are cut at
  // 255 bytes, though check() keeps them far shorter.
  void text(const uint8_t tag, const char* s) {
    if (s == nullptr || s[0] == '\0') return;
    const size_t n = strnlen(s, 255);
    u8(tag);
    u8(n);
    bytes(s, n);
  }
  size_t at() const { return at_; }
  bool fits() const { return out_ == nullptr || at_ <= size_; }
  void patch16(const size_t at, const uint16_t v) {
    if (out_ != nullptr && at + 2 <= size_) put16(out_ + at, v);
  }

 private:
  uint8_t* out_;
  size_t size_;
  size_t at_ = 0;
};

void encodeRecord(Writer& w, const Table& table, const Id id) {
  const Control& c = table[id];
  const size_t lengthAt = w.at();
  w.u16(0);  // recordLength, patched below
  w.u16(id);
  w.u8(static_cast<uint8_t>(c.type_));
  // Read-only controls can't be written at any level; send write level 0.
  const bool writable = c.type_ == Type::Action || (c.hasValue() && !c.is(flag::kReadOnly));
  w.u8((c.read_ & 0x0F) | ((writable ? c.write_ : 0) << 4));
  w.u16(parentOf(table, id));
  w.u16(c.flags_);

  w.text(attr::kKey, c.key_);
  w.text(attr::kLabel, c.label_);
  w.text(attr::kHelp, c.help_);
  switch (c.type_) {
    case Type::Bool:
      w.u8(attr::kDefault);
      w.u8(1);
      w.u8(c.initial_ ? 1 : 0);
      break;
    case Type::Int:
      w.text(attr::kUnit, c.unit_);
      w.u8(attr::kRange);
      w.u8(12);
      w.u32(c.min_);
      w.u32(c.max_);
      w.u32(c.step_);
      if (c.scale_ != 0) {
        w.u8(attr::kScale);
        w.u8(1);
        w.u8(c.scale_);
      }
      w.u8(attr::kDefault);
      w.u8(4);
      w.u32(c.initial_);
      break;
    case Type::Enum:
      for (uint8_t i = 0; i < c.optionCount_; i++) {
        const char* label = c.options_[i].label;
        const size_t n = strnlen(label, 253);
        w.u8(attr::kOption);
        w.u8(2 + n);
        w.u16(c.options_[i].value);
        w.bytes(label, n);
      }
      w.u8(attr::kDefault);
      w.u8(2);
      w.u16(c.initial_);
      break;
    case Type::Text:
    case Type::Secret: {
      w.u8(attr::kLength);
      w.u8(2);
      w.u8(c.minLength_);
      w.u8(c.maxLength_);
      if (c.type_ == Type::Text) {
        const size_t n = strnlen(c.initialText_, 255);
        w.u8(attr::kDefault);
        w.u8(1 + n);
        w.u8(n);
        w.bytes(c.initialText_, n);
      }
      break;
    }
    default:
      break;
  }
  if (c.hint_ != Hint::None) {
    w.u8(attr::kHint);
    w.u8(1);
    w.u8(static_cast<uint8_t>(c.hint_));
  }
  w.patch16(lengthAt, w.at() - lengthAt - 2);
}

}  // namespace

Id parentOf(const Table& table, const Id id) {
  const char* parent = table[id].parent_;
  if (parent == nullptr) return 0;
  for (Id p = 1; p < id; p++) {
    if (strcmp(table[p].key_, parent) == 0) return p;
  }
  return 0;
}

size_t encodeSchema(const Table& table, uint8_t* out, const size_t size) {
  Writer w(out, size);
  w.u8(kVersion);
  w.u16(table.count);
  for (Id id = 1; id <= table.count; id++) encodeRecord(w, table, id);
  return w.fits() ? w.at() : 0;
}

}  // namespace blat
