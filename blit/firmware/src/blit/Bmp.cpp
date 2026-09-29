#include "Bmp.h"

#include <cstring>

namespace blit {

namespace {

constexpr uint32_t kFileHeader = 14;
constexpr uint32_t kInfoHeader = 40;  // BITMAPINFOHEADER

// Bits per pixel of a grey format, or 0.
uint8_t greyBits(const uint8_t format) {
  return format == kFormatMono1 || format == kFormatGray2 || format == kFormatGray4 || format == kFormatGray8
             ? bitsPerPixel(format)
             : 0;
}

uint8_t greyFormat(const uint8_t bpp) {
  switch (bpp) {
    case 1:
      return kFormatMono1;
    case 2:
      return kFormatGray2;
    case 4:
      return kFormatGray4;
    case 8:
      return kFormatGray8;
    default:
      return 0;
  }
}

// BMP rows are padded to 4 bytes.
uint32_t bmpStride(const uint8_t bpp, const uint32_t width) { return ((width * bpp + 31) / 32) * 4; }

bool readAt(Reader& in, const uint32_t offset, uint8_t* out, const size_t length) {
  return in.seek(offset) && in.read(out, length) == length;
}

}  // namespace

size_t bmpSize(const uint8_t format, const uint16_t width, const uint16_t height) {
  const uint8_t bpp = greyBits(format);
  if (bpp == 0) return 0;
  return kFileHeader + kInfoHeader + 4 * (1u << bpp) + static_cast<size_t>(bmpStride(bpp, width)) * height;
}

bool encodeBmp(const uint8_t* pixels, const uint8_t format, const uint16_t w, const uint16_t h, Buffer& out) {
  const uint8_t bpp = greyBits(format);
  const size_t size = bmpSize(format, w, h);
  if (size == 0 || !out.reserve(size)) return false;
  const uint32_t colors = 1u << bpp;
  const uint32_t headerBytes = kFileHeader + kInfoHeader + 4 * colors;
  const uint32_t srcStride = rowBytes(format, w);
  const uint32_t dstStride = bmpStride(bpp, w);

  uint8_t* p = out.data();
  memset(p, 0, headerBytes);
  p[0] = 'B';
  p[1] = 'M';
  put32(p + 2, size);
  put32(p + 10, headerBytes);  // pixel data offset
  put32(p + 14, kInfoHeader);
  put32(p + 18, w);
  put32(p + 22, h);  // positive: bottom-up
  put16(p + 26, 1);  // planes
  put16(p + 28, bpp);
  put32(p + 34, dstStride * h);
  put32(p + 38, 2835);  // 72 dpi
  put32(p + 42, 2835);
  put32(p + 46, colors);
  // Palette (B, G, R, 0): white down to black.
  for (uint32_t i = 0; i < colors; i++) memset(p + kFileHeader + kInfoHeader + 4 * i, 255 - 255 * i / (colors - 1), 3);

  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* src = pixels + (h - 1 - y) * srcStride;
    uint8_t* dst = p + headerBytes + y * dstStride;
    memcpy(dst, src, srcStride);
    memset(dst + srcStride, 0, dstStride - srcStride);
  }
  return true;
}

bool decodeBmp(Reader& in, Buffer& out, FrameHeader& header, const uint16_t maxSide) {
  uint8_t head[kFileHeader + kInfoHeader];
  if (!readAt(in, 0, head, sizeof(head)) || head[0] != 'B' || head[1] != 'M') return false;
  const uint32_t dataOffset = get32(head + 10);
  const uint32_t infoSize = get32(head + 14);
  const int32_t w = static_cast<int32_t>(get32(head + 18));
  const int32_t rawH = static_cast<int32_t>(get32(head + 22));
  const int32_t h = rawH < 0 ? -rawH : rawH;
  const uint8_t bpp = get16(head + 28);
  const uint8_t format = greyFormat(bpp);
  if (format == 0 || get32(head + 30) != 0 || w <= 0 || h <= 0 || w > maxSide || h > maxSide) return false;

  // Palette index -> ink level, then (below 8 bits) a table taking a whole
  // byte of indices to a byte of levels: the pixels stay where they are.
  const uint32_t colors = 1u << bpp;
  uint32_t used = get32(head + 46);
  if (used == 0 || used > colors) used = colors;
  uint8_t palette[4 * 256];
  if (!readAt(in, kFileHeader + infoSize, palette, 4 * used)) return false;
  const uint32_t maxLevel = colors - 1;
  uint8_t level[256] = {};
  for (uint32_t i = 0; i < used; i++) {
    const uint8_t* c = palette + 4 * i;  // B, G, R
    const uint32_t lum = (29 * c[0] + 150 * c[1] + 77 * c[2]) >> 8;
    level[i] = ((255 - lum) * maxLevel + 127) / 255;
  }
  uint8_t lut[256];
  for (uint32_t b = 0; b < 256; b++) {
    if (bpp == 8) {
      lut[b] = level[b];
      continue;
    }
    uint8_t v = 0;
    for (int shift = 8 - bpp; shift >= 0; shift -= bpp) v |= level[(b >> shift) & maxLevel] << shift;
    lut[b] = v;
  }

  const uint32_t srcStride = bmpStride(bpp, w);
  const uint32_t dstStride = rowBytes(format, w);
  if (!out.reserve(static_cast<size_t>(dstStride) * h)) return false;
  for (int32_t row = 0; row < h; row++) {
    const int32_t y = rawH > 0 ? h - 1 - row : row;  // bottom-up unless the height is negative
    uint8_t* dst = out.data() + y * dstStride;
    if (!readAt(in, dataOffset + row * srcStride, dst, dstStride)) return false;
    for (uint32_t i = 0; i < dstStride; i++) dst[i] = lut[dst[i]];
  }
  header = FrameHeader{};
  header.format = format;
  header.width = w;
  header.height = h;
  return true;
}

}  // namespace blit
