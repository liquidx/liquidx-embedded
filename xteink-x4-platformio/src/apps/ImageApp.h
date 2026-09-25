#pragma once

#include <string>
#include <vector>

#include "../shell/App.h"

// Shows the BMP files in /images on the SD card, one per page, at native
// resolution: larger images are cropped from the top-left, smaller ones
// centred. Up/Down step through them.
class ImageApp : public App {
 public:
  const char* name() const override { return "Images"; }
  ui::Icon icon() const override { return ui::Icon::Image; }
  int itemCount() override;
  void onOpen() override;
  void render(GfxRenderer& r, const layout::Rect& area, bool chrome) override;
  Result handle(Action action) override;

  // Something else wrote to /images: rescan before the next count.
  static void markStale() { stale_ = true; }

 private:
  void scan();

  std::vector<std::string> files_;
  int index_ = 0;
  bool scanned_ = false;
  static inline bool stale_ = false;
};
