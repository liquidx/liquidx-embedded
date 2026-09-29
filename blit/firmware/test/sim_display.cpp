// A blit display on stdin/stdout, built from the library, for testing hosts
// without hardware. It speaks the stream framing (PROTOCOL.md#stream-transports):
// each message is u8 channel, u16 length (little-endian), then the message.
//
// Test-only channels, not part of blit:
//   host → sim  0xF0 connect (body: u16 max message size), 0xF1 disconnect,
//               0xF2 read Info, 0xF3 press a key (u8 key),
//               0xF4 set the area (u16 width, u16 height),
//               0xF5 set the formats (u8 each, most preferred first),
//               0xF6 frame sleep on or off (u8)
//   sim → host  0xF2 Info (caps), 0xF3 a frame shown (u8 format, u16 width,
//               u16 height, then the whole frame's pixels, regions applied)

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "blit.h"

namespace {

constexpr blit::Key kKeys[] = {blit::Key::Up, blit::Key::Down, blit::Key::Select};

void writeFrame(const uint8_t channel, const uint8_t* data, const size_t length) {
  const uint8_t header[3] = {channel, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8)};
  fwrite(header, 1, 3, stdout);
  if (length > 0) fwrite(data, 1, length, stdout);
  fflush(stdout);
}

bool readAll(uint8_t* out, size_t length) {
  while (length > 0) {
    const ssize_t n = read(STDIN_FILENO, out, length);
    if (n <= 0) return false;
    out += n;
    length -= n;
  }
  return true;
}

class StreamTransport : public blit::Transport {
 public:
  size_t max = 244;
  void send(const uint8_t channel, const uint8_t* data, const size_t length) override {
    writeFrame(channel, data, length);
  }
  size_t maxMessage() const override { return max; }
};

// What a display's main loop does: take frames, patch regions into the full
// frame, "show" it, and answer.
void showFrames(blit::Receiver& receiver, blit::Buffer& frame, blit::Buffer& region, blit::FrameHeader& shown) {
  blit::FrameHeader header;
  if (!receiver.takeFrame(header, frame, region)) return;
  if (header.region()) {
    blit::applyRegion(frame.data(), shown, header, region.data());
  } else {
    shown = header;
    receiver.setRegionBase(header);
  }
  if (!header.hold()) {
    std::vector<uint8_t> out(5 + shown.frameBytes());
    out[0] = shown.format;
    blit::put16(blit::put16(out.data() + 1, shown.width), shown.height);
    memcpy(out.data() + 5, frame.data(), shown.frameBytes());
    writeFrame(0xF3, out.data(), out.size());
  }
  receiver.notifyDone(receiver.sleepFor(header));
}

}  // namespace

int main() {
  static blit::Receiver receiver;
  static StreamTransport transport;
  static blit::Buffer frame, region;
  static std::vector<uint8_t> message(65536);

  blit::Config config;
  config.name = "blit-sim";
  config.panelWidth = 64;
  config.panelHeight = 32;
  config.maxBytes = 16 * 1024;
  config.features = blit::kFeaturePersist | blit::kFeatureFastRefresh;
  config.regionAlign = 8;
  config.keys = kKeys;
  config.keyCount = sizeof(kKeys) / sizeof(kKeys[0]);
  config.refreshMs = 5;
  receiver.begin(config);
  const uint8_t formats[] = {blit::kFormatMono1, blit::kFormatGray2};
  receiver.setFormats(formats, sizeof(formats));
  receiver.setPower(87, false, false);

  blit::FrameHeader shown;
  uint8_t header[3];
  while (readAll(header, 3)) {
    const uint8_t channel = header[0];
    const size_t length = header[1] | (header[2] << 8);
    uint8_t* body = message.data();
    if (!readAll(body, length)) break;
    switch (channel) {
      case blit::kChannelControl:
        receiver.onControl(body, length);
        break;
      case blit::kChannelData:
        receiver.onData(body, length);
        break;
      case 0xF0:
        if (length >= 2) transport.max = blit::get16(body);
        receiver.onConnect(transport);
        break;
      case 0xF1:
        receiver.onDisconnect();
        break;
      case 0xF2: {
        uint8_t caps[blit::Receiver::kMaxCaps];
        writeFrame(0xF2, caps, receiver.fillCaps(caps, sizeof(caps)));
        break;
      }
      case 0xF3:
        if (length >= 1) receiver.sendKey(static_cast<blit::Key>(body[0]));
        break;
      case 0xF4:
        if (length >= 4) receiver.setArea(blit::get16(body), blit::get16(body + 2));
        break;
      case 0xF5:
        receiver.setFormats(body, length);
        break;
      case 0xF6:
        receiver.setFrameSleep(length >= 1 && body[0] != 0);
        break;
      default:
        break;
    }
    showFrames(receiver, frame, region, shown);
  }
  return 0;
}
