#pragma once

#include <cstddef>
#include <cstdint>

#include "Wire.h"

// Holding and patching frames on the display.
namespace blit {

// A growable byte buffer, in PSRAM when the chip has some. Move-only; swap()
// hands buffers between the receiver and the firmware without copying.
class Buffer {
 public:
  Buffer() = default;
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  // Make room for `bytes`. Keeps the contents only if there was room already.
  bool reserve(size_t bytes);
  void swap(Buffer& other);
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  size_t capacity() const { return capacity_; }

 private:
  uint8_t* data_ = nullptr;
  size_t capacity_ = 0;
};

// The ink level of pixel `x` in a row of a grey format (mono1, gray2, gray4,
// gray8): 0 is white, (1 << bpp) - 1 is black. 0 for other formats.
inline uint8_t level(const uint8_t* row, const uint8_t format, const uint32_t x) {
  switch (format) {
    case kFormatMono1:
      return (row[x >> 3] >> (7 - (x & 7))) & 1;
    case kFormatGray2:
      return (row[x >> 2] >> (6 - 2 * (x & 3))) & 3;
    case kFormatGray4:
      return (row[x >> 1] >> (x & 1 ? 0 : 4)) & 15;
    case kFormatGray8:
      return row[x];
    default:
      return 0;
  }
}

// Copy a region's pixels into the full frame it patches. `frame` and `base`
// describe the full frame; `region` must be in its format and inside it, with
// x on a byte (Receiver checks all that before accepting a region).
void applyRegion(uint8_t* frame, const FrameHeader& base, const FrameHeader& region, const uint8_t* pixels);

}  // namespace blit
