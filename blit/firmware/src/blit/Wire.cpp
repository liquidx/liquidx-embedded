#include "Wire.h"

#include <algorithm>
#include <cstring>

namespace blit {

bool parseHeader(const uint8_t* d, const size_t length, FrameHeader& h) {
  if (length < kHeaderBytes || length < kHeaderBytes + d[28]) return false;
  h.version = d[1];
  h.format = d[2];
  h.encoding = d[3];
  h.flags = d[4];
  h.refresh = d[5] <= 2 ? static_cast<Refresh>(d[5]) : Refresh::Auto;
  h.width = get16(d + 6);
  h.height = get16(d + 8);
  h.x = h.region() ? get16(d + 10) : 0;
  h.y = h.region() ? get16(d + 12) : 0;
  h.byteLength = get32(d + 16);
  h.crc32 = get32(d + 20);
  h.nextFrameSeconds = get32(d + 24);
  const size_t nameLength = std::min<size_t>(d[28], kMaxName);
  memcpy(h.name, d + kHeaderBytes, nameLength);
  h.name[nameLength] = '\0';
  return true;
}

size_t encodeHeader(const FrameHeader& h, uint8_t* out) {
  memset(out, 0, kHeaderBytes);
  out[0] = kOpBegin;
  out[1] = h.version;
  out[2] = h.format;
  out[3] = h.encoding;
  out[4] = h.flags;
  out[5] = static_cast<uint8_t>(h.refresh);
  put16(out + 6, h.width);
  put16(out + 8, h.height);
  put16(out + 10, h.x);
  put16(out + 12, h.y);
  put32(out + 16, h.byteLength);
  put32(out + 20, h.crc32);
  put32(out + 24, h.nextFrameSeconds);
  const size_t nameLength = strnlen(h.name, kMaxName);
  out[28] = nameLength;
  memcpy(out + kHeaderBytes, h.name, nameLength);
  return kHeaderBytes + nameLength;
}

uint32_t crc32(const uint8_t* data, const size_t length, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

}  // namespace blit
