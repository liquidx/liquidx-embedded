#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// BLE cast receiver: a GATT server that accepts frames per
// the blit protocol, v1 (../blit/PROTOCOL.md at the repo root). NimBLE runs its callbacks on its own task, which
// only buffers bytes; the main loop takes finished frames with takeFrame(),
// shows them, then replies with notifyDone() / notifyError().
namespace cast {

// Pixel formats (blit/PROTOCOL.md#pixel-formats).
constexpr uint8_t kFormatRaw1 = 1;

constexpr uint8_t kFlagPersist = 0x01;

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
};

struct FrameHeader {
  uint8_t format = 0;
  uint8_t flags = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t byteLength = 0;
  uint32_t crc32 = 0;
  uint32_t nextFrameSeconds = 0;
  char name[65] = "";
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

uint32_t crc32(const uint8_t* data, size_t length);

class CastServer {
 public:
  static constexpr size_t kMaxBytes = 64 * 1024;

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

  // What Info reports: the current frame area and settings.
  void setArea(int width, int height) {
    areaW_ = width;
    areaH_ = height;
  }
  void setFrameSleep(bool on) { frameSleep_ = on; }

  // Main loop: take the committed frame, if any. Swaps its buffer into
  // `buffer` (whose old contents become the next receive buffer).
  bool takeFrame(FrameHeader& header, Buffer& buffer);
  // Main loop: true (once) if anything happened on the link since the last
  // call, for the idle-sleep timer.
  bool takeActivity() { return activity_.exchange(false); }

  void notifyDone(uint32_t sleepSeconds);
  void notifyError(Error code, uint32_t detail = 0);
  void disconnect();

  // NimBLE callbacks (BLE task).
  void onConnect(uint16_t connHandle);
  void onDisconnect();
  void onControl(const uint8_t* data, size_t length);
  void onData(const uint8_t* data, size_t length);
  void fillInfo(char* out, size_t size, uint16_t mtu) const;

 private:
  void notify(uint8_t event, uint8_t code, uint32_t value);
  void beginFrame(const uint8_t* data, size_t length);
  void commitFrame();

  bool running_ = false;
  char name_[12] = "";
  std::atomic<bool> connected_{false};
  std::atomic<bool> activity_{false};
  std::atomic<uint16_t> connHandle_{0};
  std::atomic<int> areaW_{0}, areaH_{0};
  std::atomic<bool> frameSleep_{false};

  // Receive state: BLE task only, except `ready_` / `rx_` handoff under lock.
  FrameHeader rxHeader_;
  Buffer rx_;
  std::atomic<bool> receiving_{false};
  std::atomic<bool> failed_{false};
  uint32_t expected_ = 0;  // next offset
  uint32_t unacked_ = 0;   // Data writes since the last ack (flow control)
  bool ready_ = false;     // a committed frame is waiting for takeFrame()
};

extern CastServer server;

}  // namespace cast
