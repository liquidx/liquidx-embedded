#include "BleApp.h"

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

// Write raw1 (1 = black, top-down) as a bottom-up 1-bit BMP.
bool writeBmp(const char* path, const uint8_t* bits, const int w, const int h) {
  const int srcStride = (w + 7) / 8;
  const int dstStride = ((w + 31) / 32) * 4;
  constexpr uint32_t kHeaderBytes = 14 + 40 + 8;
  uint8_t header[kHeaderBytes] = {'B', 'M'};
  put32(header + 2, kHeaderBytes + dstStride * h);  // file size
  put32(header + 10, kHeaderBytes);                  // pixel data offset
  put32(header + 14, 40);                            // BITMAPINFOHEADER
  put32(header + 18, w);
  put32(header + 22, h);  // positive: bottom-up
  put16(header + 26, 1);  // planes
  put16(header + 28, 1);  // bpp
  put32(header + 34, dstStride * h);
  put32(header + 38, 2835);  // 72 dpi
  put32(header + 42, 2835);
  put32(header + 46, 2);  // palette entries
  // Palette: index 0 black, index 1 white.
  header[58] = header[59] = header[60] = 0xFF;

  // One write for the whole file: SdFat is much slower with many small ones.
  const size_t size = kHeaderBytes + static_cast<size_t>(dstStride) * h;
  cast::Buffer out;
  if (!out.reserve(size)) return false;
  memcpy(out.data(), header, kHeaderBytes);
  for (int y = 0; y < h; y++) {
    const uint8_t* src = bits + (h - 1 - y) * srcStride;
    uint8_t* dst = out.data() + kHeaderBytes + y * dstStride;
    for (int i = 0; i < dstStride; i++) dst[i] = i < srcStride ? ~src[i] : 0;  // BMP index 1 = white
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
  const auto area = layout::cardArea(1);
  cast::server.setArea(area.w, area.h);
  cast::server.setFrameSleep(settings::value(settings::kFrameSleep) != 0);
  if (!cast::server.begin()) LOG_ERR("BLE", "Couldn't start the cast server");
}

void BleApp::onClose() { cast::server.end(); }

bool BleApp::takeActivity() { return cast::server.takeActivity(); }

uint32_t BleApp::takeSleepRequest() {
  const uint32_t seconds = sleepRequest_;
  sleepRequest_ = 0;
  return seconds;
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
      snprintf(out, size, "Connected");
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

Result BleApp::tick() {
  cast::FrameHeader header;
  if (!cast::server.takeFrame(header, frame_)) {
    // Redraw (a fast refresh) only when the status in the pill changes.
    return status() != shownStatus_ ? Result::Redraw : Result::Ignored;
  }
  header_ = header;
  haveFrame_ = true;
  nextSeconds_ = header.nextFrameSeconds;

  if (header.flags & cast::kFlagPersist) {
    char path[96];
    if (!persist(header, path, sizeof(path))) {
      cast::server.notifyError(cast::Error::SaveFailed);
      return Result::CleanRedraw;
    }
  }

  // Sleep until just before the next frame, if asked to and it's worth it.
  sleepRequest_ = 0;
  if (kCanTimerWake && settings::value(settings::kFrameSleep) != 0 &&
      header.nextFrameSeconds >= kMinSleepGapSeconds) {
    sleepRequest_ = header.nextFrameSeconds - kWakeMarginSeconds;
  }
  cast::server.notifyDone(sleepRequest_);
  return Result::CleanRedraw;
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
  const bool ok = writeBmp(path, frame_.data(), header.width, header.height);
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
  shownStatus_ = status();
  char text[32];
  statusText(text, sizeof(text));

  // A frame fills the card (the last received, or the last saved one); the
  // title pill over it shows the link status. Full screen shows just the frame.
  showingFrame_ = haveFrame_ || (lastPath_[0] != '\0' && ui::drawBitmapFile(r, lastPath_, area));
  if (haveFrame_) drawFrame(r, area);
  char title[48];
  snprintf(title, sizeof(title), "Bluetooth / %s", text);
  ui::drawTitle(r, area, chrome ? title : nullptr);
  if (showingFrame_) return;

  const int textW = r.getTextWidth(fonts::MEDIUM_22, text);
  const int midY = area.y + area.h / 2;
  ui::drawTextAt(r, fonts::MEDIUM_22, area.x + (area.w - textW) / 2, midY, text);
  if (cast::server.running()) {
    const int nameW = r.getTextWidth(fonts::SMALL_15, cast::server.name());
    ui::drawTextAt(r, fonts::SMALL_15, area.x + (area.w - nameW) / 2, midY + 30, cast::server.name());
  }
}

// Read a 1-bit BMP (what persist() writes) into frame_ as raw1. Other depths
// return false and are drawn from the file instead.
bool BleApp::loadLastFrame() {
  HalFile file;
  if (!Storage.openFileForRead("BLE", lastPath_, file)) return false;
  uint8_t head[62];
  bool ok = file.read(head, sizeof(head)) == sizeof(head) && head[0] == 'B' && head[1] == 'M';
  const uint32_t dataOffset = get32(head + 10);
  const int32_t w = static_cast<int32_t>(get32(head + 18));
  const int32_t rawH = static_cast<int32_t>(get32(head + 22));
  const int32_t h = rawH < 0 ? -rawH : rawH;
  ok = ok && get16(head + 28) == 1 && get32(head + 30) == 0 && w > 0 && h > 0 && w <= 2048 && h <= 2048;
  // Palette entry 0 (B, G, R): which bit value is black.
  const uint8_t* entry0 = head + 14 + get32(head + 14);
  const bool zeroIsBlack = ok && entry0 + 2 < head + sizeof(head) && entry0[0] + entry0[1] + entry0[2] < 384;
  const int srcStride = ((w + 31) / 32) * 4;
  const int dstStride = (w + 7) / 8;
  ok = ok && frame_.reserve(static_cast<size_t>(dstStride) * h) && file.seek(dataOffset);
  for (int32_t row = 0; ok && row < h; row++) {
    const int32_t y = rawH > 0 ? h - 1 - row : row;  // bottom-up unless height < 0
    uint8_t* dst = frame_.data() + y * dstStride;
    ok = file.read(dst, dstStride) == dstStride && (srcStride == dstStride || file.seek(file.position() + srcStride - dstStride));
    if (ok && zeroIsBlack) {
      for (int i = 0; i < dstStride; i++) dst[i] = ~dst[i];
    }
  }
  file.close();
  if (!ok) return false;
  header_ = cast::FrameHeader{};
  header_.format = cast::kFormatRaw1;
  header_.width = w;
  header_.height = h;
  return true;
}

// Blit the raw1 frame at native size: pinned top-left when larger than the
// area (the clip rect crops it), centred when smaller.
void BleApp::drawFrame(GfxRenderer& r, const layout::Rect& area) const {
  const int w = header_.width, h = header_.height;
  const int x0 = area.x + ui::imageOrigin(w, area.w);
  const int y0 = area.y + ui::imageOrigin(h, area.h);
  const int stride = (w + 7) / 8;
  const int rows = std::min(h, area.y + area.h - y0);
  const int cols = std::min(w, area.x + area.w - x0);
  const uint8_t* bits = frame_.data();
  for (int y = 0; y < rows; y++) {
    const uint8_t* row = bits + y * stride;
    for (int x = 0; x < cols; x++) {
      if (row[x >> 3] & (0x80 >> (x & 7))) r.drawPixel(x0 + x, y0 + y);
    }
  }
}
