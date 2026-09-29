#pragma once

#include <cstddef>
#include <cstdint>

#include "Frame.h"
#include "Wire.h"

// Frames as BMP files, for displays that persist them (the `persist` flag).
// Grey formats (mono1, gray2, gray4, gray8) map to indexed BMPs of the same
// depth with a white-to-black palette, so index = ink level and rows copy
// straight across. (2-bit BMPs are rare outside Windows CE, but the X4's
// image viewer and this reader take them.)
namespace blit {

// Where decodeBmp() reads from: a file, usually.
class Reader {
 public:
  virtual ~Reader() = default;
  // Read up to `length` bytes; returns how many were read.
  virtual size_t read(uint8_t* out, size_t length) = 0;
  // Move to byte `offset` from the start.
  virtual bool seek(uint32_t offset) = 0;
};

// The size of the BMP file for a frame, or 0 if the format isn't grey.
size_t bmpSize(uint8_t format, uint16_t width, uint16_t height);

// The whole file into `out` (bmpSize() bytes), so it can go to storage in one
// write. False if the format isn't grey or `out` can't grow.
bool encodeBmp(const uint8_t* pixels, uint8_t format, uint16_t width, uint16_t height, Buffer& out);

// Read an indexed BMP of 1, 2, 4 or 8 bits into `out` as mono1, gray2, gray4
// or gray8 (each palette colour to the nearest ink level), and set `header`'s
// format, width and height. False for anything else, or larger than maxSide.
bool decodeBmp(Reader& in, Buffer& out, FrameHeader& header, uint16_t maxSide = 2048);

}  // namespace blit
