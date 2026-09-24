#include <Arduino.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Rtc.h>
#include <XteinkDetect.h>

#include "Fonts.h"
#include "Settings.h"
#include "apps/ImageApp.h"
#include "apps/SettingsApp.h"
#include "shell/Input.h"
#include "shell/Layout.h"
#include "shell/Shell.h"

namespace {

constexpr unsigned long kPowerHoldToSleepMs = 1000;

GfxRenderer renderer(display);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
Rtc rtc;
Shell shell(renderer, rtc);

ImageApp imageApp;
SettingsApp settingsApp;

unsigned long lastActivityMs = 0;
// The hold that woke the device must be released before a new hold can sleep it.
bool powerReleasedSinceWake = false;

void enterDeepSleep() {
  LOG_INF("MAIN", "Entering deep sleep");
  renderer.clearScreen();
  renderer.drawCenteredText(fonts::MEDIUM_22, layout::kScreenH / 2 - 20, "Sleeping");
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  display.deepSleep();
  Storage.prepareForDeepSleep();
  powerManager.startDeepSleep(gpio);
}

}  // namespace

void setup() {
  BoardConfig::holdPowerRails();
  delay(250);  // let USB CDC enumerate so early logs are visible
  Serial.begin(115200);

  gpio.begin();
  powerManager.begin();

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      // Debounce accidental wakes: the button must still be held.
      if (!gpio.verifyPowerButtonWakeup()) powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
#if FREEINK_MCU_C3
      // Plugging in USB cold-boots the C3; go back to sleep like stock firmware.
      powerManager.startDeepSleep(gpio);
#endif
      // S3 boards stay awake so USB serial remains available for flashing/logs.
      break;
    default:
      break;
  }

  if (!Storage.begin()) LOG_ERR("MAIN", "SD card init failed");

#if !FREEINK_MCU_C3
  // The C3 resolves its panel controller in gpio.begin(). S3 boards (X4 Classic
  // ships with SSD1677, UC8179 or UC8279 glass) must pick it before display.begin().
  freeink::applyXteinkDisplayController();
#endif
  display.begin();
  renderer.begin();
  renderer.setOrientation(layout::kOrientation);

  if (!fontDecompressor.init()) LOG_ERR("MAIN", "Font decompressor init failed");
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  fonts::registerAll(renderer);
  settings::begin();

  shell.addApp(&imageApp);
  shell.addApp(&settingsApp);
  shell.begin();
  // From here on buttons are sampled on a background task (see Input.h).
  input::begin();

  LOG_INF("MAIN", "Booted " FW_VERSION " on %s", BoardConfig::ACTIVE.name);
  lastActivityMs = millis();
}

// Serial "SCREENSHOT" dumps the raw 1-bit framebuffer; scripts/screenshot.py
// turns it into a PNG.
void handleSerialCommands() {
  if (logSerial.available() <= 0) return;
  String line = logSerial.readStringUntil('\n');
  line.trim();
  if (line == "SCREENSHOT") {
    const uint32_t size = display.getBufferSize();
    logSerial.printf("SCREENSHOT_START:%u:%u:%u\n", static_cast<unsigned>(size), HalDisplay::DISPLAY_WIDTH,
                     HalDisplay::DISPLAY_HEIGHT);
    logSerial.write(display.getFrameBuffer(), size);
    logSerial.printf("SCREENSHOT_END\n");
  } else if (line.startsWith("KEY ")) {
    // Inject an action as if its key were pressed: KEY back|select|up|down|chrome
    const String name = line.substring(4);
    const struct {
      const char* name;
      Action action;
    } keys[] = {{"back", Action::Back},
                {"select", Action::Select},
                {"up", Action::Up},
                {"down", Action::Down},
                {"chrome", Action::ToggleChrome}};
    for (const auto& k : keys) {
      if (name == k.name) {
        shell.dispatch(k.action);
        lastActivityMs = millis();
      }
    }
  } else if (line.startsWith("TIME ")) {
    // Set the RTC: TIME YYYY-MM-DD HH:MM:SS W (W = weekday, 0 = Sunday)
    unsigned year, month, day, hour, minute, second, weekday;
    if (sscanf(line.c_str() + 5, "%u-%u-%u %u:%u:%u %u", &year, &month, &day, &hour, &minute, &second, &weekday) ==
        7) {
      Rtc::DateTime dt;
      dt.year = year;
      dt.month = month;
      dt.day = day;
      dt.hour = hour;
      dt.minute = minute;
      dt.second = second;
      dt.weekday = weekday % 7;
      logSerial.printf("TIME %s\n", rtc.set(dt) ? "ok" : "failed");
      shell.invalidate();
    } else {
      logSerial.printf("TIME usage: TIME YYYY-MM-DD HH:MM:SS W\n");
    }
  } else if (line == "STATUS") {
    // Input diagnostics: is the sampling task alive, what do the pins read,
    // and what does the driver think is held?
    const auto& in = BoardConfig::ACTIVE.input;
    const struct {
      const char* name;
      int8_t pin;
      uint8_t index;
    } keys[] = {{"back", in.back, HalGPIO::BTN_BACK},   {"confirm", in.confirm, HalGPIO::BTN_CONFIRM},
                {"left", in.left, HalGPIO::BTN_LEFT},   {"right", in.right, HalGPIO::BTN_RIGHT},
                {"up", in.up, HalGPIO::BTN_UP},         {"down", in.down, HalGPIO::BTN_DOWN},
                {"power", in.power, HalGPIO::BTN_POWER}};
    TaskHandle_t task = xTaskGetHandle("fi_input");
    const char* states[] = {"running", "ready", "blocked", "suspended", "deleted", "invalid"};
    logSerial.printf("STATUS uptime=%lums heap=%u input_task=%s", millis(),
                     static_cast<unsigned>(HalMemory::getInternalHeap().freeBytes),
                     task ? states[eTaskGetState(task)] : "missing");
    if (task) logSerial.printf(" stack_free=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)));
    logSerial.printf("\n");
    for (const auto& k : keys) {
      logSerial.printf("  %-8s gpio=%-3d raw=%s held=%d\n", k.name, k.pin,
                       k.pin < 0 ? "-" : (digitalRead(k.pin) ? "high" : "LOW"), gpio.isPressed(k.index) ? 1 : 0);
    }
  }
}

void loop() {
  handleSerialCommands();

  // Apply every press queued since the last pass (including ones made during
  // the previous refresh), then draw once.
  bool anyPress = false;
  for (Action action = input::next(anyPress); action != Action::None; action = input::next(anyPress)) {
    shell.dispatch(action);
  }
  if (anyPress) {
    lastActivityMs = millis();
    powerManager.setPowerSaving(false);
  }

  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;
  if (powerReleasedSinceWake && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > kPowerHoldToSleepMs) {
    enterDeepSleep();
  }
  const unsigned long sleepAfterMs = settings::value(settings::kSleep) * 60UL * 1000UL;  // 0 = never
  if (sleepAfterMs > 0 && millis() - lastActivityMs > sleepAfterMs) enterDeepSleep();

  shell.tick();
  shell.flush();

  // Input no longer depends on this loop's cadence, so idling slower is safe.
  if (millis() - lastActivityMs > HalPowerManager::IDLE_POWER_SAVING_MS) {
    powerManager.setPowerSaving(true);
    delay(50);
  } else {
    delay(10);
  }
}
