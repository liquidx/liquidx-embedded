#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Frame.h"
#include "Lock.h"
#include "Wire.h"

// The display side of blit (../../PROTOCOL.md), independent of the transport.
//
// Two tasks use a Receiver. The transport's (NimBLE's callbacks, or a test's
// read loop) calls on*() as messages arrive; it validates headers, decodes
// payload bytes straight into a frame buffer, and replies. The main loop takes
// finished frames with takeFrame(), shows them, and replies with notifyDone()
// or notifyError(). The two meet only at the frame handoff and a few atomics.
namespace blit {

// Where the Receiver's replies go.
class Transport {
 public:
  virtual ~Transport() = default;
  // One message on kChannelStatus or kChannelEvent.
  virtual void send(uint8_t channel, const uint8_t* data, size_t length) = 0;
  // The largest write or notification the link carries right now (over BLE,
  // MTU − 3, at most 512).
  virtual size_t maxMessage() const = 0;
  // Drop the link, if the transport can.
  virtual void disconnect() {}
};

// What the display is: the fixed part of its caps, and its sleep policy.
struct Config {
  const char* name = "";  // caps `name`, ≤ 32 bytes. Must outlive the Receiver.
  uint16_t panelWidth = 0;
  uint16_t panelHeight = 0;
  // Largest payload as sent, and largest frame once decoded.
  uint32_t maxBytes = 0;
  uint16_t window = 16;    // Data writes per ack
  uint16_t maxSide = 2048;  // largest width or height accepted
  // kFeaturePersist, kFeatureFastRefresh, kFeaturePointer. frameSleep is set
  // at runtime with setFrameSleep().
  uint32_t features = 0;
  // Region alignment (caps `regions`), 0 for no regions. A multiple of 8
  // keeps every region on a byte in every format.
  uint8_t regionAlign = 0;
  const Key* keys = nullptr;  // forwarded to the host (caps `keys`)
  size_t keyCount = 0;
  uint32_t minIntervalMs = 0;
  uint32_t refreshMs = 0;
  // Frame sleep: only for gaps of at least this long, waking this much early
  // (to boot and advertise before the host comes back).
  uint32_t minSleepGapSeconds = 30;
  uint32_t wakeMarginSeconds = 10;
  uint32_t (*now)() = nullptr;  // milliseconds, for key event times; optional
};

class Receiver {
 public:
  static constexpr size_t kMaxFormats = 8;
  static constexpr size_t kMaxHostName = 32;
  static constexpr size_t kMaxCaps = 160;

  // Before the transport starts. Formats default to mono1.
  void begin(const Config& config);

  // --- Caps that change at runtime (main loop). A connected host gets a caps
  // event when one does.
  // The frame area. A change also forgets the region base.
  void setArea(uint16_t width, uint16_t height);
  // Most preferred first.
  void setFormats(const uint8_t* formats, size_t count);
  // Whether the display sleeps between frames (caps `frameSleep`). Only turn
  // it on if the device can wake itself on a timer.
  void setFrameSleep(bool on);
  // The battery (percent, 255 = unknown). Sends a power event when it moves
  // 5 % from what the host last saw, or the power source changes.
  void setPower(uint8_t percent, bool charging, bool external);

  // --- State (any task).
  bool connected() const { return connected_.load(); }
  // A frame is being received (between begin and commit).
  bool receiving() const { return receiving_.load(); }
  // The last frame was rejected; cleared by the next begin.
  bool failed() const { return failed_.load(); }
  // The connected host's name from its hello, or "".
  void hostName(char* out, size_t size) const;
  // True (once) if anything happened on the link since the last call, for an
  // idle-sleep timer.
  bool takeActivity() { return activity_.exchange(false); }

  // --- Frames (main loop).
  // Take the committed frame, if any. Swaps its decoded pixels into `full`, or
  // `region` for a region update; the buffer's old contents become the next
  // receive buffer. The frame is then owed exactly one reply.
  bool takeFrame(FrameHeader& header, Buffer& full, Buffer& region);
  // A full frame is on screen, so regions may patch it. Only a frame from the
  // current connection (header.link) counts: after a reconnect the host may
  // be diffing against something else.
  void setRegionBase(const FrameHeader& header);
  // The taken frame hasn't been answered yet.
  bool owesReply() const { return owed_.load(); }
  // How long to sleep after showing `header`, per the config and
  // setFrameSleep(); 0 to stay awake. Send it with notifyDone().
  uint32_t sleepFor(const FrameHeader& header) const;
  // `done`: the frame is on the panel (or stored, for hold). Hosts take it as
  // the go-ahead for the next frame, so send it only once the panel shows it.
  void notifyDone(uint32_t sleepSeconds);
  void notifyError(Error code, uint32_t detail = 0);

  // --- Events (main loop).
  // A forwarded key, if the host's hello asked for keys.
  void sendKey(Key key, KeyAction action = KeyAction::Press);
  // A pointer in frame-area pixels, if the host's hello asked for pointers.
  void sendPointer(PointerAction action, uint16_t x, uint16_t y);
  void disconnect();

  // --- Transport (its task).
  void onConnect(Transport& transport);
  void onDisconnect();
  void onControl(const uint8_t* data, size_t length);
  void onData(const uint8_t* data, size_t length);
  // Caps, as Info. Returns the length written.
  size_t fillCaps(uint8_t* out, size_t size) const;
  // After the transport is torn down: forget the link and any frame in flight.
  void reset();

 private:
  void notify(uint8_t event, uint8_t code, uint32_t value);
  void event(const uint8_t* data, size_t length);
  void sendCaps();
  void sendPower();
  uint16_t chunk() const;
  void hello(const uint8_t* data, size_t length);
  void beginFrame(const uint8_t* data, size_t length);
  bool acceptsFormat(uint8_t format) const;
  bool regionFits(const FrameHeader& h) const;
  void decode(const uint8_t* data, size_t length);
  void put(uint8_t byte, uint32_t count);
  void commitFrame();
  void fail(Error code, uint32_t detail = 0);

  Config config_;
  Transport* transport_ = nullptr;

  std::atomic<bool> connected_{false};
  std::atomic<bool> activity_{false};
  std::atomic<bool> owed_{false};
  std::atomic<uint32_t> link_{0};  // counts connections
  std::atomic<uint16_t> areaW_{0}, areaH_{0};
  std::atomic<bool> frameSleep_{false};
  std::atomic<uint8_t> battery_{255};
  std::atomic<uint8_t> powerFlags_{0};
  std::atomic<uint8_t> sentBattery_{255};  // what the host last saw
  std::atomic<uint8_t> sentPowerFlags_{0};
  std::atomic<bool> wantsKeys_{false};  // the latest hello asked for key events
  std::atomic<bool> wantsPointer_{false};

  // Under lock_: formats, the host's name, the region base, and the handoff
  // of a committed frame (ready_, rxHeader_ and rx_ once ready_).
  mutable Lock lock_;
  uint8_t formats_[kMaxFormats] = {kFormatMono1};
  size_t formatCount_ = 1;
  char hostName_[kMaxHostName + 1] = "";
  struct {
    uint32_t link = 0;  // 0: none
    uint8_t format = 0;
    uint16_t width = 0, height = 0;
  } base_;
  bool ready_ = false;  // a committed frame is waiting for takeFrame()

  // Receive state: the transport's task only.
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
};

}  // namespace blit
