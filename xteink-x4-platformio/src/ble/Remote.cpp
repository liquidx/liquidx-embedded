#include "Remote.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_random.h>

#include <cstring>

#include "../Settings.h"
#include "Radio.h"
#include "blat/Crypto.h"

namespace remote {

namespace {

// Time for the reply to reach the host before a requested restart.
constexpr uint32_t kRestartDelayMs = 500;

#if FREEINK_MCU_C3
constexpr const char* kModel = "xteink-x4";
#else
constexpr const char* kModel = "xteink-x4c";
#endif

void espRandom(uint8_t* out, const size_t length) { esp_fill_random(out, length); }
uint32_t clockMs() { return millis(); }

class X4Delegate : public blat::Delegate {
 public:
  void showCode(const char* shown) override {
    snprintf(code, sizeof(code), "%s", shown ? shown : "");
    changed = true;
    if (shown) LOG_INF("BLAT", "Pairing code shown");
  }

  blat::Status invoke(const blat::Id action, const blat::Arg*, size_t) override {
    if (action == settings::kRestart) {
      restartAtMs = millis() + kRestartDelayMs;
      restartPending = true;
      return blat::Status::Ok;
    }
    return blat::Status::ActionFailed;
  }

  char code[8] = "";
  bool changed = false;
  bool restartPending = false;
  uint32_t restartAtMs = 0;
};

X4Delegate delegate;
blat::Device device;
blat::NimBleLink bleLink(device);

}  // namespace

void begin() {
  blat::crypto::setRandom(espRandom);
  blat::Identity identity;
  identity.name = radio::name();
  identity.model = kModel;
  identity.firmware = FW_VERSION;
  memcpy(identity.deviceId, radio::address(), 6);  // the Bluetooth address, then two zero bytes
  if (!device.begin(settings::values(), &settings::store(), identity, delegate, clockMs)) {
    LOG_ERR("BLAT", "Couldn't start");
  }
}

blat::NimBleLink& link() { return bleLink; }

void poll() {
  bleLink.poll();
  if (delegate.restartPending && static_cast<int32_t>(millis() - delegate.restartAtMs) >= 0) {
    LOG_INF("BLAT", "Restarting, as a host asked");
    ESP.restart();
  }
}

const char* code() { return delegate.code[0] != '\0' ? delegate.code : nullptr; }

bool takeCodeChange() {
  const bool was = delegate.changed;
  delegate.changed = false;
  return was;
}

bool takeActivity() { return device.takeActivity(); }

}  // namespace remote
