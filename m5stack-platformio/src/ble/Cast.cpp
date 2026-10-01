#include "Cast.h"

#include <Arduino.h>

#include "../shell/Layout.h"
#include "Links.h"
#include "Radio.h"

namespace cast {

namespace {

// Regions start on a byte in every format we take.
constexpr uint8_t kRegionAlign = 8;
constexpr uint32_t kRefreshMs = 30;  // an LCD: as fast as the frame can be pushed
// Down / Select go to the host; Back stays with the shell.
constexpr blit::Key kKeys[] = {blit::Key::Down, blit::Key::Select};

uint32_t clockMs() { return millis(); }

blit::Receiver screen;
blit::NimBleLink bleLink(screen);

}  // namespace

void begin() {
  static_assert(kMaxBytes == layout::kScreenW * layout::kScreenH * 2);
  blit::Config config;
  config.name = radio::name();
  config.panelWidth = layout::kScreenW;
  config.panelHeight = layout::kScreenH;
  config.maxBytes = kMaxBytes;
  // No persist (nowhere to save frames), no fastRefresh (every refresh is).
  config.features = 0;
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
