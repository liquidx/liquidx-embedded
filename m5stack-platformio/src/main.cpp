#include <Arduino.h>

#include "Fonts.h"
#include "Gfx.h"
#include "Log.h"
#include "Settings.h"
#include "apps/BleApp.h"
#include "apps/SettingsApp.h"
#include "ble/Cast.h"
#include "ble/Remote.h"
#include "hal/Board.h"
#include "shell/Input.h"
#include "shell/Layout.h"
#include "shell/Shell.h"

namespace {

Gfx canvas;
Shell shell(canvas, board::present);

BleApp bleApp;
SettingsApp settingsApp;

unsigned long lastActivityMs = 0;
int brightness = -1;  // what the backlight is set to

// Settings → Brightness, whoever changed it (the Settings app, or a blat host).
void applyBrightness() {
  const int wanted = settings::value(settings::kBrightness);
  if (wanted == brightness) return;
  brightness = wanted;
  board::setBrightness(wanted);
}

// Serial commands, for scripts/devctl.py.
void handleSerialCommands() {
  if (Serial.available() <= 0) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "SCREENSHOT") {
    // The canvas as it is: rgb565, high byte first, row by row.
    const size_t size = layout::kScreenW * layout::kScreenH * 2;
    Serial.printf("SCREENSHOT_START:%u:%d:%d\n", static_cast<unsigned>(size), layout::kScreenW, layout::kScreenH);
    Serial.write(static_cast<const uint8_t*>(canvas.getBuffer()), size);
    Serial.printf("SCREENSHOT_END\n");
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
    // Set the clock: TIME YYYY-MM-DD HH:MM:SS W (W = weekday, 0 = Sunday)
    unsigned year, month, day, hour, minute, second, weekday;
    if (sscanf(line.c_str() + 5, "%u-%u-%u %u:%u:%u %u", &year, &month, &day, &hour, &minute, &second, &weekday) ==
        7) {
      board::DateTime dt;
      dt.year = year;
      dt.month = month;
      dt.day = day;
      dt.hour = hour;
      dt.minute = minute;
      dt.second = second;
      dt.weekday = weekday % 7;
      Serial.printf("TIME %s\n", board::setTime(dt) ? "ok" : "failed");
      shell.invalidate();
    } else {
      Serial.printf("TIME usage: TIME YYYY-MM-DD HH:MM:SS W\n");
    }
  } else if (line == "STATUS") {
    board::About about;
    board::about(about);
    Serial.printf("STATUS " FW_VERSION " uptime=%lums %s battery=%d%% charging=%d usb=%d keys=%u\n", millis(),
                  about.memory, board::batteryPercent(), board::charging(), board::onUsb(), input::heldKeys());
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  board::begin();

  canvas.setColorDepth(16);
  if (canvas.createSprite(layout::kScreenW, layout::kScreenH) == nullptr) LOG_ERR("MAIN", "No memory for the canvas");
  fonts::begin();
  settings::begin();
  cast::begin();
  remote::begin();

  shell.addApp(&bleApp);
  shell.addApp(&settingsApp);
  shell.begin();
  applyBrightness();  // once there's something to see
  input::begin();

  LOG_INF("MAIN", "Booted " FW_VERSION);
  lastActivityMs = millis();
}

void loop() {
  handleSerialCommands();

  // Apply every press since the last pass, then draw once.
  input::poll();
  bool anyPress = false;
  for (Action action = input::next(anyPress); action != Action::None; action = input::next(anyPress)) {
    shell.dispatch(action);
  }
  if (shell.takeAppActivity()) anyPress = true;  // e.g. a Bluetooth frame
  if (anyPress) lastActivityMs = millis();

  // Sleep after the idle timeout, on battery. On USB the screen stays on (a
  // desk clock), and the serial port stays up for flashing.
  const unsigned long sleepAfterMs = settings::value(settings::kSleep) * 60UL * 1000UL;  // 0 = never
  if (board::onUsb()) lastActivityMs = millis();
  if (sleepAfterMs > 0 && millis() - lastActivityMs > sleepAfterMs) {
    if (App* app = shell.currentApp()) app->onClose();  // radio off
    board::sleep();
  }

  shell.tick();
  shell.flush();
  applyBrightness();

  delay(shell.busy() ? 1 : 5);
}
