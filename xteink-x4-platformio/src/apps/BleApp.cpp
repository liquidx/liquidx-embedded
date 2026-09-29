#include "BleApp.h"

#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "../Fonts.h"
#include "../Settings.h"
#include "../ble/Radio.h"
#include "../ble/Remote.h"
#include "ImageApp.h"

namespace {

constexpr const char* kImagesDir = "/images";
constexpr const char* kPrefs = "ble";
constexpr const char* kPrefLast = "last";  // path of the last persisted frame
constexpr const char* kPrefSeq = "seq";    // counter for unnamed frames

// The original X4's deep sleep cuts battery power, so a timer can't wake it,
// and it doesn't offer frame sleep.
#if FREEINK_MCU_C3
constexpr bool kCanTimerWake = false;
#else
constexpr bool kCanTimerWake = true;
#endif

// Battery level checks, for the host's power events.
constexpr uint32_t kBatteryPollMs = 30 * 1000;

blit::Receiver& receiver() { return cast::receiver(); }

bool frameSleep() { return kCanTimerWake && settings::value(settings::kFrameSleep) != 0; }

// "My Photo!.BMP" -> "My_Photo_.bmp"; empty when nothing usable is left.
void sanitiseName(const char* in, char* out, const size_t size) {
  size_t n = 0;
  for (; in[n] != '\0' && n < size - 5; n++) {
    const char c = in[n];
    out[n] = isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ? c : '_';
  }
  out[n] = '\0';
  if (n >= 4 && strcasecmp(out + n - 4, ".bmp") == 0) out[n -= 4] = '\0';
  while (n > 0 && out[n - 1] == '.') out[--n] = '\0';
  if (n > 0) strcat(out, ".bmp");
}

// A frame as a 1- or 2-bit BMP, in one write: SdFat is much slower with many
// small ones.
bool writeBmp(const char* path, const blit::FrameHeader& header, const uint8_t* pixels) {
  blit::Buffer out;
  if (!blit::encodeBmp(pixels, header.format, header.width, header.height, out)) return false;
  const size_t size = blit::bmpSize(header.format, header.width, header.height);
  if (Storage.exists(path)) Storage.remove(path);
  HalFile file;
  if (!Storage.openFileForWrite("BLE", path, file)) return false;
  const bool ok = file.write(out.data(), size) == size;
  file.close();
  return ok;
}

class FileReader : public blit::Reader {
 public:
  explicit FileReader(HalFile& file) : file_(file) {}
  size_t read(uint8_t* out, const size_t length) override {
    const int n = file_.read(out, length);
    return n > 0 ? n : 0;
  }
  bool seek(const uint32_t offset) override { return file_.seek(offset); }

 private:
  HalFile& file_;
};

}  // namespace

void BleApp::onOpen() {
  if (lastPath_[0] == '\0') {
    Preferences prefs;
    prefs.begin(kPrefs, true);
    prefs.getString(kPrefLast, lastPath_, sizeof(lastPath_));
    prefs.end();
  }
  // Keep the last saved frame in memory so redraws (chrome toggle, status
  // changes) don't decode it from the SD card each time.
  if (!haveFrame_ && lastPath_[0] != '\0') haveFrame_ = loadLastFrame();
  nextSeconds_ = 0;
  const auto area = layout::cardArea(1);
  receiver().setArea(area.w, area.h);
  receiver().setFrameSleep(frameSleep());
  // mono1 first: it refreshes fast and keeps the gutter usable. Hosts that
  // want greys pick gray2 explicitly.
#if FREEINK_MCU_C3
  const bool gray = false;  // no PSRAM for the grey planes
#else
  const bool gray = display.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
#endif
  const uint8_t formats[] = {blit::kFormatMono1, blit::kFormatGray2};
  receiver().setFormats(formats, gray ? 2 : 1);
  lastBatteryPollMs_ = millis() - kBatteryPollMs;  // poll now, for the caps
  pollBattery();
  if (!radio::begin()) LOG_ERR("BLE", "Couldn't start Bluetooth");
}

void BleApp::onClose() { radio::end(); }

bool BleApp::takeActivity() {
  const bool cast = receiver().takeActivity();
  return remote::takeActivity() || cast;
}

uint32_t BleApp::takeSleepRequest() {
  if (receiver().owesReply()) return 0;  // the host hasn't been told yet
  const uint32_t seconds = sleepRequest_;
  sleepRequest_ = 0;
  return seconds;
}

// Up / Down / Select belong to the host (caps `keys`); Back stays with the
// shell, for leaving the app.
Result BleApp::handle(const Action action) {
  switch (action) {
    case Action::Up:
      receiver().sendKey(blit::Key::Up);
      break;
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
  const uint32_t now = millis();
  if (now - lastBatteryPollMs_ < kBatteryPollMs) return;
  lastBatteryPollMs_ = now;
  const uint8_t percent = std::min<uint16_t>(powerManager.getBatteryPercentage(), 100);
  // The X4 can't tell charging from USB power, so it reports external power.
  receiver().setPower(percent, false, gpio.isUsbConnected());
  settings::set(settings::kBattery, percent);  // blat hosts see it as power.battery
}

Result BleApp::tick() {
  pollBattery();
  remote::poll();
  // A blat host may have changed settings. Frame sleep is read here; the rest
  // are read where they're used.
  if (settings::values().takeHostChanges()) {
    receiver().setFrameSleep(frameSleep());
  }
  // The pairing code appeared or went away.
  if (remote::takeCodeChange()) return Result::CleanRedraw;
  blit::FrameHeader header;
  if (!receiver().takeFrame(header, frame_, region_)) {
    // Redraw (a fast refresh) only when the status on screen changes: the
    // pill, or the "Listening" page. Without chrome a frame hides both.
    // Greys take a slow refresh, so they skip the short-lived "Receiving".
    const bool visible = chrome_ || !showingFrame_;
    const Status now = status();
    if (grayShown_ && now == Status::Receiving) return Result::Ignored;
    return visible && now != shownStatus_ ? Result::Redraw : Result::Ignored;
  }
  return showFrame(header);
}

// A frame or region arrived (its pixels already in frame_ or region_).
Result BleApp::showFrame(const blit::FrameHeader& header) {
  LOG_INF("BLE", "%s %ux%u received (%u bytes%s)", header.region() ? "Region" : "Frame", header.width,
          header.height, static_cast<unsigned>(header.byteLength), header.encoding ? ", packbits" : "");
  blit::FrameHeader saved = header;
  if (header.region()) {
    // Patch the full frame. The receiver checked the region fits it: same
    // format, starting on a byte, within the frame (which is the area).
    blit::applyRegion(frame_.data(), header_, header, region_.data());
    saved = header_;
    memcpy(saved.name, header.name, sizeof(saved.name));
  } else {
    header_ = header;
    haveFrame_ = true;
    receiver().setRegionBase(header);
  }
  nextSeconds_ = header.nextFrameSeconds;

  if (header.persist()) {
    char path[96];
    if (!persist(saved, path, sizeof(path))) {
      receiver().notifyError(blit::Error::SaveFailed);
      return header.hold() ? Result::Ignored : Result::CleanRedraw;
    }
  }

  // Held: more regions follow, and the next frame without hold shows them all.
  if (header.hold()) {
    receiver().notifyDone(0);
    return Result::Ignored;
  }

  // Sleep until just before the next frame, if asked to and it's worth it.
  // The reply waits for presented().
  sleepRequest_ = receiver().sleepFor(header);

  // The refresh hint: fast, full, or ours to pick (fast for a region, which
  // is usually a small UI change; clean for a new frame).
  switch (header.refresh) {
    case blit::Refresh::Fast:
      return Result::Redraw;
    case blit::Refresh::Full:
      return Result::CleanRedraw;
    case blit::Refresh::Auto:
      break;
  }
  return header.region() ? Result::Redraw : Result::CleanRedraw;
}

// `done` means "on the panel": hosts treat it as the go-ahead for the next frame.
void BleApp::presented() {
  if (receiver().owesReply()) receiver().notifyDone(sleepRequest_);
}

bool BleApp::persist(const blit::FrameHeader& header, char* path, const size_t pathSize) {
  Preferences prefs;
  prefs.begin(kPrefs, false);
  char name[72];
  sanitiseName(header.name, name, sizeof(name));
  if (name[0] == '\0') {
    const uint32_t seq = prefs.getUInt(kPrefSeq, 0) + 1;
    prefs.putUInt(kPrefSeq, seq);
    snprintf(name, sizeof(name), "cast-%04u.bmp", static_cast<unsigned>(seq));
  }
  snprintf(path, pathSize, "%s/%s", kImagesDir, name);

  const unsigned long start = millis();
  Storage.ensureDirectoryExists(kImagesDir);
  const bool ok = writeBmp(path, header, frame_.data());
  if (ok) {
    snprintf(lastPath_, sizeof(lastPath_), "%s", path);
    prefs.putString(kPrefLast, lastPath_);
    ImageApp::markStale();
    LOG_INF("BLE", "Saved %s in %lu ms", path, millis() - start);
  } else {
    LOG_ERR("BLE", "Couldn't save %s", path);
  }
  prefs.end();
  return ok;
}

void BleApp::render(GfxRenderer& r, const layout::Rect& area, const bool chrome) {
  receiver().setArea(area.w, area.h);
  chrome_ = chrome;
  shownStatus_ = status();
  char text[40];
  statusText(text, sizeof(text));
  if (const char* code = remote::code()) return renderCode(r, area, chrome, code);

  // A frame fills the card (the last received, or the last saved one); the
  // title pill over it shows the link status. Full screen shows just the frame.
  showingFrame_ = haveFrame_ || (lastPath_[0] != '\0' && ui::drawBitmapFile(r, lastPath_, area));
  grayShown_ = haveFrame_ && header_.format == blit::kFormatGray2;
  if (haveFrame_) drawFrame(r, area);
  char title[56];
  snprintf(title, sizeof(title), "Bluetooth / %s", text);
  ui::drawTitle(r, area, chrome ? title : nullptr, &pill_);
  if (showingFrame_) return;

  const int textW = r.getTextWidth(fonts::MEDIUM_22, text);
  const int midY = area.y + area.h / 2;
  ui::drawTextAt(r, fonts::MEDIUM_22, area.x + (area.w - textW) / 2, midY, text);
  if (radio::running()) {
    const int nameW = r.getTextWidth(fonts::SMALL_15, radio::name());
    ui::drawTextAt(r, fonts::SMALL_15, area.x + (area.w - nameW) / 2, midY + 30, radio::name());
  }
}

// A host asked to pair (blat): show the code for its user to type in, "482 913",
// until it's used or expires.
void BleApp::renderCode(GfxRenderer& r, const layout::Rect& area, const bool chrome, const char* code) {
  showingFrame_ = false;
  grayShown_ = false;
  ui::drawTitle(r, area, chrome ? "Bluetooth / Pairing" : nullptr, &pill_);
  char spaced[8];
  snprintf(spaced, sizeof(spaced), "%.3s %.3s", code, code + 3);
  const int midY = area.y + area.h / 2;
  const char* heading = "Pairing code";
  const int headingW = r.getTextWidth(fonts::MEDIUM_22, heading);
  ui::drawTextAt(r, fonts::MEDIUM_22, area.x + (area.w - headingW) / 2, midY - 80, heading);
  const int codeW = r.getTextWidth(fonts::DISPLAY_136, spaced);
  ui::drawTextAt(r, fonts::DISPLAY_136, area.x + (area.w - codeW) / 2, midY + 50, spaced);
  const char* hint = "Enter it on the device that's connecting";
  const int hintW = r.getTextWidth(fonts::SMALL_15, hint);
  ui::drawTextAt(r, fonts::SMALL_15, area.x + (area.w - hintW) / 2, midY + 100, hint);
}

// Read a 1- or 2-bit BMP (what persist() writes) into frame_ as mono1 or
// gray2. Other files return false and are drawn from the file instead.
bool BleApp::loadLastFrame() {
  HalFile file;
  if (!Storage.openFileForRead("BLE", lastPath_, file)) return false;
  FileReader reader(file);
  blit::FrameHeader header;
  const bool ok = blit::decodeBmp(reader, frame_, header) &&
                  (header.format == blit::kFormatMono1 || header.format == blit::kFormatGray2);
  file.close();
  if (ok) header_ = header;
  return ok;
}

// Blit the frame at native size: pinned top-left when larger than the area
// (the clip rect crops it), centred when smaller. gray2 goes in as black and
// white (dark and black ink); drawGray() adds the greys.
void BleApp::drawFrame(GfxRenderer& r, const layout::Rect& area) const {
  const int w = header_.width, h = header_.height;
  const int x0 = area.x + ui::imageOrigin(w, area.w);
  const int y0 = area.y + ui::imageOrigin(h, area.h);
  const int stride = blit::rowBytes(header_.format, w);
  const int rows = std::min(h, area.y + area.h - y0);
  const int cols = std::min(w, area.x + area.w - x0);
  const uint8_t format = header_.format;
  const uint8_t black = format == blit::kFormatGray2 ? 2 : 1;  // dark counts as black
  const uint8_t* bits = frame_.data();
  for (int y = 0; y < rows; y++) {
    const uint8_t* row = bits + y * stride;
    for (int x = 0; x < cols; x++) {
      const bool ink = blit::level(row, format, x) >= black;
      if (ink) r.drawPixel(x0 + x, y0 + y);
    }
  }
}

// The light and dark pixels of a gray2 frame, where drawFrame() put them,
// except under the title pill. Planes: light (0, 1), dark (1, 0) as LSB, MSB.
void BleApp::drawGray(uint8_t* lsb, uint8_t* msb, const layout::Rect& area) const {
  if (!grayShown_) return;
  constexpr int kStride = layout::kScreenW / 8;
  const int w = header_.width, h = header_.height;
  const int x0 = area.x + ui::imageOrigin(w, area.w);
  const int y0 = area.y + ui::imageOrigin(h, area.h);
  const int stride = blit::rowBytes(header_.format, w);
  const int yEnd = std::min({h, area.y + area.h - y0, layout::kScreenH - y0});
  const int xEnd = std::min({w, area.x + area.w - x0, layout::kScreenW - x0});
  const int xStart = std::max({0, area.x - x0, -x0});
  for (int y = std::max({0, area.y - y0, -y0}); y < yEnd; y++) {
    const uint8_t* row = frame_.data() + y * stride;
    const int sy = y0 + y;
    const bool pillRow = sy >= pill_.y && sy < pill_.y + pill_.h;
    for (int x = xStart; x < xEnd; x++) {
      const uint8_t level = blit::level(row, blit::kFormatGray2, x);
      if (level == 0 || level == 3) continue;
      const int sx = x0 + x;
      if (pillRow && sx >= pill_.x && sx < pill_.x + pill_.w) continue;
      const int at = sy * kStride + (sx >> 3);
      const uint8_t bit = 0x80 >> (sx & 7);
      if (level == 1) {  // light
        lsb[at] &= ~bit;
        msb[at] |= bit;
      } else {  // dark
        lsb[at] |= bit;
        msb[at] &= ~bit;
      }
    }
  }
}
