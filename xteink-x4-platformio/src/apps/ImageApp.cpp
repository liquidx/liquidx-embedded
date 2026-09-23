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
constexpr int kCaptionH = 28;

bool isBmp(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".bmp") == 0;
}

void drawMessage(GfxRenderer& r, const layout::Rect& area, const char* title, const char* body) {
  const int x = area.x + layout::kPad * 2;
  int y = area.h / 2 - r.getLineHeight(fonts::TITLE_18);
  r.drawText(fonts::TITLE_18, x, y, title, true, EpdFontFamily::BOLD);
  y += r.getLineHeight(fonts::TITLE_18) + 8;
  for (const auto& line : r.wrappedText(fonts::BODY_14, body, area.w - layout::kPad * 4, 4)) {
    r.drawText(fonts::BODY_14, x, y, line.c_str());
    y += r.getLineHeight(fonts::BODY_14);
  }
}

}  // namespace

const char* ImageApp::fileLabel(const void* ctx, const int index) {
  return static_cast<const ImageApp*>(ctx)->files_[index].c_str();
}

void ImageApp::onOpen() {
  // Rescan on every open so swapping SD contents is picked up.
  viewing_ = false;
  list_.reset();
  scan();
}

void ImageApp::scan() {
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
    drawMessage(r, area, "No SD card", "Insert a card and reopen Images.");
  } else if (files_.empty()) {
    drawMessage(r, area, "No images", "Copy .bmp files to /images on the SD card.");
  } else if (viewing_) {
    renderViewer(r, area, chrome);
  } else {
    list_.render(r, area, chrome ? "Images" : nullptr, static_cast<int>(files_.size()), fileLabel, this);
  }
}

void ImageApp::renderViewer(GfxRenderer& r, const layout::Rect& area, const bool chrome) const {
  const int index = list_.selected();
  const std::string path = std::string(kDir) + "/" + files_[index];
  const int maxW = area.w;
  const int maxH = area.h - (chrome ? kCaptionH : 0);

  HalFile file;
  bool drawn = false;
  if (Storage.openFileForRead("IMG", path.c_str(), file)) {
    Bitmap bitmap(file, true);
    const auto err = bitmap.parseHeaders();
    if (err == BmpReaderError::Ok) {
      // drawBitmap scales down to fit; centre using the fitted size.
      const float scale = std::min(1.0f, std::min(static_cast<float>(maxW) / bitmap.getWidth(),
                                                   static_cast<float>(maxH) / bitmap.getHeight()));
      const int w = static_cast<int>(bitmap.getWidth() * scale);
      const int h = static_cast<int>(bitmap.getHeight() * scale);
      drawn = r.drawBitmap(bitmap, area.x + (maxW - w) / 2, area.y + (maxH - h) / 2, maxW, maxH);
    } else {
      LOG_ERR("IMG", "%s: %s", path.c_str(), Bitmap::errorToString(err));
    }
    file.close();
  }
  if (!drawn) drawMessage(r, area, "Can't show image", files_[index].c_str());

  if (chrome) {
    char caption[300];
    snprintf(caption, sizeof(caption), "%d / %d   %s", index + 1, static_cast<int>(files_.size()),
             files_[index].c_str());
    const auto text = r.truncatedText(fonts::UI_10, caption, area.w - layout::kPad * 2);
    r.drawText(fonts::UI_10, area.x + layout::kPad, area.y + area.h - kCaptionH + 4, text.c_str());
  }
}

KeyHints ImageApp::hints() const {
  if (files_.empty()) return {};
  const bool canStep = files_.size() > 1;
  if (viewing_) return {nullptr, nullptr, canStep ? "Prev" : nullptr, canStep ? "Next" : nullptr};
  return {nullptr, "View", canStep ? "Up" : nullptr, canStep ? "Down" : nullptr};
}

Result ImageApp::handle(const Action action) {
  if (files_.empty()) return Result::Ignored;
  const int count = static_cast<int>(files_.size());
  switch (action) {
    case Action::Up:
    case Action::Down:
      // The viewer shares the list selection, so Back lands on the last image seen.
      if (!list_.move(action == Action::Up ? -1 : 1, count)) return Result::Ignored;
      return viewing_ ? Result::CleanRedraw : Result::Redraw;
    case Action::Select:
      if (viewing_) return Result::Ignored;
      viewing_ = true;
      return Result::CleanRedraw;
    case Action::Back:
      viewing_ = false;
      return Result::CleanRedraw;
    default:
      return Result::Ignored;
  }
}
