#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// BLE cast receiver: a GATT server that accepts frames per the blit protocol,
// v2 (../blit/PROTOCOL.md at the repo root), and still takes v1 headers.
// NimBLE runs its callbacks on its own task, which only receives (and decodes)
// bytes; the main loop takes finished frames with takeFrame(), shows them,
// then replies with notifyDone() / notifyError().
namespace cast {

// Pixel formats (blit/PROTOCOL.md#pixel-formats).
constexpr uint8_t kFormatMono1 = 1;
constexpr uint8_t kFormatGray2 = 2;

// Encodings (blit/PROTOCOL.md#encodings).
constexpr uint8_t kEncodingNone = 0;
constexpr uint8_t kEncodingPackBits = 1;

constexpr uint8_t kFlagPersist = 0x01;
constexpr uint8_t kFlagRegion = 0x02;
constexpr uint8_t kFlagHold = 0x04;

// The frame header's refresh hint.
enum class Refresh : uint8_t { Auto = 0, Fast = 1, Full = 2 };

// Key codes forwarded to the host (blit/PROTOCOL.md#key-codes).
enum class Key : uint8_t { Up = 1, Down = 2, Select = 5 };

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

// Bytes per row: rows are padded to a whole byte.
constexpr uint32_t rowBytes(const uint8_t format, const uint32_t width) {
  return (width * (format == kFormatGray2 ? 2 : 1) + 7) / 8;
}

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
  uint32_t link = 0;  // which connection it arrived on (see setRegionBase)
  char name[65] = "";

  bool region() const { return flags & kFlagRegion; }
  bool hold() const { return flags & kFlagHold; }
  // Pixel bytes once decoded.
  uint32_t frameBytes() const { return rowBytes(format, width) * height; }
};

// A growable byte buffer, in PSRAM when there is some. Move-only.
class Buffer {
 public:
  Buffer() = default;
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  bool reserve(size_t bytes);
  void swap(Buffer& other);
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }

 private:
  uint8_t* data_ = nullptr;
  size_t capacity_ = 0;
};

// IEEE CRC-32 (zlib's). Pass the previous result as `crc` to continue.
uint32_t crc32(const uint8_t* data, size_t length, uint32_t crc = 0);

class CastServer {
 public:
  // Largest payload as sent, and largest frame once decoded: a gray2 frame of
  // the whole panel on the X4C; the original X4 has no PSRAM and takes mono1.
#if FREEINK_MCU_C3
  static constexpr size_t kMaxBytes = 64 * 1024;
#else
  static constexpr size_t kMaxBytes = 96000;
#endif

  // Start advertising, bringing up the BLE stack. Returns false on failure.
  bool begin();
  // Disconnect and shut the BLE stack down (frees its memory, radio off).
  void end();
  bool running() const { return running_; }
  bool connected() const { return connected_.load(); }
  // A frame is being received (between Begin and Commit).
  bool receiving() const { return receiving_.load(); }
  // The last frame was rejected (error status sent); cleared by the next Begin.
  bool failed() const { return failed_.load(); }
  const char* name() const { return name_; }
  // The connected host's name from its hello, or "".
  void hostName(char* out, size_t size) const;

  // What caps report. setArea() tells a connected host when the area changes.
  void setArea(int width, int height);
  void setFrameSleep(bool on) { frameSleep_ = on; }
  // Offer gray2 (after mono1, which stays the preferred format).
  void setGrayscale(bool on) { grayscale_ = on; }
  void setBattery(uint8_t percent, bool usb);

  // Main loop: the full frame regions will patch, once it's on screen. Only a
  // frame received on the current connection (header.link) counts: after a
  // reconnect the host may be diffing against something else.
  void setRegionBase(const FrameHeader& header);

  // Main loop: take the committed frame, if any. Swaps its pixels (decoded)
  // into `full`, or `region` for a region update; the buffer's old contents
  // become the next receive buffer.
  bool takeFrame(FrameHeader& header, Buffer& full, Buffer& region);
  // Main loop: true (once) if anything happened on the link since the last
  // call, for the idle-sleep timer.
  bool takeActivity() { return activity_.exchange(false); }

  void notifyDone(uint32_t sleepSeconds);
  void notifyError(Error code, uint32_t detail = 0);
  // Events (v2): a forwarded key press, the battery level.
  void sendKey(Key key);
  void sendPower();
  void disconnect();

  // NimBLE callbacks (BLE task).
  void onConnect(uint16_t connHandle);
  void onDisconnect();
  void onControl(const uint8_t* data, size_t length);
  void onData(const uint8_t* data, size_t length);
  // Caps (TLV) for Info or a caps event. Returns the length written.
  size_t fillCaps(uint8_t* out, size_t size, uint16_t mtu) const;

 private:
  void notify(uint8_t event, uint8_t code, uint32_t value);
  void sendCaps();
  void hello(const uint8_t* data, size_t length);
  void beginFrame(const uint8_t* data, size_t length);
  bool parseHeader(const uint8_t* data, size_t length, FrameHeader& h);
  bool regionFits(const FrameHeader& h) const;
  void decode(const uint8_t* data, size_t length);
  void put(uint8_t byte, uint32_t count);
  void commitFrame();
  void fail(Error code, uint32_t detail = 0);

  bool running_ = false;
  char name_[12] = "";
  std::atomic<bool> connected_{false};
  std::atomic<bool> activity_{false};
  std::atomic<uint16_t> connHandle_{0};
  std::atomic<uint32_t> link_{0};  // counts connections
  std::atomic<int> areaW_{0}, areaH_{0};
  std::atomic<bool> frameSleep_{false};
  std::atomic<bool> grayscale_{false};
  std::atomic<uint8_t> battery_{255};
  std::atomic<bool> usb_{false};
  std::atomic<bool> wantsKeys_{false};  // a hello asked for key events
  char hostName_[33] = "";             // under the handoff lock

  // The frame on screen that regions may patch, under the handoff lock.
  struct {
    uint32_t link = 0;  // 0: none
    uint8_t format = 0;
    uint16_t width = 0, height = 0;
  } base_;

  // Receive state: BLE task only, except `ready_` / `rx_` handoff under lock.
  FrameHeader rxHeader_;
  Buffer rx_;  // decoded pixels
  std::atomic<bool> receiving_{false};
  std::atomic<bool> failed_{false};
  uint32_t received_ = 0;  // payload bytes, as sent (the next offset)
  uint32_t decoded_ = 0;   // pixel bytes out of the decoder
  uint32_t crc_ = 0;       // of the payload so far
  uint32_t unacked_ = 0;   // Data writes since the last ack (flow control)
  uint8_t literal_ = 0;    // PackBits: literal bytes still to copy
  uint16_t repeat_ = 0;    // PackBits: >0, the next byte repeats this often
  bool ready_ = false;     // a committed frame is waiting for takeFrame()
};

extern CastServer server;

}  // namespace cast
