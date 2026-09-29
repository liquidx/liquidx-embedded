#include "Wire.h"

namespace blat {

uint32_t crc32(const uint8_t* data, const size_t length, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

}  // namespace blat
