#include "Cast.h"

#include <Arduino.h>

#include "Radio.h"

namespace cast {

namespace {

constexpr uint16_t kPanelW = 800, kPanelH = 480;
// Regions start on a byte in every format we take (gray2 needs x % 4).
constexpr uint8_t kRegionAlign = 8;
constexpr uint32_t kRefreshMs = 1500;  // a half refresh, about
// Up / Down / Select go to the host; Back stays with the shell.
constexpr blit::Key kKeys[] = {blit::Key::Up, blit::Key::Down, blit::Key::Select};

uint32_t clockMs() { return millis(); }

blit::Receiver screen;
blit::NimBleLink bleLink(screen);

}  // namespace

void begin() {
  blit::Config config;
  config.name = radio::name();
  config.panelWidth = kPanelW;
  config.panelHeight = kPanelH;
  config.maxBytes = kMaxBytes;
  config.features = blit::kFeaturePersist | blit::kFeatureFastRefresh;
  config.regionAlign = kRegionAlign;
  config.keys = kKeys;
  config.keyCount = sizeof(kKeys) / sizeof(kKeys[0]);
  config.refreshMs = kRefreshMs;
  config.now = clockMs;
  screen.begin(config);
}

blit::Receiver& receiver() { return screen; }

blit::NimBleLink& link() { return bleLink; }

}  // namespace cast
