#pragma once

#include "../SdSpace.h"
#include "../Settings.h"
#include "../shell/App.h"

// Settings: a list of rows with their current values. Select opens a row's
// page: a vertical choice (Refresh, Sleep after, Clock) or About.
class SettingsApp : public App {
 public:
  const char* name() const override { return "Settings"; }
  ui::Icon icon() const override { return ui::Icon::Sliders; }
  void onOpen() override;
  void render(GfxRenderer& r, const layout::Rect& area, bool chrome) override;
  Result handle(Action action) override;
  int depth() const override { return page_ == Page::List ? 0 : 1; }
  Result tick() override;
  bool busy() const override { return !storageScan_.done(); }

 private:
  enum class Page : uint8_t { List, Choice, About };

  static const char* rowLabel(const void* ctx, int index);
  static const char* rowValue(const void* ctx, int index);
  void renderChoice(GfxRenderer& r, const layout::Rect& area, bool chrome) const;
  void renderAbout(GfxRenderer& r, const layout::Rect& area, bool chrome);

  Page page_ = Page::List;
  const settings::Choice* choice_ = nullptr;  // open on Page::Choice
  ui::ListView list_;
  ui::InfoView info_;
  char storage_[24] = "...";
  SdSpaceScan storageScan_;  // runs once per boot, a chunk per tick
  char version_[24] = "";
};
