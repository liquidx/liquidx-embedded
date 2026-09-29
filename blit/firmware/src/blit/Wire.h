#pragma once

#include <cstddef>
#include <cstdint>

// The blit wire format (../../PROTOCOL.md): UUIDs, codes, the frame header,
// pixel layout and CRC-32. No transport, no allocation.
namespace blit {

constexpr uint8_t kVersion = 2;

// PROTOCOL.md#transport-bluetooth-le-gatt
constexpr const char* kServiceUuid = "b1ec0000-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kInfoUuid = "b1ec0001-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kControlUuid = "b1ec0002-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kDataUuid = "b1ec0003-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kStatusUuid = "b1ec0004-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kEventUuid = "b1ec0005-5f3a-4e62-9a47-0c3d8e5f2a10";

// Channels, as numbered by the stream framing (PROTOCOL.md#stream-transports).
// Over BLE each is a characteristic.
enum Channel : uint8_t { kChannelControl = 1, kChannelData = 2, kChannelStatus = 3, kChannelEvent = 4 };

// Pixel formats (PROTOCOL.md#pixel-formats).
constexpr uint8_t kFormatMono1 = 1;
constexpr uint8_t kFormatGray2 = 2;
constexpr uint8_t kFormatGray4 = 3;
constexpr uint8_t kFormatGray8 = 4;
constexpr uint8_t kFormatRgb565 = 16;
constexpr uint8_t kFormatRgb888 = 24;

// Encodings (PROTOCOL.md#encodings).
constexpr uint8_t kEncodingNone = 0;
constexpr uint8_t kEncodingPackBits = 1;

// Frame header flags.
constexpr uint8_t kFlagPersist = 0x01;
constexpr uint8_t kFlagRegion = 0x02;
constexpr uint8_t kFlagHold = 0x04;

// Control ops.
constexpr uint8_t kOpBegin = 0x01, kOpCommit = 0x02, kOpCancel = 0x03, kOpHello = 0x04;
constexpr uint8_t kHelloKeys = 0x01, kHelloPointer = 0x02;

// Status events.
constexpr uint8_t kStatusReady = 1, kStatusDone = 2, kStatusError = 3, kStatusAck = 4;

// Event types.
constexpr uint8_t kEventCaps = 0x01, kEventKey = 0x02, kEventPointer = 0x03, kEventPower = 0x04;

// Caps TLV tags and feature bits (PROTOCOL.md#caps).
constexpr uint8_t kTagName = 0x01, kTagPanel = 0x02, kTagArea = 0x03, kTagFormats = 0x04, kTagEncodings = 0x05,
                  kTagLimits = 0x06, kTagFeatures = 0x07, kTagRegions = 0x08, kTagKeys = 0x09, kTagPacing = 0x0A,
                  kTagPower = 0x0B;
constexpr uint32_t kFeaturePersist = 1 << 0, kFeatureFrameSleep = 1 << 1, kFeatureFastRefresh = 1 << 2,
                   kFeaturePointer = 1 << 3;

// The frame header's refresh hint.
enum class Refresh : uint8_t { Auto = 0, Fast = 1, Full = 2 };

// Key codes (PROTOCOL.md#key-codes). F1..F16 are 16..31.
enum class Key : uint8_t {
  Up = 1,
  Down = 2,
  Left = 3,
  Right = 4,
  Select = 5,
  Back = 6,
  Menu = 7,
  Home = 8,
  PageNext = 9,
  PagePrev = 10,
  F1 = 16,
};
enum class KeyAction : uint8_t { Press = 1, Long = 2, Down = 3, Up = 4, Repeat = 5 };
enum class PointerAction : uint8_t { Down = 1, Move = 2, Up = 3, Tap = 4 };

// Error codes sent in an `error` status.
enum class Error : uint8_t {
  BadHeader = 1,
  Unsupported = 2,
  TooLarge = 3,
  Busy = 4,
  OutOfOrder = 5,
  Incomplete = 6,
  BadCrc = 7,
  SaveFailed = 8,
  BadRegion = 9,
  Decode = 10,
};

// Bits per pixel, or 0 for a format this library doesn't know.
constexpr uint8_t bitsPerPixel(const uint8_t format) {
  switch (format) {
    case kFormatMono1:
      return 1;
    case kFormatGray2:
      return 2;
    case kFormatGray4:
      return 4;
    case kFormatGray8:
      return 8;
    case kFormatRgb565:
      return 16;
    case kFormatRgb888:
      return 24;
    default:
      return 0;
  }
}

// Bytes per row: rows are padded to a whole byte.
constexpr uint32_t rowBytes(const uint8_t format, const uint32_t width) {
  return (width * bitsPerPixel(format) + 7) / 8;
}

constexpr size_t kHeaderBytes = 29;  // before the name
constexpr size_t kMaxName = 64;

// A begin message (PROTOCOL.md#frame-header-begin-0x01).
struct FrameHeader {
  uint8_t version = 0;
  uint8_t format = 0;
  uint8_t encoding = 0;
  uint8_t flags = 0;
  Refresh refresh = Refresh::Auto;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t x = 0;  // region origin, 0 unless kFlagRegion
  uint16_t y = 0;
  uint32_t byteLength = 0;  // as sent
  uint32_t crc32 = 0;
  uint32_t nextFrameSeconds = 0;
  uint32_t link = 0;  // which connection it arrived on (Receiver::setRegionBase)
  char name[kMaxName + 1] = "";

  bool persist() const { return flags & kFlagPersist; }
  bool region() const { return flags & kFlagRegion; }
  bool hold() const { return flags & kFlagHold; }
  // Pixel bytes once decoded.
  uint32_t frameBytes() const { return rowBytes(format, width) * height; }
};

// Parse a begin message. False if it's too short for its own name.
bool parseHeader(const uint8_t* data, size_t length, FrameHeader& out);
// The begin message for `h` (for tests and tools). Returns its length; `out`
// needs kHeaderBytes + kMaxName.
size_t encodeHeader(const FrameHeader& h, uint8_t* out);

// IEEE CRC-32 (zlib's). Pass the previous result as `crc` to continue.
uint32_t crc32(const uint8_t* data, size_t length, uint32_t crc = 0);

// Little-endian helpers.
inline uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t get32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint8_t* put16(uint8_t* p, const uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
  return p + 2;
}
inline uint8_t* put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
  return p + 4;
}

}  // namespace blit
