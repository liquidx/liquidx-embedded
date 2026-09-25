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
#include "ImageApp.h"

namespace {

constexpr const char* kImagesDir = "/images";
constexpr const char* kPrefs = "ble";
constexpr const char* kPrefLast = "last";  // path of the last persisted frame
constexpr const char* kPrefSeq = "seq";    // counter for unnamed frames

// Sleeping between frames: only worth it for gaps this long, and wake this
// much early to boot and start advertising before the sender comes back.
constexpr uint32_t kMinSleepGapSeconds = 30;
constexpr uint32_t kWakeMarginSeconds = 10;

// The original X4's deep sleep cuts battery power, so a timer can't wake it.
#if FREEINK_MCU_C3
constexpr bool kCanTimerWake = false;
#else
constexpr bool kCanTimerWake = true;
#endif

// Battery level checks, for the host's power events.
constexpr uint32_t kBatteryPollMs = 30 * 1000;
constexpr int kBatteryStep = 5;  // percent

// gray2 level (0 white .. 3 black) of pixel x in a row.
uint8_t level2(const uint8_t* row, const int x) { return (row[x >> 2] >> (6 - 2 * (x & 3))) & 3; }

void put16(uint8_t* p, const uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
void put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

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

uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// Write a mono1 or gray2 frame (top-down) as a bottom-up 1- or 2-bit BMP. The
// palette is index = ink, like the formats, so gray2 rows copy straight over.
bool writeBmp(const char* path, const uint8_t* bits, const uint8_t format, const int w, const int h) {
  const bool gray = format == cast::kFormatGray2;
  const int bpp = gray ? 2 : 1;
  const int colors = 1 << bpp;
  const int srcStride = cast::rowBytes(format, w);
  const int dstStride = ((w * bpp + 31) / 32) * 4;
  const uint32_t headerBytes = 14 + 40 + 4 * colors;
  uint8_t header[14 + 40 + 16] = {'B', 'M'};
  put32(header + 2, headerBytes + dstStride * h);  // file size
  put32(header + 10, headerBytes);                  // pixel data offset
  put32(header + 14, 40);                           // BITMAPINFOHEADER
  put32(header + 18, w);
  put32(header + 22, h);  // positive: bottom-up
  put16(header + 26, 1);  // planes
  put16(header + 28, bpp);
  put32(header + 34, dstStride * h);
  put32(header + 38, 2835);  // 72 dpi
  put32(header + 42, 2835);
  put32(header + 46, colors);
  // Palette (B, G, R, 0): white down to black.
  for (int i = 0; i < colors; i++) {
    const uint8_t v = 255 - 255 * i / (colors - 1);
    memset(header + 54 + 4 * i, v, 3);
  }

  // One write for the whole file: SdFat is much slower with many small ones.
  const size_t size = headerBytes + static_cast<size_t>(dstStride) * h;
  cast::Buffer out;
  if (!out.reserve(size)) return false;
  memcpy(out.data(), header, headerBytes);
  for (int y = 0; y < h; y++) {
    const uint8_t* src = bits + (h - 1 - y) * srcStride;
    uint8_t* dst = out.data() + headerBytes + y * dstStride;
    memcpy(dst, src, srcStride);
    memset(dst + srcStride, 0, dstStride - srcStride);
  }

  if (Storage.exists(path)) Storage.remove(path);
  HalFile file;
  if (!Storage.openFileForWrite("BLE", path, file)) return false;
  const bool ok = file.write(out.data(), size) == size;
  file.close();
  return ok;
}

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
  donePending_ = false;
  const auto area = layout::cardArea(1);
  cast::server.setArea(area.w, area.h);
  cast::server.setFrameSleep(settings::value(settings::kFrameSleep) != 0);
#if FREEINK_MCU_C3
  cast::server.setGrayscale(false);  // no PSRAM for the grey planes
#else
  cast::server.setGrayscale(display.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported());
#endif
  sentBattery_ = 255;
  lastBatteryPollMs_ = millis() - kBatteryPollMs;  // poll now, for the caps
  pollBattery();
  if (!cast::server.begin()) LOG_ERR("BLE", "Couldn't start the cast server");
}

void BleApp::onClose() {
  donePending_ = false;
  cast::server.end();
}

bool BleApp::takeActivity() { return cast::server.takeActivity(); }

uint32_t BleApp::takeSleepRequest() {
  if (donePending_) return 0;  // the host hasn't been told yet
  const uint32_t seconds = sleepRequest_;
  sleepRequest_ = 0;
  return seconds;
}

// Up / Down / Select belong to the host (caps `keys`); Back stays with the
// shell, for leaving the app.
Result BleApp::handle(const Action action) {
  switch (action) {
    case Action::Up:
      cast::server.sendKey(cast::Key::Up);
      break;
    case Action::Down:
      cast::server.sendKey(cast::Key::Down);
      break;
    case Action::Select:
      cast::server.sendKey(cast::Key::Select);
      break;
    default:
      break;
  }
  return Result::Ignored;
}

BleApp::Status BleApp::status() const {
  if (!cast::server.running()) return Status::Unavailable;
  if (cast::server.receiving()) return Status::Receiving;
  if (!cast::server.connected()) return Status::Listening;
  if (cast::server.failed()) return Status::Failed;
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
      cast::server.hostName(out, size);
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
  const bool usb = gpio.isUsbConnected();
  cast::server.setBattery(percent, usb);
  const bool first = sentBattery_ == 255;
  if (!first && std::abs(percent - sentBattery_) < kBatteryStep && usb == sentUsb_) return;
  if (!first) cast::server.sendPower();
  sentBattery_ = percent;
  sentUsb_ = usb;
}

Result BleApp::tick() {
  pollBattery();
  cast::FrameHeader header;
  if (!cast::server.takeFrame(header, frame_, region_)) {
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
Result BleApp::showFrame(const cast::FrameHeader& header) {
  cast::FrameHeader saved = header;
  if (header.region()) {
    // Patch the full frame. The server checked the region fits it: same
    // format, starting on a byte, within the frame (which is the area).
    const uint32_t stride = cast::rowBytes(header_.format, header_.width);
    const uint32_t rowBytes = cast::rowBytes(header.format, header.width);
    const uint32_t xByte = cast::rowBytes(header.format, header.x);
    for (int y = 0; y < header.height; y++) {
      memcpy(frame_.data() + (header.y + y) * stride + xByte, region_.data() + y * rowBytes, rowBytes);
    }
    saved = header_;
    memcpy(saved.name, header.name, sizeof(saved.name));
  } else {
    header_ = header;
    haveFrame_ = true;
    cast::server.setRegionBase(header);
  }
  nextSeconds_ = header.nextFrameSeconds;

  if (header.flags & cast::kFlagPersist) {
    char path[96];
    if (!persist(saved, path, sizeof(path))) {
      cast::server.notifyError(cast::Error::SaveFailed);
      return header.hold() ? Result::Ignored : Result::CleanRedraw;
    }
  }

  // Held: more regions follow, and the next frame without hold shows them all.
  if (header.hold()) {
    cast::server.notifyDone(0);
    return Result::Ignored;
  }

  // Sleep until just before the next frame, if asked to and it's worth it.
  sleepRequest_ = 0;
  if (kCanTimerWake && settings::value(settings::kFrameSleep) != 0 &&
      header.nextFrameSeconds >= kMinSleepGapSeconds) {
    sleepRequest_ = header.nextFrameSeconds - kWakeMarginSeconds;
  }
  donePending_ = true;  // see presented()

  // The refresh hint: fast, full, or ours to pick (fast for a region, which
  // is usually a small UI change; clean for a new frame).
  switch (header.refresh) {
    case cast::Refresh::Fast:
      return Result::Redraw;
    case cast::Refresh::Full:
      return Result::CleanRedraw;
    case cast::Refresh::Auto:
      break;
  }
  return header.region() ? Result::Redraw : Result::CleanRedraw;
}

// `done` means "on the panel": hosts treat it as the go-ahead for the next frame.
void BleApp::presented() {
  if (!donePending_) return;
  donePending_ = false;
  cast::server.notifyDone(sleepRequest_);
}

bool BleApp::persist(const cast::FrameHeader& header, char* path, const size_t pathSize) {
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
  const bool ok = writeBmp(path, frame_.data(), header.format, header.width, header.height);
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
  cast::server.setArea(area.w, area.h);
  chrome_ = chrome;
  shownStatus_ = status();
  char text[40];
  statusText(text, sizeof(text));

  // A frame fills the card (the last received, or the last saved one); the
  // title pill over it shows the link status. Full screen shows just the frame.
  showingFrame_ = haveFrame_ || (lastPath_[0] != '\0' && ui::drawBitmapFile(r, lastPath_, area));
  grayShown_ = haveFrame_ && header_.format == cast::kFormatGray2;
  if (haveFrame_) drawFrame(r, area);
  char title[56];
  snprintf(title, sizeof(title), "Bluetooth / %s", text);
  ui::drawTitle(r, area, chrome ? title : nullptr, &pill_);
  if (showingFrame_) return;

  const int textW = r.getTextWidth(fonts::MEDIUM_22, text);
  const int midY = area.y + area.h / 2;
  ui::drawTextAt(r, fonts::MEDIUM_22, area.x + (area.w - textW) / 2, midY, text);
  if (cast::server.running()) {
    const int nameW = r.getTextWidth(fonts::SMALL_15, cast::server.name());
    ui::drawTextAt(r, fonts::SMALL_15, area.x + (area.w - nameW) / 2, midY + 30, cast::server.name());
  }
}

// Read a 1- or 2-bit BMP (what persist() writes) into frame_ as mono1 or
// gray2. Other depths return false and are drawn from the file instead.
bool BleApp::loadLastFrame() {
  HalFile file;
  if (!Storage.openFileForRead("BLE", lastPath_, file)) return false;
  uint8_t head[54];
  bool ok = file.read(head, sizeof(head)) == sizeof(head) && head[0] == 'B' && head[1] == 'M';
  const uint32_t dataOffset = get32(head + 10);
  const int32_t w = static_cast<int32_t>(get32(head + 18));
  const int32_t rawH = static_cast<int32_t>(get32(head + 22));
  const int32_t h = rawH < 0 ? -rawH : rawH;
  const int bpp = get16(head + 28);
  ok = ok && (bpp == 1 || bpp == 2) && get32(head + 30) == 0 && w > 0 && h > 0 && w <= 2048 && h <= 2048;
  const uint8_t format = bpp == 2 ? cast::kFormatGray2 : cast::kFormatMono1;

  // Palette index -> ink level, then a table taking a whole byte of indices
  // to a byte of levels (the pixels stay where they are).
  const int colors = 1 << bpp;
  uint32_t used = get32(head + 46);
  if (used == 0 || used > static_cast<uint32_t>(colors)) used = colors;
  uint8_t palette[16] = {};
  ok = ok && file.seek(14 + get32(head + 14)) && file.read(palette, used * 4) == static_cast<int>(used * 4);
  uint8_t level[4] = {};
  for (uint32_t i = 0; i < used; i++) {
    const uint8_t* p = palette + 4 * i;  // B, G, R
    const int lum = (29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8;
    level[i] = bpp == 1 ? (lum < 128 ? 1 : 0) : (255 - lum + 42) / 85;
  }
  uint8_t lut[256];
  for (int b = 0; b < 256; b++) {
    uint8_t out = 0;
    for (int shift = 8 - bpp; shift >= 0; shift -= bpp) out |= level[(b >> shift) & (colors - 1)] << shift;
    lut[b] = out;
  }

  const int srcStride = ((w * bpp + 31) / 32) * 4;
  const int dstStride = cast::rowBytes(format, w);
  ok = ok && frame_.reserve(static_cast<size_t>(dstStride) * h) && file.seek(dataOffset);
  for (int32_t row = 0; ok && row < h; row++) {
    const int32_t y = rawH > 0 ? h - 1 - row : row;  // bottom-up unless height < 0
    uint8_t* dst = frame_.data() + y * dstStride;
    ok = file.read(dst, dstStride) == dstStride && (srcStride == dstStride || file.seek(file.position() + srcStride - dstStride));
    for (int i = 0; ok && i < dstStride; i++) dst[i] = lut[dst[i]];
  }
  file.close();
  if (!ok) return false;
  header_ = cast::FrameHeader{};
  header_.format = format;
  header_.width = w;
  header_.height = h;
  return true;
}

// Blit the frame at native size: pinned top-left when larger than the area
// (the clip rect crops it), centred when smaller. gray2 goes in as black and
// white (dark and black ink); drawGray() adds the greys.
void BleApp::drawFrame(GfxRenderer& r, const layout::Rect& area) const {
  const int w = header_.width, h = header_.height;
  const int x0 = area.x + ui::imageOrigin(w, area.w);
  const int y0 = area.y + ui::imageOrigin(h, area.h);
  const int stride = cast::rowBytes(header_.format, w);
  const int rows = std::min(h, area.y + area.h - y0);
  const int cols = std::min(w, area.x + area.w - x0);
  const bool gray = header_.format == cast::kFormatGray2;
  const uint8_t* bits = frame_.data();
  for (int y = 0; y < rows; y++) {
    const uint8_t* row = bits + y * stride;
    for (int x = 0; x < cols; x++) {
      const bool ink = gray ? level2(row, x) >= 2 : row[x >> 3] & (0x80 >> (x & 7));
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
  const int stride = cast::rowBytes(header_.format, w);
  const int yEnd = std::min({h, area.y + area.h - y0, layout::kScreenH - y0});
  const int xEnd = std::min({w, area.x + area.w - x0, layout::kScreenW - x0});
  const int xStart = std::max({0, area.x - x0, -x0});
  for (int y = std::max({0, area.y - y0, -y0}); y < yEnd; y++) {
    const uint8_t* row = frame_.data() + y * stride;
    const int sy = y0 + y;
    const bool pillRow = sy >= pill_.y && sy < pill_.y + pill_.h;
    for (int x = xStart; x < xEnd; x++) {
      const uint8_t level = level2(row, x);
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
