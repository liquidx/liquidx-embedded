#include "Frame.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace blit {

namespace {

uint8_t* allocate(const size_t bytes) {
#if defined(ESP_PLATFORM)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return static_cast<uint8_t*>(p);
#else
  return static_cast<uint8_t*>(malloc(bytes));
#endif
}

void release(uint8_t* p) {
#if defined(ESP_PLATFORM)
  heap_caps_free(p);
#else
  free(p);
#endif
}

}  // namespace

Buffer::~Buffer() { release(data_); }

bool Buffer::reserve(const size_t bytes) {
  if (bytes <= capacity_) return true;
  release(data_);
  data_ = allocate(bytes);
  capacity_ = data_ ? bytes : 0;
  return data_ != nullptr;
}

void Buffer::swap(Buffer& other) {
  std::swap(data_, other.data_);
  std::swap(capacity_, other.capacity_);
}

void applyRegion(uint8_t* frame, const FrameHeader& base, const FrameHeader& region, const uint8_t* pixels) {
  const uint32_t stride = rowBytes(base.format, base.width);
  const uint32_t length = rowBytes(region.format, region.width);
  const uint32_t xByte = region.x * bitsPerPixel(region.format) / 8;
  for (uint32_t y = 0; y < region.height; y++) {
    memcpy(frame + (region.y + y) * stride + xByte, pixels + y * length, length);
  }
}

}  // namespace blit
