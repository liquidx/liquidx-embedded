#include "ImageApp.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>

#include "../Fonts.h"

namespace {

constexpr const char* kDir = "/images";

// Caption pills, bottom-left of the card (docs/design/02-images.png).
constexpr int kPillX = 28;
constexpr int kPillGap = 8;
constexpr int kPillBottom = 29;  // from the bottom of the card

bool isBmp(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".bmp") == 0;
}

void drawMessage(GfxRenderer& r, const layout::Rect& area, const bool chrome, const char* title, const char* body) {
  const int top = ui::drawTitle(r, area, chrome ? "Images" : nullptr);
  const int x = area.x + layout::kMarginLeft;
  int baseline = top + 40;
  ui::drawTextAt(r, fonts::MEDIUM_22, x, baseline, title);
  baseline += 34;
  for (const auto& line : r.wrappedText(fonts::SMALL_15, body, 290, 4)) {
    ui::drawTextAt(r, fonts::SMALL_15, x, baseline, line.c_str());
    baseline += 19;
  }
}

// Where an image of `size` starts along an axis of `avail` pixels: centred
// when it fits, otherwise pinned to the start and cropped by the clip rect.
int imageOrigin(const int size, const int avail) { return size > avail ? 0 : (avail - size) / 2; }

// Draw the bitmap at its native resolution; never scaled.
bool drawNative(GfxRenderer& r, const Bitmap& bitmap, const layout::Rect& area) {
  const int x = area.x + imageOrigin(bitmap.getWidth(), area.w);
  const int y = area.y + imageOrigin(bitmap.getHeight(), area.h);
  return r.drawBitmap(bitmap, x, y, 0, 0);  // no max size: no scaling
}

}  // namespace

int ImageApp::itemCount() {
  // Home asks before the app is ever opened; scan once, then onOpen refreshes it.
  if (!scanned_) scan();
  return Storage.ready() ? static_cast<int>(files_.size()) : -1;
}

void ImageApp::onOpen() {
  // Rescan on every open so swapping SD contents is picked up.
  scan();
  index_ = std::clamp(index_, 0, std::max(0, static_cast<int>(files_.size()) - 1));
}

void ImageApp::scan() {
  scanned_ = true;
  files_.clear();
  if (!Storage.ready()) return;

  auto dir = Storage.open(kDir);
  if (!dir || !dir.isDirectory()) return;

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && isBmp(name)) files_.emplace_back(name);
    }
    file.close();
  }
  dir.close();
  std::sort(files_.begin(), files_.end());
}

void ImageApp::render(GfxRenderer& r, const layout::Rect& area, const bool chrome) {
  if (!Storage.ready()) {
    drawMessage(r, area, chrome, "No SD card", "Insert a card and reopen Images.");
    return;
  }
  if (files_.empty()) {
    drawMessage(r, area, chrome, "No images", "Copy .bmp files to /images on the SD card.");
    return;
  }

  const std::string path = std::string(kDir) + "/" + files_[index_];
  HalFile file;
  bool drawn = false;
  if (Storage.openFileForRead("IMG", path.c_str(), file)) {
    Bitmap bitmap(file, true);
    const auto err = bitmap.parseHeaders();
    if (err == BmpReaderError::Ok) {
      drawn = drawNative(r, bitmap, area);
    } else {
      LOG_ERR("IMG", "%s: %s", path.c_str(), Bitmap::errorToString(err));
    }
    file.close();
  }
  if (!drawn) drawMessage(r, area, chrome, "Can't show image", files_[index_].c_str());

  if (chrome) {
    const int countY = area.y + area.h - kPillBottom - ui::kPillH;
    const auto name = r.truncatedText(fonts::SMALL_15, files_[index_].c_str(), area.w / 2);
    ui::drawPill(r, fonts::SMALL_15, area.x + kPillX, countY - kPillGap - ui::kPillH, name.c_str());
    char counter[24];
    snprintf(counter, sizeof(counter), "%d / %d", index_ + 1, static_cast<int>(files_.size()));
    ui::drawPill(r, fonts::SMALL_15, area.x + kPillX, countY, counter, false);
  }
}

Result ImageApp::handle(const Action action) {
  const int count = static_cast<int>(files_.size());
  if (count < 2 || (action != Action::Up && action != Action::Down)) return Result::Ignored;
  index_ = (index_ + (action == Action::Up ? -1 : 1) + count) % count;
  return Result::CleanRedraw;
}
