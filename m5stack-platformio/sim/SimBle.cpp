// The radio, for a computer: the real blit Receiver (so frames go through the
// protocol), with no BLE under it, and a pairing code set by hand.
#include "Sim.h"
#include "ble/Cast.h"
#include "ble/Radio.h"
#include "ble/Remote.h"
#include "shell/Layout.h"

namespace sim {
const char* pairingCode = nullptr;
}

namespace {
bool radioOn = false;
blit::Receiver screen;
constexpr blit::Key kKeys[] = {blit::Key::Down, blit::Key::Select};
}  // namespace

namespace cast {

void begin() {
  blit::Config config;
  config.name = radio::name();
  config.panelWidth = layout::kScreenW;
  config.panelHeight = layout::kScreenH;
  config.maxBytes = kMaxBytes;
  config.regionAlign = 8;
  config.keys = kKeys;
  config.keyCount = 2;
  screen.begin(config);
}

blit::Receiver& receiver() { return screen; }

}  // namespace cast

namespace radio {

bool begin() { return radioOn = true; }
void end() { radioOn = false; }
bool running() { return radioOn; }
const char* name() { return "M5-1A2B"; }
const uint8_t* address() {
  static const uint8_t address[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0x1A, 0x2B};
  return address;
}

}  // namespace radio

namespace remote {

void begin() {}
void poll() {}
const char* code() { return sim::pairingCode; }
bool takeCodeChange() { return false; }
bool takeActivity() { return false; }

}  // namespace remote
