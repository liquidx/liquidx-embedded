#include "BleApp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../Log.h"
#include "../Settings.h"
#include "../ble/Radio.h"
#include "../ble/Remote.h"
#include "../hal/Board.h"

namespace {

// Battery level checks, for the host's power events.
constexpr uint32_t kBatteryPollMs = 30 * 1000;

// Colour first: it's what the panel is. Hosts with nothing but text to show
// can pick a grey format instead and send a fraction of the bytes (mono1 is a
// sixteenth of rgb565).
constexpr uint8_t kFormats[] = {blit::kFormatRgb565, blit::kFormatGray4, blit::kFormatGray2, blit::kFormatMono1};

blit::Receiver& receiver() { return cast::receiver(); }

}  // namespace

void BleApp::onOpen() {
  nextSeconds_ = 0;
  const auto area = layout::cardArea(1);
  receiver().setArea(area.w, area.h);
  receiver().setFormats(kFormats, sizeof(kFormats));
  lastBatteryPollMs_ = board::uptimeMs() - kBatteryPollMs;  // poll now, for the caps
  pollBattery();
  if (!radio::begin()) LOG_ERR("BLE", "Couldn't start Bluetooth");
}

void BleApp::onClose() { radio::end(); }

bool BleApp::takeActivity() {
  const bool cast = receiver().takeActivity();
  return remote::takeActivity() || cast;
}

// Down / Select belong to the host (caps `keys`); Back stays with the shell,
// for leaving the app.
Result BleApp::handle(const Action action) {
  switch (action) {
    case Action::Down:
      receiver().sendKey(blit::Key::Down);
      break;
    case Action::Select:
      receiver().sendKey(blit::Key::Select);
      break;
    default:
      break;
  }
  return Result::Ignored;
}

BleApp::Status BleApp::status() const {
  if (!radio::running()) return Status::Unavailable;
  if (receiver().receiving()) return Status::Receiving;
  if (!receiver().connected()) return Status::Listening;
  if (receiver().failed()) return Status::Failed;
  return nextSeconds_ > 0 ? Status::Waiting : Status::Connected;
}

void BleApp::statusText(char* out, const size_t size) const {
  switch (status()) {
    case Status::Unavailable:
      snprintf(out, size, "Unavailable");
      break;
    case Status::Listening:
      snprintf(out, size, "Listening");
      break;
    case Status::Connected:
      // The host's name from its hello, if it gave one.
      receiver().hostName(out, size);
      if (out[0] == '\0') snprintf(out, size, "Connected");
      break;
    case Status::Receiving:
      snprintf(out, size, "Receiving");
      break;
    case Status::Waiting:
      snprintf(out, size, "Next in %u s", static_cast<unsigned>(nextSeconds_));
      break;
    case Status::Failed:
      snprintf(out, size, "Transfer failed");
      break;
  }
}

void BleApp::pollBattery() {
  const uint32_t now = board::uptimeMs();
  if (now - lastBatteryPollMs_ < kBatteryPollMs) return;
  lastBatteryPollMs_ = now;
  const uint8_t percent = std::clamp(board::batteryPercent(), 0, 100);
  receiver().setPower(percent, board::charging(), board::onUsb());
  settings::set(settings::kBattery, percent);  // blat hosts see it as power.battery
}

Result BleApp::tick() {
  pollBattery();
  remote::poll();
  // A blat host may have changed settings: the accent colour is on this page
  // (the rest are read where they're used).
  const bool settingsChanged = settings::values().takeHostChanges();
  // The pairing code appeared or went away.
  if (remote::takeCodeChange() || settingsChanged) return Result::Redraw;
  blit::FrameHeader header;
  if (!receiver().takeFrame(header, frame_, region_)) {
    // Redraw only when the status on screen changes: the pill, or the
    // "Listening" page. Without chrome a frame hides both.
    const bool visible = chrome_ || !showingFrame_;
    return visible && status() != shownStatus_ ? Result::Redraw : Result::Ignored;
  }
  return showFrame(header);
}

// A frame or region arrived (its pixels already in frame_ or region_).
Result BleApp::showFrame(const blit::FrameHeader& header) {
  LOG_INF("BLE", "%s %ux%u received (%u bytes%s)", header.region() ? "Region" : "Frame", header.width, header.height,
          static_cast<unsigned>(header.byteLength), header.encoding ? ", packbits" : "");
  if (header.region()) {
    // Patch the full frame. The receiver checked the region fits it: same
    // format, starting on a byte, within the frame (which is the area).
    blit::applyRegion(frame_.data(), header_, header, region_.data());
  } else {
    header_ = header;
    haveFrame_ = true;
    receiver().setRegionBase(header);
  }
  nextSeconds_ = header.nextFrameSeconds;

  // Held: more regions follow, and the next frame without hold shows them all.
  if (header.hold()) {
    receiver().notifyDone(0);
    return Result::Ignored;
  }
  return Result::Redraw;  // the reply waits for presented()
}

// `done` means "on the panel": hosts treat it as the go-ahead for the next frame.
void BleApp::presented() {
  if (receiver().owesReply()) receiver().notifyDone(0);
}

void BleApp::render(Gfx& g, const layout::Rect& area, const bool chrome) {
  receiver().setArea(area.w, area.h);
  chrome_ = chrome;
  shownStatus_ = status();
  char text[40];
  statusText(text, sizeof(text));
  if (const char* code = remote::code()) return renderCode(g, area, chrome, code);

  // A frame fills the card; the title pill over it shows the link status.
  // Full screen shows just the frame.
  showingFrame_ = haveFrame_;
  if (haveFrame_) drawFrame(g, area);
  char title[56];
  snprintf(title, sizeof(title), "Bluetooth / %s", text);
  ui::drawTitle(g, area, chrome ? title : nullptr);
  if (showingFrame_) return;

  const int midX = area.x + area.w / 2;
  const int midY = area.y + area.h / 2;
  ui::drawTextCentred(g, fonts::MEDIUM_16, midX, midY + 6, text);
  if (radio::running()) ui::drawTextCentred(g, fonts::SMALL_12, midX, midY + 26, radio::name(), theme::kTextDim);
}

// A host asked to pair (blat): show the code for its user to type in, "482 913",
// until it's used or expires.
void BleApp::renderCode(Gfx& g, const layout::Rect& area, const bool chrome, const char* code) {
  showingFrame_ = false;
  ui::drawTitle(g, area, chrome ? "Bluetooth / Pairing" : nullptr);
  char spaced[8];
  snprintf(spaced, sizeof(spaced), "%.3s %.3s", code, code + 3);
  const int midX = area.x + area.w / 2;
  const int midY = area.y + area.h / 2;
  ui::drawTextCentred(g, fonts::DISPLAY_46, midX, midY + 24, spaced, theme::accent());
  ui::drawTextCentred(g, fonts::SMALL_12, midX, midY + 48, "Enter this code on the host", theme::kTextDim);
}

// Blit the frame at native size: pinned top-left when larger than the area
// (the clip rect crops it), centred when smaller.
void BleApp::drawFrame(Gfx& g, const layout::Rect& area) const {
  const int w = header_.width, h = header_.height;
  const int x0 = area.x + ui::imageOrigin(w, area.w);
  const int y0 = area.y + ui::imageOrigin(h, area.h);
  const uint8_t format = header_.format;
  if (format == blit::kFormatRgb565) {
    // High byte first: what the panel and the sprite both hold.
    g.pushImage(x0, y0, w, h, reinterpret_cast<const lgfx::swap565_t*>(frame_.data()));
    return;
  }

  // Grey formats count ink on white paper.
  const int stride = blit::rowBytes(format, w);
  const int black = (1 << blit::bitsPerPixel(format)) - 1;
  const int rows = std::min(h, area.y + area.h - y0);
  const int cols = std::min(w, area.x + area.w - x0);
  g.startWrite();
  for (int y = 0; y < rows; y++) {
    const uint8_t* row = frame_.data() + y * stride;
    for (int x = 0; x < cols; x++) {
      const uint8_t white = 255 - blit::level(row, format, x) * 255 / black;
      g.writePixel(x0 + x, y0 + y, lgfx::color888(white, white, white));
    }
  }
  g.endWrite();
}
