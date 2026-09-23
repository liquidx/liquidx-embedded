#pragma once

#include <string>
#include <vector>

#include "../shell/App.h"
#include "../ui/Widgets.h"

// Lists BMP files from /images on the SD card; Select views one, Up/Down step
// through them, Back returns to the list.
class ImageApp : public App {
 public:
  const char* name() const override { return "Images"; }
  void onOpen() override;
  void render(GfxRenderer& r, const layout::Rect& area, bool chrome) override;
  Result handle(Action action) override;
  bool canGoBack() const override { return viewing_; }
  KeyHints hints() const override;

 private:
  void scan();
  void renderViewer(GfxRenderer& r, const layout::Rect& area, bool chrome) const;
  static const char* fileLabel(const void* ctx, int index);

  std::vector<std::string> files_;
  ui::ListView list_;
  bool viewing_ = false;
};
