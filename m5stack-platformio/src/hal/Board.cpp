#include "Board.h"

#include <M5Unified.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <sys/time.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "../Log.h"
#include "../shell/Layout.h"

namespace board {

namespace {

// The front key (A), which wakes the device from sleep. GPIO 0–21 can.
constexpr gpio_num_t kWakePin = GPIO_NUM_11;
// The battery is read over I2C from the power chip; the gutter asks on every
// frame.
constexpr uint32_t kPowerPollMs = 5000;
// Any time before this means the clock was never set (it starts at 1970).
constexpr time_t kClockSetAfter = 1704067200;  // 2024-01-01

uint32_t powerReadMs = 0;
bool powerRead = false;
int battery = 0;
bool isCharging = false;
bool usb = false;

void readPower() {
  const uint32_t now = millis();
  if (powerRead && now - powerReadMs < kPowerPollMs) return;
  powerRead = true;
  powerReadMs = now;
  battery = std::clamp<int>(M5.Power.getBatteryLevel(), 0, 100);
  isCharging = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
  usb = M5.Power.getVBUSVoltage() > 4000;  // mV
}

}  // namespace

void begin() {
  auto cfg = M5.config();
  // Not used yet; leaving them off saves power.
  cfg.internal_imu = false;
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setRotation(layout::kRotation);
  M5.Display.fillScreen(TFT_BLACK);
  // The clock is the system clock, kept as local time (no time zone).
  if (M5.Rtc.isEnabled()) M5.Rtc.setSystemTimeFromRtc();
  LOG_INF("BOARD", "%s, %dx%d", M5.getBoard() == m5::board_t::board_M5StickS3 ? "M5StickS3" : "not an M5StickS3?",
          static_cast<int>(M5.Display.width()), static_cast<int>(M5.Display.height()));
}

void present(Gfx& canvas) { canvas.pushSprite(&M5.Display, 0, 0); }

uint32_t uptimeMs() { return millis(); }

void setBrightness(const int percent) {
  // Never fully off: a dark screen looks like a dead device.
  M5.Display.setBrightness(std::clamp(percent, 5, 100) * 255 / 100);
}

bool now(DateTime& out) {
  const time_t t = time(nullptr);
  if (t < kClockSetAfter) return false;
  struct tm tm;
  gmtime_r(&t, &tm);
  out.year = tm.tm_year + 1900;
  out.month = tm.tm_mon + 1;
  out.day = tm.tm_mday;
  out.hour = tm.tm_hour;
  out.minute = tm.tm_min;
  out.second = tm.tm_sec;
  out.weekday = tm.tm_wday;
  return true;
}

bool setTime(const DateTime& time) {
  struct tm tm = {};
  tm.tm_year = time.year - 1900;
  tm.tm_mon = time.month - 1;
  tm.tm_mday = time.day;
  tm.tm_hour = time.hour;
  tm.tm_min = time.minute;
  tm.tm_sec = time.second;
  // No time zone is set, so local time is UTC and mktime is its inverse.
  const struct timeval tv = {mktime(&tm), 0};
  if (tv.tv_sec < kClockSetAfter || settimeofday(&tv, nullptr) != 0) return false;
  if (M5.Rtc.isEnabled()) M5.Rtc.setDateTime(&tm);
  return true;
}

int batteryPercent() {
  readPower();
  return battery;
}

bool charging() {
  readPower();
  return isCharging;
}

bool onUsb() {
  readPower();
  return usb;
}

void sleep() {
  LOG_INF("BOARD", "Sleeping");
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  // The keys pull to ground when pressed.
  esp_sleep_enable_ext0_wakeup(kWakePin, 0);
  esp_deep_sleep_start();
}

void about(About& out) {
  snprintf(out.chip, sizeof(out.chip), "%s rev %u", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()));
  snprintf(out.flash, sizeof(out.flash), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)));
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out.mac, sizeof(out.mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  snprintf(out.memory, sizeof(out.memory), "%u KB free", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

}  // namespace board
