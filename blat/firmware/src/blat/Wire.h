#pragma once

#include <cstddef>
#include <cstdint>

// Numbers from the wire format (PROTOCOL.md at blat/).
namespace blat {

constexpr uint8_t kVersion = 1;

// GATT (PROTOCOL.md#transport-bluetooth-le-gatt).
constexpr const char* kServiceUuid = "b1a70000-e295-445e-b079-edd9ea2725cb";
constexpr const char* kInfoUuid = "b1a70001-e295-445e-b079-edd9ea2725cb";
constexpr const char* kRequestUuid = "b1a70002-e295-445e-b079-edd9ea2725cb";
constexpr const char* kReplyUuid = "b1a70003-e295-445e-b079-edd9ea2725cb";
constexpr const char* kDataUuid = "b1a70004-e295-445e-b079-edd9ea2725cb";
constexpr const char* kDataOutUuid = "b1a70005-e295-445e-b079-edd9ea2725cb";
constexpr const char* kEventUuid = "b1a70006-e295-445e-b079-edd9ea2725cb";

// Channels, as numbered for stream transports and sealing nonces.
enum Channel : uint8_t {
  kChannelRequest = 1,
  kChannelData = 2,
  kChannelReply = 3,
  kChannelDataOut = 4,
  kChannelEvent = 5,
};

// Request ops (PROTOCOL.md#requests).
namespace op {
constexpr uint8_t kHello = 0x01;
constexpr uint8_t kAuthBegin = 0x02;
constexpr uint8_t kAuthCode = 0x03;
constexpr uint8_t kAuthConfirm = 0x04;
constexpr uint8_t kAuthResume = 0x05;
constexpr uint8_t kRemember = 0x06;
constexpr uint8_t kGet = 0x10;
constexpr uint8_t kSet = 0x11;
constexpr uint8_t kInvoke = 0x12;
constexpr uint8_t kOptions = 0x13;
constexpr uint8_t kReadOpen = 0x20;
constexpr uint8_t kWriteOpen = 0x21;
constexpr uint8_t kAck = 0x22;
constexpr uint8_t kCommit = 0x23;
constexpr uint8_t kCancel = 0x24;
constexpr uint8_t kList = 0x25;
constexpr uint8_t kDelete = 0x26;
}  // namespace op

// Reply statuses (PROTOCOL.md#replies).
enum class Status : uint8_t {
  Ok = 0,
  Running = 1,
  BadRequest = 2,
  UnsupportedVersion = 3,
  UnknownId = 4,
  InvalidValue = 5,
  ReadOnly = 6,
  NotPermitted = 7,
  Busy = 8,
  TooLarge = 9,
  OutOfOrder = 10,
  BadCrc = 11,
  StorageError = 12,
  NotFound = 13,
  ActionFailed = 14,
  AuthRequired = 15,
  WrongCode = 16,
  LockedOut = 17,
};

constexpr uint8_t kReplyMore = 0x01;  // reply flag: the body was cut off
constexpr size_t kReplyHeaderBytes = 6;
constexpr size_t kRequestHeaderBytes = 2;

// Events (PROTOCOL.md#events).
namespace event {
constexpr uint8_t kChanged = 0x01;
constexpr uint8_t kSchema = 0x02;
constexpr uint8_t kOptionsChanged = 0x03;
constexpr uint8_t kProgress = 0x04;
constexpr uint8_t kActionDone = 0x05;
constexpr uint8_t kLog = 0x06;
}  // namespace event

// Info TLV tags (PROTOCOL.md#info).
namespace info {
constexpr uint8_t kName = 0x01;
constexpr uint8_t kModel = 0x02;
constexpr uint8_t kFirmware = 0x03;
constexpr uint8_t kDeviceId = 0x04;
constexpr uint8_t kSchema = 0x05;
constexpr uint8_t kLimits = 0x06;
constexpr uint8_t kFeatures = 0x07;
constexpr uint8_t kAuth = 0x08;

constexpr uint32_t kFeatureEvents = 1 << 0;
constexpr uint32_t kFeatureFiles = 1 << 1;
constexpr uint32_t kFeatureDirs = 1 << 2;

constexpr uint8_t kMethodScreen = 1 << 0;
constexpr uint8_t kMethodLabel = 1 << 1;
constexpr uint8_t kMethodRemember = 1 << 2;
}  // namespace info

// Schema attribute tags (PROTOCOL.md#attributes).
namespace attr {
constexpr uint8_t kKey = 0x01;
constexpr uint8_t kLabel = 0x02;
constexpr uint8_t kHelp = 0x03;
constexpr uint8_t kUnit = 0x04;
constexpr uint8_t kRange = 0x05;
constexpr uint8_t kScale = 0x06;
constexpr uint8_t kOption = 0x07;
constexpr uint8_t kLength = 0x08;
constexpr uint8_t kHint = 0x09;
constexpr uint8_t kDefault = 0x0A;
}  // namespace attr

// Authentication (PROTOCOL.md#authentication).
namespace auth {
constexpr uint8_t kMethodCode = 1;
constexpr uint8_t kMethodResume = 2;
constexpr size_t kPointBytes = 65;  // uncompressed SEC1 P-256
constexpr size_t kMacBytes = 32;
constexpr size_t kSaltBytes = 16;
constexpr size_t kHostIdBytes = 8;
constexpr size_t kHostKeyBytes = 32;
constexpr size_t kNonceBytes = 16;
constexpr size_t kTagBytes = 8;  // AES-CCM tag on sealed messages
}  // namespace auth

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

// IEEE CRC-32 (zlib's). Pass the previous result as `crc` to continue.
uint32_t crc32(const uint8_t* data, size_t length, uint32_t crc = 0);

}  // namespace blat
