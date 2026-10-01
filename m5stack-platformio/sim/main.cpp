// Renders the firmware's screens on a computer: the same shell, widgets and
// apps, drawing into the same M5GFX sprite, written out as PPM files.
// scripts/sim.sh builds this, runs it and turns the PPMs into PNGs.
//
//   program OUT_DIR
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Fonts.h"
#include "Gfx.h"
#include "Settings.h"
#include "Sim.h"
#include "apps/BleApp.h"
#include "apps/SettingsApp.h"
#include "ble/Cast.h"
#include "shell/Layout.h"
#include "shell/Shell.h"

namespace {

Gfx canvas;
Shell shell(canvas, nullptr);
BleApp bleApp;
SettingsApp settingsApp;
std::string outDir;

// A few passes of the main loop: enough for a frame to be taken and drawn.
void pump() {
  for (int i = 0; i < 3; i++) {
    shell.tick();
    shell.flush();
  }
}

void save(const char* name) {
  pump();
  const std::string path = outDir + "/" + name + ".ppm";
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) {
    fprintf(stderr, "Can't write %s\n", path.c_str());
    exit(1);
  }
  fprintf(f, "P6\n%d %d\n255\n", layout::kScreenW, layout::kScreenH);
  for (int y = 0; y < layout::kScreenH; y++) {
    for (int x = 0; x < layout::kScreenW; x++) {
      const auto c = canvas.readPixelRGB(x, y);
      const uint8_t rgb[3] = {c.r, c.g, c.b};
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
  printf("%s\n", path.c_str());
}

void press(std::initializer_list<Action> actions) {
  for (const Action a : actions) shell.dispatch(a);
}

// A host, as far as the Receiver can tell: its messages arrive, its replies
// go nowhere.
class NullTransport : public blit::Transport {
 public:
  void send(uint8_t, const uint8_t*, size_t) override {}
  size_t maxMessage() const override { return 512; }
};
NullTransport transport;

void connectHost(const char* name) {
  cast::receiver().onConnect(transport);
  std::vector<uint8_t> hello = {blit::kOpHello, blit::kVersion, blit::kHelloKeys, static_cast<uint8_t>(strlen(name))};
  hello.insert(hello.end(), name, name + strlen(name));
  cast::receiver().onControl(hello.data(), hello.size());
}

// Send a frame the way a host does: begin, Data writes, commit.
void sendFrame(const uint8_t format, const int w, const int h, const std::vector<uint8_t>& pixels) {
  blit::FrameHeader header;
  header.version = blit::kVersion;
  header.format = format;
  header.width = w;
  header.height = h;
  header.byteLength = pixels.size();
  header.crc32 = blit::crc32(pixels.data(), pixels.size());
  uint8_t begin[blit::kHeaderBytes + blit::kMaxName];
  cast::receiver().onControl(begin, blit::encodeHeader(header, begin));
  constexpr size_t kChunk = 500;
  for (size_t at = 0; at < pixels.size(); at += kChunk) {
    const size_t n = std::min(kChunk, pixels.size() - at);
    uint8_t data[4 + kChunk];
    blit::put32(data, at);
    memcpy(data + 4, pixels.data() + at, n);
    cast::receiver().onData(data, 4 + n);
  }
  const uint8_t commit = blit::kOpCommit;
  cast::receiver().onControl(&commit, 1);
  pump();
}

// A colour test card, rgb565 high byte first.
std::vector<uint8_t> colourFrame(const int w, const int h) {
  std::vector<uint8_t> out;
  out.reserve(w * h * 2);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int r = 255 * x / w, g = 255 * y / h, b = 255 - 255 * x / w;
      const float d = std::hypot(x - w * 0.5f, y - h * 0.5f);
      if (d < h * 0.3f) r = g = b = 255 - static_cast<int>(d * 255 / (h * 0.3f)) / 2;  // a soft white disc
      if (y >= h - 16) r = g = b = (x * 8 / w) * 255 / 7;                              // grey steps
      const uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
      out.push_back(c >> 8);
      out.push_back(c & 0xFF);
    }
  }
  return out;
}

// A 1-bit frame: ink where set.
std::vector<uint8_t> monoFrame(const int w, const int h) {
  const int stride = blit::rowBytes(blit::kFormatMono1, w);
  std::vector<uint8_t> out(stride * h, 0);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const bool border = x < 3 || y < 3 || x >= w - 3 || y >= h - 3;
      const bool checks = y > h / 2 && ((x / 12 + y / 12) % 2 == 0);
      const bool stripe = y > 40 && y < 48;
      if (border || checks || stripe) out[y * stride + x / 8] |= 0x80 >> (x % 8);
    }
  }
  return out;
}

}  // namespace

int main(const int argc, char** argv) {
  outDir = argc > 1 ? argv[1] : ".";

  canvas.setColorDepth(16);
  canvas.createSprite(layout::kScreenW, layout::kScreenH);
  fonts::begin();
  settings::begin();
  cast::begin();
  shell.addApp(&bleApp);
  shell.addApp(&settingsApp);
  shell.begin();

  save("01-home");
  press({Action::Down});
  sim::heldKeys = 1 << layout::kKeyA;  // the front key, still down
  save("02-home-settings");
  sim::heldKeys = 0;

  press({Action::Select});
  save("03-settings");
  press({Action::Down, Action::Down, Action::Select});
  save("04-settings-sleep");
  press({Action::Down});
  save("05-settings-sleep-10");
  press({Action::Back, Action::Up, Action::Select, Action::Down});
  save("06-settings-accent-blue");
  press({Action::Up, Action::Back, Action::Down, Action::Down, Action::Down, Action::Select});
  save("07-about");
  press({Action::Down});
  save("08-about-scrolled");
  press({Action::Back, Action::Back, Action::Up});

  press({Action::Select});
  save("09-bluetooth-listening");
  sim::pairingCode = "482913";
  shell.invalidate();
  save("10-bluetooth-pairing");
  sim::pairingCode = nullptr;
  shell.invalidate();

  connectHost("MacBook");
  auto area = layout::cardArea(1);
  sendFrame(blit::kFormatRgb565, area.w, area.h, colourFrame(area.w, area.h));
  save("11-bluetooth-frame");
  sendFrame(blit::kFormatMono1, area.w, area.h, monoFrame(area.w, area.h));
  save("12-bluetooth-mono");
  press({Action::ToggleChrome});
  save("13-bluetooth-resizing");  // the old frame, until the host sends one for the new area
  sendFrame(blit::kFormatRgb565, layout::kScreenW, layout::kScreenH, colourFrame(layout::kScreenW, layout::kScreenH));
  save("14-bluetooth-full");
  sim::battery = 12;
  press({Action::ToggleChrome, Action::Back});
  save("15-home-low-battery");
  return 0;
}
