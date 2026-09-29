// A blat device on stdin/stdout, for testing hosts without hardware. It
// speaks the stream framing (PROTOCOL.md#stream-transports): each message is
// u8 channel, u16 length (little-endian), then the message.
//
// Test-only channels, not part of blat:
//   host → sim  0xF0 connect (body: u16 max message size), 0xF1 disconnect,
//               0xF2 the firmware sets a value (u16 id, i32 value),
//               0xF3 move the clock forward (u32 ms)
//   sim → host  0xF0 code shown (the digits; empty when hidden),
//               0xF1 action invoked (u16 id, u8 param count)

#include <poll.h>
#include <sys/random.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "blat.h"
#include "blat/Crypto.h"

namespace {

constexpr blat::Option kModes[] = {{0, "Off"}, {1, "Eco"}, {2, "Full"}};

constexpr blat::Control kControls[] = {
    blat::group("display", "Display"),
    blat::choice("display.mode", kModes).in("display").label("Mode").initial(1).saveAs("mode").live(),
    blat::number("display.brightness")
        .in("display")
        .label("Brightness")
        .unit("%")
        .range(0, 100, 5)
        .initial(50)
        .saveAs("bright"),
    blat::toggle("display.invert").in("display").label("Invert").saveAs("invert"),
    blat::group("wifi", "Wi-Fi"),
    blat::text("wifi.ssid").in("wifi").label("Network").length(1, 32).write(2).saveAs("ssid"),
    blat::secret("wifi.password").in("wifi").label("Password").length(8, 63).write(2).saveAs("pass"),
    blat::number("power.battery").label("Battery").unit("%").range(0, 100).initial(100).readOnly().live(),
    blat::text("device.name").label("Name").initial("Sim").length(1, 20).read(1).saveAs("name"),
    blat::action("system.restart").label("Restart").confirm().write(2),
    blat::action("display.message").label("Show a message"),
    blat::text("display.message.text").in("display.message").label("Message").length(1, 40).required(),
    blat::number("display.message.seconds").in("display.message").label("For").unit("s").range(1, 60).initial(5),
};
static_assert(blat::check(kControls));

uint32_t clockOffset = 0;

uint32_t now() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) +
         clockOffset;
}

void osRandom(uint8_t* out, size_t length) {
  while (length > 0) {
    const ssize_t n = getrandom(out, length, 0);
    if (n <= 0) continue;
    out += n;
    length -= n;
  }
}

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

class StreamTransport : public blat::Transport {
 public:
  size_t max = 244;
  void send(const uint8_t channel, const uint8_t* data, const size_t length) override {
    writeFrame(channel, data, length);
  }
  size_t maxMessage() const override { return max; }
};

class SimDelegate : public blat::Delegate {
 public:
  void showCode(const char* code) override {
    writeFrame(0xF0, reinterpret_cast<const uint8_t*>(code ? code : ""), code ? strlen(code) : 0);
  }
  blat::Status invoke(const blat::Id action, const blat::Arg*, const size_t count) override {
    const uint8_t body[3] = {static_cast<uint8_t>(action), static_cast<uint8_t>(action >> 8),
                             static_cast<uint8_t>(count)};
    writeFrame(0xF1, body, sizeof(body));
    return blat::Status::Ok;
  }
};

}  // namespace

int main() {
  blat::crypto::setRandom(osRandom);
  static blat::MemoryStore store;
  static blat::Values values;
  static blat::Device device;
  static SimDelegate delegate;
  static StreamTransport transport;
  if (!values.begin(blat::table(kControls), &store)) return 1;
  blat::Identity identity;
  identity.name = "Sim-0001";
  identity.model = "blat-sim";
  identity.firmware = "0.1.0";
  memcpy(identity.deviceId, "\x5a\x11\x00\x00\x00\x00\x00\x01", 8);
  if (!device.begin(values, &store, identity, delegate, now)) return 1;

  bool connected = false;
  for (;;) {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, 10) > 0) {
      uint8_t header[3];
      if (!readAll(header, 3)) return 0;
      const size_t length = header[1] | (header[2] << 8);
      uint8_t data[65536];
      if (!readAll(data, length)) return 0;
      switch (header[0]) {
        case 0xF0:
          transport.max = length >= 2 ? blat::get16(data) : 244;
          connected = true;
          device.connected(transport);
          break;
        case 0xF1:
          connected = false;
          device.disconnected();
          break;
        case 0xF2:
          if (length >= 6) values.set(blat::get16(data), static_cast<int32_t>(blat::get32(data + 2)));
          break;
        case 0xF3:
          if (length >= 4) clockOffset += blat::get32(data);
          break;
        default:
          if (connected) device.receive(header[0], data, length);
      }
    }
    device.poll();
  }
}
